// Ui::FlatLabel — a text label on top of Ui::Text::String.
//
//   const auto label = Ui::CreateChild<Ui::FlatLabel>(this, text, st::defaultFlatLabel);
//   label->resizeToWidth(available);            // wraps, height follows
//   label->setMarkedText({ text, entities });   // links, spoilers, bold…
//   label->setSelectable(true);                 // mouse selection, Ctrl+C, context menu
//   label->setLinkHandler([=](const Ui::Text::Link &link, Qt::MouseButton) { ...; return true; });
//
// Styles: st::defaultFlatLabel (body), st::boxLabel, st::captionLabel, st::titleLabel…
// Links that the handler does not take are opened with QDesktopServices.
#pragma once

#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "styles/style_widgets.h"

#include <functional>
#include <optional>

namespace Ui {

class PopupMenu;

class FlatLabel : public RpWidget {
public:
	explicit FlatLabel(
		QWidget *parent,
		const style::FlatLabel &st = st::defaultFlatLabel);
	FlatLabel(
		QWidget *parent,
		const QString &text,
		const style::FlatLabel &st = st::defaultFlatLabel);

	void setText(const QString &text);
	void setMarkedText(const Text::TextWithEntities &text);
	[[nodiscard]] const Text::String &text() const {
		return _text;
	}

	void setSelectable(bool selectable);
	void setTextColorOverride(std::optional<QColor> color);
	// 0 = unlimited (overrides the style's maxLines).
	void setMaxLines(int lines);
	void setAlign(style::align align);
	// Paragraphs align by their own direction instead of the interface's.
	void setAutoDirection(bool autoDirection);
	// Return true if the click was handled.
	void setLinkHandler(
		std::function<bool(const Text::Link&, Qt::MouseButton)> handler);

	[[nodiscard]] int naturalWidth() const override;
	// The label keeps following its text: min(natural width, maxWidth).
	void resizeToNaturalWidth(int maxWidth) override;
	[[nodiscard]] Text::Selection selection() const {
		return _selection;
	}
	[[nodiscard]] QString selectedText() const;
	void copySelectedText() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] Text::PaintContext geometry() const;
	[[nodiscard]] int maxLines() const;
	void refreshSize();
	void updateHover(QPoint position);
	void refreshCursor();
	void activateLink(int index, Qt::MouseButton button);

	enum class WidthMode {
		Natural, // nobody set a width: follows the text
		Fixed, // resizeToWidth(w): wraps at w
		NaturalMax, // resizeToNaturalWidth(w)
	};

	const style::FlatLabel &_st;
	Text::String _text;
	WidthMode _widthMode = WidthMode::Natural;
	int _widthValue = 0;
	bool _inOwnResize = false;
	std::optional<QColor> _colorOverride;
	std::optional<style::align> _alignOverride;
	int _maxLinesOverride = -1;
	bool _autoDirection = false;
	bool _selectable = false;
	std::function<bool(const Text::Link&, Qt::MouseButton)> _linkHandler;

	Text::Selection _selection;
	int _dragStart = -1;
	bool _dragging = false;
	Text::SelectType _selectType = Text::SelectType::Letters;
	Text::Selection _dragBase;
	int _hoveredLink = -1;
	int _pressedLink = -1;
	bool _overSpoiler = false;
	bool _pressedSpoiler = false;
	QPointer<PopupMenu> _contextMenu;

};

} // namespace Ui
