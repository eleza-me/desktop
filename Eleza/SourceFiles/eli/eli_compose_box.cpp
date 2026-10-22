// Compose («الی تازه» / reply / quote / invite / edit): text with a ring
// counter and highlighted mentions / hashtags, mention autocomplete, photos and
// videos (picked, pasted or dropped; reorder by dragging, alt text, upload
// progress with cancel), a poll builder, thread parts (and «تبدیل به رشته» for
// a text that is too long), the reply policy, a preview of what is replied to
// or quoted, a local draft, Ctrl+Enter to publish and a confirmation before a
// draft is thrown away.
#include "eli/eli_actions.h"

#include "api/api_client_config.h"
#include "data/data_feed.h"
#include "data/data_image_decode.h"
#include "data/data_session.h"
#include "data/data_upload.h"
#include "data/data_users.h"
#include "eli/eli_logic.h"
#include "eli/eli_post_card.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <QtGui/QClipboard>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QSyntaxHighlighter>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>

namespace Eli {

namespace {

constexpr auto kPollOptionLength = 60;
constexpr auto kAltLength = 420;
constexpr auto kMentionDelay = 250;
constexpr auto kThumbSide = 320;

using Kind = ComposeArgs::Kind;

struct Duration {
	int hours = 0;
	QString label;
};

[[nodiscard]] QVector<Duration> PollDurations() {
	return {
		{ 1, tr::lng_eli_duration_1h(tr::now) },
		{ 24, tr::lng_eli_duration_1d(tr::now) },
		{ 72, tr::lng_eli_duration_3d(tr::now) },
		{ 168, tr::lng_eli_duration_7d(tr::now) },
	};
}

struct Policy {
	QString key;
	QString label;
};

[[nodiscard]] QVector<Policy> Policies() {
	return {
		{ u"everyone"_s, tr::lng_eli_policy_everyone_long(tr::now) },
		{ u"followers"_s, tr::lng_eli_policy_followers_long(tr::now) },
		{ u"following"_s, tr::lng_eli_policy_following_long(tr::now) },
		{ u"mentioned"_s, tr::lng_eli_policy_mentioned_long(tr::now) },
	};
}

[[nodiscard]] Logic::ComposeKind LogicKind(Kind kind) {
	switch (kind) {
	case Kind::New: return Logic::ComposeKind::New;
	case Kind::Reply: return Logic::ComposeKind::Reply;
	case Kind::Quote: return Logic::ComposeKind::Quote;
	case Kind::Invite: return Logic::ComposeKind::Invite;
	case Kind::Edit: return Logic::ComposeKind::Edit;
	}
	return Logic::ComposeKind::New;
}

// Colors @mentions, #hashtags and links while typing.
class Highlighter final : public QSyntaxHighlighter {
public:
	using QSyntaxHighlighter::QSyntaxHighlighter;

protected:
	void highlightBlock(const QString &text) override {
		const auto parsed = Ui::Text::ParseEntities(text);
		auto format = QTextCharFormat();
		format.setForeground(st::eliComposeMentionFg->c);
		for (const auto &entity : parsed.entities) {
			if (Ui::Text::IsLinkType(entity.type)) {
				setFormat(entity.offset, entity.length, format);
			}
		}
	}

};

// The ring that fills as the text nears the limit.
class CounterRing final : public Ui::RpWidget {
public:
	explicit CounterRing(QWidget *parent) : RpWidget(parent) {
		resize(st::eliComposeRingSize + 26, st::eliComposeRingSize);
	}

	void setState(int length, int limit) {
		_length = length;
		_limit = std::max(limit, 1);
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		PainterHighQualityEnabler hq(p);
		const auto size = st::eliComposeRingSize;
		const auto thickness = st::eliComposeRingThickness;
		const auto ring = QRectF(
			style::rtlrect(width() - size, 0, size, size, width())).adjusted(
				thickness / 2.,
				thickness / 2.,
				-thickness / 2.,
				-thickness / 2.);
		auto pen = QPen(st::eliComposeRingBg->c);
		pen.setWidthF(thickness);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.drawEllipse(ring);
		const auto over = (_length > _limit);
		const auto fraction = std::min(double(_length) / _limit, 1.);
		pen.setColor(over ? st::eliComposeRingFgOver->c : st::eliComposeRingFg->c);
		p.setPen(pen);
		if (fraction > 0.) {
			p.drawArc(ring, 90 * 16, -int(fraction * 360 * 16));
		}
		const auto remaining = _limit - _length;
		if (remaining <= 20) {
			p.setPen(over ? st::eliComposeRingFgOver : st::textSecondary);
			p.setFont(st::eliComposeRingFont);
			const auto text = Lang::Num(remaining);
			const auto textWidth = st::eliComposeRingFont->width(text);
			p.drawTextLeft(
				width() - size - 6 - textWidth,
				(height() - st::eliComposeRingFont->height) / 2,
				width(),
				text,
				textWidth);
		}
	}

private:
	int _length = 0;
	int _limit = 500;

};

// What is replied to / quoted, in a frame.
class PreviewCard final : public Ui::RpWidget {
public:
	PreviewCard(
		QWidget *parent,
		Main::Session *session,
		const Data::Post &post)
	: RpWidget(parent) {
		const auto author = post.author;
		const auto createdAt = post.createdAt;
		const auto refresh = [=, this] {
			const auto user = session->data().users().get(author);
			_name = AuthorName(*session, author);
			_meta = ((user.handle.isEmpty() || user.deleted)
				? QString()
				: (IsolatedHandle(user.handle) + tr::lng_eli_meta_separator(tr::now)))
				+ PostTime(createdAt);
			update();
		};
		refresh();
		connect(&session->data().users(), &Data::Users::changed, this, refresh);
		auto text = post.text;
		if (text.isEmpty() && post.conversation) {
			text = post.conversation->topic;
		}
		_text.setMarkedText(st::eliQuoteTextStyle, PostText(text, false));
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::eliPreviewPadding;
		const auto inner = newWidth - padding.left() - padding.right();
		auto height = padding.top() + st::eliQuoteHeaderHeight;
		if (!_text.isEmpty()) {
			height += 2 + _text.countHeight(inner, st::eliQuoteTextLines);
		}
		return height + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::eliPreviewPadding;
		const auto inner = width() - padding.left() - padding.right();
		{
			PainterHighQualityEnabler hq(p);
			p.setPen(st::eliPreviewBorderFg);
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(
				QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
				st::eliPreviewRadius,
				st::eliPreviewRadius);
		}
		const auto metaWidth = std::min(st::eliQuoteMetaFont->width(_meta), inner / 2);
		const auto nameWidth = std::min(
			st::eliQuoteNameFont->width(_name),
			std::max(inner - metaWidth - 6, 0));
		const auto top = padding.top()
			+ (st::eliQuoteHeaderHeight - st::eliQuoteNameFont->height) / 2;
		p.setPen(st::eliNameFg);
		p.setFont(st::eliQuoteNameFont);
		p.drawTextLeft(
			padding.left(),
			top,
			width(),
			st::eliQuoteNameFont->elided(_name, nameWidth),
			nameWidth);
		p.setPen(st::eliMetaFg);
		p.setFont(st::eliQuoteMetaFont);
		const auto meta = st::eliQuoteMetaFont->elided(_meta, metaWidth);
		p.drawTextLeft(
			padding.left() + nameWidth + 6,
			top,
			width(),
			meta,
			st::eliQuoteMetaFont->width(meta));
		if (!_text.isEmpty()) {
			p.setPen(st::eliTextFg);
			auto context = Ui::Text::PaintContext();
			context.position = QPoint(
				padding.left(),
				padding.top() + st::eliQuoteHeaderHeight + 2);
			context.availableWidth = inner;
			context.elisionLines = st::eliQuoteTextLines;
			context.autoDirection = true;
			_text.draw(p, context);
		}
	}

private:
	QString _name;
	QString _meta;
	Ui::Text::String _text;

};

// Attached photos / videos.
class Tiles final : public Ui::RpWidget {
public:
	struct Item {
		QString path;
		bool video = false;
		QString alt;
		QString mediaId; // uploaded
		QImage thumb;
		QPixmap prepared;
		double progress = -1.; // < 0: not uploading
	};

