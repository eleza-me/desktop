#include "history/history_bot_widgets.h"

#include "history/history_themes.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/widgets/scroll_area.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtGui/QMouseEvent>

namespace History {
namespace {

// The inline list asks for its next page this close to its end.
constexpr auto kNearEndThreshold = 120;

[[nodiscard]] QString FirstLetter(const QString &text) {
	const auto trimmed = text.trimmed();
	if (trimmed.isEmpty()) {
		return QString();
	}
	const auto length = (trimmed.size() > 1 && trimmed[0].isHighSurrogate()) ? 2 : 1;
	return trimmed.left(length).toUpper();
}

} // namespace

// ---- the rows inside the scroll area

class BotSuggestPanel::Inner final : public Ui::RpWidget {
public:
	struct Row {
		enum class Type {
			Header,
			Command,
			Result,
		};
		Type type = Type::Header;
		QString text;                // the header
		Data::BotCommand command;
		Data::InlineResult result;
	};

	Inner(QWidget *parent, Main::Session *session);

	void setRows(std::vector<Row> rows, bool keepSelection);
	[[nodiscard]] const std::vector<Row> &rows() const {
		return _rows;
	}
	[[nodiscard]] int contentHeight() const;
	void selectSkip(int direction);
	[[nodiscard]] int selected() const {
		return _selected;
	}
	// [top, bottom) of a row inside this widget.
	[[nodiscard]] std::pair<int, int> rowRange(int index) const;
	void setChosen(Fn<void(int index)> callback) {
		_chosen = std::move(callback);
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	[[nodiscard]] static int RowHeight(const Row &row);
	[[nodiscard]] static bool Selectable(const Row &row);
	[[nodiscard]] int rowAt(QPoint position) const;
	void paintThumb(Painter &p, QRect rect, const Data::InlineResult &result);

	Main::Session *_session = nullptr;
	std::vector<Row> _rows;
	int _selected = -1;
	int _pressed = -1;
	Fn<void(int)> _chosen;
	QHash<QString, std::shared_ptr<Ui::ImageSource>> _thumbs;
	QHash<QString, std::pair<qint64, QImage>> _prepared; // url -> (source key, rounded)

};

BotSuggestPanel::Inner::Inner(QWidget *parent, Main::Session *session)
: RpWidget(parent)
, _session(session) {
	setMouseTracking(true);
	setFocusPolicy(Qt::NoFocus);
}

int BotSuggestPanel::Inner::RowHeight(const Row &row) {
	switch (row.type) {
	case Row::Type::Header: return st::historyBotPanelHeaderHeight;
	case Row::Type::Command: return st::historyBotPanelCommandHeight;
	case Row::Type::Result: return st::historyBotPanelResultHeight;
	}
	return 0;
}

bool BotSuggestPanel::Inner::Selectable(const Row &row) {
	return row.type != Row::Type::Header;
}

void BotSuggestPanel::Inner::setRows(std::vector<Row> rows, bool keepSelection) {
	_rows = std::move(rows);
	_pressed = -1;
	if (!keepSelection || _selected >= int(_rows.size()) || _selected < 0) {
		_selected = -1;
		for (auto i = 0; i != int(_rows.size()); ++i) {
			if (Selectable(_rows[i])) {
				_selected = i;
				break;
			}
		}
	}
	// Pictures of results that left the list are let go.
	auto urls = QSet<QString>();
	for (const auto &row : _rows) {
		if (row.type == Row::Type::Result && !row.result.thumbUrl.isEmpty()) {
			urls.insert(row.result.thumbUrl);
		}
	}
	for (auto i = _thumbs.begin(); i != _thumbs.end();) {
		if (urls.contains(i.key())) {
			++i;
		} else {
			_prepared.remove(i.key());
			i = _thumbs.erase(i);
		}
	}
	resizeToWidth(width());
	update();
}

int BotSuggestPanel::Inner::contentHeight() const {
	auto result = 0;
	for (const auto &row : _rows) {
		result += RowHeight(row);
	}
	return result;
}

std::pair<int, int> BotSuggestPanel::Inner::rowRange(int index) const {
	auto top = 0;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto height = RowHeight(_rows[i]);
		if (i == index) {
			return { top, top + height };
		}
		top += height;
	}
	return { 0, 0 };
}

void BotSuggestPanel::Inner::selectSkip(int direction) {
	const auto count = int(_rows.size());
	if (count <= 0) {
		return;
	}
	auto index = _selected;
	for (auto step = 0; step != count; ++step) {
		index = ((index + direction) % count + count) % count;
		if (Selectable(_rows[index])) {
			_selected = index;
			update();
			return;
		}
	}
}

int BotSuggestPanel::Inner::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return contentHeight();
}

