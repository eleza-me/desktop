#include "history/history_tabbed_panel.h"

#include "api/api_client_config.h"
#include "core/core_application.h"
#include "core/core_settings.h"
#include "data/data_session.h"
#include "data/data_stickers.h"
#include "history/history_emoji_data.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/media_image.h"
#include "support/sticker_player.h"
#include "ui/effects/shadow.h"
#include "ui/emoji/emoji_pack.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>

#include <algorithm>

namespace History {
namespace {

// Pressed and held this long on an emoji that takes a skin tone: the tones.
constexpr auto kEmojiTonesDelay = 350;
constexpr auto kStickerColumns = 4;
// Pressed and held this long: the big preview instead of sending.
constexpr auto kStickerPreviewDelay = 350;

struct EmojiItem {
	QString base; // the panel's own (without a tone)
	QString code; // what is shown and sent
};

struct EmojiSection {
	QString title;
	std::vector<EmojiItem> items;
	int top = 0;
	int category = -1; // index in EmojiCategories(), -1 for recents / results
	bool tones = false; // a long press offers the skin tones
};

// ---- the emoji grid

// The skin tones of one emoji over the grid: the emoji without a tone and
// with each of the five, light to dark (in both directions, as everywhere).
class EmojiTones final : public Ui::RpWidget {
public:
	explicit EmojiTones(QWidget *parent)
	: RpWidget(parent) {
		setMouseTracking(true);
		setCursor(style::cur_pointer);
		hide();
	}

	void showFor(const QString &base, int current, QRect cell) {
		_base = base;
		_current = current;
		_over = -1;
		const auto side = int(st::historyEmojiCell);
		const auto padding = int(st::historyEmojiTonesPadding);
		const auto w = (Ui::Emoji::kToneCount + 1) * side + 2 * padding;
		const auto h = side + 2 * padding;
		const auto parent = parentWidget();
		const auto left = std::clamp(
			cell.center().x() - w / 2,
			padding,
			std::max(parent->width() - w - padding, padding));
		const auto above = cell.top() - h;
		const auto &extend = st::historyEmojiTonesShadow.extend;
		setGeometry(QRect(
			left,
			(above >= 0) ? above : (cell.bottom() + 1),
			w,
			h).marginsAdded(extend));
		show();
		raise();
		update();
	}
	[[nodiscard]] QString base() const {
		return _base;
	}
	void setChosenCallback(Fn<void(int tone)> callback) {
		_chosen = std::move(callback);
	}
	// While the press that opened it is still held on the grid.
	void dragTo(QPoint global) {
		setOver(toneAt(mapFromGlobal(global)));
	}
	// The press is released over the grid: picks the tone under it, if any.
	bool dropAt(QPoint global) {
		const auto tone = toneAt(mapFromGlobal(global));
		if (tone == -2) {
			return false;
		}
		choose(tone);
		return true;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto body = bodyRect();
		Ui::Shadow::Paint(
			p,
			body,
			st::historyEmojiTonesRadius,
			st::historyEmojiTonesShadow);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(QPen(st::historyPanelBorder->c, 1.));
			p.setBrush(st::historyPanelBg);
			p.drawRoundedRect(
				QRectF(body).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
				st::historyEmojiTonesRadius,
				st::historyEmojiTonesRadius);
		}
		for (auto tone = -1; tone != Ui::Emoji::kToneCount; ++tone) {
			const auto rect = toneRect(tone);
			if (tone == _current || tone == _over) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush((tone == _over)
					? st::historyEmojiOverBg
					: st::historyEmojiCategoryActiveBg);
				p.drawRoundedRect(
					rect.marginsRemoved(QMargins(1, 1, 1, 1)),
					st::historyEmojiOverRadius,
					st::historyEmojiOverRadius);
			}
			p.setPen(st::textPrimary);
			Ui::Emoji::PaintOne(
				p,
				Ui::Emoji::WithTone(_base, tone),
				rect,
				st::historyEmojiSize,
				st::historyEmojiFont->f);
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		setOver(toneAt(e->pos()));
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (!bodyRect().contains(e->pos())) {
			// The shadow: as if it was not there.
			hide();
		}
	}
	void leaveEvent(QEvent *e) override {
		Q_UNUSED(e);
		setOver(-2);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto tone = toneAt(e->pos());
		if (e->button() == Qt::LeftButton && tone != -2) {
			choose(tone);
		}
	}

private:
	[[nodiscard]] QRect bodyRect() const {
		return rect().marginsRemoved(st::historyEmojiTonesShadow.extend);
	}
	// -1: without a tone, 0…4 the tones; -2: none of them.
	[[nodiscard]] QRect toneRect(int tone) const {
		const auto side = int(st::historyEmojiCell);
		const auto padding = int(st::historyEmojiTonesPadding);
		const auto body = bodyRect();
		return QRect(
			body.x() + padding + (tone + 1) * side,
			body.y() + padding,
			side,
			side);
	}
	[[nodiscard]] int toneAt(QPoint position) const {
		for (auto tone = -1; tone != Ui::Emoji::kToneCount; ++tone) {
			if (toneRect(tone).contains(position)) {
				return tone;
			}
		}
		return -2;
	}
	void setOver(int tone) {
		if (_over != tone) {
			_over = tone;
			update();
		}
	}
	void choose(int tone) {
		hide();
		if (const auto callback = _chosen) {
			callback(tone);
		}
	}

	QString _base;
	int _current = -1;
	int _over = -2;
	Fn<void(int tone)> _chosen;

};

class EmojiGrid final : public Ui::RpWidget {
public:
	explicit EmojiGrid(QWidget *parent)
	: RpWidget(parent) {
		setMouseTracking(true);
		_holdTimer.setSingleShot(true);
		connect(&_holdTimer, &QTimer::timeout, this, [this] {
			if (_pressed.x() >= 0) {
				offerTones(_pressed);
			}
		});
	}