	explicit Tiles(QWidget *parent)
	: RpWidget(parent)
	, _radial([this] { update(); }) {
		setMouseTracking(true);
	}

	Fn<void()> changed;
	Fn<void(int index)> altRequested;

	[[nodiscard]] std::vector<Item> &items() {
		return _items;
	}
	[[nodiscard]] int count() const {
		return int(_items.size());
	}
	void setLocked(bool locked) {
		_locked = locked;
		if (locked) {
			_radial.start();
		} else {
			_radial.stop();
		}
		update();
	}

	void add(const QString &path, bool video) {
		auto item = Item();
		item.path = path;
		item.video = video;
		_items.push_back(std::move(item));
		if (!video) {
			loadThumb(path);
		}
		relayout();
	}

	void setProgress(int index, double progress) {
		if (index >= 0 && index < count()) {
			_items[index].progress = progress;
			update(tileRect(index));
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_items.empty()) {
			return 0;
		}
		const auto tile = st::eliComposeTile;
		const auto skip = st::eliComposeTileSkip;
		_perRow = std::max((newWidth + skip) / (tile + skip), 1);
		const auto rows = (count() + _perRow - 1) / _perRow;
		return rows * tile + (rows - 1) * skip + skip;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		PainterHighQualityEnabler hq(p);
		for (auto i = 0; i != count(); ++i) {
			if (i == _dragging && _dragMoved) {
				continue;
			}
			paintTile(p, i, tileRect(i), 1.);
		}
		if (_dragging >= 0 && _dragMoved) {
			const auto rect = tileRect(_dragging).translated(_dragPoint - _dragStart);
			paintTile(p, _dragging, rect, 0.8);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		if (_dragging >= 0) {
			_dragPoint = e->pos();
			if (!_dragMoved
				&& (_dragPoint - _dragStart).manhattanLength() >= QApplication::startDragDistance()) {
				_dragMoved = true;
			}
			if (_dragMoved) {
				const auto over = indexAt(e->pos());
				if (over >= 0 && over != _dragging) {
					auto moved = std::move(_items[_dragging]);
					_items.erase(_items.begin() + _dragging);
					_items.insert(_items.begin() + over, std::move(moved));
					_dragStart += tileRect(over).topLeft() - tileRect(_dragging).topLeft();
					_dragging = over;
					if (changed) {
						changed();
					}
				}
				update();
			}
			return;
		}
		const auto index = indexAt(e->pos());
		const auto part = (index >= 0) ? partAt(index, e->pos()) : 0;
		setCursor((index >= 0 && !_locked) ? style::cur_pointer : style::cur_default);
		if (index != _over || part != _overPart) {
			_over = index;
			_overPart = part;
			update();
		}
	}

	void leaveEvent(QEvent *e) override {
		RpWidget::leaveEvent(e);
		_over = -1;
		_overPart = 0;
		update();
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton || _locked) {
			return;
		}
		const auto index = indexAt(e->pos());
		if (index < 0) {
			return;
		}
		_pressPart = partAt(index, e->pos());
		_dragging = index;
		_dragStart = _dragPoint = e->pos();
		_dragMoved = false;
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton || _dragging < 0) {
			return;
		}
		const auto index = std::exchange(_dragging, -1);
		const auto moved = std::exchange(_dragMoved, false);
		update();
		if (moved || index >= count()) {
			return;
		}
		const auto part = partAt(index, e->pos());
		if (part != _pressPart) {
			return;
		}
		if (part == 1) {
			_items.erase(_items.begin() + index);
			relayout();
			if (changed) {
				changed();
			}
		} else if (part == 2 && altRequested) {
			altRequested(index);
		}
	}

private:
	[[nodiscard]] QRect tileRect(int index) const {
		const auto tile = st::eliComposeTile;
		const auto skip = st::eliComposeTileSkip;
		const auto column = index % std::max(_perRow, 1);
		const auto row = index / std::max(_perRow, 1);
		return style::rtlrect(
			column * (tile + skip),
			row * (tile + skip),
			tile,
			tile,
			width());
	}
	[[nodiscard]] QRect closeRect(const QRect &tile) const {
		const auto size = st::eliComposeTileClose;
		return style::RightToLeft()
			? QRect(tile.x() + 6, tile.y() + 6, size, size)
			: QRect(tile.right() + 1 - 6 - size, tile.y() + 6, size, size);
	}
	[[nodiscard]] QRect altRect(const QRect &tile) const {
		const auto text = tr::lng_eli_alt_badge(tr::now);
		const auto width = st::eliComposeTileAltFont->width(text) + 12;
		const auto height = 20;
		return style::RightToLeft()
			? QRect(tile.right() + 1 - 6 - width, tile.bottom() + 1 - 6 - height, width, height)
			: QRect(tile.x() + 6, tile.bottom() + 1 - 6 - height, width, height);
	}
	[[nodiscard]] int indexAt(QPoint point) const {
		for (auto i = 0; i != count(); ++i) {
			if (tileRect(i).contains(point)) {
				return i;
			}
		}
		return -1;
	}
	// 1: the remove button, 2: the alt badge, 0: the picture itself.
	[[nodiscard]] int partAt(int index, QPoint point) const {
		const auto rect = tileRect(index);
		if (closeRect(rect).adjusted(-4, -4, 4, 4).contains(point)) {
			return 1;
		} else if (!_items[index].video && altRect(rect).adjusted(-4, -4, 4, 4).contains(point)) {
			return 2;
		}
		return 0;
	}

	void paintTile(Painter &p, int index, const QRect &rect, double opacity) {
		auto &item = _items[index];
		p.setOpacity(opacity);
		const auto radius = st::eliComposeTileRadius;
		if (!item.thumb.isNull() && item.prepared.isNull()) {
			const auto ratio = Ui::DevicePixelRatio(this);
			auto image = Ui::Images::Round(
				Ui::Images::PrepareCover(item.thumb, rect.size(), ratio),
				radius);
			item.prepared = QPixmap::fromImage(std::move(image));
			item.prepared.setDevicePixelRatio(ratio);
		}
		if (!item.prepared.isNull()) {
			p.drawPixmap(rect.topLeft(), item.prepared);
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush(item.video ? st::bgNight : st::eliComposeTileBg);
			p.drawRoundedRect(rect, radius, radius);
		}
		if (item.video) {
			st::eliComposeTileVideoIcon.paintInCenter(p, rect);
			const auto name = QFileInfo(item.path).fileName();
			p.setPen(st::staticWhite);
			p.setFont(st::eliComposeTileNameFont);
			p.drawTextCentered(
				QRect(rect.x() + 6, rect.bottom() - 22, rect.width() - 12, 18),
				st::eliComposeTileNameFont->elided(name, rect.width() - 12, Qt::ElideMiddle));
		}
		if (item.progress >= 0.) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliComposeTileDim);
			p.drawRoundedRect(rect, radius, radius);
			const auto size = 36;
			const auto ring = QRectF(
				rect.center().x() - size / 2. + 0.5,
				rect.center().y() - size / 2. + 0.5,
				size,
				size);
			auto pen = QPen(st::staticWhite->c);
			pen.setWidthF(3.);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			if (item.progress >= 1.) {
				p.drawEllipse(ring);
			} else {
				p.setOpacity(opacity * 0.3);
				p.drawEllipse(ring);
				p.setOpacity(opacity);
				const auto span = std::max(item.progress, 0.04);
				p.drawArc(ring, 90 * 16, -int(span * 360 * 16));
			}
		} else if (!_locked) {
			const auto close = closeRect(rect);
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliComposeTileCloseBg);
			p.drawEllipse(close);
			st::eliComposeTileCloseIcon.paintInCenter(p, close);
			if (!item.video) {
				const auto alt = altRect(rect);
				p.setBrush(item.alt.isEmpty()
					? st::eliComposeTileAltBg
					: st::eliComposeTileAltBgOn);
				p.drawRoundedRect(alt, 6, 6);
				p.setPen(st::staticWhite);
				p.setFont(st::eliComposeTileAltFont);
				p.drawTextCentered(alt, tr::lng_eli_alt_badge(tr::now));
			}
		}
		p.setOpacity(1.);
	}

	void relayout() {
		for (auto &item : _items) {
			item.prepared = QPixmap();
		}
		resizeToWidth(width());
		update();
	}

	void loadThumb(const QString &path) {
		const auto weak = QPointer<Tiles>(this);
		QThreadPool::globalInstance()->start([=] {
			const auto image = Data::DecodeImageFile(path, kThumbSide * 2);
			QMetaObject::invokeMethod(qApp, [=] {
				if (!weak) {
					return;
				}
				for (auto &item : weak->_items) {
					if (item.path == path && item.thumb.isNull()) {
						item.thumb = image;
						item.prepared = QPixmap();
					}
				}
				weak->update();
			}, Qt::QueuedConnection);
		});
	}

	std::vector<Item> _items;
	int _perRow = 1;
	int _over = -1;
	int _overPart = 0;
	int _pressPart = 0;
	int _dragging = -1;
	bool _dragMoved = false;
	QPoint _dragStart;
	QPoint _dragPoint;
	bool _locked = false;
	Ui::InfiniteRadialAnimation _radial;

};

