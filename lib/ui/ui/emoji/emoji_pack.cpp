#include "ui/emoji/emoji_pack.h"

#include "ui/emoji/emoji_pack_reader.h"

#include <QtCore/QCache>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QPointer>
#include <QtCore/QResource>
#include <QtCore/QRunnable>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPaintDevice>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>

static void InitEmojiResource() {
	Q_INIT_RESOURCE(emoji);
}

namespace Ui::Emoji {
namespace {

const auto kPackPath = QStringLiteral(":/emoji/emoji.pack");
// Decoded pictures kept: a few thousand at the sizes of a chat at 2x.
constexpr auto kCacheBytes = 24 * 1024 * 1024;
// Pictures waiting for the decoder: beyond that the oldest requests are
// dropped (the list scrolled past them; drawn again, they are asked again).
constexpr auto kMaxQueued = 1024;
// One repaint for the pictures decoded within this time.
constexpr auto kRepaintDelay = 16;
constexpr auto kMaxSide = 512;
// Faded (an inactive icon): grey, and this opaque.
constexpr auto kFadedOpacity = 0.6;

// The cache key: the id (without kMirrored: drawn mirrored from the same
// picture), the side in device pixels, faded or not.
[[nodiscard]] quint64 Key(Id id, int side, bool faded) {
	return (quint64(quint32(id & ~kMirrored)) << 20)
		| (quint64(faded ? 1 : 0) << 16)
		| quint64(side & 0xFFFF);
}

struct Job {
	quint64 key = 0;
	Id id = kNone;
	int side = 0;
	bool faded = false;
};

struct Result {
	quint64 key = 0;
	QImage image; // null: could not be decoded
};

[[nodiscard]] QImage Fade(QImage image) {
	// Grey by luma, alpha kept (premultiplied: the grey is scaled alike).
	for (auto y = 0; y != image.height(); ++y) {
		auto line = reinterpret_cast<QRgb*>(image.scanLine(y));
		for (auto x = 0; x != image.width(); ++x) {
			const auto c = line[x];
			const auto grey = (qRed(c) * 11 + qGreen(c) * 16 + qBlue(c) * 5) / 32;
			line[x] = qRgba(grey, grey, grey, qAlpha(c));
		}
	}
	return image;
}

[[nodiscard]] QImage HandshakeHalf(int tone, bool right) {
	auto result = QImage(QStringLiteral(":/emoji/compound/0_%1_%2.png")
		.arg(tone)
		.arg(right ? 1 : 0));
	return result.isNull()
		? result
		: std::move(result).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Shared by the pack (GUI thread) and its decoder (a pool thread).
struct Shared {
	QMutex mutex;
	std::deque<Job> queue; // newest at the back, decoded first
	bool running = false;
	std::unique_ptr<PackReader> reader; // the decoder's only
	QByteArray copy; // the pack's bytes when the resource was compressed

	// GUI thread.
	QPointer<QObject> pack;
};

class Pack final : public QObject {
public:
	explicit Pack(QObject *parent);

	[[nodiscard]] bool available() const {
		return _available;
	}
	[[nodiscard]] const QImage *picture(Id id, int side, bool faded);
	void request(Id id, int side, bool faded);
	void waiting(QPainter &p, const QRectF &rect);
	void subscribe(QObject *guard, std::function<void()> callback);
	void clear();

	void ready(std::vector<Result> results);

private:
	struct Waiting {
		QPointer<QWidget> widget;
		QRect rect;
	};
	struct Subscriber {
		QPointer<QObject> guard;
		std::function<void()> callback;
	};

	void startDecoder();
	void repaint();

	std::shared_ptr<Shared> _shared;
	bool _available = false;
	QCache<quint64, QImage> _cache;
	QSet<quint64> _pending;
	std::vector<Waiting> _waiting;
	std::vector<Subscriber> _subscribers;
	QTimer _repaintTimer;

};

class Decoder final : public QRunnable {
public:
	explicit Decoder(std::shared_ptr<Shared> shared)
	: _shared(std::move(shared)) {
		setAutoDelete(true);
	}

	void run() override;

private:
	[[nodiscard]] QImage decode(const Job &job);
	void post(std::vector<Result> &results);

	std::shared_ptr<Shared> _shared;

};

QImage Decoder::decode(const Job &job) {
	auto image = QImage();
	if (IsHandshake(job.id)) {
		const auto left = HandshakeHalf(HandshakeTone(job.id, false), false);
		const auto right = HandshakeHalf(HandshakeTone(job.id, true), true);
		if (left.isNull() || right.isNull()) {
			return QImage();
		}
		image = left;
		auto p = QPainter(&image);
		p.drawImage(QRect(QPoint(), image.size()), right);
	} else {
		auto error = QString();
		image = _shared->reader->decode(Page(job.id), Slot(job.id), &error);
		if (image.isNull()) {
			if (!error.isEmpty()) {
				qWarning("Emoji: %s", qPrintable(error));
			}
			return QImage();
		}
		image = std::move(image).convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}
	if (image.width() != job.side || image.height() != job.side) {
		image = image.scaled(
			job.side,
			job.side,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	return job.faded ? Fade(std::move(image)) : image;
}

void Decoder::run() {
	auto results = std::vector<Result>();
	auto timer = QElapsedTimer();
	timer.start();
	while (true) {
		auto job = Job();
		{
			QMutexLocker lock(&_shared->mutex);
			if (_shared->queue.empty()) {
				_shared->running = false;
				break;
			}
			job = _shared->queue.back();
			_shared->queue.pop_back();
		}
		results.push_back({ job.key, decode(job) });
		if (timer.elapsed() >= kRepaintDelay) {
			post(results);
			timer.restart();
		}
	}
	post(results);
}

void Decoder::post(std::vector<Result> &results) {
	if (results.empty()) {
		return;
	}
	const auto app = QCoreApplication::instance();
	if (!app) {
		return;
	}
	QMetaObject::invokeMethod(app, [
			shared = _shared,
			list = std::move(results)]() mutable {
		if (const auto pack = static_cast<Pack*>(shared->pack.data())) {
			pack->ready(std::move(list));
		}
	}, Qt::QueuedConnection);
	results = std::vector<Result>();
}

Pack::Pack(QObject *parent)
: QObject(parent)
, _shared(std::make_shared<Shared>())
, _cache(kCacheBytes) {
	_shared->pack = this;
	_repaintTimer.setSingleShot(true);
	_repaintTimer.setInterval(kRepaintDelay);
	connect(&_repaintTimer, &QTimer::timeout, this, [this] { repaint(); });

	InitEmojiResource();
	auto resource = QResource(kPackPath);
	if (!resource.isValid()) {
		qWarning("Emoji: no pack, the fonts draw every emoji.");
		return;
	}
	auto data = static_cast<const uchar*>(nullptr);
	auto size = qsizetype();
	if (resource.compressionAlgorithm() == QResource::NoCompression) {
		// In place, in the binary: nothing is read until an emoji is drawn.
		data = resource.data();
		size = qsizetype(resource.size());
	} else {
		_shared->copy = resource.uncompressedData();
		data = reinterpret_cast<const uchar*>(_shared->copy.constData());
		size = _shared->copy.size();
	}
	_shared->reader = std::make_unique<PackReader>(data, size);
	if (!_shared->reader->valid()) {
		qWarning(
			"Emoji: %s, the fonts draw every emoji.",
			qPrintable(_shared->reader->error()));
		_shared->reader = nullptr;
		return;
	}
	_available = true;
}

const QImage *Pack::picture(Id id, int side, bool faded) {
	return _cache.object(Key(id, side, faded));
}

void Pack::request(Id id, int side, bool faded) {
	if (!_available || id < 0 || side <= 0 || side > kMaxSide) {
		return;
	}
	const auto key = Key(id, side, faded);
	if (_pending.contains(key) || _cache.contains(key)) {
		return;
	}
	_pending.insert(key);
	auto start = false;
	{
		QMutexLocker lock(&_shared->mutex);
		_shared->queue.push_back({ key, id & ~kMirrored, side, faded });
		while (int(_shared->queue.size()) > kMaxQueued) {
			_pending.remove(_shared->queue.front().key);
			_shared->queue.pop_front();
		}
		if (!_shared->running) {
			_shared->running = start = true;
		}
	}
	if (start) {
		startDecoder();
	}
}

void Pack::startDecoder() {
	QThreadPool::globalInstance()->start(new Decoder(_shared));
}

void Pack::waiting(QPainter &p, const QRectF &rect) {
	const auto device = p.device();
	if (!device || device->devType() != QInternal::Widget) {
		return;
	}
	const auto widget = static_cast<QWidget*>(device);
	const auto mapped = p.combinedTransform().mapRect(rect).toAlignedRect()
		.marginsAdded(QMargins(1, 1, 1, 1));
	for (auto &entry : _waiting) {
		if (entry.widget == widget) {
			entry.rect = entry.rect.united(mapped);
			return;
		}
	}
	_waiting.push_back({ widget, mapped });
}

void Pack::subscribe(QObject *guard, std::function<void()> callback) {
	_subscribers.push_back({ guard, std::move(callback) });
}

void Pack::clear() {
	_cache.clear();
}

void Pack::ready(std::vector<Result> results) {
	for (auto &result : results) {
		_pending.remove(result.key);
		if (result.image.isNull()) {
			// Not decodable: kept as an empty picture, so that it is not asked
			// for again and again (the place stays blank).
			_cache.insert(result.key, new QImage(), 1);
			continue;
		}
		const auto cost = int(result.image.sizeInBytes());
		_cache.insert(result.key, new QImage(std::move(result.image)), cost);
	}
	if (!_repaintTimer.isActive()) {
		_repaintTimer.start();
	}
}

void Pack::repaint() {
	auto waiting = std::exchange(_waiting, {});
	for (const auto &entry : waiting) {
		if (const auto widget = entry.widget.data()) {
			widget->update(entry.rect);
		}
	}
	auto subscribers = _subscribers;
	for (const auto &subscriber : subscribers) {
		if (subscriber.guard) {
			subscriber.callback();
		}
	}
	_subscribers.erase(
		std::remove_if(
			_subscribers.begin(),
			_subscribers.end(),
			[](const Subscriber &s) { return !s.guard; }),
		_subscribers.end());
}

[[nodiscard]] Pack *Instance() {
	static auto instance = QPointer<Pack>();
	static auto created = false;
	if (!instance && !created) {
		const auto app = QCoreApplication::instance();
		if (!app) {
			return nullptr;
		}
		created = true;
		instance = new Pack(app);
	}
	return instance.data();
}

[[nodiscard]] int DeviceSide(QPainter &p, const QRectF &rect) {
	const auto device = p.device();
	const auto ratio = device ? device->devicePixelRatioF() : 1.;
	return std::max(
		int(std::lround(std::min(rect.width(), rect.height()) * ratio)),
		1);
}

} // namespace

bool Available() {
	const auto pack = Instance();
	return pack && pack->available();
}

bool Paint(QPainter &p, Id id, const QRectF &rect, bool faded) {
	const auto pack = Instance();
	if (!pack || !pack->available() || id < 0 || rect.isEmpty()) {
		return false;
	}
	const auto side = DeviceSide(p, rect);
	const auto image = pack->picture(id, side, faded);
	if (!image) {
		pack->request(id, side, faded);
		pack->waiting(p, rect);
		return false;
	} else if (image->isNull()) {
		return true; // A damaged record: nothing to draw, and no fallback.
	}
	const auto mirrored = IsMirrored(id);
	const auto opacity = p.opacity();
	if (faded) {
		p.setOpacity(opacity * kFadedOpacity);
	}
	const auto smooth = !p.testRenderHint(QPainter::SmoothPixmapTransform);
	if (smooth) {
		p.setRenderHint(QPainter::SmoothPixmapTransform);
	}
	if (mirrored) {
		p.save();
		p.translate(rect.center());
		p.scale(-1., 1.);
		p.translate(-rect.center());
	}
	p.drawImage(rect, *image);
	if (mirrored) {
		p.restore();
	}
	if (smooth) {
		p.setRenderHint(QPainter::SmoothPixmapTransform, false);
	}
	if (faded) {
		p.setOpacity(opacity);
	}
	return true;
}

void PaintOne(
		QPainter &p,
		QStringView emoji,
		const QRect &rect,
		int size,
		const QFont &fallback,
		bool faded) {
	const auto id = Available() ? Find(emoji) : kNone;
	if (id != kNone) {
		const auto target = QRectF(
			rect.x() + (rect.width() - size) / 2.,
			rect.y() + (rect.height() - size) / 2.,
			size,
			size);
		Paint(p, id, target, faded);
		return;
	}
	const auto opacity = p.opacity();
	if (faded) {
		p.setOpacity(opacity * kFadedOpacity);
	}
	p.setFont(fallback);
	p.drawText(rect, Qt::AlignCenter, emoji.toString());
	if (faded) {
		p.setOpacity(opacity);
	}
}

int OneWidth(QStringView emoji, int size, const QFont &fallback) {
	return (Available() && Find(emoji) != kNone)
		? size
		: int(std::ceil(QFontMetricsF(fallback).horizontalAdvance(
			emoji.toString())));
}

void Preload(Id id, int devicePixels) {
	if (const auto pack = Instance(); pack && pack->available()) {
		if (!pack->picture(id, devicePixels, false)) {
			pack->request(id, devicePixels, false);
		}
	}
}

void OnUpdated(QObject *guard, std::function<void()> callback) {
	if (const auto pack = Instance()) {
		pack->subscribe(guard, std::move(callback));
	}
}

void ClearCache() {
	if (const auto pack = Instance()) {
		pack->clear();
	}
}

} // namespace Ui::Emoji
