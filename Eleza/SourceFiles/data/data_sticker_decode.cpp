#include "data/data_sticker_decode.h"

#include "base/basic_types.h"
#include "data/data_image_decode.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QPainter>

#include <miniz.h>

#ifdef ELEZA_HAVE_RLOTTIE
#include <rlottie.h>
#endif // ELEZA_HAVE_RLOTTIE

#include <algorithm>
#include <cmath>
#include <vector>

namespace Data {
namespace {

// RIFF header (12) + the VP8X chunk header (8) + its flags byte.
constexpr auto kWebpHead = 21;
constexpr auto kWebpAnimationFlag = 0x02;
// The players' own sanity bounds (the server checks the files already).
constexpr auto kMaxFrames = 600;
constexpr auto kMaxSide = 2048;
constexpr auto kMaxStickerBytes = qint64(8) << 20;
constexpr auto kMinDelayMs = 16;
constexpr auto kDefaultDelayMs = 100;

[[nodiscard]] QSize FitSize(QSize size, QSize box) {
	if (size.isEmpty()) {
		return QSize();
	} else if (box.isEmpty()) {
		return size;
	}
	return size.scaled(box, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

// An animated WebP, read chunk by chunk: every ANMF frame is wrapped into a
// still WebP of its own (decoded by the one image decoder) and composited
// here, with its blending and disposal — what the image plugin does not do.
class WebpAnimation final : public StickerAnimation {
public:
	WebpAnimation(const QByteArray &bytes, QSize box)
	: _bytes(bytes)
	, _box(box) {
	}

	bool open() {
		const auto b = reinterpret_cast<const uchar*>(_bytes.constData());
		const auto n = qint64(_bytes.size());
		if (n < 30 || !_bytes.startsWith("RIFF") || _bytes.mid(8, 4) != "WEBP") {
			return false;
		}
		const auto end = std::min(n, 8 + qint64(Le32(b + 4)));
		for (auto offset = qint64(12); offset + 8 <= end;) {
			const auto tag = _bytes.mid(offset, 4);
			const auto size = qint64(Le32(b + offset + 4));
			const auto data = offset + 8;
			if (size < 0 || data + size > end) {
				return false;
			}
			if (tag == "VP8X" && size >= 10) {
				_canvas = QSize(Le24(b + data + 4) + 1, Le24(b + data + 7) + 1);
			} else if (tag == "ANMF" && size >= 16) {
				if (int(_frames.size()) >= kMaxFrames) {
					return false;
				}
				auto frame = Frame();
				frame.rect = QRect(
					Le24(b + data) * 2,
					Le24(b + data + 3) * 2,
					Le24(b + data + 6) + 1,
					Le24(b + data + 9) + 1);
				const auto duration = Le24(b + data + 12);
				frame.delayMs = (duration > 0)
					? std::max(duration, kMinDelayMs)
					: kDefaultDelayMs;
				const auto flags = b[data + 15];
				frame.blend = !(flags & 0x02);
				frame.dispose = (flags & 0x01);
				frame.still = WrapFrame(
					_bytes.mid(data + 16, size - 16),
					frame.rect.size());
				if (frame.still.isEmpty()) {
					return false;
				}
				_frames.push_back(std::move(frame));
			}
			offset = data + size + (size & 1);
		}
		if (_canvas.isEmpty()
			|| _canvas.width() > kMaxSide
			|| _canvas.height() > kMaxSide
			|| _frames.size() < 2) {
			return false;
		}
		const auto bounds = QRect(QPoint(), _canvas);
		for (const auto &frame : _frames) {
			if (!bounds.contains(frame.rect)) {
				return false;
			}
		}
		_size = FitSize(_canvas, _box);
		return !_size.isEmpty();
	}

	int frameCount() const override {
		return int(_frames.size());
	}

	QSize frameSize() const override {
		return _size;
	}

	QImage renderFrame(int index, int *delayMs) override {
		if (index < 0 || index >= int(_frames.size())) {
			return QImage();
		}
		if (index <= _last || _composed.isNull()) {
			_composed = QImage(_canvas, QImage::Format_ARGB32_Premultiplied);
			if (_composed.isNull()) {
				return QImage();
			}
			_composed.fill(Qt::transparent);
			_last = -1;
		}
		while (_last < index) {
			if (_last >= 0 && _frames[_last].dispose) {
				auto p = QPainter(&_composed);
				p.setCompositionMode(QPainter::CompositionMode_Source);
				p.fillRect(_frames[_last].rect, Qt::transparent);
			}
			const auto &frame = _frames[++_last];
			const auto image = DecodeImage(QString(), frame.still);
			if (image.size() != frame.rect.size()) {
				_composed = QImage();
				return QImage();
			}
			auto p = QPainter(&_composed);
			p.setCompositionMode(frame.blend
				? QPainter::CompositionMode_SourceOver
				: QPainter::CompositionMode_Source);
			p.drawImage(frame.rect.topLeft(), image);
		}
		if (delayMs) {
			*delayMs = _frames[index].delayMs;
		}
		return (_composed.size() == _size)
			? _composed.copy()
			: _composed.scaled(_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	}

private:
	struct Frame {
		QRect rect;
		int delayMs = kDefaultDelayMs;
		bool blend = true;
		bool dispose = false;
		QByteArray still;
	};

	[[nodiscard]] static quint32 Le32(const uchar *p) {
		return quint32(p[0])
			| (quint32(p[1]) << 8)
			| (quint32(p[2]) << 16)
			| (quint32(p[3]) << 24);
	}
	[[nodiscard]] static int Le24(const uchar *p) {
		return int(p[0]) | (int(p[1]) << 8) | (int(p[2]) << 16);
	}
	static void PutLe32(QByteArray &to, quint32 value) {
		for (auto i = 0; i != 4; ++i) {
			to.append(char((value >> (8 * i)) & 0xFF));
		}
	}
	static void PutLe24(QByteArray &to, int value) {
		for (auto i = 0; i != 3; ++i) {
			to.append(char((value >> (8 * i)) & 0xFF));
		}
	}

	// The chunks of one frame (ALPH + VP8, or VP8L) as a file of its own.
	[[nodiscard]] static QByteArray WrapFrame(const QByteArray &chunks, QSize size) {
		auto alpha = false;
		auto image = false;
		const auto b = reinterpret_cast<const uchar*>(chunks.constData());
		const auto n = qint64(chunks.size());
		auto kept = QByteArray();
		for (auto offset = qint64(0); offset + 8 <= n;) {
			const auto tag = chunks.mid(offset, 4);
			const auto size = qint64(Le32(b + offset + 4));
			const auto total = 8 + size + (size & 1);
			if (offset + 8 + size > n) {
				return QByteArray();
			}
			if (tag == "ALPH") {
				alpha = true;
			} else if (tag == "VP8 " || tag == "VP8L") {
				image = true;
			} else {
				offset += total;
				continue; // unknown chunks stay out
			}
			kept.append(chunks.mid(offset, std::min(total, n - offset)));
			offset += total;
		}
		if (!image) {
			return QByteArray();
		}
		auto body = QByteArray("WEBP");
		if (alpha) {
			body.append("VP8X");
			PutLe32(body, 10);
			body.append(char(0x10)); // alpha
			body.append(QByteArray(3, '\0'));
			PutLe24(body, size.width() - 1);
			PutLe24(body, size.height() - 1);
		}
		body.append(kept);
		auto result = QByteArray("RIFF");
		PutLe32(result, quint32(body.size()));
		result.append(body);
		return result;
	}

	QByteArray _bytes;
	QSize _box;
	QSize _canvas;
	QSize _size;
	std::vector<Frame> _frames;
	QImage _composed;
	int _last = -1;

};

// Images of a Lottie document must be inside it (data: URIs): a path would
// be read from the disk by the player.
[[nodiscard]] bool LottieSelfContained(const QByteArray &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return false;
	}
	const auto root = document.object();
	const auto w = root.value(u"w"_s).toDouble();
	const auto h = root.value(u"h"_s).toDouble();
	if (!(w > 0. && h > 0. && w <= 4096. && h <= 4096.)
		|| !root.value(u"layers"_s).isArray()) {
		return false;
	}
	for (const auto &value : root.value(u"assets"_s).toArray()) {
		const auto asset = value.toObject();
		const auto path = asset.value(u"p"_s);
		if (path.isString()
			&& !asset.value(u"layers"_s).isArray()
			&& !path.toString().startsWith(u"data:"_s)) {
			return false;
		}
	}
	return true;
}

#ifdef ELEZA_HAVE_RLOTTIE
class LottieAnimation final : public StickerAnimation {
public:
	explicit LottieAnimation(std::unique_ptr<rlottie::Animation> animation, QSize box)
	: _animation(std::move(animation)) {
		auto w = size_t(0);
		auto h = size_t(0);
		_animation->size(w, h);
		_size = FitSize(QSize(int(std::min(w, size_t(4096))), int(std::min(h, size_t(4096)))), box);
		_count = int(std::min(_animation->totalFrame(), size_t(kMaxFrames)));
		const auto rate = _animation->frameRate();
		_delayMs = (rate >= 1. && rate <= 120.)
			? std::max(int(std::lround(1000. / rate)), kMinDelayMs)
			: kDefaultDelayMs / 3;
	}

	[[nodiscard]] bool valid() const {
		return _count > 0 && !_size.isEmpty();
	}

	int frameCount() const override {
		return _count;
	}

	QSize frameSize() const override {
		return _size;
	}

	QImage renderFrame(int index, int *delayMs) override {
		if (index < 0 || index >= _count) {
			return QImage();
		}
		auto image = QImage(_size, QImage::Format_ARGB32_Premultiplied);
		if (image.isNull()) {
			return QImage();
		}
		image.fill(Qt::transparent);
		auto surface = rlottie::Surface(
			reinterpret_cast<uint32_t*>(image.bits()),
			size_t(image.width()),
			size_t(image.height()),
			size_t(image.bytesPerLine()));
		_animation->renderSync(size_t(index), surface);
		if (delayMs) {
			*delayMs = _delayMs;
		}
		return image;
	}

private:
	std::unique_ptr<rlottie::Animation> _animation;
	QSize _size;
	int _count = 0;
	int _delayMs = kDefaultDelayMs;

};
#endif // ELEZA_HAVE_RLOTTIE

struct GunzipState {
	QByteArray *out = nullptr;
	qint64 max = 0;
	bool overflow = false;
};

int PutGunzipped(const void *data, int len, void *user) {
	const auto state = static_cast<GunzipState*>(user);
	if (state->out->size() + qint64(len) > state->max) {
		state->overflow = true;
		return 0;
	}
	state->out->append(static_cast<const char*>(data), len);
	return 1;
}

} // namespace

StickerFormat SniffStickerFormat(const QByteArray &head) {
	const auto b = reinterpret_cast<const uchar*>(head.constData());
	const auto n = head.size();
	if (n >= 3 && b[0] == 0x1F && b[1] == 0x8B && b[2] == 0x08) {
		return StickerFormat::Lottie;
	} else if (n >= kWebpHead
		&& head.startsWith("RIFF")
		&& head.mid(8, 4) == "WEBP"
		&& head.mid(12, 4) == "VP8X"
		&& (b[20] & kWebpAnimationFlag)) {
		return StickerFormat::AnimatedWebp;
	}
	return StickerFormat::Still;
}

QByteArray GunzipSticker(const QByteArray &bytes, qint64 maxSize) {
	// RFC 1952: the header, a raw deflate stream, CRC32 and size (8 bytes).
	const auto b = reinterpret_cast<const uchar*>(bytes.constData());
	const auto n = qint64(bytes.size());
	if (n < 18 || b[0] != 0x1F || b[1] != 0x8B || b[2] != 0x08) {
		return QByteArray();
	}
	const auto flags = b[3];
	auto offset = qint64(10);
	if (flags & 0x04) { // FEXTRA
		if (offset + 2 > n) {
			return QByteArray();
		}
		offset += 2 + (qint64(b[offset]) | (qint64(b[offset + 1]) << 8));
	}
	for (const auto bit : { 0x08, 0x10 }) { // FNAME, FCOMMENT
		if (flags & bit) {
			while (offset < n && b[offset] != 0) {
				++offset;
			}
			++offset;
		}
	}
	if (flags & 0x02) { // FHCRC
		offset += 2;
	}
	if (offset >= n - 8) {
		return QByteArray();
	}
	auto result = QByteArray();
	auto state = GunzipState{ &result, maxSize, false };
	auto size = size_t(n - 8 - offset);
	const auto ok = tinfl_decompress_mem_to_callback(
		b + offset,
		&size,
		PutGunzipped,
		&state,
		0);
	if (!ok || state.overflow) {
		return QByteArray();
	}
	return result;
}

bool LottieSupported() {
#ifdef ELEZA_HAVE_RLOTTIE
	return true;
#else // ELEZA_HAVE_RLOTTIE
	return false;
#endif // ELEZA_HAVE_RLOTTIE
}

std::unique_ptr<StickerAnimation> OpenStickerAnimation(
		const QByteArray &bytes,
		QSize box) {
	if (bytes.size() > kMaxStickerBytes) {
		return nullptr;
	}
	switch (SniffStickerFormat(bytes.left(kWebpHead))) {
	case StickerFormat::AnimatedWebp: {
		auto result = std::make_unique<WebpAnimation>(bytes, box);
		if (!result->open()) {
			return nullptr;
		}
		return result;
	}
	case StickerFormat::Lottie: {
#ifdef ELEZA_HAVE_RLOTTIE
		const auto json = GunzipSticker(bytes);
		if (json.isEmpty() || !LottieSelfContained(json)) {
			return nullptr;
		}
		auto animation = rlottie::Animation::loadFromData(
			json.toStdString(),
			std::string(),
			std::string(),
			false);
		if (!animation) {
			return nullptr;
		}
		auto result = std::make_unique<LottieAnimation>(std::move(animation), box);
		if (!result->valid()) {
			return nullptr;
		}
		return result;
#else // ELEZA_HAVE_RLOTTIE
		Q_UNUSED(box);
		return nullptr;
#endif // ELEZA_HAVE_RLOTTIE
	}
	case StickerFormat::Still:
		break;
	}
	return nullptr;
}

} // namespace Data