// The poll builder: options, «افزودن گزینه», the duration.
class PollEditor final : public Ui::RpWidget {
public:
	PollEditor(QWidget *parent, int minOptions, int maxOptions)
	: RpWidget(parent)
	, _min(std::max(minOptions, 2))
	, _max(std::max(maxOptions, _min))
	, _add(new Ui::LinkButton(this, tr::lng_eli_add_option(tr::now)))
	, _remove(new Ui::LinkButton(this, tr::lng_eli_remove_poll(tr::now))) {
		for (auto i = 0; i != _min; ++i) {
			addOption(QString());
		}
		for (const auto &duration : PollDurations()) {
			const auto chip = new Ui::PillButton(this, duration.label, st::eliComposeChip);
			const auto hours = duration.hours;
			chip->setActive(hours == _hours, false);
			chip->setClickedCallback([=, this] {
				_hours = hours;
				refreshChips();
				if (changed) {
					changed();
				}
			});
			_chips.push_back({ hours, chip });
		}
		_add->setClickedCallback([this] {
			if (int(_options.size()) < _max) {
				addOption(QString());
				_options.back().field->setFocusFast();
				resizeToWidth(width());
				if (changed) {
					changed();
				}
			}
		});
		_remove->setClickedCallback([this] {
			if (removeRequested) {
				removeRequested();
			}
		});
	}

	Fn<void()> changed;
	Fn<void()> removeRequested;

	[[nodiscard]] QStringList options() const {
		auto result = QStringList();
		for (const auto &option : _options) {
			result.push_back(option.field->text());
		}
		return result;
	}
	[[nodiscard]] int hours() const {
		return _hours;
	}
	void focusFirst() {
		if (!_options.empty()) {
			_options.front().field->setFocusFast();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::eliComposePollPadding;
		const auto left = padding.left();
		const auto inner = newWidth - left - padding.right();
		auto y = padding.top();
		const auto removable = (int(_options.size()) > _min);
		for (auto &option : _options) {
			const auto buttonWidth = removable ? (option.remove->width() + 4) : 0;
			option.field->resizeToWidth(inner - buttonWidth);
			option.field->moveToLeft(left, y, newWidth);
			option.remove->setVisible(removable);
			option.remove->moveToRight(
				padding.right(),
				y + (option.field->height() - option.remove->height()) / 2,
				newWidth);
			y += option.field->height() + st::eliComposePollSkip;
		}
		_add->setVisible(int(_options.size()) < _max);
		_add->resizeToNaturalWidth(inner);
		_add->moveToLeft(left, y + 2, newWidth);
		_remove->resizeToNaturalWidth(inner);
		_remove->moveToRight(padding.right(), y + 2, newWidth);
		y += _add->height() + 12;
		_durationTop = y;
		auto x = left + st::smallSemiboldFont->width(tr::lng_eli_duration(tr::now)) + 10;
		auto rowHeight = 0;
		for (const auto &chip : _chips) {
			chip.button->resizeToNaturalWidth(inner);
			if (x + chip.button->width() > left + inner && x > left) {
				x = left;
				y += rowHeight + 6;
			}
			chip.button->moveToLeft(x, y, newWidth);
			x += chip.button->width() + 6;
			rowHeight = chip.button->height();
		}
		_durationHeight = rowHeight;
		return y + rowHeight + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.setPen(st::textSecondary);
		p.setFont(st::smallSemiboldFont);
		p.drawTextLeft(
			st::eliComposePollPadding.left(),
			_durationTop + (_durationHeight - st::smallSemiboldFont->height) / 2,
			width(),
			tr::lng_eli_duration(tr::now));
	}

private:
	struct Option {
		Ui::InputField *field = nullptr;
		Ui::IconButton *remove = nullptr;
	};
	struct Chip {
		int hours = 0;
		Ui::PillButton *button = nullptr;
	};

	void addOption(const QString &text) {
		auto option = Option();
		const auto index = int(_options.size());
		option.field = new Ui::InputField(
			this,
			st::eliComposePollField,
			tr::lng_eli_poll_option(tr::now, lt_n, Lang::Num(index + 1)),
			text);
		option.field->setMaxLength(kPollOptionLength);
		option.remove = new Ui::IconButton(this, st::eliComposePollRemove);
		const auto field = option.field;
		connect(field, &Ui::InputField::changed, this, [this] {
			if (changed) {
				changed();
			}
		});
		connect(field, &Ui::InputField::submitted, this, [=, this] {
			// Enter moves to the next option (adds one while there is room).
			for (auto i = 0; i != int(_options.size()); ++i) {
				if (_options[i].field != field) {
					continue;
				}
				if (i + 1 < int(_options.size())) {
					_options[i + 1].field->setFocusFast();
				} else if (int(_options.size()) < _max) {
					_add->click();
				}
				break;
			}
		});
		option.remove->setClickedCallback([=, this] {
			for (auto i = _options.begin(); i != _options.end(); ++i) {
				if (i->field == field) {
					delete i->field;
					delete i->remove;
					_options.erase(i);
					break;
				}
			}
			for (auto i = 0; i != int(_options.size()); ++i) {
				_options[i].field->setPlaceholder(
					tr::lng_eli_poll_option(tr::now, lt_n, Lang::Num(i + 1)));
			}
			resizeToWidth(width());
			if (changed) {
				changed();
			}
		});
		option.field->show();
		option.remove->show();
		_options.push_back(option);
	}

	void refreshChips() {
		for (const auto &chip : _chips) {
			chip.button->setActive(chip.hours == _hours);
		}
	}

	int _min = 2;
	int _max = 4;
	int _hours = 24;
	std::vector<Option> _options;
	std::vector<Chip> _chips;
	Ui::LinkButton *_add = nullptr;
	Ui::LinkButton *_remove = nullptr;
	int _durationTop = 0;
	int _durationHeight = 0;

};

// One more part of a thread: its number, the text, a remove button.
class PartRow final : public Ui::RpWidget {
public:
	PartRow(QWidget *parent, int number, const QString &text, int maxLength)
	: RpWidget(parent)
	, _number(number)
	, _field(new Ui::InputField(
		this,
		st::eliComposePartField,
		Ui::InputField::Mode::MultiLine,
		tr::lng_eli_part_placeholder(tr::now, lt_n, Lang::Num(number)),
		text))
	, _remove(new Ui::IconButton(this, st::eliComposePollRemove)) {
		Q_UNUSED(maxLength);
		_field->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
		new Highlighter(_field->rawTextEdit()->document());
		connect(_field, &Ui::RpWidget::heightChanged, this, [this] {
			resizeToWidth(width());
		});
	}

