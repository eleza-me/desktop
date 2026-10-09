// style::color — a handle to a palette entry (or to a literal color).
//
// The handle stays valid for the whole run; when the palette switches between light and
// dark the data behind it changes, so widgets only need to repaint:
//
//   p.fillRect(rect(), st::bgCanvas);       // as a brush
//   p.setPen(st::textPrimary);              // as a pen
//   const QColor &c = st::brandPrimary->c;  // as a QColor
#pragma once

#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QPen>

namespace style {
namespace internal {

struct ColorData {
	QColor c;
	QPen p;
	QBrush b;

	void set(const QColor &color);
};

struct PaletteRow {
	const char *name;
	quint32 light; // 0xRRGGBBAA
	quint32 dark;
};

struct PaletteDescriptor {
	ColorData *data = nullptr;
	const PaletteRow *rows = nullptr;
	int count = 0;
};

// Defined in the generated styles/palette.cpp.
[[nodiscard]] PaletteDescriptor GeneratedPalette();

} // namespace internal

class color {
public:
	constexpr color() = default;
	explicit constexpr color(const internal::ColorData *data) : _data(data) {
	}

	[[nodiscard]] const internal::ColorData *operator->() const {
		return _data;
	}
	[[nodiscard]] const internal::ColorData *get() const {
		return _data;
	}
	explicit operator bool() const {
		return _data != nullptr;
	}

	operator const QPen &() const {
		return _data->p;
	}
	operator const QBrush &() const {
		return _data->b;
	}

	friend bool operator==(color a, color b) {
		return a._data == b._data;
	}
	friend bool operator!=(color a, color b) {
		return a._data != b._data;
	}

private:
	const internal::ColorData *_data = nullptr;

};

namespace internal {

// A palette-independent color, e.g. `#00000040` written in a .style file.
[[nodiscard]] color LiteralColor(uchar r, uchar g, uchar b, uchar a);

} // namespace internal

// `c` with its alpha multiplied by `opacity` (0..1).
[[nodiscard]] QColor WithOpacity(const QColor &c, double opacity);
[[nodiscard]] inline QColor WithOpacity(color c, double opacity) {
	return WithOpacity(c->c, opacity);
}

// Linear interpolation between two colors, `ratio` = 0 gives `a`, 1 gives `b`.
[[nodiscard]] QColor Interpolate(const QColor &a, const QColor &b, double ratio);
[[nodiscard]] inline QColor Interpolate(color a, color b, double ratio) {
	return Interpolate(a->c, b->c, ratio);
}

} // namespace style
