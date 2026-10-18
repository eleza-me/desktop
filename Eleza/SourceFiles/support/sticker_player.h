// A sticker for custom-painted views: a still picture, an animated WebP or a
// Lottie animation (Data::SniffStickerFormat tells them apart), decoded and
// rendered off the main thread at the size it is shown at.
//
//   _player = Support::StickerPlayer::Get(&session, sticker.url, rect.size() * ratio);
//   connect(_player.get(), &Support::StickerPlayer::updated, this, [=] { update(); });
//   ... in paintEvent:
//   const auto frame = _player->frame();   // null: paint the emoji instead
//
// The cost is kept low on purpose:
// * an animation moves only while it is painted: a view that is scrolled away
//   or hidden stops asking for frame() and the player pauses by itself;
// * at most kMaxPlaying animations move at a time (the others show their
//   current frame until a slot is free);
// * frames are rendered at the shown size, all of them kept when a loop fits
//   into kFrameCacheBytes (then a loop costs nothing after the first one);
// * views showing the same sticker at the same size share one player;
// * with animations off (Ui::AnimationsDisabled: tests, screenshots) the
//   first frame stays.
#pragma once

#include "data/data_media.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Data {
class StickerAnimation;
} // namespace Data

namespace Support {

class StickerPlayer final : public QObject {
	Q_OBJECT

public:
	static constexpr auto kMaxPlaying = 12;
	static constexpr auto kFrameCacheBytes = qint64(6) << 20;

	// `size`: device pixels the sticker is painted at (the frames fit it).
	[[nodiscard]] static std::shared_ptr<StickerPlayer> Get(
		Main::Session *session,
		const QString &url,
		QSize size);

	~StickerPlayer();

	// What to paint now; null while loading or when there is no picture (a
	// Lottie sticker in a build without rlottie, a broken file). `play`:
	// this view lets it move (false: a pack's cover, always the first frame).
	[[nodiscard]] QImage frame(bool play = true);
	[[nodiscard]] bool animated() const {
		return _count > 1;
	}
	[[nodiscard]] bool failed() const {
		return _failed;
	}
	[[nodiscard]] QSize size() const {
		return _size;
	}

	// How many animations move right now (all players).
	[[nodiscard]] static int PlayingCount();

Q_SIGNALS:
	void updated();

private:
	struct Shared;

	StickerPlayer(Main::Session *session, const QString &url, QSize size);

	void start();
	void loaded();
	void opened(
		std::unique_ptr<Data::StickerAnimation> animation,
		const QImage &first,
		int delayMs);
	void failedToLoad();
	void ensurePlaying();
	void stopPlaying();
	void tick();
	void requestFrame(int index);
	void frameReady(int index, const QImage &image, int delayMs);
	[[nodiscard]] int delayOf(int index) const;

	QPointer<Main::Session> _session;
	QString _url;
	QSize _size;
	Data::MediaHandlePtr _handle;
	std::shared_ptr<Shared> _shared;

	QImage _first;
	QImage _current;
	int _currentIndex = 0;
	int _count = 0;
	std::vector<int> _delays;
	std::vector<QImage> _cache; // all the frames, when they fit
	bool _caching = false;
	QImage _next;
	int _nextIndex = -1;
	int _decodingIndex = -1;
	bool _failed = false;
	bool _playing = false;
	qint64 _lastShown = 0;
	QTimer _timer;

};

} // namespace Support