	[[nodiscard]] Ui::InputField *field() const {
		return _field;
	}
	[[nodiscard]] Ui::IconButton *removeButton() const {
		return _remove;
	}
	void setNumber(int number) {
		_number = number;
		_field->setPlaceholder(
			tr::lng_eli_part_placeholder(tr::now, lt_n, Lang::Num(number)));
		update();
	}
	void setOver(bool over) {
		if (_overLimit != over) {
			_overLimit = over;
			update();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto left = st::eliCardUserpic + st::eliCardUserpicSkip;
		_field->resizeToWidth(newWidth - left - _remove->width() - 4);
		_field->moveToLeft(left, 8, newWidth);
		_remove->moveToRight(0, 8 + 6, newWidth);
		return 8 + _field->height();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(
			style::rtlrect(
				st::eliCardUserpic + st::eliCardUserpicSkip,
				0,
				width() - st::eliCardUserpic - st::eliCardUserpicSkip,
				1,
				width()),
			st::eliComposePartLineFg);
		const auto circle = style::rtlrect(
			(st::eliCardUserpic - st::eliThreadNumberSize) / 2,
			8 + 10,
			st::eliThreadNumberSize,
			st::eliThreadNumberSize,
			width());
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(_overLimit ? st::statusErrorSoft : st::eliThreadNumberBg);
		p.drawEllipse(circle);
		p.setPen(_overLimit ? st::statusError : st::eliThreadNumberFg);
		p.setFont(st::eliThreadNumberFont);
		p.drawTextCentered(circle, Lang::Num(_number));
	}

private:
	int _number = 0;
	Ui::InputField *_field = nullptr;
	Ui::IconButton *_remove = nullptr;
	bool _overLimit = false;

};

// People matching the `@abc` being typed.
class Mentions final : public Ui::RpWidget {
public:
	Mentions(QWidget *parent, Main::Session *session)
	: RpWidget(parent)
	, _userpics(session, [this](const QString &) { update(); }, this) {
		setMouseTracking(true);
		setCursor(style::cur_pointer);
	}

	Fn<void(const Data::Profile &)> chosen;

	void setRows(const QVector<Data::Profile> &rows) {
		_rows = rows;
		_selected = _rows.isEmpty() ? -1 : 0;
		resizeToWidth(width());
		update();
	}
	[[nodiscard]] bool empty() const {
		return _rows.isEmpty();
	}
	void selectSkip(int delta) {
		if (_rows.isEmpty()) {
			return;
		}
		const auto count = int(_rows.size());
		_selected = (_selected + delta + count) % count;
		update();
	}
	bool chooseSelected() {
		if (_selected < 0 || _selected >= _rows.size()) {
			return false;
		}
		if (chosen) {
			chosen(_rows[_selected]);
		}
		return true;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return _rows.isEmpty()
			? 0
			: (int(_rows.size()) * st::eliComposeMentionHeight + 8);
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_rows.isEmpty()) {
			return;
		}
		auto p = Painter(this);
		const auto left = st::eliCardUserpic + st::eliCardUserpicSkip;
		const auto body = style::rtlrect(left, 0, width() - left, height() - 8, width());
		{
			PainterHighQualityEnabler hq(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliComposeMentionBg);
			p.drawRoundedRect(body, st::eliComposeMentionRadius, st::eliComposeMentionRadius);
		}
		for (auto i = 0; i != int(_rows.size()); ++i) {
			const auto top = i * st::eliComposeMentionHeight;
			const auto &row = _rows[i];
			if (i == _selected) {
				PainterHighQualityEnabler hq(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::eliComposeMentionBgOver);
				p.drawRoundedRect(
					QRect(body.x(), top, body.width(), st::eliComposeMentionHeight),
					st::eliComposeMentionRadius,
					st::eliComposeMentionRadius);
			}
			const auto userpic = st::eliComposeMentionUserpic;
			_userpics.paint(
				p,
				style::rtlrect(
					left + 10,
					top + (st::eliComposeMentionHeight - userpic) / 2,
					userpic,
					userpic,
					width()),
				row.card());
			const auto textLeft = left + 10 + userpic + 10;
			const auto available = width() - textLeft - 12;
			const auto textTop = top
				+ (st::eliComposeMentionHeight - st::semiboldFont->height - st::smallFont->height) / 2;
			p.setPen(st::textPrimary);
			p.setFont(st::semiboldFont);
			p.drawTextLeft(
				textLeft,
				textTop,
				width(),
				st::semiboldFont->elided(row.displayName, available));
			p.setPen(st::textTertiary);
			p.setFont(st::smallFont);
			p.drawTextLeft(
				textLeft,
				textTop + st::semiboldFont->height,
				width(),
				st::smallFont->elided(IsolatedHandle(row.handle), available));
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto index = e->pos().y() / st::eliComposeMentionHeight;
		if (index >= 0 && index < _rows.size() && index != _selected) {
			_selected = index;
			update();
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = e->pos().y() / st::eliComposeMentionHeight;
		if (e->button() == Qt::LeftButton && index >= 0 && index < _rows.size()) {
			_selected = index;
			chooseSelected();
		}
	}

private:
	Support::UserpicPainter _userpics;
	QVector<Data::Profile> _rows;
	int _selected = -1;

};

// The first row: my userpic next to the text.
class EditorRow final : public Ui::RpWidget {
public:
	EditorRow(
		QWidget *parent,
		Main::Session *session,
		const QString &placeholder)
	: RpWidget(parent)
	, _userpic(new Support::UserpicButton(
		this,
		session,
		session->userId(),
		st::eliCardUserpic))
	, _field(new Ui::InputField(
		this,
		st::eliComposeField,
		Ui::InputField::Mode::MultiLine,
		placeholder)) {
		Ui::MakeTransparentForMouse(_userpic);
		connect(_field, &Ui::RpWidget::heightChanged, this, [this] {
			resizeToWidth(width());
		});
	}

	[[nodiscard]] Ui::InputField *field() const {
		return _field;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto left = st::eliCardUserpic + st::eliCardUserpicSkip;
		_userpic->moveToLeft(0, st::eliComposeUserpicTop, newWidth);
		_field->resizeToWidth(newWidth - left);
		_field->moveToLeft(left, 0, newWidth);
		return std::max(
			_field->height(),
			st::eliComposeUserpicTop + st::eliCardUserpic);
	}

private:
	Support::UserpicButton *_userpic = nullptr;
	Ui::InputField *_field = nullptr;

};

class StatusLine final : public Ui::RpWidget {
public:
	explicit StatusLine(QWidget *parent)
	: RpWidget(parent)
	, _cancel(new Ui::LinkButton(this, tr::lng_cancel(tr::now), st::smallLinkButton)) {
		_cancel->hide();
	}

	void setText(const QString &text, bool cancellable) {
		_text = text;
		_cancel->setVisible(cancellable && !text.isEmpty());
		resizeToWidth(width());
		update();
	}
	void setCancel(Fn<void()> callback) {
		_cancel->setClickedCallback(std::move(callback));
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_cancel->resizeToNaturalWidth(newWidth);
		_cancel->moveToRight(
			0,
			(st::eliComposeStatusHeight - _cancel->height()) / 2,
			newWidth);
		return _text.isEmpty() ? 0 : st::eliComposeStatusHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_text.isEmpty()) {
			return;
		}
		auto p = Painter(this);
		p.setPen(st::eliComposeStatusFg);
		p.setFont(st::eliComposeStatusFont);
		p.drawTextLeft(
			st::eliCardUserpic + st::eliCardUserpicSkip,
			(height() - st::eliComposeStatusFont->height) / 2,
			width(),
			_text);
	}

private:
	QString _text;
	Ui::LinkButton *_cancel = nullptr;

};

class ComposeBox final : public Ui::BoxContent {
public:
	ComposeBox(
		QWidget *parent,
		Window::SessionController *controller,
		ComposeArgs args);
	~ComposeBox();

	void setInnerFocus() override;

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	[[nodiscard]] Main::Session &session() const {
		return _controller->session();
	}
	[[nodiscard]] QString draftKey() const;
	void readDraft();
	void writeDraft();
	void clearDraft();

	void attach(const QStringList &paths);
	void attachDropped(const Support::DroppedData &data);
	void pickFiles();
	void togglePoll();
	void addPart(const QString &text, bool focus);
	void removePart(PartRow *row);
	void splitIntoThread();
	void showPolicyMenu();
	void editAlt(int index);
	void checkMention();
	void insertMention(const Data::Profile &profile);

	[[nodiscard]] Logic::ComposeDraft draft() const;
	void refresh();
	void updateHeight();
	void setStatus(const QString &text, bool cancellable = false);

	void tryClose();
	void publish();
	void uploadNext();
	void createPost();
	void failed(const Api::Error &error);
	void cancelUpload();