int BotSuggestPanel::Inner::rowAt(QPoint position) const {
	if (position.x() < 0 || position.x() >= width()) {
		return -1;
	}
	auto top = 0;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto height = RowHeight(_rows[i]);
		if (position.y() >= top && position.y() < top + height) {
			return Selectable(_rows[i]) ? i : -1;
		}
		top += height;
	}
	return -1;
}

void BotSuggestPanel::Inner::paintThumb(
		Painter &p,
		QRect rect,
		const Data::InlineResult &result) {
	const auto radius = int(st::historyBotPanelThumbRadius);
	auto image = QImage();
	if (!result.thumbUrl.isEmpty()) {
		auto &source = _thumbs[result.thumbUrl];
		if (!source) {
			source = Support::MakeImageSource(
				_session,
				Data::MediaRef::Url(result.thumbUrl),
				Data::MediaVariant::Thumb);
			if (source) {
				connect(source.get(), &Ui::ImageSource::updated, this, [this] {
					update();
				});
			}
		}
		if (source && !source->failed()) {
			const auto ratio = devicePixelRatioF();
			auto loaded = source->request(rect.size() * ratio);
			if (loaded.isNull()) {
				loaded = source->preview();
			}
			if (!loaded.isNull()) {
				auto &prepared = _prepared[result.thumbUrl];
				if (prepared.second.isNull() || prepared.first != loaded.cacheKey()) {
					prepared.first = loaded.cacheKey();
					prepared.second = Ui::Images::Round(
						Ui::Images::PrepareCover(loaded, rect.size(), ratio),
						radius);
				}
				image = prepared.second;
			}
		}
	}
	if (!image.isNull()) {
		p.drawImage(rect.topLeft(), image);
		return;
	}
	// No picture (yet): the first letter of the title on a tile.
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyBotPanelLetterBg);
		p.drawRoundedRect(rect, radius, radius);
	}
	p.setPen(st::textSecondary);
	p.setFont(st::historyBotPanelLetterFont);
	p.drawText(rect, Qt::AlignCenter, FirstLetter(result.title));
}

