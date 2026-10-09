#include "profile/profile_qr.h"

#include <QtGui/QPainter>

#include <array>
#include <cstdlib>
#include <limits>

namespace Profile::Qr {
namespace {

struct VersionInfo {
	int dataCodewords = 0;
	int blocks = 0;
	int ecPerBlock = 0;
	int alignment = 0; // center of the only alignment pattern, 0 = none
};

// Error correction level M.
constexpr VersionInfo kVersions[] = {
	{ 16, 1, 10, 0 },
	{ 28, 1, 16, 18 },
	{ 44, 1, 26, 22 },
	{ 64, 2, 18, 26 },
	{ 86, 2, 24, 30 },
	{ 108, 4, 16, 34 },
};
constexpr auto kVersionsCount = int(sizeof(kVersions) / sizeof(kVersions[0]));

struct Field {
	std::array<uchar, 256> exp = {};
	std::array<uchar, 256> log = {};
};

[[nodiscard]] const Field &Gf() {
	static const auto result = [] {
		auto field = Field();
		auto value = 1;
		for (auto i = 0; i != 255; ++i) {
			field.exp[i] = uchar(value);
			field.log[value] = uchar(i);
			value <<= 1;
			if (value & 0x100) {
				value ^= 0x11D;
			}
		}
		field.exp[255] = field.exp[0];
		return field;
	}();
	return result;
}

[[nodiscard]] uchar Multiply(uchar a, uchar b) {
	if (!a || !b) {
		return 0;
	}
	const auto &field = Gf();
	return field.exp[(int(field.log[a]) + int(field.log[b])) % 255];
}

class Matrix final {
public:
	explicit Matrix(int size)
	: _size(size)
	, _modules(size * size, uchar(0))
	, _function(size * size, uchar(0)) {
	}