	Window::SessionController *_controller = nullptr;
	ComposeArgs _args;
	Logic::ComposeLimits _limits;
	QString _policy = u"everyone"_s;
	// One key per post: a retry of the same body reuses it (the server
	// answers with the post it made), a body changed since gets a new one.
	QString _idempotencyKey;
	QByteArray _idempotencyBody;

	Ui::VerticalLayout *_content = nullptr;
	EditorRow *_editor = nullptr;
	Ui::InputField *_field = nullptr;
	Mentions *_mentions = nullptr;
	Tiles *_tiles = nullptr;
	PollEditor *_poll = nullptr;
	Ui::VerticalLayout *_partsLayout = nullptr;
	std::vector<PartRow*> _parts;
	StatusLine *_status = nullptr;

	Ui::RpWidget *_toolbar = nullptr;
	Ui::IconButton *_imageTool = nullptr;
	Ui::IconButton *_pollTool = nullptr;
	Ui::IconButton *_threadTool = nullptr;
	Ui::RoundButton *_policyButton = nullptr;
	Ui::RoundButton *_splitButton = nullptr;
	CounterRing *_ring = nullptr;
	Ui::RoundButton *_publish = nullptr;

	base::Timer _mentionTimer;
	QString _mentionQuery;
	int _mentionFrom = -1;
	int _mentionRequest = 0;

	bool _dirty = false;
	bool _busy = false;
	bool _keepDraft = true;
	int _uploadIndex = -1;
	Data::Upload::Id _uploadId = 0;

};

ComposeBox::ComposeBox(
	QWidget *parent,
	Window::SessionController *controller,
	ComposeArgs args)
: BoxContent(parent)
, _controller(controller)
, _args(std::move(args))
, _idempotencyKey(QUuid::createUuid().toString(QUuid::WithoutBraces))
, _mentionTimer([this] {
	const auto query = _mentionQuery;
	const auto request = ++_mentionRequest;
	session().data().feed().searchUsers(
		query,
		base::Guard(this),
		[=, this](const QVector<Data::Profile> &users) {
			if (request != _mentionRequest || _mentionFrom < 0) {
				return;
			}
			auto rows = QVector<Data::Profile>();
			for (const auto &user : users) {
				if (!user.handle.isEmpty()) {
					rows.push_back(user);
				}
			}
			_mentions->setRows(rows);
			updateHeight();
		});
}) {
	const auto &config = session().account().clientConfig();
	if (const auto value = config.postMaxTextChars(); value > 0) {
		_limits.maxText = value;
	}
	if (const auto value = config.postMaxThreadParts(); value > 0) {
		_limits.maxThreadParts = value;
	}
	if (const auto value = config.postMaxAlbumItems(); value > 0) {
		_limits.maxMedia = value;
	}
	if (const auto value = config.pollMinOptions(); value > 0) {
		_limits.pollMinOptions = value;
	}
	if (const auto value = config.pollMaxOptions(); value > 0) {
		_limits.pollMaxOptions = value;
	}
	setAcceptDrops(true);
}

ComposeBox::~ComposeBox() {
	if (_uploadId) {
		session().data().upload().cancel(_uploadId);
	}
	if (_args.kind == Kind::New && _keepDraft) {
		writeDraft();
	}
}

QString ComposeBox::draftKey() const {
	return u"eli.draft."_s + session().userId();
}

void ComposeBox::readDraft() {
	const auto stored = session().account().settings().object(draftKey());
	const auto text = stored.value("text"_L1).toString();
	if (!text.isEmpty()) {
		_field->setText(text);
		_field->setCursorToEnd();
	}
	for (const auto &part : stored.value("parts"_L1).toArray()) {
		if (int(_parts.size()) + 1 < _limits.maxThreadParts) {
			addPart(part.toString(), false);
		}
	}
}

void ComposeBox::writeDraft() {
	auto parts = QJsonArray();
	for (const auto row : _parts) {
		const auto text = row->field()->text();
		if (!text.trimmed().isEmpty()) {
			parts.push_back(text);
		}
	}
	const auto text = _field ? _field->text() : QString();
	if (text.trimmed().isEmpty() && parts.isEmpty()) {
		clearDraft();
		return;
	}
	session().account().settings().set(draftKey(), QJsonObject{
		{ u"text"_s, text },
		{ u"parts"_s, parts },
	});
}

void ComposeBox::clearDraft() {
	session().account().settings().remove(draftKey());
}

void ComposeBox::prepare() {
	auto &feed = session().data().feed();
	const auto target = _args.postId.isEmpty() ? nullptr : feed.post(_args.postId);
	const auto kind = _args.kind;
	auto title = QString();
	auto placeholder = QString();
	switch (kind) {
	case Kind::New:
		title = tr::lng_eli_new_post(tr::now);
		placeholder = tr::lng_eli_whats_new(tr::now);
		break;
	case Kind::Reply:
		title = tr::lng_reply(tr::now);
		placeholder = tr::lng_eli_reply_to_placeholder(
			tr::now,
			lt_name,
			target ? AuthorName(session(), target->author) : QString());
		break;
	case Kind::Quote:
		title = tr::lng_chat_quote(tr::now);
		placeholder = tr::lng_eli_compose_quote_placeholder(tr::now);
		break;
	case Kind::Invite:
		title = tr::lng_eli_invite_conversation(tr::now);
		placeholder = tr::lng_eli_compose_invite_placeholder(tr::now);
		break;
	case Kind::Edit:
		title = tr::lng_edit(tr::now);
		placeholder = tr::lng_eli_whats_new(tr::now);
		break;
	}
	setTitle(title);
	setCloseByOutsideClick(false);
	setCloseByEscape(false);

	_publish = addButton(
		(kind == Kind::Edit) ? tr::lng_eli_save(tr::now) : tr::lng_eli_publish(tr::now),
		[this] { publish(); },
		st::defaultRoundButton);
	addButton(tr::lng_cancel(tr::now), [this] { tryClose(); });

	const auto &padding = st::eliComposePadding;
	_content = setInnerWidget(
		Ui::object_ptr<Ui::VerticalLayout>(this),
		0,
		st::eliComposeToolbarHeight);
	const auto rowMargins = style::margins(padding.left(), 0, padding.right(), 0);

	if ((kind == Kind::Reply || kind == Kind::Quote) && target) {
		_content->add(
			Ui::object_ptr<PreviewCard>(_content, &session(), *target),
			style::margins(
				padding.left(),
				padding.top(),
				padding.right(),
				st::eliComposeContextSkip));
	} else {
		_content->add(
			Ui::object_ptr<Ui::FixedHeightWidget>(_content, padding.top()));
	}

	_editor = _content->add(
		Ui::object_ptr<EditorRow>(_content, &session(), placeholder),
		rowMargins);
	_field = _editor->field();
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
	new Highlighter(_field->rawTextEdit()->document());
	_field->rawTextEdit()->installEventFilter(this);
	connect(_field, &Ui::InputField::changed, this, [this] {
		_dirty = true;
		refresh();
		checkMention();
	});
	connect(_field, &Ui::InputField::submitted, this, [this] {
		publish();
	});
	connect(_field, &Ui::InputField::cancelled, this, [this] {
		if (_mentionFrom >= 0 && !_mentions->empty()) {
			_mentionFrom = -1;
			_mentions->setRows({});
			updateHeight();
		} else {
			tryClose();
		}
	});

	_mentions = _content->add(
		Ui::object_ptr<Mentions>(_content, &session()),
		rowMargins);
	_mentions->chosen = [this](const Data::Profile &profile) {
		insertMention(profile);
	};

	_tiles = _content->add(
		Ui::object_ptr<Tiles>(_content),
		style::margins(
			padding.left() + st::eliCardUserpic + st::eliCardUserpicSkip,
			0,
			padding.right(),
			0));
	_tiles->changed = [this] {
		_dirty = true;
		refresh();
	};
	_tiles->altRequested = [this](int index) { editAlt(index); };

	_partsLayout = _content->add(
		Ui::object_ptr<Ui::VerticalLayout>(_content),
		rowMargins);

	_status = _content->add(Ui::object_ptr<StatusLine>(_content), rowMargins);
	_status->setCancel([this] { cancelUpload(); });
	if (kind == Kind::Edit) {
		_status->setText(tr::lng_eli_edit_note(tr::now), false);
	}
	_content->add(Ui::object_ptr<Ui::FixedHeightWidget>(_content, padding.bottom()));

	// The toolbar.
	_toolbar = new Ui::RpWidget(this);
	const auto extras = (kind == Kind::New || kind == Kind::Invite || kind == Kind::Quote);
	if (extras || kind == Kind::Reply) {
		_imageTool = new Ui::IconButton(_toolbar, st::eliComposeToolImage);
		_imageTool->setClickedCallback([this] { pickFiles(); });
		Ui::InstallTooltip(_imageTool, [] { return tr::lng_chat_photo_video(tr::now); });
	}
	if (extras) {
		_pollTool = new Ui::IconButton(_toolbar, st::eliComposeToolPoll);
		_pollTool->setClickedCallback([this] { togglePoll(); });
		Ui::InstallTooltip(_pollTool, [] { return tr::lng_eli_poll_tool(tr::now); });
	}
	if (kind == Kind::New) {
		_threadTool = new Ui::IconButton(_toolbar, st::eliComposeToolThread);
		_threadTool->setClickedCallback([this] {
			if (1 + int(_parts.size()) >= _limits.maxThreadParts) {
				_controller->showToast(tr::lng_eli_max_parts(
					tr::now,
					lt_n,
					Lang::Num(_limits.maxThreadParts)));
				return;
			}
			addPart(QString(), true);
			_dirty = true;
			refresh();
		});
		Ui::InstallTooltip(_threadTool, [] { return tr::lng_eli_add_part(tr::now); });
		_splitButton = new Ui::RoundButton(
			_toolbar,
			tr::lng_eli_split_thread(tr::now),
			st::eliComposeSplitButton);
		_splitButton->setClickedCallback([this] { splitIntoThread(); });
		_splitButton->hide();
	}
	if (kind != Kind::Edit && kind != Kind::Reply) {
		_policyButton = new Ui::RoundButton(
			_toolbar,
			QString(),
			st::eliComposePolicyButton);
		_policyButton->setClickedCallback([this] { showPolicyMenu(); });
	}
	_ring = new CounterRing(_toolbar);

	if (kind == Kind::Edit && target) {
		_field->setText(target->text);
		_field->setCursorToEnd();
		_policy = target->replyPolicy.isEmpty() ? u"everyone"_s : target->replyPolicy;
	} else if (!_args.prefill.isEmpty()) {
		_field->setText(_args.prefill);
		_field->setCursorToEnd();
	} else if (kind == Kind::New) {
		readDraft();
	}
	_dirty = false;
	if (!_args.files.isEmpty()) {
		attach(_args.files);
	}
	refresh();
	connect(_content, &Ui::RpWidget::heightChanged, this, [this] {
		updateHeight();
	});
	updateHeight();
}

void ComposeBox::setInnerFocus() {
	if (_field) {
		_field->setFocusFast();
	}
}

void ComposeBox::updateHeight() {
	const auto wanted = _content->height() + st::eliComposeToolbarHeight;
	setDimensions(
		st::eliComposeWidth,
		std::clamp(
			wanted,
			int(st::eliComposeMinHeight),
			int(st::eliComposeMaxHeight)));
}

void ComposeBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	if (!_toolbar) {
		return;
	}
	const auto height = st::eliComposeToolbarHeight;
	_toolbar->setGeometry(0, this->height() - height, width(), height);
	const auto &padding = st::eliComposeToolbarPadding;
	auto x = padding.left();
	for (const auto button : { _imageTool, _pollTool, _threadTool }) {
		if (button) {
			button->moveToLeft(x, (height - button->height()) / 2, width());
			x += button->width();
		}
	}
	auto right = padding.right();
	_ring->moveToRight(right, (height - _ring->height()) / 2, width());
	right += _ring->width() + 8;
	if (_splitButton && !_splitButton->isHidden()) {
		_splitButton->resizeToNaturalWidth(width());
		_splitButton->moveToRight(right, (height - _splitButton->height()) / 2, width());
		right += _splitButton->width() + 8;
	}
	if (_policyButton) {
		_policyButton->resizeToNaturalWidth(width());
		const auto available = width() - x - right - 8;
		const auto shown = (available >= _policyButton->width())
			&& (!_splitButton || _splitButton->isHidden());
		_policyButton->setVisible(shown);
		_policyButton->moveToRight(right, (height - _policyButton->height()) / 2, width());
	}
}

void ComposeBox::paintEvent(QPaintEvent *e) {
	BoxContent::paintEvent(e);
	if (_toolbar) {
		auto p = QPainter(this);
		p.fillRect(
			QRect(0, height() - st::eliComposeToolbarHeight, width(), 1),
			st::borderSubtle);
	}
}

void ComposeBox::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		tryClose();
	} else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
		&& (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
		publish();
	} else if (e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter) {
		BoxContent::keyPressEvent(e);
	}
}