void BotSuggestPanel::Inner::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto outer = width();
	const auto skip = int(st::historyBotPanelRowSkip);
	auto top = 0;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		const auto height = RowHeight(row);
		const auto rect = QRect(0, top, outer, height);
		top += height;
		if (!rect.intersects(clip)) {
			continue;
		}
		if (i == _selected) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyMentionSelectedBg);
			p.drawRoundedRect(rect, st::historyMentionsRadius - 4, st::historyMentionsRadius - 4);
		}
		const auto available = outer - 2 * skip;
		switch (row.type) {
		case Row::Type::Header: {
			const auto &font = st::historyBotPanelHeaderFont;
			p.setFont(font);
			p.setPen(st::historyBotPanelHeaderFg);
			p.drawTextLeft(
				skip,
				rect.y() + (height - font->height) / 2,
				outer,
				font->elided(row.text, std::max(available, 1)));
		} break;
		case Row::Type::Command: {
			const auto command = u'/' + row.command.command;
			const auto &commandFont = st::historyBotPanelCommandFont;
			const auto commandWidth = std::min(commandFont->width(command), available);
			p.setFont(commandFont);
			p.setPen(st::historyBotPanelCommandFg);
			p.drawTextLeft(
				skip,
				rect.y() + (height - commandFont->height) / 2,
				outer,
				commandFont->elided(command, available),
				commandWidth);
			const auto left = skip + commandWidth + skip;
			const auto rest = outer - left - skip;
			if (rest > 0 && !row.command.description.isEmpty()) {
				const auto &font = st::historyBotPanelTextFont;
				p.setFont(font);
				p.setPen(st::historyBotPanelTextFg);
				p.drawTextLeft(
					left,
					rect.y() + (height - font->height) / 2,
					outer,
					font->elided(row.command.description.simplified(), rest));
			}
		} break;
		case Row::Type::Result: {
			const auto size = int(st::historyBotPanelThumb);
			paintThumb(
				p,
				style::rtlrect(skip, rect.y() + (height - size) / 2, size, size, outer),
				row.result);
			const auto left = skip + size + skip;
			const auto rest = std::max(outer - left - skip, 1);
			const auto &titleFont = st::historyBotPanelTitleFont;
			const auto &textFont = st::historyBotPanelTextFont;
			const auto description = row.result.description.simplified();
			const auto block = titleFont->height
				+ (description.isEmpty() ? 0 : (textFont->height + 2));
			auto y = rect.y() + (height - block) / 2;
			p.setFont(titleFont);
			p.setPen(st::textPrimary);
			p.drawTextLeft(
				left,
				y,
				outer,
				titleFont->elided(row.result.title.simplified(), rest));
			if (!description.isEmpty()) {
				y += titleFont->height + 2;
				p.setFont(textFont);
				p.setPen(st::historyBotPanelTextFg);
				p.drawTextLeft(left, y, outer, textFont->elided(description, rest));
			}
		} break;
		}
	}
}

void BotSuggestPanel::Inner::mouseMoveEvent(QMouseEvent *e) {
	const auto index = rowAt(e->pos());
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
	if (index >= 0 && index != _selected) {
		_selected = index;
		update();
	}
}

void BotSuggestPanel::Inner::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) ? rowAt(e->pos()) : -1;
}

void BotSuggestPanel::Inner::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	const auto index = rowAt(e->pos());
	if (e->button() == Qt::LeftButton
		&& index >= 0
		&& index == pressed
		&& _chosen) {
		_chosen(index);
	}
}

void BotSuggestPanel::Inner::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	_pressed = -1;
}

// ---- the panel

BotSuggestPanel::BotSuggestPanel(QWidget *parent, Main::Session *session)
: RpWidget(parent)
, _session(session)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this)) {
	_inner = _scroll->setOwnedWidget(Ui::object_ptr<Inner>(_scroll, session));
	_inner->setChosen([this](int index) {
		const auto &rows = _inner->rows();
		if (index < 0 || index >= int(rows.size())) {
			return;
		}
		// Copies: the callbacks change the rows.
		if (rows[index].type == Inner::Row::Type::Command) {
			const auto command = rows[index].command;
			if (_commandChosen) {
				_commandChosen(command, false);
			}
		} else if (rows[index].type == Inner::Row::Type::Result) {
			const auto result = rows[index].result;
			if (_resultChosen) {
				_resultChosen(result);
			}
		}
	});
	_scroll->setNearEdgeThreshold(kNearEndThreshold);
	connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
		if (_mode == Mode::Inline && _needMore && !isHidden()) {
			_needMore();
		}
	});
	// The caret stays in the field.
	setFocusPolicy(Qt::NoFocus);
	_scroll->setFocusPolicy(Qt::NoFocus);
	hide();
}

BotSuggestPanel::~BotSuggestPanel() = default;

void BotSuggestPanel::setCommandChosen(
		Fn<void(const Data::BotCommand &, bool insert)> callback) {
	_commandChosen = std::move(callback);
}

