#include "ui/style/style_core_color.h"

#include <QtCore/QHash>

#include <deque>

namespace style {
namespace internal {

void ColorData::set(const QColor &color) {
	c = color;
	p = QPen(color);
	b = QBrush(color);
}

color LiteralColor(uchar r, uchar g, uchar b, uchar a) {
	static auto storage = std::deque<ColorData>();
	static auto index = QHash<quint32, const ColorData*>();

	const auto key = (quint32(r) << 24)
		| (quint32(g) << 16)
		| (quint32(b) << 8)
		| quint32(a);
	const auto i = index.constFind(key);
	if (i != index.cend()) {
		return color(i.value());
	}
	storage.emplace_back();
	storage.back().set(QColor(r, g, b, a));
	index.insert(key, &storage.back());
	return color(&storage.back());
}

} // namespace internal

QColor WithOpacity(const QColor &c, double opacity) {
	auto result = c;
	result.setAlphaF(float(qBound(0., c.alphaF() * opacity, 1.)));
	return result;
}

QColor Interpolate(const QColor &a, const QColor &b, double ratio) {
	const auto r = qBound(0., ratio, 1.);
	const auto mix = [&](int from, int to) {
		return int(qRound(from + (to - from) * r));
	};
	return QColor(
		mix(a.red(), b.red()),
		mix(a.green(), b.green()),
		mix(a.blue(), b.blue()),
		mix(a.alpha(), b.alpha()));
}

} // namespace style