bool ComposeBox::eventFilter(QObject *object, QEvent *event) {
	if (_field && object == _field->rawTextEdit() && event->type() == QEvent::KeyPress) {
		const auto e = static_cast<QKeyEvent*>(event);
		if (e->matches(QKeySequence::Paste)) {
			const auto data = Support::ReadMimeData(
				QGuiApplication::clipboard()->mimeData());
			if (data.hasMedia()) {
				attachDropped(data);
				return true;
			}
		} else if (_mentionFrom >= 0 && !_mentions->empty()) {
			if (e->key() == Qt::Key_Down) {
				_mentions->selectSkip(1);
				return true;
			} else if (e->key() == Qt::Key_Up) {
				_mentions->selectSkip(-1);
				return true;
			} else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Tab)
				&& !(e->modifiers() & Qt::ControlModifier)) {
				return _mentions->chooseSelected();
			}
		}
	}
	return BoxContent::eventFilter(object, event);
}

void ComposeBox::dragEnterEvent(QDragEnterEvent *e) {
	if (_imageTool && !_busy && Support::CanAcceptDrop(e->mimeData())) {
		e->acceptProposedAction();
	}
}

void ComposeBox::dropEvent(QDropEvent *e) {
	const auto data = Support::ReadMimeData(e->mimeData());
	if (data.hasMedia() && _imageTool && !_busy) {
		e->acceptProposedAction();
		attachDropped(data);
	}
}

// ---- media

void ComposeBox::attachDropped(const Support::DroppedData &data) {
	if (!data.files.isEmpty()) {
		attach(data.files);
	} else if (!data.image.isNull()) {
		Support::SaveImageToTemp(data.image, this, [this](const QString &path) {
			if (!path.isEmpty()) {
				attach({ path });
			}
		});
	}
}

void ComposeBox::pickFiles() {
	if (_busy) {
		return;
	} else if (_poll) {
		_controller->showToast(tr::lng_eli_poll_and_media(tr::now));
		return;
	} else if (_tiles->count() >= _limits.maxMedia) {
		_controller->showToast(
			tr::lng_eli_max_files(tr::now, lt_n, Lang::Num(_limits.maxMedia)));
		return;
	}
	auto args = Support::PickArgs();
	args.kind = Support::PickKind::Media;
	args.multiple = true;
	Support::PickFiles(
		this,
		std::move(args),
		[this](const QStringList &paths) { attach(paths); });
}

void ComposeBox::attach(const QStringList &paths) {
	if (_busy || !_imageTool) {
		return;
	} else if (_poll) {
		_controller->showToast(tr::lng_eli_poll_and_media(tr::now));
		return;
	}
	const auto &config = session().account().clientConfig();
	auto refused = QString();
	for (const auto &path : paths) {
		if (_tiles->count() >= _limits.maxMedia) {
			refused = tr::lng_eli_max_files(tr::now, lt_n, Lang::Num(_limits.maxMedia));
			break;
		}
		const auto probe = Data::Upload::Probe(path);
		const auto video = (probe.kind == Data::UploadKind::Video);
		const auto image = (probe.kind == Data::UploadKind::Image)
			|| (probe.kind == Data::UploadKind::Gif);
		if (!probe.exists || (!video && !image)) {
			refused = tr::lng_chat_file_cant_be_sent(tr::now);
			continue;
		}
		const auto limit = video ? config.maxVideoBytes() : config.maxImageBytes();
		if (limit > 0 && probe.size > limit) {
			refused = tr::lng_chat_file_too_large(tr::now);
			continue;
		}
		auto duplicate = false;
		for (const auto &item : _tiles->items()) {
			duplicate = duplicate || (item.path == path);
		}
		if (!duplicate) {
			_tiles->add(path, video);
			_dirty = true;
		}
	}
	if (!refused.isEmpty()) {
		_controller->showToast(refused);
	}
	refresh();
}