	[[nodiscard]] int size() const {
		return _size;
	}
	[[nodiscard]] bool get(int x, int y) const {
		return _modules[y * _size + x] != 0;
	}
	[[nodiscard]] bool isFunction(int x, int y) const {
		return _function[y * _size + x] != 0;
	}
	void set(int x, int y, bool dark) {
		_modules[y * _size + x] = dark ? 1 : 0;
	}
	void setFunction(int x, int y, bool dark) {
		if (x < 0 || y < 0 || x >= _size || y >= _size) {
			return;
		}
		_modules[y * _size + x] = dark ? 1 : 0;
		_function[y * _size + x] = 1;
	}
	[[nodiscard]] const QVector<uchar> &modules() const {
		return _modules;
	}

private:
	int _size = 0;
	QVector<uchar> _modules;
	QVector<uchar> _function;

};

void DrawFinder(Matrix &matrix, int centerX, int centerY) {
	for (auto dy = -4; dy <= 4; ++dy) {
		for (auto dx = -4; dx <= 4; ++dx) {
			const auto distance = std::max(std::abs(dx), std::abs(dy));
			matrix.setFunction(
				centerX + dx,
				centerY + dy,
				(distance != 2) && (distance != 4));
		}
	}
}

void DrawAlignment(Matrix &matrix, int centerX, int centerY) {
	for (auto dy = -2; dy <= 2; ++dy) {
		for (auto dx = -2; dx <= 2; ++dx) {
			matrix.setFunction(
				centerX + dx,
				centerY + dy,
				std::max(std::abs(dx), std::abs(dy)) != 1);
		}
	}
}

void DrawFormat(Matrix &matrix, int mask) {
	const auto bits = FormatBits(mask);
	const auto size = matrix.size();
	const auto bit = [&](int index) {
		return ((bits >> index) & 1) != 0;
	};
	for (auto i = 0; i <= 5; ++i) {
		matrix.setFunction(8, i, bit(i));
	}
	matrix.setFunction(8, 7, bit(6));
	matrix.setFunction(8, 8, bit(7));
	matrix.setFunction(7, 8, bit(8));
	for (auto i = 9; i < 15; ++i) {
		matrix.setFunction(14 - i, 8, bit(i));
	}
	for (auto i = 0; i < 8; ++i) {
		matrix.setFunction(size - 1 - i, 8, bit(i));
	}
	for (auto i = 8; i < 15; ++i) {
		matrix.setFunction(8, size - 15 + i, bit(i));
	}
	matrix.setFunction(8, size - 8, true);
}

void DrawFunctionPatterns(Matrix &matrix, const VersionInfo &info) {
	const auto size = matrix.size();
	for (auto i = 0; i != size; ++i) {
		matrix.setFunction(6, i, (i % 2) == 0);
		matrix.setFunction(i, 6, (i % 2) == 0);
	}
	DrawFinder(matrix, 3, 3);
	DrawFinder(matrix, size - 4, 3);
	DrawFinder(matrix, 3, size - 4);
	if (info.alignment) {
		DrawAlignment(matrix, info.alignment, info.alignment);
	}
	// Reserves the format areas; the real bits come with the chosen mask.
	DrawFormat(matrix, 0);
}

[[nodiscard]] bool MaskBit(int mask, int x, int y) {
	switch (mask) {
	case 0: return ((x + y) % 2) == 0;
	case 1: return (y % 2) == 0;
	case 2: return (x % 3) == 0;
	case 3: return ((x + y) % 3) == 0;
	case 4: return ((x / 3 + y / 2) % 2) == 0;
	case 5: return ((x * y) % 2 + (x * y) % 3) == 0;
	case 6: return (((x * y) % 2 + (x * y) % 3) % 2) == 0;
	case 7: return (((x + y) % 2 + (x * y) % 3) % 2) == 0;
	}
	return false;
}

void ApplyMask(Matrix &matrix, int mask) {
	const auto size = matrix.size();
	for (auto y = 0; y != size; ++y) {
		for (auto x = 0; x != size; ++x) {
			if (!matrix.isFunction(x, y) && MaskBit(mask, x, y)) {
				matrix.set(x, y, !matrix.get(x, y));
			}
		}
	}
}

void PlaceCodewords(Matrix &matrix, const QByteArray &codewords) {
	const auto size = matrix.size();
	const auto total = int(codewords.size()) * 8;
	auto index = 0;
	for (auto right = size - 1; right >= 1; right -= 2) {
		if (right == 6) {
			right = 5;
		}
		const auto upward = ((right + 1) & 2) == 0;
		for (auto vertical = 0; vertical != size; ++vertical) {
			for (auto j = 0; j != 2; ++j) {
				const auto x = right - j;
				const auto y = upward ? (size - 1 - vertical) : vertical;
				if (matrix.isFunction(x, y) || index >= total) {
					continue;
				}
				const auto byte = uchar(codewords[index >> 3]);
				matrix.set(x, y, ((byte >> (7 - (index & 7))) & 1) != 0);
				++index;
			}
		}
	}
}

// The four penalty rules of the standard.
[[nodiscard]] int Penalty(const Matrix &matrix) {
	const auto size = matrix.size();
	auto result = 0;

	const auto runs = [&](bool horizontal) {
		auto penalty = 0;
		for (auto a = 0; a != size; ++a) {
			auto color = false;
			auto run = 0;
			// The last eleven modules, for the 1:1:3:1:1 finder-like rule.
			auto history = 0;
			for (auto b = 0; b != size; ++b) {
				const auto dark = horizontal
					? matrix.get(b, a)
					: matrix.get(a, b);
				if (b > 0 && dark == color) {
					++run;
					if (run == 5) {
						penalty += 3;
					} else if (run > 5) {
						++penalty;
					}
				} else {
					color = dark;
					run = 1;
				}
				history = ((history << 1) | (dark ? 1 : 0)) & 0x7FF;
				if (b >= 10 && (history == 0x05D || history == 0x5D0)) {
					penalty += 40;
				}
			}
		}
		return penalty;
	};
	result += runs(true) + runs(false);

	for (auto y = 0; y + 1 < size; ++y) {
		for (auto x = 0; x + 1 < size; ++x) {
			const auto color = matrix.get(x, y);
			if (color == matrix.get(x + 1, y)
				&& color == matrix.get(x, y + 1)
				&& color == matrix.get(x + 1, y + 1)) {
				result += 3;
			}
		}
	}

	auto dark = 0;
	for (const auto module : matrix.modules()) {
		dark += module ? 1 : 0;
	}
	const auto total = size * size;
	const auto percent = (dark * 100) / total;
	result += (std::abs(percent - 50) / 5) * 10;
	return result;
}

[[nodiscard]] QByteArray MakeCodewords(
		const QByteArray &data,
		const VersionInfo &info) {
	// Mode 0100, an eight bit length, the bytes, a terminator, padding.
	auto bits = QVector<uchar>();
	bits.reserve(info.dataCodewords * 8);
	const auto append = [&](int value, int count) {
		for (auto i = count - 1; i >= 0; --i) {
			bits.push_back(uchar((value >> i) & 1));
		}
	};
	append(0x4, 4);
	append(int(data.size()), 8);
	for (const auto byte : data) {
		append(uchar(byte), 8);
	}
	const auto capacity = info.dataCodewords * 8;
	append(0, std::min(4, capacity - int(bits.size())));
	while (bits.size() % 8) {
		bits.push_back(0);
	}
	auto stream = QByteArray();
	stream.reserve(info.dataCodewords);
	for (auto i = 0; i < bits.size(); i += 8) {
		auto byte = 0;
		for (auto j = 0; j != 8; ++j) {
			byte = (byte << 1) | bits[i + j];
		}
		stream.push_back(char(byte));
	}
	for (auto pad = 0xEC; stream.size() < info.dataCodewords; pad ^= 0xEC ^ 0x11) {
		stream.push_back(char(pad));
	}

	// Blocks of equal length at level M for these versions.
	const auto blockLength = info.dataCodewords / info.blocks;
	auto dataBlocks = QVector<QByteArray>();
	auto ecBlocks = QVector<QByteArray>();
	for (auto i = 0; i != info.blocks; ++i) {
		const auto block = stream.mid(i * blockLength, blockLength);
		dataBlocks.push_back(block);
		ecBlocks.push_back(ReedSolomon(block, info.ecPerBlock));
	}
	auto result = QByteArray();
	for (auto i = 0; i != blockLength; ++i) {
		for (const auto &block : dataBlocks) {
			result.push_back(block[i]);
		}
	}
	for (auto i = 0; i != info.ecPerBlock; ++i) {
		for (const auto &block : ecBlocks) {
			result.push_back(block[i]);
		}
	}
	return result;
}

} // namespace

QByteArray ReedSolomon(const QByteArray &data, int ecLength) {
	// The generator polynomial: (x - a^0)(x - a^1)…(x - a^(n-1)).
	auto generator = QVector<uchar>(ecLength, uchar(0));
	generator[ecLength - 1] = 1;
	auto root = uchar(1);
	for (auto i = 0; i != ecLength; ++i) {
		for (auto j = 0; j != ecLength; ++j) {
			generator[j] = Multiply(generator[j], root);
			if (j + 1 < ecLength) {
				generator[j] ^= generator[j + 1];
			}
		}
		root = Multiply(root, 0x02);
	}
	auto remainder = QVector<uchar>(ecLength, uchar(0));
	for (const auto byte : data) {
		const auto factor = uchar(uchar(byte) ^ remainder.front());
		remainder.pop_front();
		remainder.push_back(0);
		for (auto i = 0; i != ecLength; ++i) {
			remainder[i] ^= Multiply(generator[i], factor);
		}
	}
	auto result = QByteArray();
	result.reserve(ecLength);
	for (const auto byte : remainder) {
		result.push_back(char(byte));
	}
	return result;
}

int FormatBits(int mask) {
	// Level M is 00; BCH(15, 5) with the generator 10100110111.
	const auto data = (0 << 3) | (mask & 7);
	auto remainder = data;
	for (auto i = 0; i != 10; ++i) {
		remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537);
	}
	return ((data << 10) | remainder) ^ 0x5412;
}