	void setSections(std::vector<EmojiSection> sections) {
		_sections = std::move(sections);
		_over = QPoint(-1, -1);
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}
	void setChosenCallback(Fn<void(const EmojiItem &)> callback) {
		_chosen = std::move(callback);
	}
	// A press held on an emoji that takes a tone: `cell` in the grid.
	void setTonesCallback(Fn<void(const EmojiItem &, QRect cell)> callback) {
		_tones = std::move(callback);
	}
	void setTonesPopup(EmojiTones *popup) {
		_tonesPopup = popup;
	}
	// The tone of an emoji was chosen: the grid shows it from now on.
	void updateCode(const QString &base, const QString &code) {
		for (auto &section : _sections) {
			if (!section.tones) {
				continue;
			}
			for (auto &item : section.items) {
				if (item.base == base) {
					item.code = code;
				}
			}
		}
		update();
	}
	[[nodiscard]] int sectionTop(int category) const {
		for (const auto &section : _sections) {
			if (section.category == category) {
				return section.top;
			}
		}
		return 0;
	}
	[[nodiscard]] int categoryAt(int y) const {
		auto result = -1;
		for (const auto &section : _sections) {
			if (section.top <= y) {
				result = section.category;
			}
		}
		return result;
	}
	[[nodiscard]] bool empty() const {
		for (const auto &section : _sections) {
			if (!section.items.empty()) {
				return false;
			}
		}
		return true;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::historyPanelPadding;
		_columns = std::max(
			(newWidth - padding.left() - padding.right()) / st::historyEmojiCell,
			1);
		auto top = 0;
		for (auto &section : _sections) {
			section.top = top;
			if (section.items.empty()) {
				continue;
			}
			top += st::historyPanelHeaderHeight;
			const auto rows = (int(section.items.size()) + _columns - 1) / _columns;
			top += rows * st::historyEmojiCell;
		}
		return top + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto clip = e->rect();
		const auto cell = int(st::historyEmojiCell);
		const auto &padding = st::historyPanelPadding;
		for (auto s = 0; s != int(_sections.size()); ++s) {
			const auto &section = _sections[s];
			if (section.items.empty()) {
				continue;
			}
			const auto rows = (int(section.items.size()) + _columns - 1) / _columns;
			const auto bottom = section.top + st::historyPanelHeaderHeight + rows * cell;
			if (bottom < clip.top() || section.top > clip.bottom()) {
				continue;
			}
			p.setPen(st::historyPanelHeaderFg);
			p.setFont(st::historyPanelHeaderFont);
			p.drawTextLeft(
				padding.left() + st::historyPanelHeaderPadding,
				section.top
					+ (st::historyPanelHeaderHeight - st::historyPanelHeaderFont->height) / 2
					+ 2,
				width(),
				section.title);
			p.setPen(st::textPrimary);
			for (auto i = 0; i != int(section.items.size()); ++i) {
				const auto rect = cellRect(s, i);
				if (!rect.intersects(clip)) {
					continue;
				}
				if (_over == QPoint(s, i)) {
					auto hq = PainterHighQualityEnabler(p);
					p.setPen(Qt::NoPen);
					p.setBrush(st::historyEmojiOverBg);
					p.drawRoundedRect(
						rect.marginsRemoved(QMargins(1, 1, 1, 1)),
						st::historyEmojiOverRadius,
						st::historyEmojiOverRadius);
					p.setPen(st::textPrimary);
				}
				Ui::Emoji::PaintOne(
					p,
					section.items[i].code,
					rect,
					st::historyEmojiSize,
					st::historyEmojiFont->f);
			}
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		if (_tonesShownByPress && _tonesPopup && _tonesPopup->isVisible()) {
			_tonesPopup->dragTo(e->globalPosition().toPoint());
			return;
		}
		const auto over = itemAt(e->pos());
		if (_over != over) {
			_over = over;
			setCursor((over.x() >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
		if (_pressed.x() >= 0 && _pressed != over) {
			_holdTimer.stop();
		}
	}
	void leaveEvent(QEvent *e) override {
		Q_UNUSED(e);
		if (_over.x() >= 0) {
			_over = QPoint(-1, -1);
			update();
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (_tonesPopup && _tonesPopup->isVisible()) {
			_tonesPopup->hide();
		}
		_tonesShownByPress = false;
		_pressed = (e->button() == Qt::LeftButton) ? itemAt(e->pos()) : QPoint(-1, -1);
		if (_pressed.x() >= 0 && takesTones(_pressed)) {
			_holdTimer.start(kEmojiTonesDelay);
		} else if (e->button() == Qt::RightButton) {
			// The context click offers the tones at once.
			const auto item = itemAt(e->pos());
			if (item.x() >= 0 && takesTones(item)) {
				offerTones(item);
			}
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		_holdTimer.stop();
		const auto pressed = std::exchange(_pressed, QPoint(-1, -1));
		if (std::exchange(_tonesShownByPress, false)) {
			// Released over a tone: that one; anywhere else: the tones stay.
			if (_tonesPopup) {
				_tonesPopup->dropAt(e->globalPosition().toPoint());
			}
			return;
		}
		const auto item = itemAt(e->pos());
		if (e->button() == Qt::LeftButton && item.x() >= 0 && item == pressed) {
			if (const auto callback = _chosen) {
				callback(_sections[item.x()].items[item.y()]);
			}
		}
	}

private:
	[[nodiscard]] bool takesTones(QPoint item) const {
		const auto &section = _sections[item.x()];
		return section.tones && Ui::Emoji::TakesTone(section.items[item.y()].base);
	}
	void offerTones(QPoint item) {
		if (const auto callback = _tones) {
			_tonesShownByPress = (_pressed == item);
			callback(_sections[item.x()].items[item.y()], cellRect(item.x(), item.y()));
		}
	}
	[[nodiscard]] QRect cellRect(int section, int index) const {
		const auto cell = int(st::historyEmojiCell);
		const auto column = index % _columns;
		const auto row = index / _columns;
		return style::rtlrect(
			st::historyPanelPadding.left() + column * cell,
			_sections[section].top + st::historyPanelHeaderHeight + row * cell,
			cell,
			cell,
			width());
	}
	[[nodiscard]] QPoint itemAt(QPoint position) const {
		const auto cell = int(st::historyEmojiCell);
		for (auto s = 0; s != int(_sections.size()); ++s) {
			const auto &section = _sections[s];
			const auto top = section.top + st::historyPanelHeaderHeight;
			const auto rows = (int(section.items.size()) + _columns - 1) / _columns;
			if (position.y() < top || position.y() >= top + rows * cell) {
				continue;
			}
			for (auto i = 0; i != int(section.items.size()); ++i) {
				if (cellRect(s, i).contains(position)) {
					return QPoint(s, i);
				}
			}
		}
		return QPoint(-1, -1);
	}

	std::vector<EmojiSection> _sections;
	int _columns = 8;
	QPoint _over = QPoint(-1, -1); // (section, index)
	QPoint _pressed = QPoint(-1, -1);
	QTimer _holdTimer;
	bool _tonesShownByPress = false;
	QPointer<EmojiTones> _tonesPopup;
	Fn<void(const EmojiItem &)> _chosen;
	Fn<void(const EmojiItem &, QRect)> _tones;

};

// ---- the stickers grid

struct StickerSection {
	QString id; // pack id, "recent"
	QString title;
	QVector<Data::Sticker> stickers;
	bool removable = false;
	int top = 0;
};

struct MorePack {
	Data::StickerPack pack;
	int top = 0;
};

// The big copy of a sticker while it is pressed and held (it plays).
class StickerPreview final : public Ui::RpWidget {
public:
	StickerPreview(QWidget *parent, Main::Session *session)
	: RpWidget(parent)
	, _session(session) {
		Ui::MakeTransparentForMouse(this);
		setAttribute(Qt::WA_NoSystemBackground);
	}

	void showSticker(const Data::Sticker &sticker) {
		if (_sticker.id == sticker.id && _sticker.url == sticker.url) {
			return;
		}
		_sticker = sticker;
		QObject::disconnect(_watching);
		_player = nullptr;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(rect(), st::overlayScrim);
		const auto side = std::min(
			int(st::historyStickerPreviewSize),
			std::min(width(), height()) - 2 * st::historyStickerPreviewSkip);
		if (side <= 0) {
			return;
		}
		const auto box = QRect(
			(width() - side) / 2,
			(height() - side) / 2,
			side,
			side);
		const auto ratio = devicePixelRatioF();
		const auto size = box.size() * ratio;
		if (!_sticker.url.isEmpty() && (!_player || _player->size() != size)) {
			QObject::disconnect(_watching);
			_player = Support::StickerPlayer::Get(_session, _sticker.url, size);
			if (_player) {
				_watching = QObject::connect(
					_player.get(),
					&Support::StickerPlayer::updated,
					this,
					[this] { update(); });
			}
		}
		const auto image = _player ? _player->frame() : QImage();
		if (image.isNull()) {
			if (!_sticker.emoji.isEmpty()) {
				auto font = st::historyJumboStyle1.font->f;
				font.setPixelSize(side / 2);
				p.setFont(font);
				p.setPen(st::staticWhite);
				p.drawText(box, Qt::AlignCenter, _sticker.emoji);
			}
			return;
		}
		const auto shown = QSizeF(image.size()) / ratio;
		p.drawImage(
			QRectF(
				box.x() + (box.width() - shown.width()) / 2.,
				box.y() + (box.height() - shown.height()) / 2.,
				shown.width(),
				shown.height()),
			image);
		if (!_sticker.emoji.isEmpty()) {
			p.setFont(st::historyEmojiFont);
			p.setPen(st::staticWhite);
			p.drawText(
				QRect(0, box.bottom() + 8, width(), st::historyEmojiFont->height + 8),
				Qt::AlignHCenter | Qt::AlignTop,
				_sticker.emoji);
		}
	}

private:
	Main::Session *_session = nullptr;
	Data::Sticker _sticker;
	std::shared_ptr<Support::StickerPlayer> _player;
	QMetaObject::Connection _watching;

};

class StickerGrid final : public Ui::RpWidget {
public:
	StickerGrid(QWidget *parent, Main::Session *session)
	: RpWidget(parent)
	, _session(session) {
		setMouseTracking(true);
		_previewTimer.setSingleShot(true);
		connect(&_previewTimer, &QTimer::timeout, this, [this] {
			if (_pressed.kind == Hit::Kind::Sticker) {
				showPreview(_pressed);
			}
		});
	}
	~StickerGrid() {
		hidePreview();
	}

	void setContent(std::vector<StickerSection> sections, std::vector<MorePack> more) {
		_sections = std::move(sections);
		_more = std::move(more);
		_over = Hit();
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}
	void setChosenCallback(Fn<void(const Data::Sticker &)> callback) {
		_chosen = std::move(callback);
	}
	void setInstallCallback(Fn<void(const QString &packId, bool install)> callback) {
		_install = std::move(callback);
	}
	[[nodiscard]] int sectionTop(const QString &id) const {
		for (const auto &section : _sections) {
			if (section.id == id) {
				return section.top;
			}
		}
		return _moreTop;
	}
	[[nodiscard]] QString sectionAt(int y) const {
		auto result = QString();
		for (const auto &section : _sections) {
			if (section.top <= y) {
				result = section.id;
			}
		}
		if (!_more.empty() && _moreTop <= y) {
			result = u"more"_s;
		}
		return result;
	}
	[[nodiscard]] bool empty() const {
		return _sections.empty() && _more.empty();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::historyPanelPadding;
		_cell = std::max(
			(newWidth - padding.left() - padding.right()) / kStickerColumns,
			40);
		auto top = 0;
		for (auto &section : _sections) {
			section.top = top;
			top += st::historyPanelHeaderHeight;
			const auto rows = (int(section.stickers.size()) + kStickerColumns - 1)
				/ kStickerColumns;
			top += rows * _cell;
		}
		_moreTop = top;
		if (!_more.empty()) {
			top += st::historyPanelHeaderHeight;
			for (auto &pack : _more) {
				pack.top = top;
				top += st::historySendFilesRowHeight;
			}
		}
		return top + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto clip = e->rect();
		const auto &padding = st::historyPanelPadding;
		const auto headerLeft = padding.left() + st::historyPanelHeaderPadding;
		const auto headerShift = (st::historyPanelHeaderHeight
			- st::historyPanelHeaderFont->height) / 2 + 2;
		for (auto s = 0; s != int(_sections.size()); ++s) {
			const auto &section = _sections[s];
			const auto rows = (int(section.stickers.size()) + kStickerColumns - 1)
				/ kStickerColumns;
			const auto bottom = section.top + st::historyPanelHeaderHeight + rows * _cell;
			if (bottom < clip.top() || section.top > clip.bottom()) {
				continue;
			}
			p.setPen(st::historyPanelHeaderFg);
			p.setFont(st::historyPanelHeaderFont);
			p.drawTextLeft(headerLeft, section.top + headerShift, width(), section.title);
			if (section.removable) {
				const auto over = (_over.kind == Hit::Kind::Remove) && (_over.section == s);
				const auto text = tr::lng_history_stickers_remove(tr::now);
				p.setPen(over ? st::statusError : st::historyPanelHeaderFg);
				p.drawTextRight(
					padding.right() + st::historyPanelHeaderPadding,
					section.top + headerShift,
					width(),
					text);
			}
			for (auto i = 0; i != int(section.stickers.size()); ++i) {
				const auto rect = cellRect(s, i);
				if (!rect.intersects(clip)) {
					continue;
				}
				if (_over.kind == Hit::Kind::Sticker
					&& _over.section == s
					&& _over.index == i) {
					auto hq = PainterHighQualityEnabler(p);
					p.setPen(Qt::NoPen);
					p.setBrush(st::historyEmojiOverBg);
					p.drawRoundedRect(rect, st::radiusSm, st::radiusSm);
				}
				paintSticker(
					p,
					rect.marginsRemoved(QMargins(
						st::historyStickerPadding,
						st::historyStickerPadding,
						st::historyStickerPadding,
						st::historyStickerPadding)),
					section.stickers[i]);
			}
		}
		if (_more.empty()) {
			return;
		}
		p.setPen(st::historyPanelHeaderFg);
		p.setFont(st::historyPanelHeaderFont);
		p.drawTextLeft(
			headerLeft,
			_moreTop + headerShift,
			width(),
			tr::lng_history_stickers_more(tr::now));
		for (auto i = 0; i != int(_more.size()); ++i) {
			const auto &pack = _more[i];
			const auto rowHeight = int(st::historySendFilesRowHeight);
			if (pack.top + rowHeight < clip.top() || pack.top > clip.bottom()) {
				continue;
			}
			const auto icon = rowHeight - 12;
			const auto left = padding.left() + st::historyPanelHeaderPadding;
			if (!pack.pack.stickers.isEmpty()) {
				// A pack's cover: its first frame, never moving.
				paintSticker(
					p,
					style::rtlrect(left, pack.top + 6, icon, icon, width()),
					pack.pack.stickers.front(),
					false);
			}
			p.setPen(st::textPrimary);
			p.setFont(st::historySendFilesNameFont);
			p.drawTextLeft(
				left + icon + 10,
				pack.top + 9,
				width(),
				pack.pack.title);
			p.setPen(st::textTertiary);
			p.setFont(st::historySendFilesSizeFont);
			p.drawTextLeft(
				left + icon + 10,
				pack.top + 9 + st::historySendFilesNameFont->height + 2,
				width(),
				tr::lng_history_stickers_count(
					tr::now,
					lt_count,
					int(pack.pack.stickers.size())));
			const auto button = addRect(i);
			const auto over = (_over.kind == Hit::Kind::Add) && (_over.index == i);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(over ? st::brandPrimary : st::brandSoft);
			p.drawRoundedRect(button, button.height() / 2., button.height() / 2.);
			p.setPen(over ? st::textOnBrand : st::brandText);
			p.setFont(st::smallSemiboldFont);
			p.drawText(button, Qt::AlignCenter, tr::lng_history_stickers_add(tr::now));
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = hitAt(e->pos());
		if (_preview && over.kind == Hit::Kind::Sticker) {
			// Held down and moved: the preview follows the pointer.
			showPreview(over);
		}
		if (over != _over) {
			_over = over;
			setCursor((over.kind != Hit::Kind::None)
				? style::cur_pointer
				: style::cur_default);
			update();
		}
	}
	void leaveEvent(QEvent *e) override {
		Q_UNUSED(e);
		if (_over.kind != Hit::Kind::None) {
			_over = Hit();
			update();
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		_pressed = (e->button() == Qt::LeftButton) ? hitAt(e->pos()) : Hit();
		if (_pressed.kind == Hit::Kind::Sticker) {
			_previewTimer.start(kStickerPreviewDelay);
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		_previewTimer.stop();
		if (_preview) {
			// A preview ends where it began: nothing is sent.
			hidePreview();
			_pressed = Hit();
			return;
		}
		const auto pressed = std::exchange(_pressed, Hit());
		const auto hit = hitAt(e->pos());
		if (e->button() != Qt::LeftButton || hit != pressed) {
			return;
		}
		switch (hit.kind) {
		case Hit::Kind::Sticker:
			if (const auto callback = _chosen) {
				callback(_sections[hit.section].stickers[hit.index]);
			}
			break;
		case Hit::Kind::Remove:
			if (const auto callback = _install) {
				callback(_sections[hit.section].id, false);
			}
			break;
		case Hit::Kind::Add:
			if (const auto callback = _install) {
				callback(_more[hit.index].pack.id, true);
			}
			break;
		case Hit::Kind::None:
			break;
		}
	}

private:
	struct Hit {
		enum class Kind {
			None,
			Sticker,
			Remove,
			Add,
		};
		Kind kind = Kind::None;
		int section = -1;
		int index = -1;

		friend bool operator==(const Hit &, const Hit &) = default;
	};
	struct Picture {
		std::shared_ptr<Support::StickerPlayer> player;
		QMetaObject::Connection watching;
		std::vector<QRect> rects; // where it was painted (a new frame repaints them)
	};

	void showPreview(const Hit &hit) {
		if (hit.section < 0
			|| hit.section >= int(_sections.size())
			|| hit.index < 0
			|| hit.index >= int(_sections[hit.section].stickers.size())) {
			return;
		}
		const auto host = window();
		if (!_preview) {
			_preview = new StickerPreview(host, _session);
			_preview->setGeometry(host->rect());
			_preview->show();
			_preview->raise();
		}
		_preview->showSticker(_sections[hit.section].stickers[hit.index]);
	}
	void hidePreview() {
		if (_preview) {
			delete _preview.data();
		}
	}

	[[nodiscard]] QRect cellRect(int section, int index) const {
		const auto column = index % kStickerColumns;
		const auto row = index / kStickerColumns;
		return style::rtlrect(
			st::historyPanelPadding.left() + column * _cell,
			_sections[section].top + st::historyPanelHeaderHeight + row * _cell,
			_cell,
			_cell,
			width());
	}
	[[nodiscard]] QRect addRect(int index) const {
		const auto text = tr::lng_history_stickers_add(tr::now);
		const auto buttonWidth = st::smallSemiboldFont->width(text) + 28;
		const auto rowHeight = int(st::historySendFilesRowHeight);
		return style::rtlrect(
			width()
				- st::historyPanelPadding.right()
				- st::historyPanelHeaderPadding
				- buttonWidth,
			_more[index].top + (rowHeight - 30) / 2,
			buttonWidth,
			30,
			width());
	}
	[[nodiscard]] QRect removeRect(int section) const {
		const auto text = tr::lng_history_stickers_remove(tr::now);
		const auto textWidth = st::historyPanelHeaderFont->width(text);
		return style::rtlrect(
			width()
				- st::historyPanelPadding.right()
				- st::historyPanelHeaderPadding
				- textWidth
				- 6,
			_sections[section].top,
			textWidth + 12,
			st::historyPanelHeaderHeight,
			width());
	}
	[[nodiscard]] Hit hitAt(QPoint position) const {
		for (auto s = 0; s != int(_sections.size()); ++s) {
			const auto &section = _sections[s];
			if (section.removable && removeRect(s).contains(position)) {
				return { Hit::Kind::Remove, s, -1 };
			}
			for (auto i = 0; i != int(section.stickers.size()); ++i) {
				if (cellRect(s, i).contains(position)) {
					return { Hit::Kind::Sticker, s, i };
				}
			}
		}
		for (auto i = 0; i != int(_more.size()); ++i) {
			if (addRect(i).contains(position)) {
				return { Hit::Kind::Add, -1, i };
			}
		}
		return Hit();
	}
	void paintSticker(
			QPainter &p,
			const QRect &rect,
			const Data::Sticker &sticker,
			bool play = true) {
		if (sticker.url.isEmpty() || rect.isEmpty()) {
			return;
		}
		const auto ratio = devicePixelRatioF();
		const auto size = rect.size() * ratio;
		auto &picture = _pictures[sticker.url];
		if (!picture.player || picture.player->size() != size) {
			if (picture.player) {
				QObject::disconnect(picture.watching);
			}
			// Animated stickers move while they are painted (the panel is
			// open, the cell is in view), at most a dozen at a time.
			picture.player = Support::StickerPlayer::Get(_session, sticker.url, size);
			if (!picture.player) {
				return;
			}
			const auto url = sticker.url;
			picture.watching = QObject::connect(
				picture.player.get(),
				&Support::StickerPlayer::updated,
				this,
				[this, url] {
					const auto i = _pictures.constFind(url);
					if (i == _pictures.cend()) {
						return;
					}
					for (const auto &rect : i->rects) {
						update(rect);
					}
				});
		}
		if (std::find(picture.rects.begin(), picture.rects.end(), rect)
			== picture.rects.end()) {
			// The same sticker may be in «recent» and in its pack.
			if (picture.rects.size() >= 4) {
				picture.rects.erase(picture.rects.begin());
			}
			picture.rects.push_back(rect);
		}
		const auto image = picture.player->frame(play);
		if (image.isNull()) {
			if (!sticker.emoji.isEmpty()) {
				p.setOpacity(0.4);
				p.setFont(st::historyEmojiFont);
				p.setPen(st::textPrimary);
				p.drawText(rect, Qt::AlignCenter, sticker.emoji);
				p.setOpacity(1.);
			}
			return;
		}
		const auto shown = QSizeF(image.size()) / ratio;
		p.drawImage(
			QRectF(
				rect.x() + (rect.width() - shown.width()) / 2.,
				rect.y() + (rect.height() - shown.height()) / 2.,
				shown.width(),
				shown.height()),
			image);
	}

	Main::Session *_session = nullptr;
	std::vector<StickerSection> _sections;
	std::vector<MorePack> _more;
	QHash<QString, Picture> _pictures;
	int _cell = 84;
	int _moreTop = 0;
	Hit _over;
	Hit _pressed;
	Fn<void(const Data::Sticker &)> _chosen;
	Fn<void(const QString &, bool)> _install;
	QTimer _previewTimer;
	QPointer<StickerPreview> _preview;

};

// A row of small icons that jump to the sections of a grid.
class IconsStrip final : public Ui::RpWidget {
public:
	explicit IconsStrip(QWidget *parent)
	: RpWidget(parent) {
		setMouseTracking(true);
		setCursor(style::cur_pointer);
	}

	struct Item {
		QString id;
		QString glyph;          // an emoji …
		const style::icon *icon = nullptr; // … or an icon
	};
	void setItems(std::vector<Item> items) {
		_items = std::move(items);
		update();
	}
	void setActive(const QString &id) {
		if (_active != id) {
			_active = id;
			update();
		}
	}
	void setClickedCallback(Fn<void(const QString &id)> callback) {
		_clicked = std::move(callback);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::historyEmojiCategoryHeight;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(0, 0, width(), st::lineWidth, st::borderSubtle);
		p.setFont(st::historyEmojiCategoryFont);
		for (auto i = 0; i != int(_items.size()); ++i) {
			const auto rect = itemRect(i);
			if (_items[i].id == _active || i == _over) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush((_items[i].id == _active)
					? st::historyEmojiCategoryActiveBg
					: st::historyEmojiOverBg);
				p.drawRoundedRect(
					rect.marginsRemoved(QMargins(2, 4, 2, 4)),
					st::radiusSm,
					st::radiusSm);
			}
			if (_items[i].icon) {
				_items[i].icon->paintInCenter(p, rect);
			} else {
				// The pack's picture (the fonts' emoji without it), grey
				// unless chosen.
				p.setPen(st::textPrimary);
				Ui::Emoji::PaintOne(
					p,
					_items[i].glyph,
					rect,
					st::historyEmojiCategorySize,
					st::historyEmojiCategoryFont->f,
					_items[i].id != _active);
			}
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = itemAt(e->pos());
		if (_over != over) {
			_over = over;
			update();
		}
	}
	void leaveEvent(QEvent *e) override {
		Q_UNUSED(e);
		if (_over >= 0) {
			_over = -1;
			update();
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = itemAt(e->pos());
		if (e->button() == Qt::LeftButton && index >= 0) {
			if (const auto callback = _clicked) {
				callback(_items[index].id);
			}
		}
	}

private:
	[[nodiscard]] QRect itemRect(int index) const {
		const auto count = std::max(int(_items.size()), 1);
		const auto padding = int(st::historyPanelPadding.left());
		const auto cell = std::min((width() - 2 * padding) / count, 40);
		return style::rtlrect(padding + index * cell, 0, cell, height(), width());
	}
	[[nodiscard]] int itemAt(QPoint position) const {
		for (auto i = 0; i != int(_items.size()); ++i) {
			if (itemRect(i).contains(position)) {
				return i;
			}
		}
		return -1;
	}

	std::vector<Item> _items;
	QString _active;
	int _over = -1;
	Fn<void(const QString &id)> _clicked;

};

} // namespace

// ---- emoji tab

class TabbedPanel::EmojiPane final : public Ui::RpWidget {
public:
	EmojiPane(QWidget *parent, TabbedPanel *panel)
	: RpWidget(parent)
	, _panel(panel)
	, _search(Ui::CreateChild<Ui::SearchField>(
		this,
		tr::lng_history_emoji_search(tr::now)))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this))
	, _grid(_scroll->setOwnedWidget(Ui::object_ptr<EmojiGrid>(_scroll)))
	, _strip(Ui::CreateChild<IconsStrip>(this))
	, _tones(Ui::CreateChild<EmojiTones>(this)) {
		_scroll->setTopBottomShadows(false, false);
		_grid->setTonesPopup(_tones);
		_grid->setChosenCallback([this](const EmojiItem &item) {
			choose(item.code);
		});
		_grid->setTonesCallback([this](const EmojiItem &item, QRect cell) {
			const auto &tones = Core::App().settings().emojiTones();
			const auto i = tones.constFind(Ui::Emoji::Strip(item.base));
			_tones->showFor(
				item.base,
				(i != tones.cend()) ? *i : -1,
				QRect(_grid->mapTo(this, cell.topLeft()), cell.size()));
		});
		_tones->setChosenCallback([this](int tone) {
			const auto base = _tones->base();
			auto &settings = Core::App().settings();
			auto tones = settings.emojiTones();
			if (tone >= 0) {
				tones.insert(Ui::Emoji::Strip(base), tone);
			} else {
				tones.remove(Ui::Emoji::Strip(base));
			}
			settings.setEmojiTones(tones);
			const auto code = Ui::Emoji::WithTone(base, tone);
			_grid->updateCode(base, code);
			choose(code);
		});
		QObject::connect(_search, &Ui::SearchField::changed, this, [this](const QString &) {
			rebuild();
		});
		QObject::connect(_search, &Ui::SearchField::cancelled, this, [this] {
			if (_search->text().isEmpty()) {
				_panel->hideAnimated();
			} else {
				_search->clear();
			}
		});
		QObject::connect(_scroll, &Ui::ScrollArea::scrolled, this, [this] {
			_tones->hide();
			const auto category = _grid->categoryAt(_scroll->scrollTop() + 20);
			_strip->setActive((category < 0) ? u"recent"_s : QString::number(category));
		});
		_strip->setClickedCallback([this](const QString &id) {
			_tones->hide();
			if (!_search->text().isEmpty()) {
				_search->clear();
			}
			_scroll->scrollToY((id == u"recent")
				? 0
				: _grid->sectionTop(id.toInt()));
		});
		auto items = std::vector<IconsStrip::Item>();
		items.push_back({ u"recent"_s, QString(), &st::historyStickerRecentIcon });
		const auto &categories = EmojiCategories();
		for (auto i = 0; i != int(categories.size()); ++i) {
			items.push_back({ QString::number(i), categories[i].icon, nullptr });
		}
		_strip->setItems(std::move(items));
		sortRecent();
		rebuild();
		// «Recent» only when there is such a section at the top.
		const auto category = _grid->categoryAt(_scroll->scrollTop() + 20);
		_strip->setActive((category < 0) ? u"recent"_s : QString::number(category));
	}

	void activate() {
		_search->setFocusFast();
	}
	// The panel is shown: the recent ones in their new order.
	void refreshRecent() {
		_tones->hide();
		sortRecent();
		if (_search->text().isEmpty()) {
			rebuild();
		}
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		const auto &padding = st::historyPanelSearchPadding;
		_search->resizeToWidth(width() - padding.left() - padding.right());
		_search->moveToLeft(padding.left(), padding.top(), width());
		const auto top = padding.top() + _search->height() + padding.bottom();
		_strip->resizeToWidth(width());
		_strip->move(0, height() - _strip->height());
		_scroll->setGeometry(0, top, width(), height() - top - _strip->height());
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (!_grid->empty()) {
			return;
		}
		auto p = Painter(this);
		p.setPen(st::historyPanelEmptyFg);
		p.setFont(st::historyPanelEmptyFont);
		p.drawText(
			_scroll->geometry(),
			Qt::AlignCenter,
			_searching ? tr::lng_loading(tr::now) : tr::lng_nothing_found(tr::now));
	}
	void keyPressEvent(QKeyEvent *e) override {
		if (e->key() == Qt::Key_Escape && _tones->isVisible()) {
			_tones->hide();
		} else {
			RpWidget::keyPressEvent(e);
		}
	}

private:
	void choose(const QString &code) {
		_tones->hide();
		auto &settings = Core::App().settings();
		auto uses = settings.emojiUses().value_or(DefaultEmojiUses());
		CountEmojiUse(uses, _recent, code);
		settings.setEmojiUses(uses);
		// Written as other apps expect it (U+FE0F where an emoji needs it).
		Q_EMIT _panel->emojiChosen(Ui::Emoji::Qualify(code));
	}
	void sortRecent() {
		auto &settings = Core::App().settings();
		auto uses = settings.emojiUses().value_or(DefaultEmojiUses());
		_recent = SortRecentEmoji(uses);
		settings.setEmojiUses(uses);
	}
	void rebuild() {
		auto sections = std::vector<EmojiSection>();
		const auto &tones = Core::App().settings().emojiTones();
		const auto query = _search->text().trimmed();
		_searching = false;
		if (!query.isEmpty()) {
			const auto index = EmojiSearch(this, [this] { rebuild(); });
			auto found = EmojiSection();
			found.title = tr::lng_history_emoji_results(tr::now);
			found.tones = true;
			if (index) {
				for (const auto &base : index->search(query)) {
					found.items.push_back({ base, EmojiWithTone(base, tones) });
				}
			} else {
				_searching = true;
			}
			sections.push_back(std::move(found));
		} else {
			auto recent = EmojiSection();
			recent.title = tr::lng_history_emoji_recent(tr::now);
			for (const auto &code : _recent) {
				recent.items.push_back({ code, code });
			}
			sections.push_back(std::move(recent));
			const auto &categories = EmojiCategories();
			for (auto i = 0; i != int(categories.size()); ++i) {
				auto section = EmojiSection();
				section.title = EmojiCategoryTitle(i);
				section.category = i;
				section.tones = true;
				section.items.reserve(categories[i].items.size());
				for (const auto &base : categories[i].items) {
					section.items.push_back({ base, EmojiWithTone(base, tones) });
				}
				sections.push_back(std::move(section));
			}
		}
		_tones->hide();
		_grid->setSections(std::move(sections));
		_scroll->scrollToY(0);
		update();
	}

	TabbedPanel *_panel = nullptr;
	Ui::SearchField *_search = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	EmojiGrid *_grid = nullptr;
	IconsStrip *_strip = nullptr;
	EmojiTones *_tones = nullptr;
	QStringList _recent; // in the order shown while the panel is open
	bool _searching = false;

};

// ---- stickers tab

class TabbedPanel::StickersPane final : public Ui::RpWidget {
public:
	StickersPane(QWidget *parent, TabbedPanel *panel, Main::Session *session)
	: RpWidget(parent)
	, _panel(panel)
	, _session(session)
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this))
	, _grid(_scroll->setOwnedWidget(Ui::object_ptr<StickerGrid>(_scroll, session)))
	, _strip(Ui::CreateChild<IconsStrip>(this)) {
		_scroll->setTopBottomShadows(false, false);
		auto &stickers = _session->data().stickers();
		_grid->setChosenCallback([this](const Data::Sticker &sticker) {
			Q_EMIT _panel->stickerChosen(sticker);
		});
		_grid->setInstallCallback([this](const QString &packId, bool install) {
			_session->data().stickers().setInstalled(packId, install);
		});
		QObject::connect(&stickers, &Data::Stickers::changed, this, [this] {
			rebuild();
		});
		QObject::connect(_scroll, &Ui::ScrollArea::scrolled, this, [this] {
			_strip->setActive(_grid->sectionAt(_scroll->scrollTop() + 20));
		});
		_strip->setClickedCallback([this](const QString &id) {
			_scroll->scrollToY(_grid->sectionTop(id));
		});
		rebuild();
	}

	void activate() {
		if (!std::exchange(_requested, true)) {
			_loading = !_session->data().stickers().loaded();
			_session->data().stickers().load(this, [this] {
				_loading = false;
				rebuild();
			});
			update();
		}
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_strip->resizeToWidth(width());
		_strip->move(0, height() - _strip->height());
		_scroll->setGeometry(0, 0, width(), height() - _strip->height());
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (!_grid->empty()) {
			return;
		}
		auto p = Painter(this);
		p.setPen(st::historyPanelEmptyFg);
		p.setFont(st::historyPanelEmptyFont);
		p.drawText(
			_scroll->geometry(),
			Qt::AlignCenter,
			_loading
				? tr::lng_loading(tr::now)
				: tr::lng_history_stickers_empty(tr::now));
	}

private:
	void rebuild() {
		const auto &stickers = _session->data().stickers();
		auto sections = std::vector<StickerSection>();
		auto more = std::vector<MorePack>();
		auto items = std::vector<IconsStrip::Item>();
		if (!stickers.recent().isEmpty()) {
			auto recent = StickerSection();
			recent.id = u"recent"_s;
			recent.title = tr::lng_history_emoji_recent(tr::now);
			recent.stickers = stickers.recent();
			sections.push_back(std::move(recent));
			items.push_back({ u"recent"_s, QString(), &st::historyStickerRecentIcon });
		}
		for (const auto &pack : stickers.packs()) {
			if (pack.stickers.isEmpty()) {
				continue;
			} else if (!pack.installed) {
				more.push_back({ pack, 0 });
				continue;
			}
			auto section = StickerSection();
			section.id = pack.id;
			section.title = pack.title;
			section.stickers = pack.stickers;
			section.removable = !pack.isDefault;
			sections.push_back(std::move(section));
			items.push_back({
				pack.id,
				pack.stickers.front().emoji.isEmpty()
					? QString(QChar(0x2022))
					: pack.stickers.front().emoji,
				nullptr,
			});
		}
		if (!more.empty()) {
			items.push_back({ u"more"_s, QString(QChar(u'+')), nullptr });
		}
		_grid->setContent(std::move(sections), std::move(more));
		_strip->setItems(std::move(items));
		_strip->setActive(_grid->sectionAt(_scroll->scrollTop() + 20));
		update();
	}

	TabbedPanel *_panel = nullptr;
	Main::Session *_session = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	StickerGrid *_grid = nullptr;
	IconsStrip *_strip = nullptr;
	bool _requested = false;
	bool _loading = false;

};

// ---- the panel

TabbedPanel::TabbedPanel(QWidget *parent, Main::Session *session)
: RpWidget(parent)
, _session(session)
, _tabs(Ui::CreateChild<Ui::TabBar>(this))
, _emoji(new EmojiPane(this, this))
, _stickers(new StickersPane(this, this, session)) {
	hide();
	setFocusPolicy(Qt::StrongFocus);
	const auto config = session->data().clientConfig();
	_stickersAllowed = !config || config->stickersEnabled();
	_tabs->setStretch(true);
	_tabs->setTabs(_stickersAllowed
		? QStringList{
			tr::lng_history_tab_emoji(tr::now),
			tr::lng_history_tab_stickers(tr::now),
		}
		: QStringList{ tr::lng_history_tab_emoji(tr::now) });
	connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		switchTab(index);
	});
	const auto &extend = st::defaultShadow.extend;
	resize(
		st::historyPanelSize.width() + extend.left() + extend.right(),
		st::historyPanelSize.height() + extend.top() + extend.bottom());
	_stickers->hide();
}

TabbedPanel::~TabbedPanel() {
	if (qApp) {
		qApp->removeEventFilter(this);
	}
}

QRect TabbedPanel::bodyRect() const {
	return rect().marginsRemoved(st::defaultShadow.extend);
}

void TabbedPanel::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto body = bodyRect();
	_tabs->resizeToWidth(body.width());
	_tabs->move(body.topLeft());
	const auto top = body.y() + _tabs->height();
	const auto content = QRect(body.x(), top, body.width(), body.bottom() + 1 - top);
	_emoji->setGeometry(content);
	_stickers->setGeometry(content);
}

void TabbedPanel::switchTab(int index) {
	const auto stickers = (index == 1) && _stickersAllowed;
	_emoji->setVisible(!stickers);
	_stickers->setVisible(stickers);
	if (stickers) {
		_stickers->activate();
	} else {
		_emoji->activate();
	}
}

void TabbedPanel::showStickers() {
	if (_stickersAllowed) {
		_tabs->setActive(1, anim::type::instant);
		switchTab(1);
	}
}

void TabbedPanel::toggle(QWidget *anchor) {
	if (_shown) {
		hideAnimated();
	} else {
		showAbove(anchor);
	}
}

void TabbedPanel::place() {
	const auto parent = parentWidget();
	if (!parent || !_anchor) {
		return;
	}
	const auto &extend = st::defaultShadow.extend;
	const auto anchor = QRect(
		_anchor->mapTo(parent, QPoint()),
		_anchor->size());
	const auto bodyWidth = std::min(
		st::historyPanelSize.width(),
		parent->width() - 2 * st::historyPanelMargin);
	const auto bodyHeight = std::min(
		st::historyPanelSize.height(),
		anchor.top() - 2 * st::historyPanelMargin);
	// Trailing edges aligned, kept inside the parent.
	auto left = style::RightToLeft()
		? anchor.left()
		: (anchor.right() + 1 - bodyWidth);
	left = std::clamp(
		left,
		int(st::historyPanelMargin),
		std::max(parent->width() - bodyWidth - st::historyPanelMargin, int(st::historyPanelMargin)));
	const auto top = anchor.top() - st::historyPanelMargin - bodyHeight;
	setGeometry(
		left - extend.left(),
		top - extend.top(),
		bodyWidth + extend.left() + extend.right(),
		std::max(bodyHeight, 160) + extend.top() + extend.bottom());
}

void TabbedPanel::updatePosition() {
	if (_shown) {
		place();
	}
}

void TabbedPanel::showAbove(QWidget *anchor) {
	_anchor = anchor;
	place();
	if (_shown) {
		return;
	}
	_shown = true;
	_emoji->refreshRecent();
	show();
	raise();
	switchTab(_tabs->active());
	qApp->installEventFilter(this);
	_showAnimation.start([this] { update(); }, 0., 1., st::motionFast, anim::easeOutCubic);
	Q_EMIT panelShownChanged(true);
}

void TabbedPanel::hideAnimated() {
	hideFast();
}

void TabbedPanel::hideFast() {
	if (!_shown) {
		return;
	}
	_shown = false;
	qApp->removeEventFilter(this);
	hide();
	Q_EMIT panelShownChanged(false);
}

void TabbedPanel::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		hideAnimated();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void TabbedPanel::mousePressEvent(QMouseEvent *e) {
	// A press that nothing inside took (a category tab, the space between
	// the cells) stays here: below the panel it reached the chat, which
	// closed the panel.
	e->accept();
}

bool TabbedPanel::eventFilter(QObject *object, QEvent *event) {
	if (_shown && event->type() == QEvent::MouseButtonPress) {
		const auto widget = qobject_cast<QWidget*>(object);
		// A press outside closes the panel (the anchor toggles it itself).
		if (widget
			&& widget->window() == window()
			&& !Ui::IsAncestorOf(this, widget)
			&& !(_anchor && Ui::IsAncestorOf(_anchor, widget))) {
			hideAnimated();
		}
	}
	return RpWidget::eventFilter(object, event);
}

void TabbedPanel::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto body = bodyRect();
	Ui::Shadow::Paint(p, body, st::historyPanelRadius, st::defaultShadow);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(st::historyPanelBorder->c, 1.));
	p.setBrush(st::historyPanelBg);
	p.drawRoundedRect(
		QRectF(body).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
		st::historyPanelRadius,
		st::historyPanelRadius);
}

} // namespace History