void ComposeBox::editAlt(int index) {
	if (index < 0 || index >= _tiles->count() || _busy) {
		return;
	}
	const auto path = _tiles->items()[index].path;
	const auto current = _tiles->items()[index].alt;
	const auto weak = QPointer<ComposeBox>(this);
	showBox(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_eli_alt_text(tr::now));
		const auto field = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_eli_alt_question(tr::now),
			current));
		field->setMaxLength(kAltLength);
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto save = [=] {
			if (weak) {
				for (auto &item : weak->_tiles->items()) {
					if (item.path == path) {
						item.alt = field->text().trimmed();
					}
				}
				weak->_tiles->update();
				weak->_dirty = true;
			}
			box->closeBox();
		};
		box->addButton(tr::lng_eli_save(tr::now), save, st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- poll and thread

void ComposeBox::togglePoll() {
	if (_busy) {
		return;
	}
	if (_poll) {
		_content->removeChild(_poll);
		_poll = nullptr;
	} else {
		if (_tiles->count() > 0) {
			_controller->showToast(tr::lng_eli_poll_and_media(tr::now));
			return;
		}
		// Right under the media row.
		auto position = 0;
		for (auto i = 0; i != _content->count(); ++i) {
			if (_content->widgetAt(i) == _tiles) {
				position = i + 1;
			}
		}
		const auto &padding = st::eliComposePadding;
		_poll = _content->insert(
			position,
			Ui::object_ptr<PollEditor>(
				_content,
				_limits.pollMinOptions,
				_limits.pollMaxOptions),
			style::margins(padding.left(), 0, padding.right(), 0));
		_poll->changed = [this] {
			_dirty = true;
			refresh();
		};
		_poll->removeRequested = [this] {
			Ui::PostponeCall(this, [this] { togglePoll(); });
		};
		_poll->focusFirst();
	}
	_dirty = true;
	refresh();
	updateHeight();
}

void ComposeBox::addPart(const QString &text, bool focus) {
	const auto row = _partsLayout->add(Ui::object_ptr<PartRow>(
		_partsLayout,
		int(_parts.size()) + 2,
		text,
		_limits.maxText));
	_parts.push_back(row);
	connect(row->field(), &Ui::InputField::changed, this, [this] {
		_dirty = true;
		refresh();
	});
	connect(row->field(), &Ui::InputField::submitted, this, [this] {
		publish();
	});
	connect(row->field(), &Ui::InputField::cancelled, this, [this] {
		tryClose();
	});
	row->removeButton()->setClickedCallback([=, this] {
		Ui::PostponeCall(this, [=, this] { removePart(row); });
	});
	if (focus) {
		row->field()->setFocusFast();
		Ui::PostponeCall(this, [=, this] {
			if (const auto scroll = scrollArea()) {
				scroll->scrollToBottom();
			}
		});
	}
}

void ComposeBox::removePart(PartRow *row) {
	const auto i = std::find(_parts.begin(), _parts.end(), row);
	if (i == _parts.end() || _busy) {
		return;
	}
	_parts.erase(i);
	_partsLayout->removeChild(row);
	for (auto index = 0; index != int(_parts.size()); ++index) {
		_parts[index]->setNumber(index + 2);
	}
	_dirty = true;
	refresh();
	updateHeight();
}

void ComposeBox::splitIntoThread() {
	if (_busy) {
		return;
	}
	const auto room = _limits.maxThreadParts - int(_parts.size());
	const auto pieces = Logic::SplitIntoThread(_field->text(), _limits.maxText, room);
	if (pieces.size() < 2) {
		return;
	}
	_field->setText(pieces.front());
	// The new parts go right after the first one, before the existing ones.
	auto existing = QStringList();
	for (const auto row : _parts) {
		existing.push_back(row->field()->text());
	}
	while (!_parts.empty()) {
		const auto row = _parts.back();
		_parts.pop_back();
		_partsLayout->removeChild(row);
	}
	for (const auto &piece : pieces.mid(1)) {
		addPart(piece, false);
	}
	for (const auto &text : existing) {
		addPart(text, false);
	}
	_dirty = true;
	refresh();
	updateHeight();
}

void ComposeBox::showPolicyMenu() {
	const auto menu = new Ui::PopupMenu(this);
	for (const auto &policy : Policies()) {
		auto item = Ui::MenuItem();
		item.text = policy.label;
		item.checked = (policy.key == _policy);
		const auto key = policy.key;
		item.callback = [=, this] {
			_policy = key;
			refresh();
		};
		menu->addItem(std::move(item));
	}
	menu->popupAnchored(_policyButton);
}

// ---- mentions

void ComposeBox::checkMention() {
	static const auto kPattern = QRegularExpression(
		u"(^|\\s)@([A-Za-z0-9._]{1,32})$"_s);
	const auto cursor = _field->rawTextEdit()->textCursor();
	const auto position = cursor.position();
	const auto before = _field->text().left(position);
	const auto match = kPattern.match(before);
	if (!match.hasMatch()) {
		_mentionTimer.cancel();
		++_mentionRequest;
		if (_mentionFrom >= 0) {
			_mentionFrom = -1;
			_mentions->setRows({});
			updateHeight();
		}
		return;
	}
	_mentionQuery = match.captured(2);
	_mentionFrom = int(position - _mentionQuery.size() - 1);
	_mentionTimer.callOnce(kMentionDelay);
}

void ComposeBox::insertMention(const Data::Profile &profile) {
	if (_mentionFrom < 0 || profile.handle.isEmpty()) {
		return;
	}
	auto cursor = _field->rawTextEdit()->textCursor();
	const auto end = cursor.position();
	cursor.setPosition(_mentionFrom);
	cursor.setPosition(end, QTextCursor::KeepAnchor);
	cursor.insertText(u"@"_s + profile.handle + u" "_s);
	_field->rawTextEdit()->setTextCursor(cursor);
	_mentionFrom = -1;
	++_mentionRequest;
	_mentions->setRows({});
	_field->setFocusFast();
	updateHeight();
}

// ---- state

Logic::ComposeDraft ComposeBox::draft() const {
	auto result = Logic::ComposeDraft();
	result.kind = LogicKind(_args.kind);
	result.text = _field->text();
	for (const auto row : _parts) {
		result.parts.push_back(row->field()->text());
	}
	result.mediaCount = _tiles->count();
	result.hasPoll = (_poll != nullptr);
	if (_poll) {
		result.pollOptions = _poll->options();
	}
	return result;
}

void ComposeBox::refresh() {
	const auto current = draft();
	const auto problem = Logic::ValidateCompose(current, _limits);
	const auto length = Logic::TextLength(current.text.trimmed());
	if (_publish) {
		_publish->setDisabled(_busy || problem != Logic::ComposeProblem::None);
	}
	_ring->setState(length, _limits.maxText);
	for (const auto row : _parts) {
		row->setOver(Logic::TextLength(row->field()->text().trimmed()) > _limits.maxText);
	}
	if (_imageTool) {
		_imageTool->setIconColorOverride((_poll || _busy)
			? std::make_optional(st::eliComposeToolDisabledFg->c)
			: std::nullopt);
	}
	if (_pollTool) {
		_pollTool->setIconColorOverride((_tiles->count() > 0 || _busy)
			? std::make_optional(st::eliComposeToolDisabledFg->c)
			: std::nullopt);
		_pollTool->setForceRippled(_poll != nullptr);
	}
	if (_policyButton) {
		for (const auto &policy : Policies()) {
			if (policy.key == _policy) {
				_policyButton->setText(policy.label);
			}
		}
		_policyButton->setIcon((_policy == u"everyone")
			? &st::eliComposePolicyIconGlobe
			: &st::eliComposePolicyIconLock);
	}
	if (_splitButton) {
		const auto offer = (length > _limits.maxText)
			&& !_poll
			&& (1 + int(_parts.size()) < _limits.maxThreadParts);
		if (offer == _splitButton->isHidden()) {
			_splitButton->setVisible(offer);
		}
	}
	resizeEvent(nullptr);
}

void ComposeBox::setStatus(const QString &text, bool cancellable) {
	_status->setText(text, cancellable);
	updateHeight();
}

void ComposeBox::tryClose() {
	if (_busy) {
		return;
	}
	const auto hasContent = !_field->text().trimmed().isEmpty()
		|| (_tiles->count() > 0)
		|| !Logic::CleanThreadParts(draft().parts).isEmpty();
	if (!_dirty || !hasContent || _args.kind == Kind::Edit) {
		if (_args.kind != Kind::New) {
			_keepDraft = false;
		}
		closeBox();
		return;
	}
	const auto weak = QPointer<ComposeBox>(this);
	const auto isNew = (_args.kind == Kind::New);
	auto args = Ui::ConfirmBoxArgs();
	args.text = tr::lng_eli_discard_draft(tr::now);
	args.confirmText = tr::lng_eli_discard(tr::now);
	args.cancelText = isNew
		? tr::lng_eli_keep_later(tr::now)
		: tr::lng_cancel(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		if (weak) {
			weak->_keepDraft = false;
			weak->clearDraft();
			weak->closeBox();
		}
	};
	if (isNew) {
		// «نگه‌دار برای بعد»: the text stays as the draft of the next «الی تازه».
		args.cancelled = [=] {
			if (weak) {
				weak->_keepDraft = true;
				weak->closeBox();
			}
		};
	}
	showBox(Ui::MakeConfirmBox(std::move(args)));
}

// ---- publishing

void ComposeBox::publish() {
	if (_busy) {
		return;
	}
	const auto current = draft();
	if (Logic::ValidateCompose(current, _limits) != Logic::ComposeProblem::None) {
		return;
	}
	_busy = true;
	_tiles->setLocked(true);
	_field->setReadOnly(true);
	for (const auto row : _parts) {
		row->field()->setReadOnly(true);
	}
	_publish->setLoading(true);
	refresh();
	if (_args.kind == Kind::Edit) {
		setStatus(tr::lng_eli_publishing(tr::now));
		const auto controller = _controller;
		session().data().feed().editPost(
			_args.postId,
			current.text.trimmed(),
			base::Guard(this),
			[=, this](const Data::Post &) {
				controller->showToast(tr::lng_eli_edited(tr::now));
				_keepDraft = false;
				_busy = false;
				closeBox();
			},
			[this](const Api::Error &error) { failed(error); });
		return;
	}
	_uploadIndex = -1;
	uploadNext();
}

void ComposeBox::uploadNext() {
	auto &items = _tiles->items();
	++_uploadIndex;
	while (_uploadIndex < int(items.size()) && !items[_uploadIndex].mediaId.isEmpty()) {
		++_uploadIndex;
	}
	if (_uploadIndex >= int(items.size())) {
		createPost();
		return;
	}
	const auto index = _uploadIndex;
	const auto total = int(items.size());
	const auto status = [=, this](int percent) {
		setStatus(tr::lng_eli_uploading(
			tr::now,
			lt_index,
			Lang::Num(index + 1),
			lt_total,
			Lang::Num(total),
			lt_percent,
			Lang::Num(percent)), true);
	};
	status(0);
	_tiles->setProgress(index, 0.);
	auto request = Data::UploadRequest();
	request.filePath = items[index].path;
	request.kind = items[index].video
		? Data::UploadKind::Video
		: Data::UploadKind::Image;
	request.purpose = Data::UploadPurpose::Post;
	auto callbacks = Data::UploadCallbacks();
	callbacks.progress = [=, this](qint64 sent, qint64 total) {
		const auto progress = (total > 0) ? (double(sent) / total) : 0.;
		_tiles->setProgress(index, std::min(progress, 0.99));
		status(int(progress * 100));
	};
	callbacks.done = [=, this](const Data::UploadResult &result) {
		_uploadId = 0;
		auto &items = _tiles->items();
		if (index < int(items.size())) {
			items[index].mediaId = result.mediaId;
			_tiles->setProgress(index, 1.);
		}
		uploadNext();
	};
	callbacks.fail = [this](const Api::Error &error) {
		_uploadId = 0;
		failed(error);
	};
	_uploadId = session().data().upload().start(
		request,
		base::Guard(this),
		std::move(callbacks));
}

void ComposeBox::cancelUpload() {
	if (!_busy || !_uploadId) {
		return;
	}
	session().data().upload().cancel(_uploadId);
	_uploadId = 0;
	failed(Api::Error::Cancelled());
}

void ComposeBox::createPost() {
	setStatus(tr::lng_eli_publishing(tr::now));
	const auto current = draft();
	auto body = Data::CreatePost();
	body.type = Logic::ComposeType(current);
	body.text = current.text.trimmed();
	if (_args.kind == Kind::Reply) {
		body.replyTo = _args.postId;
	} else if (_args.kind == Kind::Quote) {
		body.quoteOf = _args.postId;
	} else if (_args.kind == Kind::Invite) {
		body.conversation = _args.conversationId;
	}
	body.replyPolicy = _policy;
	for (const auto &item : _tiles->items()) {
		body.media.push_back({ item.mediaId, item.alt });
	}
	if (_poll) {
		body.pollQuestion = body.text;
		for (const auto &option : _poll->options()) {
			if (!option.trimmed().isEmpty()) {
				body.pollOptions.push_back(option.trimmed());
			}
		}
		body.pollDuration = qint64(_poll->hours()) * 3600;
	}
	body.threadParts = Logic::CleanThreadParts(current.parts);
	const auto controller = _controller;
	const auto reply = (_args.kind == Kind::Reply);
	const auto serialized = QJsonDocument(body.toJson()).toJson(
		QJsonDocument::Compact);
	if (!_idempotencyBody.isEmpty() && _idempotencyBody != serialized) {
		_idempotencyKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	_idempotencyBody = serialized;
	session().data().feed().createPost(
		body,
		_idempotencyKey,
		base::Guard(this),
		[=, this](const Data::Post &created) {
			_keepDraft = false;
			clearDraft();
			if (reply) {
				// Not every answer repeats what the post replies to.
				auto copy = created;
				if (copy.replyTo.isEmpty()) {
					copy.replyTo = _args.postId;
				}
				BumpReplies(controller, copy);
			}
			controller->showToast(reply
				? tr::lng_eli_reply_was_sent(tr::now)
				: tr::lng_eli_published(tr::now));
			_busy = false;
			closeBox();
		},
		[this](const Api::Error &error) { failed(error); });
}

void ComposeBox::failed(const Api::Error &error) {
	_busy = false;
	_uploadId = 0;
	_tiles->setLocked(false);
	for (auto i = 0; i != _tiles->count(); ++i) {
		_tiles->setProgress(i, -1.);
	}
	_field->setReadOnly(false);
	for (const auto row : _parts) {
		row->field()->setReadOnly(false);
	}
	_publish->setLoading(false);
	setStatus(
		(_args.kind == Kind::Edit) ? tr::lng_eli_edit_note(tr::now) : QString());
	refresh();
	if (error.code != u"cancelled") {
		Support::ShowError(
			_controller,
			error,
			tr::lng_eli_couldnt_post_try_again(tr::now));
	}
}

} // namespace

void OpenCompose(Window::SessionController *controller, ComposeArgs args) {
	auto &feed = controller->session().data().feed();
	if (!args.postId.isEmpty() && !feed.post(args.postId)) {
		// The post is needed for the preview (and for the text being edited).
		const auto show = [=] {
			controller->show(Ui::Box<ComposeBox>(controller, args));
		};
		feed.fetchPost(
			args.postId,
			base::Guard(controller),
			[=](const Data::Post &) { show(); },
			[=](const Api::Error &error) { Support::ShowError(controller, error); });
		return;
	}
	controller->show(Ui::Box<ComposeBox>(controller, std::move(args)));
}

} // namespace Eli
