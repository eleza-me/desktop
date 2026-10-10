#include "data/data_image_decode.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtGui/QImageReader>

namespace Data {
namespace {

constexpr auto kMaxImageBytes = qint64(64) << 20;
constexpr auto kMaxImageSide = 16384;
constexpr auto kMaxImagePixels = qint64(48) << 20;

// Opens the source and a reader with the format forced; false when the data is
// not an accepted picture. `size` is the declared one (not yet oriented).
[[nodiscard]] bool Prepare(
		const QString &filePath,
		const QByteArray &bytes,
		QFile &file,
		QBuffer &buffer,
		QImageReader &reader,
		QSize &size) {
	auto device = static_cast<QIODevice*>(nullptr);
	if (!filePath.isEmpty()) {
		file.setFileName(filePath);
		if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxImageBytes) {
			return false;
		}
		device = &file;
	} else {
		if (bytes.size() > kMaxImageBytes) {
			return false;
		}
		buffer.setData(bytes);
		buffer.open(QIODevice::ReadOnly);
		device = &buffer;
	}
	const auto format = SniffImageFormat(device->peek(16));
	if (format.isEmpty()) {
		return false;
	}
	// The format is forced: no plugin other than the four is ever asked.
	reader.setAutoDetectImageFormat(false);
	reader.setDecideFormatFromContent(false);
	reader.setDevice(device);
	reader.setFormat(format);
	reader.setAutoTransform(true);
	if (!reader.canRead()) {
		return false;
	}
	// The header is checked before anything is allocated for the pixels.
	size = reader.size();
	return size.isValid()
		&& !size.isEmpty()
		&& size.width() <= kMaxImageSide
		&& size.height() <= kMaxImageSide
		&& qint64(size.width()) * size.height() <= kMaxImagePixels;
}

} // namespace

QByteArray SniffImageFormat(const QByteArray &head) {
	const auto bytes = reinterpret_cast<const uchar*>(head.constData());
	const auto size = head.size();
	if (size >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
		return QByteArrayLiteral("jpeg");
	} else if (size >= 8 && head.startsWith("\x89PNG\r\n\x1a\n")) {
		return QByteArrayLiteral("png");
	} else if (size >= 6 && (head.startsWith("GIF87a") || head.startsWith("GIF89a"))) {
		return QByteArrayLiteral("gif");
	} else if (size >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
		return QByteArrayLiteral("webp");
	}
	return QByteArray();
}

QImage DecodeImage(const QString &filePath, const QByteArray &bytes, QSize fit) {
	auto file = QFile();
	auto buffer = QBuffer();
	auto reader = QImageReader();
	auto declared = QSize();
	if (!Prepare(filePath, bytes, file, buffer, reader, declared)) {
		return QImage();
	}
	if (fit.isValid() && !fit.isEmpty()) {
		const auto size = declared;
		if (size.width() > fit.width() || size.height() > fit.height()) {
			// Scaled by the decoder where the format can (JPEG): cheaper than a full decode.
			auto bounds = fit;
			if (reader.transformation() & QImageIOHandler::TransformationRotate90) {
				bounds.transpose();
			}
			reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
		}
	}
	auto image = reader.read();
	if (image.isNull()) {
		return QImage();
	}
	if (fit.isValid() && !fit.isEmpty()
		&& (image.width() > fit.width() || image.height() > fit.height())) {
		image = image.scaled(fit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}
	const auto pixelFormat = image.hasAlphaChannel()
		? QImage::Format_ARGB32_Premultiplied
		: QImage::Format_RGB32;
	if (image.format() != pixelFormat) {
		image = image.convertToFormat(pixelFormat);
	}
	return image;
}

QImage DecodeImageFile(const QString &path, int maxSide) {
	return path.isEmpty()
		? QImage()
		: DecodeImage(
			path,
			QByteArray(),
			(maxSide > 0) ? QSize(maxSide, maxSide) : QSize());
}

QSize ImageFileDimensions(const QString &path) {
	if (path.isEmpty()) {
		return QSize();
	}
	auto file = QFile();
	auto buffer = QBuffer();
	auto reader = QImageReader();
	auto size = QSize();
	if (!Prepare(path, QByteArray(), file, buffer, reader, size)) {
		return QSize();
	}
	if (reader.transformation() & QImageIOHandler::TransformationRotate90) {
		size.transpose();
	}
	return size;
}

} // namespace Data
