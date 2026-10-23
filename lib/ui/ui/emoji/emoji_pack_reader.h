// Reads one emoji out of Telegram's emoji.pack (format «EPK3», versions 2 and 3)
// as a 64×64 picture. Our own reader of the format; the WebP pictures inside it
// go to Qt's WebP plugin (qtimageformats), the DEFLATE streams to miniz.
//
//   auto reader = Ui::Emoji::PackReader(data, size);   // the bytes stay the caller's
//   if (reader.valid()) {
//       auto error = QString();
//       const auto image = reader.decode(page, index, &error);
//       // null and no error: the pack has no such emoji; null and an error: damaged
//   }
//
// The file: a 32-byte header, then one 20-byte entry per record, sorted by key —
// first the emoji (key = page × 4096 + index in the page, the order of
// Tables::kPages), then the alpha masks (key = mask id) — then the records'
// bytes. All numbers are little-endian.
//
//   header: "EPK3" | u16 version | u16 entry size (20) | i32 width (64)
//           | i32 height (64) | i32 records | i32 emoji records | i32 file length | i32 0
//   entry:  u16 key | u16 mask id (0xFFFF none) | i32 offset | u16 length
//           | u16 root A | u16 root B | u16 colours | u8 codec | u8 predictor
//           | u8 channels (bit 7: palette deflated)
//           | u8 flags (bit 0: 7-bit palette, bits 1–3: transform of root A, 4–6: of root B)
//
// A record is either a whole picture (a VP8L bitstream without its RIFF header,
// or a complete image file: WebP, a few PNG), or a palette plus a 64×64 map of palette indexes. The
// index map is stored deflated or as the low byte of a lossless WebP, on its own
// or as a correction of the map of one or two other records (the «roots», which
// never depend on anything themselves): through a lookup table per root index,
// XOR-ed or added, the two roots splitting the square at a column or a row, or
// the root's map taken as it is. A root's map may be mirrored or transposed
// first. An emoji with a mask takes its alpha from the mask's picture (its blue
// channel) times its own.
//
// Not thread-safe: one set of work buffers. Use one reader per thread.
#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>

#include <array>
#include <cstdint>
#include <vector>

namespace Ui::Emoji {

class PackReader final {
public:
	static constexpr auto kSide = 64;
	static constexpr auto kPixels = kSide * kSide;

	PackReader(const uchar *data, qsizetype size);

	[[nodiscard]] bool valid() const {
		return _valid;
	}
	// Why the pack was refused (for the log).
	[[nodiscard]] QString error() const {
		return _error;
	}
	[[nodiscard]] int version() const {
		return _version;
	}
	// Records in the file (emoji and masks).
	[[nodiscard]] int recordCount() const {
		return _records;
	}
	[[nodiscard]] int emojiCount() const {
		return _emojiRecords;
	}
	// How many emoji of `page` the pack holds.
	[[nodiscard]] int pageSize(int page) const;
	[[nodiscard]] bool has(int page, int index) const;

	// The emoji at `index` of `page`, 64×64 Format_ARGB32 (not premultiplied).
	// Null when the pack has no such emoji, or when its record is damaged
	// (then `error`, if given, says why).
	[[nodiscard]] QImage decode(int page, int index, QString *error = nullptr);

	// Forgets the cached root maps.
	void trim();

private:
	using Map = std::array<uchar, kPixels>;

	[[nodiscard]] uint u8(qsizetype at) const;
	[[nodiscard]] uint u16(qsizetype at) const;
	[[nodiscard]] qint32 i32(qsizetype at) const;
	[[nodiscard]] static qsizetype entry(int record);
	[[nodiscard]] uint key(int record) const;
	[[nodiscard]] int recordOf(int page, int index) const;
	[[nodiscard]] int search(uint key, int from, int till) const;

	bool fail(const QString &what);
	[[nodiscard]] bool picture(int record, std::vector<quint32> &out);
	[[nodiscard]] bool checkRange(qint64 offset, qint64 length);
	[[nodiscard]] bool paletteSize(qsizetype entryAt, int *result);
	[[nodiscard]] bool loadPalette(qsizetype entryAt, int colors);
	[[nodiscard]] bool indexMap(int record, Map &out, int depth);
	[[nodiscard]] const Map *root(int record, int child, int depth);
	[[nodiscard]] bool copy(qint64 offset, qint64 length, int *at);
	[[nodiscard]] bool inflate(int at, int length, int expected);
	[[nodiscard]] bool vp8l(int at, int length, std::vector<quint32> &out);
	[[nodiscard]] bool image(
		int at,
		int length,
		std::vector<quint32> &out,
		const char *format);
	void putLe(int at, quint32 value);

	const uchar *_data = nullptr;
	qsizetype _size = 0;
	bool _valid = false;
	QString _error;
	int _version = 0;
	int _records = 0;
	int _emojiRecords = 0;

	// Work buffers.
	std::vector<uchar> _scratch;
	std::vector<uchar> _inflated;
	std::array<quint32, 256> _palette = {};
	std::array<uchar, 1024> _paletteBytes = {};
	std::vector<quint32> _pixels;
	// A handful of root maps used recently (a skin-tone family shares its roots).
	static constexpr auto kRoots = 8;
	std::array<int, kRoots> _rootKeys = {};
	std::array<Map, kRoots> _rootMaps = {};
	std::array<quint64, kRoots> _rootUsed = {};
	quint64 _clock = 0;

};

} // namespace Ui::Emoji
