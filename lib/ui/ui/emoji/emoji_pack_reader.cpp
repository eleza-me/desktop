#include "ui/emoji/emoji_pack_reader.h"

#include <QtCore/QByteArray>

#include <miniz.h>

#include <algorithm>
#include <cstring>

namespace Ui::Emoji {
namespace {

constexpr auto kMagic = quint32(0x334B5045); // "EPK3"
constexpr auto kHeader = 32;
constexpr auto kEntry = 20;
constexpr auto kNone = 0xFFFFU;
// RIFF + size + WEBP + VP8L + size: the bytes a bare VP8L record lacks.
constexpr auto kRiffHeader = 20;
constexpr auto kMaxDepth = 2;

enum Codec : uint {
	kCodecDeflate = 0,
	kCodecIndexWebp = 1,
	kCodecVp8l = 2,
	kCodecWebpFile = 3,
	kCodecSameMap = 4,
};

enum Predictor : uint {
	kPredNone = 0,
	kPredXor = 1,
	kPredAdd = 2,
	kPredHalves = 3,
	kPredCopy = 4,
	kPredColumns = 5,
	kPredRows = 6,
};

// Index of pixel `i` in a root map seen through `transform` (bit 0: mirror x,
// bit 1: mirror y, bit 2: transpose).
[[nodiscard]] int From(int i, uint transform) {
	auto x = i & 63;
	auto y = i >> 6;
	if (transform & 1) {
		x = 63 - x;
	}
	if (transform & 2) {
		y = 63 - y;
	}
	return (transform & 4) ? ((x << 6) | y) : ((y << 6) | x);
}

} // namespace

PackReader::PackReader(const uchar *data, qsizetype size)
: _data(data)
, _size(size) {
	_rootKeys.fill(-1);
	_rootUsed.fill(0);
	if (!_data || _size < kHeader || quint32(i32(0)) != kMagic) {
		_error = QStringLiteral("not an EPK3 emoji pack");
		return;
	}
	_version = int(u16(4));
	if ((_version != 2 && _version != 3)
		|| u16(6) != kEntry
		|| i32(8) != kSide
		|| i32(12) != kSide
		|| i32(24) != _size) {
		_error = QStringLiteral("unsupported EPK3 header (version %1)").arg(_version);
		return;
	}
	_records = i32(16);
	_emojiRecords = i32(20);
	if (_records <= 0
		|| _records > 0xFFFF
		|| _emojiRecords < 0
		|| _emojiRecords > _records
		|| kHeader + qint64(_records) * kEntry > _size) {
		_error = QStringLiteral("EPK3 record table out of range");
		return;
	}
	_scratch.resize(8192);
	_inflated.resize(kPixels + 2 * 256 + 64);
	_valid = true;
}

int PackReader::pageSize(int page) const {
	auto result = 0;
	for (auto r = 0; r < _emojiRecords; ++r) {
		if (int(key(r) >> 12) == page) {
			++result;
		}
	}
	return result;
}

bool PackReader::has(int page, int index) const {
	return recordOf(page, index) >= 0;
}

QImage PackReader::decode(int page, int index, QString *error) {
	if (error) {
		error->clear();
	}
	const auto r = recordOf(page, index);
	if (r < 0) {
		return QImage();
	}
	_error.clear();
	auto out = std::vector<quint32>();
	auto ok = picture(r, out);
	const auto mask = u16(entry(r) + 2);
	if (ok && mask != kNone) {
		const auto m = search(mask, _emojiRecords, _records);
		auto alpha = std::vector<quint32>();
		ok = (m >= 0)
			? picture(m, alpha)
			: fail(QStringLiteral("missing mask %1").arg(mask));
		if (ok) {
			for (auto i = 0; i != kPixels; ++i) {
				const auto c = out[i];
				const auto a = ((c >> 24) * (alpha[i] & 0xFF) + 127) / 255;
				out[i] = (a << 24) | (c & 0x00FFFFFFU);
			}
		}
	}
	if (!ok) {
		if (error) {
			*error = _error;
		}
		return QImage();
	}
	auto result = QImage(kSide, kSide, QImage::Format_ARGB32);
	for (auto y = 0; y != kSide; ++y) {
		std::memcpy(
			result.scanLine(y),
			out.data() + y * kSide,
			kSide * sizeof(quint32));
	}
	return result;
}

void PackReader::trim() {
	_rootKeys.fill(-1);
	_rootUsed.fill(0);
}

uint PackReader::u8(qsizetype at) const {
	return (at >= 0 && at < _size) ? uint(_data[at]) : 0U;
}

uint PackReader::u16(qsizetype at) const {
	return u8(at) | (u8(at + 1) << 8);
}

qint32 PackReader::i32(qsizetype at) const {
	return qint32(u8(at)
		| (u8(at + 1) << 8)
		| (u8(at + 2) << 16)
		| (u8(at + 3) << 24));
}

qsizetype PackReader::entry(int record) {
	return kHeader + qsizetype(record) * kEntry;
}

uint PackReader::key(int record) const {
	return u16(entry(record));
}

int PackReader::recordOf(int page, int index) const {
	if (!_valid || page < 0 || page > 15 || index < 0 || index > 4095) {
		return -1;
	}
	return search(uint(page << 12) | uint(index), 0, _emojiRecords);
}

int PackReader::search(uint key, int from, int till) const {
	auto lo = from;
	auto hi = till - 1;
	while (lo <= hi) {
		const auto mid = lo + (hi - lo) / 2;
		const auto k = this->key(mid);
		if (k == key) {
			return mid;
		} else if (k < key) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return -1;
}

bool PackReader::fail(const QString &what) {
	if (_error.isEmpty()) {
		_error = QStringLiteral("emoji.pack: ") + what;
	}
	return false;
}

bool PackReader::picture(int record, std::vector<quint32> &out) {
	const auto e = entry(record);
	const auto codec = u8(e + 16);
	const auto offset = qint64(i32(e + 4));
	const auto length = qint64(u16(e + 8));
	if (!checkRange(offset, length)) {
		return false;
	}
	if (codec == kCodecVp8l || codec == kCodecWebpFile) {
		auto at = 0;
		if (!copy(offset, length, &at)) {
			return false;
		}
		// A whole file is mostly a WebP, but a few are PNG: by their bytes.
		return (codec == kCodecVp8l)
			? vp8l(at, int(length), out)
			: image(at, int(length), out, nullptr);
	}
	if (codec != kCodecDeflate
		&& codec != kCodecIndexWebp
		&& codec != kCodecSameMap) {
		return fail(QStringLiteral("codec %1").arg(codec));
	}
	const auto colors = int(u16(e + 14));
	if (colors < 1 || colors > 256) {
		return fail(QStringLiteral("palette of %1").arg(colors));
	}
	auto map = Map();
	if (!indexMap(record, map, 0) || !loadPalette(e, colors)) {
		return false;
	}
	out.resize(kPixels);
	for (auto i = 0; i != kPixels; ++i) {
		const auto index = int(map[i]);
		if (index >= colors) {
			return fail(QStringLiteral("index %1 of %2").arg(index).arg(colors));
		}
		out[i] = _palette[index];
	}
	return true;
}

bool PackReader::checkRange(qint64 offset, qint64 length) {
	if (offset < kHeader + qint64(_records) * kEntry
		|| length <= 0
		|| offset + length > _size) {
		return fail(QStringLiteral("record bytes out of range"));
	}
	return true;
}

bool PackReader::paletteSize(qsizetype entryAt, int *result) {
	const auto channels = u8(entryAt + 18);
	if (channels & 0x80) {
		const auto stored = int(u16(i32(entryAt + 4)));
		if (stored <= 0 || stored + 2 > int(u16(entryAt + 8))) {
			return fail(QStringLiteral("deflated palette size"));
		}
		*result = stored + 2;
		return true;
	}
	const auto n = int(u16(entryAt + 14) * (channels & 0x7F));
	*result = (u8(entryAt + 19) & 1) ? ((n * 7 + 7) / 8) : n;
	return true;
}

bool PackReader::loadPalette(qsizetype entryAt, int colors) {
	const auto channels = int(u8(entryAt + 18) & 0x7F);
	const auto deflated = (u8(entryAt + 18) & 0x80) != 0;
	const auto sevenBit = (u8(entryAt + 19) & 1) != 0;
	if (channels == 0) {
		// No palette stored: the 256 greys.
		for (auto i = 0; i != colors; ++i) {
			_palette[i] = 0xFF000000U | (quint32(i) * 0x010101U);
		}
		return true;
	}
	if (channels != 1 && channels != 3 && channels != 4) {
		return fail(QStringLiteral("channels %1").arg(channels));
	}
	const auto values = colors * channels;
	const auto offset = qint64(i32(entryAt + 4));
	const auto packed = sevenBit ? ((values * 7 + 7) / 8) : values;
	const uchar *src = nullptr;
	if (deflated) {
		auto size = 0;
		if (!paletteSize(entryAt, &size)) {
			return false;
		}
		const auto stored = size - 2;
		auto at = 0;
		if (!copy(offset + 2, stored, &at) || !inflate(at, stored, packed)) {
			return false;
		}
		src = _inflated.data();
	} else {
		auto at = 0;
		if (!copy(offset, packed, &at)) {
			return false;
		}
		src = _scratch.data() + at;
	}
	if (sevenBit) {
		// Values of 7 bits packed LSB first, widened to 8 bits by repeating
		// the top bit.
		for (auto i = 0; i != values; ++i) {
			const auto bit = i * 7;
			const auto lo = uint(src[bit >> 3]);
			const auto hi = ((bit & 7) > 1) ? uint(src[(bit >> 3) + 1]) : 0U;
			const auto v = ((lo | (hi << 8)) >> (bit & 7)) & 0x7FU;
			_paletteBytes[i] = uchar((v << 1) | (v >> 6));
		}
	} else {
		std::memcpy(_paletteBytes.data(), src, values);
	}
	for (auto i = 0, p = 0; i != colors; ++i) {
		if (channels == 1) {
			_palette[i] = 0xFF000000U | (quint32(_paletteBytes[p++]) * 0x010101U);
		} else {
			const auto red = quint32(_paletteBytes[p++]);
			const auto green = quint32(_paletteBytes[p++]);
			const auto blue = quint32(_paletteBytes[p++]);
			const auto alpha = (channels == 4) ? quint32(_paletteBytes[p++]) : 0xFFU;
			_palette[i] = (alpha << 24) | (red << 16) | (green << 8) | blue;
		}
	}
	return true;
}

bool PackReader::indexMap(int record, Map &out, int depth) {
	if (depth > kMaxDepth) {
		return fail(QStringLiteral("roots nested too deep"));
	}
	const auto e = entry(record);
	const auto pred = u8(e + 17);
	const auto flags = u8(e + 19);
	const auto transformA = (flags >> 1) & 7U;
	const auto transformB = (flags >> 4) & 7U;
	const Map *rootA = nullptr;
	const Map *rootB = nullptr;
	auto colorsA = 0;
	auto colorsB = 0;
	if (pred > kPredRows) {
		return fail(QStringLiteral("predictor %1").arg(pred));
	}
	if (pred != kPredNone) {
		const auto a = int(u16(e + 10));
		if (!(rootA = root(a, record, depth))) {
			return false;
		}
		colorsA = int(u16(entry(a) + 14));
		if (pred == kPredHalves || pred == kPredColumns || pred == kPredRows) {
			const auto b = int(u16(e + 12));
			if (!(rootB = root(b, record, depth))) {
				return false;
			}
			colorsB = int(u16(entry(b) + 14));
		}
	}
	if (pred == kPredCopy) {
		for (auto i = 0; i != kPixels; ++i) {
			out[i] = (*rootA)[From(i, transformA)];
		}
		return true;
	}
	const auto lutSize = colorsA + colorsB;
	auto skip = 0;
	if (!paletteSize(e, &skip)) {
		return false;
	}
	auto offset = qint64(i32(e + 4)) + skip;
	auto length = int(u16(e + 8)) - skip;
	auto cut = kSide / 2;
	if (pred == kPredColumns || pred == kPredRows) {
		if (length < 2) {
			return fail(QStringLiteral("split record too short"));
		}
		cut = int(u8(offset));
		++offset;
		--length;
		if (cut <= 0 || cut >= kSide) {
			return fail(QStringLiteral("split at %1").arg(cut));
		}
	}
	if (length <= 0) {
		return fail(QStringLiteral("no index data"));
	}
	auto at = 0;
	if (!copy(offset, length, &at)) {
		return false;
	}
	const auto lut = _inflated.data();
	if (u8(e + 16) == kCodecDeflate) {
		if (!inflate(at, length, lutSize + kPixels)) {
			return false;
		}
		std::memcpy(out.data(), _inflated.data() + lutSize, kPixels);
	} else {
		if (length <= lutSize) {
			return fail(QStringLiteral("index picture too short"));
		}
		std::memcpy(_inflated.data(), _scratch.data() + at, lutSize);
		if (!vp8l(at + lutSize, length - lutSize, _pixels)) {
			return false;
		}
		for (auto i = 0; i != kPixels; ++i) {
			out[i] = uchar(_pixels[i]);
		}
	}
	switch (pred) {
	case kPredNone:
		break;
	case kPredXor:
	case kPredAdd:
		for (auto i = 0; i != kPixels; ++i) {
			const auto index = int((*rootA)[From(i, transformA)]);
			if (index >= colorsA) {
				return fail(QStringLiteral("root index"));
			}
			const auto d = lut[index];
			out[i] = (pred == kPredXor) ? uchar(out[i] ^ d) : uchar(out[i] + d);
		}
		break;
	default: // Two roots: split at the middle column, at a column or at a row.
		for (auto i = 0; i != kPixels; ++i) {
			const auto first = (pred == kPredRows)
				? ((i >> 6) < cut)
				: ((i & 63) < cut);
			const auto index = first
				? int((*rootA)[From(i, transformA)])
				: int((*rootB)[From(i, transformB)]);
			if (index >= (first ? colorsA : colorsB)) {
				return fail(QStringLiteral("root index"));
			}
			out[i] = uchar(out[i] + lut[first ? index : (colorsA + index)]);
		}
		break;
	}
	return true;
}

const PackReader::Map *PackReader::root(int record, int child, int depth) {
	if (record >= _records || record == child) {
		fail(QStringLiteral("root %1").arg(record));
		return nullptr;
	}
	const auto e = entry(record);
	if (u8(e + 17) != kPredNone || u8(e + 16) > kCodecIndexWebp) {
		fail(QStringLiteral("root with dependencies"));
		return nullptr;
	}
	auto victim = 0;
	for (auto s = 0; s != kRoots; ++s) {
		if (_rootKeys[s] == record) {
			_rootUsed[s] = ++_clock;
			return &_rootMaps[s];
		}
		if (_rootUsed[s] < _rootUsed[victim]) {
			victim = s;
		}
	}
	// A failure below leaves no half-written map behind as a hit.
	_rootKeys[victim] = -1;
	// (Decodes into the scratch / inflated buffers, which the caller has not
	// filled yet.)
	if (!indexMap(record, _rootMaps[victim], depth + 1)) {
		return nullptr;
	}
	_rootKeys[victim] = record;
	_rootUsed[victim] = ++_clock;
	return &_rootMaps[victim];
}

bool PackReader::copy(qint64 offset, qint64 length, int *at) {
	if (!checkRange(offset, length)) {
		return false;
	}
	const auto need = size_t(kRiffHeader + length + 1);
	if (_scratch.size() < need) {
		_scratch.resize(std::max(need, _scratch.size() * 2));
	}
	std::memcpy(_scratch.data() + kRiffHeader, _data + offset, size_t(length));
	*at = kRiffHeader;
	return true;
}

bool PackReader::inflate(int at, int length, int expected) {
	if (expected > int(_inflated.size()) || expected <= 0) {
		return fail(QStringLiteral("inflated size %1").arg(expected));
	}
	// Raw DEFLATE (no zlib header). Room for a little more than expected:
	// a stream that holds more than that is damaged as well.
	auto out = std::vector<uchar>(size_t(expected) + 1);
	const auto got = tinfl_decompress_mem_to_mem(
		out.data(),
		out.size(),
		_scratch.data() + at,
		size_t(length),
		0);
	if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got < size_t(expected)) {
		return fail(QStringLiteral("short DEFLATE stream"));
	}
	std::memcpy(_inflated.data(), out.data(), size_t(expected));
	return true;
}

bool PackReader::vp8l(int at, int length, std::vector<quint32> &out) {
	// Every record is copied kRiffHeader bytes into the scratch buffer: the
	// header goes in front of it (over the lookup table of an index picture,
	// which has been copied out by then).
	const auto start = at - kRiffHeader;
	if (start < 0) {
		return fail(QStringLiteral("no room for the RIFF header"));
	}
	const auto padded = length + (length & 1);
	if (int(_scratch.size()) < at + padded) {
		return fail(QStringLiteral("VP8L past the buffer"));
	}
	if (length & 1) {
		_scratch[at + length] = 0;
	}
	putLe(start, 0x46464952U); // "RIFF"
	putLe(start + 4, quint32(4 + 8 + padded)); // "WEBP" + chunk header + chunk
	putLe(start + 8, 0x50424557U); // "WEBP"
	putLe(start + 12, 0x4C385056U); // "VP8L"
	putLe(start + 16, quint32(length));
	return image(start, kRiffHeader + padded, out, "WEBP");
}

bool PackReader::image(
		int at,
		int length,
		std::vector<quint32> &out,
		const char *format) {
	auto decoded = QImage::fromData(_scratch.data() + at, length, format);
	if (decoded.isNull()) {
		return fail(QStringLiteral("picture not decoded"));
	} else if (decoded.size() != QSize(kSide, kSide)) {
		return fail(QStringLiteral("picture of %1x%2")
			.arg(decoded.width())
			.arg(decoded.height()));
	}
	if (decoded.format() != QImage::Format_ARGB32) {
		decoded = std::move(decoded).convertToFormat(QImage::Format_ARGB32);
	}
	out.resize(kPixels);
	for (auto y = 0; y != kSide; ++y) {
		std::memcpy(
			out.data() + y * kSide,
			decoded.constScanLine(y),
			kSide * sizeof(quint32));
	}
	return true;
}

void PackReader::putLe(int at, quint32 value) {
	_scratch[at] = uchar(value);
	_scratch[at + 1] = uchar(value >> 8);
	_scratch[at + 2] = uchar(value >> 16);
	_scratch[at + 3] = uchar(value >> 24);
}

} // namespace Ui::Emoji
