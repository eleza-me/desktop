// A small QR code encoder for the «share profile» box: byte mode, error
// correction level M, versions 1–6 (up to 106 bytes — a profile link is at
// most 43). Written from the ISO/IEC 18004 description; no third-party code.
//
//   const auto code = Profile::Qr::Encode(link.toUtf8());
//   if (code.valid()) { … code.size, code.dark(x, y) … }
//   const auto image = Profile::Qr::Render(code, 6, 4);   // px per module, quiet zone
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QVector>
#include <QtGui/QColor>
#include <QtGui/QImage>

namespace Profile::Qr {

struct Code {
	int version = 0;
	int size = 0; // modules per side
	int mask = 0;
	QVector<uchar> modules; // size * size, 1 = dark

	[[nodiscard]] bool valid() const {
		return size > 0;
	}
	[[nodiscard]] bool dark(int x, int y) const {
		return modules[y * size + x] != 0;
	}
};

inline constexpr auto kMaxBytes = 106;

// An invalid code when `data` is empty or longer than kMaxBytes.
[[nodiscard]] Code Encode(const QByteArray &data);

// Exposed for the tests.
[[nodiscard]] QByteArray ReedSolomon(const QByteArray &data, int ecLength);
[[nodiscard]] int FormatBits(int mask);

[[nodiscard]] QImage Render(
	const Code &code,
	int modulePixels,
	int quietModules,
	QColor dark = Qt::black,
	QColor light = Qt::white);

} // namespace Profile::Qr
