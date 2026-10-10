// The one image decoder for bytes we did not make ourselves: media from the
// network and every file the user attaches, pastes, drops or opens.
//
// The format is recognised by the first bytes and must be one of JPEG, PNG, GIF
// or WebP. The decoder is never chosen by Qt's own detection: with the image
// plugins that may be installed that would also mean SVG (a document format with
// external references, rendered by QtSvg), PDF, TIFF, ICO, TGA... The declared
// size is checked before anything is allocated for the pixels.
//
// Everything here may be called from any thread. Our own bundled resources
// (":/…") do not go through this.
//
//   const auto image = Data::DecodeImageFile(path, 2560);  // null: not a picture
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QImage>

namespace Data {

// "jpeg", "png", "gif", "webp", or empty for anything else.
[[nodiscard]] QByteArray SniffImageFormat(const QByteArray &head);

// From a file (when `filePath` is not empty) or from `bytes`. `fit`: scale
// down while decoding so the image fits this box (aspect kept); empty: the
// original size. The EXIF orientation is applied. RGB32 or
// ARGB32_Premultiplied; null when the data is not an accepted picture.
[[nodiscard]] QImage DecodeImage(
	const QString &filePath,
	const QByteArray &bytes,
	QSize fit = QSize());

// A file, scaled down so that neither side is longer than `maxSide` (0: as is).
[[nodiscard]] QImage DecodeImageFile(const QString &path, int maxSide = 0);

// Reads the header only: the size as the picture is shown (orientation
// applied), or an empty size when the file is not an accepted picture.
[[nodiscard]] QSize ImageFileDimensions(const QString &path);

// The file can be shown as a picture; anything else is sent as a plain file.
[[nodiscard]] inline bool IsSupportedImageFile(const QString &path) {
	return !ImageFileDimensions(path).isEmpty();
}

} // namespace Data