Code Encode(const QByteArray &data) {
	if (data.isEmpty() || data.size() > kMaxBytes) {
		return Code();
	}
	auto version = 0;
	for (auto i = 0; i != kVersionsCount; ++i) {
		// Four mode bits and eight length bits take two codewords (rounded up).
		if (int(data.size()) + 2 <= kVersions[i].dataCodewords) {
			version = i + 1;
			break;
		}
	}
	if (!version) {
		return Code();
	}
	const auto &info = kVersions[version - 1];
	const auto size = 17 + 4 * version;
	const auto codewords = MakeCodewords(data, info);

	auto base = Matrix(size);
	DrawFunctionPatterns(base, info);
	PlaceCodewords(base, codewords);

	auto best = Matrix(size);
	auto bestMask = 0;
	auto bestPenalty = std::numeric_limits<int>::max();
	for (auto mask = 0; mask != 8; ++mask) {
		auto candidate = base;
		ApplyMask(candidate, mask);
		DrawFormat(candidate, mask);
		const auto penalty = Penalty(candidate);
		if (penalty < bestPenalty) {
			bestPenalty = penalty;
			bestMask = mask;
			best = candidate;
		}
	}
	auto result = Code();
	result.version = version;
	result.size = size;
	result.mask = bestMask;
	result.modules = best.modules();
	return result;
}

QImage Render(
		const Code &code,
		int modulePixels,
		int quietModules,
		QColor dark,
		QColor light) {
	if (!code.valid() || modulePixels <= 0) {
		return QImage();
	}
	const auto side = (code.size + 2 * quietModules) * modulePixels;
	auto result = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
	result.fill(light);
	auto p = QPainter(&result);
	for (auto y = 0; y != code.size; ++y) {
		for (auto x = 0; x != code.size; ++x) {
			if (code.dark(x, y)) {
				p.fillRect(
					(x + quietModules) * modulePixels,
					(y + quietModules) * modulePixels,
					modulePixels,
					modulePixels,
					dark);
			}
		}
	}
	return result;
}

} // namespace Profile::Qr
