#include "ui/style/style_core_icon.h"

#include "ui/style/style_core_palette.h"
#include "ui/style/style_core_types.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>

#include <deque>

static void InitIconsResource() {
	Q_INIT_RESOURCE(icons);
}

namespace style {
namespace {

enum class Mirror {
	Never,
	InLtr,
	InRtl,
};

bool IconsStarted = false;

[[nodiscard]] QHash<QString, QByteArray> &Sources() {
	static auto result = QHash<QString, QByteArray>();
	return result;
}

[[nodiscard]] const QByteArray &Source(const QString &name) {
	auto &sources = Sources();
	const auto i = sources.constFind(name);
	if (i != sources.cend()) {
		return i.value();
	}
	auto file = QFile(QStringLiteral(":/icons/") + name + QStringLiteral(".svg"));
	auto content = QByteArray();
	if (file.open(QIODevice::ReadOnly)) {
		content = file.readAll();
	} else {
		qWarning("Icons: ':/icons/%s.svg' not found.", qPrintable(name));
	}
	return *sources.insert(name, content);
}

[[nodiscard]] QByteArray HexRgb(const QColor &color) {
	return color.name(QColor::HexRgb).toLatin1();
}

[[nodiscard]] QColor Named(const char *name, const QColor &fallback) {
	const auto found = FindPaletteColor(QString::fromLatin1(name));
	return found ? (*found)->c : fallback;
}

[[nodiscard]] QPixmap Render(
		const QString &name,
		const QColor &tint,
		int size,
		double ratio,
		bool mirrored) {
	const auto pixels = qMax(int(qRound(size * ratio)), 1);
	auto image = QImage(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto svg = Source(name);
	if (!svg.isEmpty()) {
		// Template colors of the design's icons, see codegen/scripts/gen_icons.py.
		svg.replace("#161A17", "#\x01\x01\x01");
		svg.replace("#018A54", HexRgb(Named("brandPrimary", QColor(1, 138, 84))));
		svg.replace("#4D524B", HexRgb(Named("textSecondary", QColor(77, 82, 75))));
		svg.replace("#\x01\x01\x01", HexRgb(tint));
		auto renderer = QSvgRenderer(svg);
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.setOpacity(tint.alphaF());
		if (mirrored) {
			p.translate(pixels, 0);
			p.scale(-1., 1.);
		}
		renderer.render(&p, QRectF(0, 0, pixels, pixels));
	}
	auto result = QPixmap::fromImage(std::move(image));
	result.setDevicePixelRatio(ratio);
	return result;
}

} // namespace

namespace internal {

class IconData {
public:
	IconData(const QString &name, color tint, int size)
	: _tint(tint)
	, _size(size) {
		const auto colon = name.indexOf(QLatin1Char(':'));
		_name = (colon < 0) ? name : name.left(colon);
		const auto modifier = (colon < 0) ? QString() : name.mid(colon + 1);
		if (modifier == QStringLiteral("mirror-ltr")) {
			_mirror = Mirror::InLtr;
		} else if (modifier == QStringLiteral("mirror-rtl")) {
			_mirror = Mirror::InRtl;
		} else if (!modifier.isEmpty()) {
			qWarning("Icons: unknown modifier in '%s'.", qPrintable(name));
		}
	}

	[[nodiscard]] int size() const {
		return _size;
	}

	[[nodiscard]] const QPixmap &pixmap(double ratio, const QColor *over) const {
		if (_version != PaletteVersion()) {
			_version = PaletteVersion();
			_cache.clear();
		}
		const auto mirrored = (_mirror == Mirror::InLtr && !RightToLeft())
			|| (_mirror == Mirror::InRtl && RightToLeft());
		const auto &tint = over ? *over : _tint->c;
		const auto key = (quint64(tint.rgba()) << 32)
			| (quint64(qRound(ratio * 64.)) << 1)
			| (mirrored ? 1U : 0U);
		const auto i = _cache.constFind(key);
		if (i != _cache.cend()) {
			return i.value();
		}
		if (_cache.size() > 16) {
			_cache.clear();
		}
		return *_cache.insert(key, Render(_name, tint, _size, ratio, mirrored));
	}

private:
	QString _name;
	color _tint;
	int _size = 0;
	Mirror _mirror = Mirror::Never;
	mutable QHash<quint64, QPixmap> _cache;
	mutable int _version = 0;

};

void StartIcons() {
	if (IconsStarted) {
		return;
	}
	IconsStarted = true;
	InitIconsResource();
}

} // namespace internal

icon::icon(const QString &name, color tint, int size) {
	static auto storage = std::deque<internal::IconData>();
	internal::StartIcons();
	storage.emplace_back(name, tint, size);
	_data = &storage.back();
}

int icon::width() const {
	return _data ? _data->size() : 0;
}

int icon::height() const {
	return _data ? _data->size() : 0;
}

QSize icon::size() const {
	return QSize(width(), height());
}

void icon::paint(QPainter &p, int x, int y, int outerw) const {
	if (!_data) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	const auto left = RightToLeft() ? (outerw - x - _data->size()) : x;
	p.drawPixmap(QPoint(left, y), _data->pixmap(ratio, nullptr));
}

void icon::paint(QPainter &p, const QPoint &pos, int outerw) const {
	paint(p, pos.x(), pos.y(), outerw);
}

void icon::paint(
		QPainter &p,
		int x,
		int y,
		int outerw,
		const QColor &colorOverride) const {
	if (!_data) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	const auto left = RightToLeft() ? (outerw - x - _data->size()) : x;
	p.drawPixmap(QPoint(left, y), _data->pixmap(ratio, &colorOverride));
}

void icon::paintInCenter(QPainter &p, const QRect &outer) const {
	if (!_data) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	p.drawPixmap(
		QPoint(
			outer.x() + (outer.width() - _data->size()) / 2,
			outer.y() + (outer.height() - _data->size()) / 2),
		_data->pixmap(ratio, nullptr));
}

void icon::paintInCenter(
		QPainter &p,
		const QRect &outer,
		const QColor &colorOverride) const {
	if (!_data) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	p.drawPixmap(
		QPoint(
			outer.x() + (outer.width() - _data->size()) / 2,
			outer.y() + (outer.height() - _data->size()) / 2),
		_data->pixmap(ratio, &colorOverride));
}

QPixmap icon::pixmap(double devicePixelRatio) const {
	return _data ? _data->pixmap(devicePixelRatio, nullptr) : QPixmap();
}

QPixmap icon::pixmap(
		double devicePixelRatio,
		const QColor &colorOverride) const {
	return _data ? _data->pixmap(devicePixelRatio, &colorOverride) : QPixmap();
}

QPixmap IconPixmap(
		const QString &name,
		const QColor &tint,
		int size,
		double devicePixelRatio) {
	internal::StartIcons();
	return Render(name, tint, size, devicePixelRatio, false);
}

} // namespace style
