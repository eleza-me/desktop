#include "history/history_themes.h"

#include "base/basic_types.h"
#include "lang_auto.h"
#include "ui/painter.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QHash>

#include <cmath>

namespace History {
namespace {

struct TileKey {
	QString theme;
	int palette = 0;
	int ratio = 0;

	friend bool operator==(const TileKey &, const TileKey &) = default;
};

[[nodiscard]] size_t qHash(const TileKey &key, size_t seed = 0) {
	return ::qHash(key.theme, seed) ^ size_t(key.palette * 31 + key.ratio);
}

[[nodiscard]] const ChatThemePreset *FindPreset(const QString &id) {
	for (const auto &preset : ChatThemePresets()) {
		if (preset.id == id) {
			return &preset;
		}
	}
	return nullptr;
}

[[nodiscard]] QColor Hsl(int hue, double saturation, double lightness, double alpha = 1.) {
	auto result = QColor::fromHslF(
		float(((hue % 360) + 360) % 360) / 360.f,
		float(saturation),
		float(lightness));
	result.setAlphaF(float(alpha));
	return result;
}

// The silhouette of a glyph (an emoji) filled with one colour.
[[nodiscard]] QImage GlyphMask(
		const QString &glyph,
		int size,
		double ratio,
		const QColor &color) {
	auto result = QImage(
		QSize(size, size) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		p.setRenderHint(QPainter::TextAntialiasing);
		auto font = st::normalFont->f;
		font.setPixelSize(int(size * 0.78));
		p.setFont(font);
		p.setPen(Qt::black);
		p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, glyph);
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(QRect(0, 0, size, size), color);
	}
	return result;
}

[[nodiscard]] QPixmap RenderTile(const QString &theme, double ratio) {
	const auto tile = st::historyWallpaperTile;
	const auto icon = st::historyWallpaperIcon;
	const auto colors = ColorsForTheme(theme, false);
	auto image = QImage(
		QSize(tile, tile) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(colors.wallpaper);
	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	// A whisper behind the bubbles, never a competitor of the text.
	p.setOpacity(0.55);
	static const auto kIcons = QStringList{
		u"eli"_s, u"conversation"_s, u"like"_s, u"music"_s, u"listener"_s,
		u"bookmark"_s, u"poll"_s, u"message"_s, u"anonymous"_s,
	};
	const auto preset = FindPreset(theme);
	const auto glyph = (preset && !preset->emoji.isEmpty())
		? GlyphMask(preset->emoji, icon, ratio, colors.pattern)
		: QImage();
	for (auto index = 0; index != 9; ++index) {
		const auto column = index % 3;
		const auto row = index / 3;
		const auto x = column * 64 + (row % 2) * 24 + 12;
		const auto y = row * 64 + 12;
		const auto degrees = ((index * 37) % 50) - 25;
		// The tile repeats: what hangs over the right edge comes back on the left.
		for (const auto shift : { 0, -tile }) {
			p.save();
			p.translate(x + shift + icon / 2., y + icon / 2.);
			p.rotate(degrees);
			p.translate(-icon / 2., -icon / 2.);
			if (!glyph.isNull()) {
				p.drawImage(QPointF(0., 0.), glyph);
			} else {
				p.drawPixmap(
					QPointF(0., 0.),
					style::IconPixmap(kIcons[index], colors.pattern, icon, ratio));
			}
			p.restore();
		}
	}
	p.end();
	return QPixmap::fromImage(std::move(image));
}

} // namespace

const QVector<ChatThemePreset> &ChatThemePresets() {
	static const auto result = QVector<ChatThemePreset>{
		{ QString(), QString(), 150 },
		{ u"love"_s, QString::fromUtf8("\xE2\x9D\xA4"), 345 },
		{ u"dogs"_s, QString::fromUtf8("\xF0\x9F\x90\xB6"), 28 },
		{ u"cats"_s, QString::fromUtf8("\xF0\x9F\x90\xB1"), 42 },
		{ u"football"_s, QString::fromUtf8("\xE2\x9A\xBD"), 120 },
		{ u"gaming"_s, QString::fromUtf8("\xF0\x9F\x8E\xAE"), 265 },
		{ u"space"_s, QString::fromUtf8("\xF0\x9F\x9A\x80"), 235 },
		{ u"ocean"_s, QString::fromUtf8("\xF0\x9F\x8C\x8A"), 200 },
		{ u"spring"_s, QString::fromUtf8("\xF0\x9F\x8C\xB8"), 320 },
		{ u"coffee"_s, QString::fromUtf8("\xE2\x98\x95"), 22 },
		{ u"music"_s, QString::fromUtf8("\xF0\x9F\x8E\xB5"), 285 },
		{ u"snow"_s, QString::fromUtf8("\xE2\x9D\x84"), 210 },
		{ u"candy"_s, QString::fromUtf8("\xF0\x9F\x8D\xAC"), 330 },
		{ u"night"_s, QString::fromUtf8("\xF0\x9F\x8C\x99"), 250 },
	};
	return result;
}

bool IsKnownChatTheme(const QString &id) {
	return FindPreset(id) != nullptr;
}

QString ChatThemeName(const QString &id) {
	if (id == u"love") {
		return tr::lng_history_theme_love(tr::now);
	} else if (id == u"dogs") {
		return tr::lng_history_theme_dogs(tr::now);
	} else if (id == u"cats") {
		return tr::lng_history_theme_cats(tr::now);
	} else if (id == u"football") {
		return tr::lng_history_theme_football(tr::now);
	} else if (id == u"gaming") {
		return tr::lng_history_theme_gaming(tr::now);
	} else if (id == u"space") {
		return tr::lng_history_theme_space(tr::now);
	} else if (id == u"ocean") {
		return tr::lng_history_theme_ocean(tr::now);
	} else if (id == u"spring") {
		return tr::lng_history_theme_spring(tr::now);
	} else if (id == u"coffee") {
		return tr::lng_history_theme_coffee(tr::now);
	} else if (id == u"music") {
		return tr::lng_history_theme_music(tr::now);
	} else if (id == u"snow") {
		return tr::lng_history_theme_snow(tr::now);
	} else if (id == u"candy") {
		return tr::lng_history_theme_candy(tr::now);
	} else if (id == u"night") {
		return tr::lng_history_theme_night(tr::now);
	}
	return tr::lng_history_theme_default(tr::now);
}

QColor ServiceBg() {
	return style::IsDark() ? QColor(255, 255, 255, 28) : st::historyServiceBg->c;
}

ChatColors ColorsForTheme(const QString &id, bool anonymous) {
	auto result = ChatColors();
	result.wallpaper = st::chatWallpaper->c;
	result.pattern = st::chatPattern->c;
	result.bubbleIn = st::chatBubbleIn->c;
	result.bubbleOut = st::chatBubbleOut->c;
	result.textIn = st::historyTextFg->c;
	result.textOut = st::historyTextFg->c;
	result.metaIn = st::historyMetaFgIn->c;
	result.metaOut = st::historyMetaFgOut->c;
	result.tintIn = st::chatTintIn->c;
	result.tintOut = st::chatTintOut->c;
	result.accentIn = st::brandPrimary->c;
	result.accentOut = st::brandPrimary->c;
	result.shadow = st::historyBubbleShadow->c;
	const auto preset = id.isEmpty() ? nullptr : FindPreset(id);
	if (preset) {
		const auto hue = preset->hue;
		const auto dark = style::IsDark();
		// The «night» preset is dark in both palettes of the wallpaper only;
		// bubbles keep following the palette so text stays readable.
		if (dark) {
			result.wallpaper = Hsl(hue, 0.34, 0.07);
			result.pattern = Hsl(hue, 0.36, 0.13);
			result.bubbleOut = Hsl(hue, 0.46, 0.24);
			result.metaOut = Hsl(hue, 0.62, 0.78);
			result.tintOut = Hsl(hue, 0.62, 0.78, 0.14);
			result.tintIn = Hsl(hue, 0.62, 0.78, 0.11);
			result.accentIn = Hsl(hue, 0.58, 0.66);
			result.accentOut = Hsl(hue, 0.62, 0.78);
		} else {
			result.wallpaper = Hsl(hue, 0.36, 0.88);
			result.pattern = Hsl(hue, 0.34, 0.80);
			result.bubbleOut = Hsl(hue, 0.74, 0.92);
			result.metaOut = Hsl(hue, 0.62, 0.34);
			result.tintOut = Hsl(hue, 0.70, 0.40, 0.11);
			result.tintIn = Hsl(hue, 0.70, 0.40, 0.08);
			result.accentIn = Hsl(hue, 0.66, 0.42);
			result.accentOut = Hsl(hue, 0.66, 0.38);
		}
	}
	if (anonymous) {
		result.bubbleIn = st::anonSurface->c;
		result.textIn = st::historyAnonTextFg->c;
		result.metaIn = st::historyMetaFgAnon->c;
		result.accentIn = st::anonAccent->c;
	}
	return result;
}

void PaintWallpaper(
		QPainter &p,
		const QRect &rect,
		QPoint origin,
		const QString &theme) {
	static auto cache = QHash<TileKey, QPixmap>();
	const auto ratio = p.device()->devicePixelRatioF();
	const auto key = TileKey{
		theme,
		style::PaletteVersion(),
		int(std::lround(ratio * 100)),
	};
	auto i = cache.find(key);
	if (i == cache.end()) {
		if (cache.size() > 12) {
			cache.clear();
		}
		i = cache.insert(key, RenderTile(theme, ratio));
	}
	p.drawTiledPixmap(rect, *i, rect.topLeft() - origin);
}

} // namespace History
