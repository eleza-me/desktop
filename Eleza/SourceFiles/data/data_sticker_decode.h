// The three kinds of sticker files (ADR-0039) and the decoders of the two
// animated ones. The URL of a sticker has no extension and the wire no format
// field: the kind is told from the first bytes, like the other apps do —
// gzip magic: Lottie (".tgs", gzipped JSON); RIFF/WEBP whose VP8X header has
// the animation flag: an animated WebP; anything else: a still picture (the
// one image decoder, Data::DecodeImage).
//
// Lottie needs rlottie (the CMake option ELEZA_ENABLE_RLOTTIE, on when the
// library is found); without it a Lottie sticker has no picture and the views
// keep its emoji. Animated WebP is read by Qt's webp plugin.
//
// A StickerAnimation is not thread-safe: one thread at a time renders its
// frames (Support::StickerPlayer does it on a worker thread).
//
//   auto animation = Data::OpenStickerAnimation(bytes, QSize(320, 320));
//   if (animation) {
//       auto delay = 0;
//       const auto frame = animation->renderFrame(0, &delay);
//   }
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtGui/QImage>

#include <memory>

namespace Data {

enum class StickerFormat {
	Still,
	AnimatedWebp,
	Lottie,
};

// From the first bytes of the file (at least 21 to see a WebP's flags).
[[nodiscard]] StickerFormat SniffStickerFormat(const QByteArray &head);

// The JSON of a .tgs: the gzip stream inflated, at most `maxSize` bytes
// (more, or anything that is not gzip: an empty result).
[[nodiscard]] QByteArray GunzipSticker(
	const QByteArray &bytes,
	qint64 maxSize = qint64(4) << 20);

// This build plays Lottie stickers.
[[nodiscard]] bool LottieSupported();

class StickerAnimation {
public:
	virtual ~StickerAnimation() = default;

	// Frames in one loop (an animated WebP: known once it was opened).
	[[nodiscard]] virtual int frameCount() const = 0;
	// The size every frame is rendered at (fits the requested box).
	[[nodiscard]] virtual QSize frameSize() const = 0;
	// Frame `index` (0 .. frameCount() - 1), ARGB32_Premultiplied, and how
	// long it is shown. A WebP is read in order: asking for an earlier frame
	// than the last one starts the file again. Null on a broken file.
	[[nodiscard]] virtual QImage renderFrame(int index, int *delayMs) = 0;
};

// Null when the bytes are not an animated sticker this build can play (a
// still picture, Lottie without rlottie, a broken or oversized file).
// `box`: frames are rendered to fit it (device pixels, aspect kept).
[[nodiscard]] std::unique_ptr<StickerAnimation> OpenStickerAnimation(
	const QByteArray &bytes,
	QSize box);

} // namespace Data