void BotSuggestPanel::setResultChosen(Fn<void(const Data::InlineResult &)> callback) {
	_resultChosen = std::move(callback);
}

void BotSuggestPanel::setNeedMore(Fn<void()> callback) {
	_needMore = std::move(callback);
}

void BotSuggestPanel::showCommands(const QVector<Data::BotCommand> &commands) {
	auto rows = std::vector<Inner::Row>();
	rows.reserve(commands.size());
	for (const auto &command : commands) {
		auto row = Inner::Row();
		row.type = Inner::Row::Type::Command;
		row.command = command;
		rows.push_back(std::move(row));
	}
	if (rows.empty()) {
		hideFast();
		return;
	}
	const auto was = _mode;
	_mode = Mode::Commands;
	_inner->setRows(std::move(rows), false);
	refreshGeometry();
	if (was != Mode::Commands || isHidden()) {
		_scroll->scrollToY(0);
	}
	show();
	raise();
}

void BotSuggestPanel::showInline(
		const QString &header,
		const QVector<Data::InlineResult> &results,
		bool more) {
	auto rows = std::vector<Inner::Row>();
	if (!header.isEmpty()) {
		auto row = Inner::Row();
		row.type = Inner::Row::Type::Header;
		row.text = header;
		rows.push_back(std::move(row));
	}
	for (const auto &result : results) {
		auto row = Inner::Row();
		row.type = Inner::Row::Type::Result;
		row.result = result;
		rows.push_back(std::move(row));
	}
	if (rows.empty()) {
		hideFast();
		return;
	}
	const auto keep = more && (_mode == Mode::Inline) && !isHidden();
	_mode = Mode::Inline;
	_inner->setRows(std::move(rows), keep);
	refreshGeometry();
	if (!keep) {
		_scroll->scrollToY(0);
	}
	show();
	raise();
}

void BotSuggestPanel::hideFast() {
	_mode = Mode::None;
	_inner->setRows({}, false);
	hide();
}

void BotSuggestPanel::selectSkip(int direction) {
	_inner->selectSkip(direction);
	const auto [top, bottom] = _inner->rowRange(_inner->selected());
	if (bottom > top) {
		_scroll->scrollToY(top, bottom);
	}
}

bool BotSuggestPanel::chooseSelected(bool insert) {
	const auto index = _inner->selected();
	const auto &rows = _inner->rows();
	if (isHidden() || index < 0 || index >= int(rows.size())) {
		return false;
	}
	if (rows[index].type == Inner::Row::Type::Command) {
		const auto command = rows[index].command;
		if (_commandChosen) {
			_commandChosen(command, insert);
		}
		return true;
	} else if (rows[index].type == Inner::Row::Type::Result) {
		const auto result = rows[index].result;
		if (_resultChosen) {
			_resultChosen(result);
		}
		return true;
	}
	return false;
}

void BotSuggestPanel::placeAbove(const QRect &anchor) {
	_anchor = anchor;
	refreshGeometry();
}

void BotSuggestPanel::refreshGeometry() {
	if (_anchor.isEmpty() || _inner->rows().empty()) {
		return;
	}
	const auto &padding = st::historyBotPanelPadding;
	const auto content = _inner->contentHeight();
	const auto height = std::min(
		padding.top() + content + padding.bottom(),
		int(st::historyBotPanelMaxHeight));
	const auto width = std::min(_anchor.width(), int(st::historyBotPanelMaxWidth));
	// At the leading edge of the field, like the text it completes.
	const auto left = style::RightToLeft()
		? (_anchor.x() + _anchor.width() - width)
		: _anchor.x();
	setGeometry(
		left,
		std::max(_anchor.y() - height - st::historyMentionsSkip, 0),
		width,
		height);
}

void BotSuggestPanel::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto &padding = st::historyBotPanelPadding;
	_scroll->setGeometry(rect().marginsRemoved(padding));
	_inner->resizeToWidth(_scroll->width());
}

