#include "data/data_blurhash.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Data {
namespace {

constexpr auto kPi = 3.14159265358979323846;

constexpr char kAlphabet[] =
	"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

// -1 for a character outside the base-83 alphabet.
[[nodiscard]] int Digit(QChar ch) {
	const auto code = ch.unicode();
	if (code > 127) {
		return -1;
	}
	for (auto i = 0; i != 83; ++i) {
		if (kAlphabet[i] == char(code)) {
			return i;
		}
	}
	return -1;
}

[[nodiscard]] int Decode83(QStringView text) {
	auto value = 0;
	for (const auto ch : text) {
		const auto digit = Digit(ch);
		if (digit < 0) {
			return -1;
		}
		value = value * 83 + digit;
	}
	return value;
}

[[nodiscard]] double SrgbToLinear(int value) {
	const auto x = value / 255.;
	return (x <= 0.04045) ? (x / 12.92) : std::pow((x + 0.055) / 1.055, 2.4);
}

[[nodiscard]] int LinearToSrgb(double value) {
	const auto x = std::clamp(value, 0., 1.);
	const auto result = (x <= 0.0031308)
		? (x * 12.92)
		: (1.055 * std::pow(x, 1 / 2.4) - 0.055);
	return int(std::lround(result * 255.));
}

[[nodiscard]] double SignedSquare(double value) {
	return ((value < 0.) ? -1. : 1.) * value * value;
}

} // namespace

QImage BlurhashImage(const QString &hash, int width, int height) {
	if (hash.size() < 6 || width <= 0 || height <= 0 || width > 256 || height > 256) {
		return QImage();
	}
	const auto view = QStringView(hash);
	const auto sizeFlag = Decode83(view.mid(0, 1));
	const auto quantizedMax = Decode83(view.mid(1, 1));
	if (sizeFlag < 0 || quantizedMax < 0) {
		return QImage();
	}
	const auto countY = sizeFlag / 9 + 1;
	const auto countX = sizeFlag % 9 + 1;
	if (hash.size() != 4 + 2 * countX * countY) {
		return QImage();
	}
	const auto maxValue = (quantizedMax + 1) / 166.;
	auto colors = QVector<std::array<double, 3>>(countX * countY);
	for (auto i = 0; i != countX * countY; ++i) {
		if (i == 0) {
			const auto value = Decode83(view.mid(2, 4));
			if (value < 0) {
				return QImage();
			}
			colors[i] = {
				SrgbToLinear(value >> 16),
				SrgbToLinear((value >> 8) & 255),
				SrgbToLinear(value & 255),
			};
		} else {
			const auto value = Decode83(view.mid(4 + i * 2, 2));
			if (value < 0) {
				return QImage();
			}
			colors[i] = {
				SignedSquare((value / (19 * 19) - 9) / 9.) * maxValue,
				SignedSquare(((value / 19) % 19 - 9) / 9.) * maxValue,
				SignedSquare((value % 19 - 9) / 9.) * maxValue,
			};
		}
	}
	auto result = QImage(width, height, QImage::Format_RGB32);
	auto cosX = QVector<double>(width * countX);
	auto cosY = QVector<double>(height * countY);
	for (auto x = 0; x != width; ++x) {
		for (auto i = 0; i != countX; ++i) {
			cosX[x * countX + i] = std::cos(kPi * x * i / width);
		}
	}
	for (auto y = 0; y != height; ++y) {
		for (auto j = 0; j != countY; ++j) {
			cosY[y * countY + j] = std::cos(kPi * y * j / height);
		}
	}
	for (auto y = 0; y != height; ++y) {
		const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != width; ++x) {
			auto r = 0.;
			auto g = 0.;
			auto b = 0.;
			for (auto j = 0; j != countY; ++j) {
				for (auto i = 0; i != countX; ++i) {
					const auto basis = cosX[x * countX + i] * cosY[y * countY + j];
					const auto &color = colors[i + j * countX];
					r += color[0] * basis;
					g += color[1] * basis;
					b += color[2] * basis;
				}
			}
			line[x] = qRgb(LinearToSrgb(r), LinearToSrgb(g), LinearToSrgb(b));
		}
	}
	return result;
}

} // namespace Data
