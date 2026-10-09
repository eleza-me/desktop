// Chat themes: one look per 1:1 chat, shared by both members. The server keeps
// only the preset's id (ChatInfo::theme); the colours live here. The default
// look ("") is the palette's own.
#pragma once

#include "ui/style/style_core.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QColor>
#include <QtGui/QPixmap>

class QPainter;

namespace History {

struct ChatColors {
	QColor wallpaper;
	QColor pattern;
	QColor bubbleIn;
	QColor bubbleOut;
	QColor textIn;
	QColor textOut;
	QColor metaIn;
	QColor metaOut;
	QColor tintIn;   // reply headers, reactions, previews inside incoming bubbles
	QColor tintOut;
	QColor accentIn; // bars, names, links' companions
	QColor accentOut;
	QColor shadow;
};

struct ChatThemePreset {
	QString id;
	QString emoji; // what the wallpaper's pattern is made of
	int hue = 0;   // 0..359
};

// "" first, then the presets the server accepts.
[[nodiscard]] const QVector<ChatThemePreset> &ChatThemePresets();
[[nodiscard]] bool IsKnownChatTheme(const QString &id);
[[nodiscard]] QString ChatThemeName(const QString &id);

// The colours of a theme in the current palette (light / dark). `anonymous`:
// incoming bubbles of a masked counterpart use the «anonymous» surface.
[[nodiscard]] ChatColors ColorsForTheme(const QString &id, bool anonymous);

// The fill of day pills, service lines and the unread divider: dark glass
// over a light wallpaper, light glass over a dark one.
[[nodiscard]] QColor ServiceBg();

// Fills `rect` with the wallpaper; the pattern is anchored at `origin` so it
// stays still while the content scrolls.
void PaintWallpaper(
	QPainter &p,
	const QRect &rect,
	QPoint origin,
	const QString &theme);

} // namespace History
