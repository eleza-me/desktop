#include "ui/widgets/labels.h"

#include "ui/widgets/popup_menu.h"
#include "ui/painter.h"
#include "ui/ui_phrases.h"

#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>

namespace Ui {

FlatLabel::FlatLabel(QWidget *parent, const style::FlatLabel &st)
: RpWidget(parent)
, _st(st) {
	setMouseTracking(true);
}

FlatLabel::FlatLabel(
	QWidget *parent,
	const QString &text,
	const style::FlatLabel &st)
: FlatLabel(parent, st) {
	setText(text);
}

void FlatLabel::setText(const QString &text) {
	_text.setText(_st.style, text);
	_selection = Text::Selection();
	_hoveredLink = _pressedLink = -1;
	refreshSize();
}

void FlatLabel::setMarkedText(const Text::TextWithEntities &text) {
	_text.setMarkedText(_st.style, text);
	_selection = Text::Selection();
	_hoveredLink = _pressedLink = -1;
	refreshSize();
}

void FlatLabel::refreshSize() {
	_inOwnResize = true;
	const auto natural = naturalWidth();
	switch (_widthMode) {
	case WidthMode::Natural: resizeToWidth(natural); break;
	case WidthMode::Fixed: resizeToWidth(_widthValue); break;
	case WidthMode::NaturalMax:
		resizeToWidth(std::min(natural, _widthValue));
		break;
	}
	_inOwnResize = false;
	update();
}

void FlatLabel::resizeToNaturalWidth(int maxWidth) {
	_widthMode = WidthMode::NaturalMax;
	_widthValue = maxWidth;
	refreshSize();
}

void FlatLabel::setSelectable(bool selectable) {
	_selectable = selectable;
	setFocusPolicy(selectable ? Qt::ClickFocus : Qt::NoFocus);
	refreshCursor();
}

void FlatLabel::setTextColorOverride(std::optional<QColor> color) {
	_colorOverride = color;
	update();
}

void FlatLabel::setMaxLines(int lines) {
	_maxLinesOverride = lines;
	refreshSize();
}

void FlatLabel::setAlign(style::align align) {
	_alignOverride = align;
	update();
}

void FlatLabel::setAutoDirection(bool autoDirection) {
	_autoDirection = autoDirection;
	update();
}

void FlatLabel::setLinkHandler(
		std::function<bool(const Text::Link&, Qt::MouseButton)> handler) {
	_linkHandler = std::move(handler);
}

int FlatLabel::maxLines() const {
	return (_maxLinesOverride >= 0) ? _maxLinesOverride : _st.maxLines;
}

int FlatLabel::naturalWidth() const {
	return std::max(_text.maxWidth(), _st.minWidth);
}

int FlatLabel::resizeGetHeight(int newWidth) {
	if (!_inOwnResize) {
		_widthMode = WidthMode::Fixed;
		_widthValue = newWidth;
	}
	if (_text.isEmpty()) {
		return _text.minHeight();
	}
	return std::max(
		_text.countHeight(std::max(newWidth, 1), maxLines()),
		_text.minHeight());
}

Text::PaintContext FlatLabel::geometry() const {
	return Text::PaintContext{
		.position = QPoint(0, 0),
		.availableWidth = width(),
		.align = _alignOverride.value_or(_st.align),
		.elisionLines = maxLines(),
		.autoDirection = _autoDirection,
	};
}

QString FlatLabel::selectedText() const {
	return _text.toString(_selection);
}

void FlatLabel::copySelectedText() const {
	const auto text = _selection.empty() ? _text.toString() : selectedText();
	if (!text.isEmpty()) {
		QGuiApplication::clipboard()->setText(text);
	}
}

void FlatLabel::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	p.setPen(_colorOverride ? QPen(*_colorOverride) : _st.textFg->p);
	auto context = geometry();
	context.selection = _selection;
	context.clip = e->rect();
	context.hoveredLink = _hoveredLink;
	_text.draw(p, context);
}

void FlatLabel::updateHover(QPoint position) {
	const auto state = _text.getState(position, geometry());
	const auto link = state.link;
	const auto spoiler = state.spoiler;
	if (_hoveredLink != link || _overSpoiler != spoiler) {
		_hoveredLink = link;
		_overSpoiler = spoiler;
		refreshCursor();
		update();
	}
	if (_dragging && _dragStart >= 0) {
		auto selection = (state.cursor >= _dragStart)
			? Text::Selection{ _dragStart, state.cursor }
			: Text::Selection{ state.cursor, _dragStart };
		if (_selectType != Text::SelectType::Letters) {
			selection = _text.adjustSelection(selection, _selectType);
			selection.from = std::min(selection.from, _dragBase.from);
			selection.to = std::max(selection.to, _dragBase.to);
		} else {
			selection = _text.adjustSelection(selection, _selectType);
		}
		if (_selection != selection) {
			_selection = selection;
			update();
		}
	}
}