void BotSuggestPanel::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(st::historyComposeLineFg->c, 1.));
	p.setBrush(st::historyComposeBg);
	p.drawRoundedRect(
		QRectF(rect()).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
		st::historyMentionsRadius,
		st::historyMentionsRadius);
}

// ---- the card of an empty chat

BotIntroCard::BotIntroCard(QWidget *parent, Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller) {
	setMouseTracking(true);
	hide();
}

void BotIntroCard::setDescription(const QString &description) {
	if (_description == description) {
		return;
	}
	_description = description;
	_text.setMarkedText(
		st::historyBotIntroTextStyle,
		Ui::Text::IsolateLinks(Ui::Text::ParseEntities(
			description,
			Ui::Text::ParseLinks | Ui::Text::ParseMentions)));
	_hovered = _pressed = -1;
	update();
}

void BotIntroCard::placeIn(const QRect &area) {
	if (_text.isEmpty() || area.isEmpty()) {
		return;
	}
	const auto &padding = st::historyBotIntroPadding;
	const auto margin = int(st::historyBotIntroMargin);
	const auto maxInner = std::min(
		int(st::historyBotIntroMaxWidth),
		area.width() - 2 * margin) - padding.left() - padding.right();
	if (maxInner <= 0) {
		return;
	}
	const auto title = tr::lng_bot_intro_title(tr::now);
	const auto titleWidth = st::historyBotIntroTitleFont->width(title);
	_textWidth = std::min(maxInner, std::max(_text.countMaxLineWidth(maxInner), titleWidth));
	const auto textHeight = _text.countHeight(_textWidth);
	const auto width = padding.left() + _textWidth + padding.right();
	const auto height = padding.top()
		+ st::historyBotIntroTitleFont->height
		+ st::historyBotIntroTitleSkip
		+ std::min(textHeight, std::max(area.height() - 2 * margin, 0))
		+ padding.bottom();
	setGeometry(
		area.x() + (area.width() - width) / 2,
		area.y() + std::max((area.height() - height) / 2, 0),
		width,
		std::min(height, area.height()));
}

Ui::Text::PaintContext BotIntroCard::textContext() const {
	const auto &padding = st::historyBotIntroPadding;
	auto result = Ui::Text::PaintContext();
	result.position = QPoint(
		padding.left(),
		padding.top()
			+ st::historyBotIntroTitleFont->height
			+ st::historyBotIntroTitleSkip);
	result.availableWidth = _textWidth;
	result.autoDirection = true;
	result.hoveredLink = _hovered;
	return result;
}

int BotIntroCard::linkAt(QPoint position) const {
	const auto state = _text.getState(position, textContext());
	return state.uponSymbol ? state.link : -1;
}

void BotIntroCard::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(ServiceBg());
		p.drawRoundedRect(rect(), st::historyBotIntroRadius, st::historyBotIntroRadius);
	}
	const auto &padding = st::historyBotIntroPadding;
	const auto &titleFont = st::historyBotIntroTitleFont;
	p.setPen(st::historyServiceFg);
	p.setFont(titleFont);
	p.drawText(
		QRect(padding.left(), padding.top(), _textWidth, titleFont->height),
		Qt::AlignCenter,
		titleFont->elided(tr::lng_bot_intro_title(tr::now), _textWidth));
	auto context = textContext();
	context.clip = rect();
	_text.draw(p, context);
}

void BotIntroCard::mouseMoveEvent(QMouseEvent *e) {
	const auto link = linkAt(e->pos());
	if (_hovered != link) {
		_hovered = link;
		setCursor((link >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void BotIntroCard::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) ? linkAt(e->pos()) : -1;
}

void BotIntroCard::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == linkAt(e->pos()) && pressed < _text.linksCount()) {
		Support::ActivateTextLink(_controller, _text.link(pressed), e->button());
	}
}

void BotIntroCard::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	_pressed = -1;
	if (_hovered >= 0) {
		_hovered = -1;
		setCursor(style::cur_default);
		update();
	}
}

} // namespace History
