// Ui::Text::String — a block of rich text with our own line layout.
//
//   Ui::Text::String _text;
//   _text.setText(st::defaultTextStyle, tr::lng_some(tr::now));
//   _text.setMarkedText(st::messageTextStyle, { text, entities });  // UTF-16 offsets
//
//   const auto h = _text.countHeight(width);
//   p.setPen(st::textPrimary);
//   _text.draw(p, { .position = { x, y }, .availableWidth = width });
//   _text.drawLeftElided(p, x, y, width, outerWidth);   // one line, RTL-aware x
//
// Features: word wrap with multi-line elision, bidi (the base direction of every
// paragraph is detected from its content), links with hover / click hit-testing
// (getState), spoilers hidden behind "dust" until revealed, selection painting.
// Emoji are left to the font fallback.
//
// The object caches its layout for the last used width, so keep one String per
// painted item instead of recreating it in paintEvent.
#pragma once

#include "ui/text/text_entity.h"
#include "ui/style/style_core.h"
#include "styles/style_basic.h"

#include <QtCore/QPoint>
#include <QtCore/QRect>

#include <memory>
#include <vector>

class QPainter;
class QTextLayout;

namespace Ui::Text {

struct PaintContext {
	// Top-left corner of the text box, physical painter coordinates.
	QPoint position = {};
	// Width of the text box. Lines wrap at it.
	int availableWidth = 0;
	// al_left = the start side, al_right = the end side, al_center.
	style::align align = style::al_left;
	// Maximum number of lines, the last one gets "…". 0 = unlimited.
	int elisionLines = 0;
	// false: the start side is the interface's (labels); true: every paragraph
	// aligns by its own direction (messages, posts).
	bool autoDirection = false;
	// Painted with the style's selectBg.
	Selection selection = {};
	// Lines completely outside are skipped. Null = paint everything.
	QRect clip = {};
	// Index of the link to underline (hover), -1 = none.
	int hoveredLink = -1;
};

struct StateResult {
	int link = -1; // index for link(), when the point is over a link
	bool uponSymbol = false; // the point is over a glyph
	bool spoiler = false; // the point is over a hidden spoiler
	int symbol = 0; // offset of the character under the point
	int cursor = 0; // nearest caret position, for selections
};

enum class SelectType {
	Letters,
	Words,
	Paragraphs,
};

class String final {
public:
	String();
	String(const style::TextStyle &st, const QString &text);
	String(const style::TextStyle &st, const TextWithEntities &text);
	String(String &&other) noexcept;
	String &operator=(String &&other) noexcept;
	~String();

	void setText(const style::TextStyle &st, const QString &text);
	void setMarkedText(const style::TextStyle &st, const TextWithEntities &text);
	void clear();

	[[nodiscard]] bool isEmpty() const;
	[[nodiscard]] int length() const;
	[[nodiscard]] const QString &toString() const;
	[[nodiscard]] QString toString(Selection selection) const;
	[[nodiscard]] TextWithEntities toTextWithEntities() const;

	// Width of the longest paragraph when nothing wraps.
	[[nodiscard]] int maxWidth() const;
	// Height of one line.
	[[nodiscard]] int minHeight() const;
	[[nodiscard]] int lineHeight() const {
		return minHeight();
	}
	[[nodiscard]] int countHeight(int width, int elisionLines = 0) const;
	[[nodiscard]] int countLines(int width) const;
	// Width of the widest line after wrapping at `width` (to shrink a bubble).
	[[nodiscard]] int countMaxLineWidth(int width, int elisionLines = 0) const;
	// The last line after wrapping at `width`: its natural width and the
	// direction of its paragraph (to put a timestamp at the end of it).
	struct LastLine {
		int width = 0;
		bool rtl = false;
	};
	[[nodiscard]] LastLine countLastLine(int width) const;

	[[nodiscard]] bool hasLinks() const;
	[[nodiscard]] int linksCount() const;
	[[nodiscard]] const Link &link(int index) const;

	[[nodiscard]] bool hasSpoilers() const;
	[[nodiscard]] bool spoilersRevealed() const;
	void setSpoilersRevealed(bool revealed);

	// The text color is the painter's pen.
	void draw(QPainter &p, const PaintContext &context) const;
	// Shortcuts. `left` is a physical x.
	void draw(
		QPainter &p,
		int left,
		int top,
		int width,
		style::align align = style::al_left) const;
	void drawElided(
		QPainter &p,
		int left,
		int top,
		int width,
		int lines = 1,
		style::align align = style::al_left) const;
	// `x` from the leading side of `outerw` (mirrored in RTL).
	void drawLeft(
		QPainter &p,
		int x,
		int y,
		int width,
		int outerw,
		style::align align = style::al_left) const;
	void drawLeftElided(
		QPainter &p,
		int x,
		int y,
		int width,
		int outerw,
		int lines = 1,
		style::align align = style::al_left) const;

	// Hit test. `point` is in the same coordinates as `geometry.position`.
	[[nodiscard]] StateResult getState(
		QPoint point,
		const PaintContext &geometry) const;
	// Where a range of the text is drawn for `geometry`: one rectangle per
	// line (a line high), relative to geometry.position. To mark a part of
	// a text (a coloured pill under a word).
	[[nodiscard]] std::vector<QRectF> rangeRects(
		Selection range,
		const PaintContext &geometry) const;

	// Expands a selection to whole words / paragraphs.
	[[nodiscard]] Selection adjustSelection(
		Selection selection,
		SelectType type) const;

private:
	struct Private;

	std::unique_ptr<Private> _d;

};

// Whether a string of text is non-empty after trimming whitespace.
[[nodiscard]] bool HasVisibleText(const QString &text);

} // namespace Ui::Text
