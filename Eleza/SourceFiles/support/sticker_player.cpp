#include "support/sticker_player.h"

#include "data/data_image_decode.h"
#include "data/data_session.h"
#include "data/data_sticker_decode.h"
#include "main/main_session.h"
#include "ui/ui_utility.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QThreadPool>

#include <atomic>
#include <mutex>

namespace Support {
namespace {

constexpr auto kMaxStickerFileBytes = qint64(8) << 20;
// Not painted for this long (and two frames): nobody sees it, it pauses.
constexpr auto kHiddenAfterMs = qint64(250);

int Playing = 0;

[[nodiscard]] QThreadPool &Pool() {
	// Two threads for every sticker of the app: animations never take more.
	static auto pool = [] {
		auto result = std::make_unique<QThreadPool>();
		result->setMaxThreadCount(2);
		return result;
	}();
	return *pool;
}

[[nodiscard]] QHash<QString, std::weak_ptr<StickerPlayer>> &Players() {
	static auto result = QHash<QString, std::weak_ptr<StickerPlayer>>();
	return result;
}

[[nodiscard]] QString PlayerKey(Main::Session *session, const QString &url, QSize size) {
	return QString::number(quintptr(session), 16)
		+ u'|' + QString::number(size.width())
		+ u'x' + QString::number(size.height())
		+ u'|' + url;
}

[[nodiscard]] QByteArray ReadSticker(const QString &path, const QByteArray &bytes) {
	if (path.isEmpty()) {
		return bytes;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxStickerFileBytes) {
		return QByteArray();
	}
	return file.readAll();
}

void LogNoLottie() {
	static auto logged = std::atomic<bool>(false);
	if (!logged.exchange(true)) {
		qInfo("Stickers: a Lottie sticker, but this build has no rlottie "
			"(its emoji is shown).");
	}
}

} // namespace

struct StickerPlayer::Shared {
	std::mutex mutex;
	std::unique_ptr<Data::StickerAnimation> animation;
};

std::shared_ptr<StickerPlayer> StickerPlayer::Get(
		Main::Session *session,
		const QString &url,
		QSize size) {
	if (!session || url.isEmpty() || size.isEmpty()) {
		return nullptr;
	}
	auto &players = Players();
	const auto key = PlayerKey(session, url, size);
	if (auto existing = players.value(key).lock()) {
		return existing;
	}
	for (auto i = players.begin(); i != players.end();) {
		i = i->expired() ? players.erase(i) : std::next(i);
	}
	auto result = std::shared_ptr<StickerPlayer>(new StickerPlayer(session, url, size));
	players.insert(key, result);
	result->start();
	return result;
}

int StickerPlayer::PlayingCount() {
	return Playing;
}

StickerPlayer::StickerPlayer(Main::Session *session, const QString &url, QSize size)
: _session(session)
, _url(url)
, _size(size)
, _shared(std::make_shared<Shared>()) {
	_timer.setSingleShot(true);
	connect(&_timer, &QTimer::timeout, this, [this] { tick(); });
}

StickerPlayer::~StickerPlayer() {
	stopPlaying();
}

void StickerPlayer::start() {
	if (!_session) {
		failedToLoad();
		return;
	}
	auto options = Data::Media::Options();
	options.decode = false;
	_handle = _session->data().media().request(
		Data::MediaRef::Url(_url),
		Data::MediaVariant::Full,
		options);
	if (_handle->isReady()) {
		loaded();
	} else if (_handle->isFailed()) {
		failedToLoad();
	} else {
		connect(_handle.get(), &Data::MediaHandle::ready, this, [this] { loaded(); });
		connect(_handle.get(), &Data::MediaHandle::failed, this, [this] { failedToLoad(); });
	}
}

void StickerPlayer::loaded() {
	const auto path = _handle->filePath();
	const auto bytes = _handle->bytes();
	const auto size = _size;
	const auto shared = _shared;
	const auto weak = QPointer<StickerPlayer>(this);
	Pool().start([=] {
		const auto data = ReadSticker(path, bytes);
		const auto format = Data::SniffStickerFormat(data.left(32));
		auto animation = std::unique_ptr<Data::StickerAnimation>();
		auto first = QImage();
		auto delay = 0;
		if (format != Data::StickerFormat::Still) {
			animation = Data::OpenStickerAnimation(data, size);
			if (animation) {
				first = animation->renderFrame(0, &delay);
				if (first.isNull()) {
					animation = nullptr;
				}
			} else if (format == Data::StickerFormat::Lottie
				&& !Data::LottieSupported()) {
				LogNoLottie();
			}
		}
		if (!animation && format != Data::StickerFormat::Lottie) {
			// A still picture, or the first frame of a WebP that cannot move.
			first = Data::DecodeImage(QString(), data, size);
		}
		// The animation goes back to the main thread inside a holder: the
		// lambda must stay copyable.
		const auto holder = std::make_shared<std::unique_ptr<Data::StickerAnimation>>(
			std::move(animation));
		QMetaObject::invokeMethod(qApp, [=] {
			if (!weak) {
				return;
			} else if (first.isNull()) {
				weak->failedToLoad();
			} else {
				weak->opened(std::move(*holder), first, delay);
			}
		}, Qt::QueuedConnection);
	});
}

void StickerPlayer::opened(
		std::unique_ptr<Data::StickerAnimation> animation,
		const QImage &first,
		int delayMs) {
	_handle = nullptr; // the bytes are not needed any more
	_first = _current = first;
	_currentIndex = 0;
	if (animation) {
		_count = animation->frameCount();
		_delays.assign(_count, 0);
		_delays[0] = delayMs;
		const auto frameBytes = qint64(first.sizeInBytes());
		_caching = (frameBytes * _count <= kFrameCacheBytes);
		if (_caching) {
			_cache.assign(_count, QImage());
			_cache[0] = first;
		}
		{
			const auto lock = std::lock_guard(_shared->mutex);
			_shared->animation = std::move(animation);
		}
	}
	Q_EMIT updated();
}

void StickerPlayer::failedToLoad() {
	_handle = nullptr;
	_failed = true;
	Q_EMIT updated();
}

QImage StickerPlayer::frame(bool play) {
	if (!play) {
		return _first;
	}
	_lastShown = QDateTime::currentMSecsSinceEpoch();
	ensurePlaying();
	return _current;
}

void StickerPlayer::ensurePlaying() {
	if (_playing || !animated() || Ui::AnimationsDisabled()) {
		return;
	} else if (Playing >= kMaxPlaying) {
		return; // tried again at the next paint
	}
	++Playing;
	_playing = true;
	requestFrame((_currentIndex + 1) % _count);
	_timer.start(delayOf(_currentIndex));
}

void StickerPlayer::stopPlaying() {
	_timer.stop();
	if (_playing) {
		_playing = false;
		--Playing;
	}
}

int StickerPlayer::delayOf(int index) const {
	const auto value = (index >= 0 && index < int(_delays.size()))
		? _delays[index]
		: 0;
	return (value > 0) ? value : 40;
}

void StickerPlayer::tick() {
	if (!_playing) {
		return;
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - _lastShown > std::max(kHiddenAfterMs, 2 * qint64(delayOf(_currentIndex)))) {
		// Not painted any more: scrolled away, hidden, closed.
		stopPlaying();
		return;
	}
	const auto index = (_currentIndex + 1) % _count;
	auto image = QImage();
	if (_caching && !_cache[index].isNull()) {
		image = _cache[index];
	} else if (_nextIndex == index && !_next.isNull()) {
		image = std::exchange(_next, QImage());
		_nextIndex = -1;
	}
	if (image.isNull()) {
		// Late: shown as soon as it is rendered.
		requestFrame(index);
		_timer.start(10);
		return;
	}
	_currentIndex = index;
	_current = image;
	Q_EMIT updated();
	requestFrame((index + 1) % _count);
	_timer.start(delayOf(index));
}

void StickerPlayer::requestFrame(int index) {
	if (_decodingIndex >= 0
		|| (_caching && !_cache[index].isNull())
		|| (_nextIndex == index && !_next.isNull())) {
		return;
	}
	_decodingIndex = index;
	const auto shared = _shared;
	const auto weak = QPointer<StickerPlayer>(this);
	Pool().start([=] {
		auto image = QImage();
		auto delay = 0;
		{
			const auto lock = std::lock_guard(shared->mutex);
			if (shared->animation) {
				image = shared->animation->renderFrame(index, &delay);
			}
		}
		QMetaObject::invokeMethod(qApp, [=] {
			if (weak) {
				weak->frameReady(index, image, delay);
			}
		}, Qt::QueuedConnection);
	});
}

void StickerPlayer::frameReady(int index, const QImage &image, int delayMs) {
	_decodingIndex = -1;
	if (image.isNull()) {
		// A broken frame: the animation stops where it is.
		stopPlaying();
		_count = 1;
		return;
	}
	if (index >= 0 && index < int(_delays.size())) {
		_delays[index] = delayMs;
	}
	if (_caching) {
		_cache[index] = image;
	} else {
		_next = image;
		_nextIndex = index;
	}
	// Waiting for this one (late): it goes at once.
	if (_playing && index == (_currentIndex + 1) % _count && _timer.remainingTime() <= 10) {
		_timer.start(0);
	}
}

} // namespace Support
