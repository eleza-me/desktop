// Telegram's emoji pictures on screen: Eleza/Resources/emoji/emoji.pack (embedded,
// uncompressed: read in place, never copied) decoded on a pool thread the first
// time an emoji is drawn at a size, kept in a bounded LRU cache of device-pixel
// pictures; once a batch is ready, the widgets that asked for them are repainted
// (one update per widget for the batch).
//
//   if (!Ui::Emoji::Paint(p, id, rect)) { … }   // false: not drawn (yet), see below
//   Ui::Emoji::PaintOne(p, u"👍🏽", cell, size, st::someFont);  // with the fonts' fallback
//
// Everything degrades to the fonts: no pack (it could not be read), an emoji it
// lacks. Ui::Text::String draws its emoji through this already.
//
// GUI thread only.
#pragma once

#include "ui/emoji/emoji_index.h"

#include <QtCore/QRectF>
#include <QtGui/QFont>

#include <functional>

class QObject;
class QPainter;

namespace Ui::Emoji {

// Whether the pack is there: emoji are drawn as its pictures.
[[nodiscard]] bool Available();

// Paints pack emoji `id` (Find / FindAll) into `rect`, logical pixels of the
// painter. False when nothing was painted: there is no pack, or the picture is
// still being decoded — then a widget painted into directly is repainted where
// it asked once the picture is there; anything else (a picture painted into an
// image) has to listen to OnUpdated(). `faded`: greyed out (inactive icons).
bool Paint(QPainter &p, Id id, const QRectF &rect, bool faded = false);

// One emoji given as text, centred in `rect` at `size` (logical pixels): the
// pack's picture, or the text in `fallback` when the pack does not draw it.
void PaintOne(
	QPainter &p,
	QStringView emoji,
	const QRect &rect,
	int size,
	const QFont &fallback,
	bool faded = false);

// Starts decoding ahead (the panel's rows below the visible ones).
void Preload(Id id, int devicePixels);

// `callback` after newly decoded pictures are ready (at most once per batch),
// until `guard` is destroyed.
void OnUpdated(QObject *guard, std::function<void()> callback);

// Drops the decoded pictures (they are decoded again when drawn).
void ClearCache();

} // namespace Ui::Emoji