void FlatLabel::refreshCursor() {
	setCursor((_hoveredLink >= 0 || _overSpoiler)
		? style::cur_pointer
		: _selectable
		? style::cur_text
		: style::cur_default);
}

void FlatLabel::mouseMoveEvent(QMouseEvent *e) {
	updateHover(e->pos());
}

void FlatLabel::mousePressEvent(QMouseEvent *e) {
	updateHover(e->pos());
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressedLink = _hoveredLink;
	_pressedSpoiler = _overSpoiler;
	if (_selectable && _pressedLink < 0 && !_pressedSpoiler) {
		const auto state = _text.getState(e->pos(), geometry());
		_dragStart = state.cursor;
		_dragging = true;
		_selectType = Text::SelectType::Letters;
		_dragBase = Text::Selection{ _dragStart, _dragStart };
		if (!_selection.empty()) {
			_selection = Text::Selection();
			update();
		}
	}
}

void FlatLabel::mouseReleaseEvent(QMouseEvent *e) {
	updateHover(e->pos());
	const auto wasDragging = std::exchange(_dragging, false);
	_dragStart = -1;
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressedLink = std::exchange(_pressedLink, -1);
	const auto pressedSpoiler = std::exchange(_pressedSpoiler, false);
	if (pressedSpoiler && _overSpoiler) {
		_text.setSpoilersRevealed(true);
		_overSpoiler = false;
		refreshCursor();
		update();
	} else if (pressedLink >= 0
		&& pressedLink == _hoveredLink
		&& (!wasDragging || _selection.empty())) {
		activateLink(pressedLink, e->button());
	}
}

void FlatLabel::mouseDoubleClickEvent(QMouseEvent *e) {
	if (!_selectable || e->button() != Qt::LeftButton) {
		return;
	}
	const auto state = _text.getState(e->pos(), geometry());
	if (!state.uponSymbol) {
		return;
	}
	_selectType = Text::SelectType::Words;
	_selection = _text.adjustSelection(
		Text::Selection{ state.symbol, state.symbol + 1 },
		_selectType);
	_dragBase = _selection;
	_dragStart = state.symbol;
	_dragging = true;
	update();
}

void FlatLabel::leaveEvent(QEvent *e) {
	if (_hoveredLink >= 0 || _overSpoiler) {
		_hoveredLink = -1;
		_overSpoiler = false;
		refreshCursor();
		update();
	}
	RpWidget::leaveEvent(e);
}

void FlatLabel::focusOutEvent(QFocusEvent *e) {
	if (!_selection.empty() && !_contextMenu) {
		_selection = Text::Selection();
		update();
	}
	RpWidget::focusOutEvent(e);
}

void FlatLabel::keyPressEvent(QKeyEvent *e) {
	if (_selectable && e->matches(QKeySequence::Copy)) {
		copySelectedText();
	} else if (_selectable && e->matches(QKeySequence::SelectAll)) {
		_selection = Text::Selection{ 0, _text.length() };
		update();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void FlatLabel::contextMenuEvent(QContextMenuEvent *e) {
	const auto state = _text.getState(e->pos(), geometry());
	const auto hasLink = (state.link >= 0);
	if (!_selectable && !hasLink) {
		e->ignore();
		return;
	}
	const auto &phrases = CurrentPhrases();
	_contextMenu = new PopupMenu(this);
	_contextMenu->setAttribute(Qt::WA_DeleteOnClose);
	if (_selectable) {
		_contextMenu->addAction(phrases.copy, [this] {
			copySelectedText();
		}, &st::menuIconCopy);
	}
	if (hasLink) {
		const auto link = _text.link(state.link);
		const auto url = Text::LinkUrl(link);
		_contextMenu->addAction(phrases.copyLink, [=] {
			QGuiApplication::clipboard()->setText(
				url.isEmpty() ? link.text : url);
		}, &st::menuIconLink);
	}
	if (_selectable) {
		_contextMenu->addAction(phrases.selectAll, [this] {
			_selection = Text::Selection{ 0, _text.length() };
			update();
		});
	}
	_contextMenu->popup(e->globalPos());
}

void FlatLabel::activateLink(int index, Qt::MouseButton button) {
	if (index < 0 || index >= _text.linksCount()) {
		return;
	}
	const auto link = _text.link(index);
	if (_linkHandler && _linkHandler(link, button)) {
		return;
	}
	// Without a handler only addresses that are safe to hand to the system are
	// opened: a "file:" or a custom scheme inside somebody's text is not.
	const auto url = QUrl(Text::LinkUrl(link));
	const auto scheme = url.scheme().toLower();
	if (url.isValid()
		&& (scheme == QStringLiteral("https")
			|| scheme == QStringLiteral("http")
			|| scheme == QStringLiteral("mailto")
			|| scheme == QStringLiteral("tel"))) {
		QDesktopServices::openUrl(url);
	}
}

} // namespace Ui
