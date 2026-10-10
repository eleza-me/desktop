// style::font — a shared, immutable font with cached metrics.
//
//   p.setFont(st::normalFont);                      // implicit QFont
//   const auto w = st::normalFont->width(text);
//   const auto line = st::normalFont->height;       // ascent + descent
//   p.drawText(x, y + font->ascent, text);
//   st::normalFont->semibold()                      // derived variant
#pragma once

#include <QtGui/QFont>
#include <QtGui/QFontMetrics>

namespace style {
namespace internal {

enum FontFlag : uint {
	FontBold = 0x01,
	FontSemibold = 0x02,
	FontMedium = 0x04,
	FontItalic = 0x08,
	FontUnderline = 0x10,
	FontStrikeOut = 0x20,
	FontMonospace = 0x40,
};

class FontData;

void StartFonts();

} // namespace internal

class font {
public:
	font() = default;
	// `family` empty = the text family (Vazirmatn). "display" = Markazi Text.
	font(int size, uint flags, const QString &family);

	[[nodiscard]] const internal::FontData *operator->() const {
		return _data;
	}
	[[nodiscard]] const internal::FontData *get() const {
		return _data;
	}
	explicit operator bool() const {
		return _data != nullptr;
	}
	operator const QFont &() const;

	friend bool operator==(const font &a, const font &b) {
		return a._data == b._data;
	}
	friend bool operator!=(const font &a, const font &b) {
		return a._data != b._data;
	}

private:
	explicit font(const internal::FontData *data) : _data(data) {
	}

	const internal::FontData *_data = nullptr;

	friend class internal::FontData;

};

namespace internal {

class FontData {
public:
	QFont f;
	QFontMetrics m;
	int height = 0; // line height (ascent + descent)
	int ascent = 0;
	int descent = 0;
	int spacew = 0;
	int elidew = 0;

	[[nodiscard]] int width(const QString &text) const;
	[[nodiscard]] int width(QChar ch) const;
	[[nodiscard]] QString elided(
		const QString &text,
		int width,
		Qt::TextElideMode mode = Qt::ElideRight) const;

	[[nodiscard]] int size() const {
		return _size;
	}
	[[nodiscard]] uint flags() const {
		return _flags;
	}
	[[nodiscard]] const QString &family() const {
		return _family;
	}

	// Derived fonts (same size / family).
	[[nodiscard]] font regular() const;
	[[nodiscard]] font medium() const;
	[[nodiscard]] font semibold() const;
	[[nodiscard]] font bold() const;
	[[nodiscard]] font italic(bool set = true) const;
	[[nodiscard]] font underline(bool set = true) const;
	[[nodiscard]] font strikeout(bool set = true) const;
	[[nodiscard]] font monospace() const;
	[[nodiscard]] font withSize(int size) const;

private:
	FontData(int size, uint flags, const QString &family);

	[[nodiscard]] font withFlags(uint flags) const;

	int _size = 0;
	uint _flags = 0;
	QString _family;

	friend class ::style::font;

};

} // namespace internal

inline font::operator const QFont &() const {
	return _data->f;
}

// Families resolved at startup.
[[nodiscard]] QString TextFontFamily(); // "Vazirmatn"
[[nodiscard]] QString DisplayFontFamily(); // "Markazi Text"
[[nodiscard]] QString MonospaceFontFamily();
[[nodiscard]] QString EmojiFontFamily(); // empty when the system has none

} // namespace style
