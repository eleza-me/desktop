// Media::AudioPlayer — the app's one audio player: chat songs, voice
// messages and the sound of Eli posts. Play / pause / seek, the voice speed
// (1× / 1.5× / 2×), a queue with ⏮ ⏭, repeat and shuffle, position updates
// for whoever shows the track.
//
//   auto track = Media::AudioTrack::ForMessage(message);     // a chat song or voice
//   Media::Player().toggle(track, &session);                  // play, or pause / resume if current
//   Media::Player().toggle(track, &session, songsOfTheTab);   // … with the queue it belongs to
//
//   track.filePath = path;                                    // … or a local file
//
//   connect(&Media::Player(), &Media::AudioPlayer::stateChanged, this, [=] { update(); });
//   connect(&Media::Player(), &Media::AudioPlayer::positionChanged, this, [=](qint64 position, qint64 duration) { ... });
//   if (Media::Player().isCurrent(track.id)) { progress = Media::Player().progress(); }
//
// The queue (Telegram's): a chat song plays with the chat's songs, newest
// first (⏭ is the older song above in the chat, and goes round to the first
// after the last); older ones page in from the chat's media pages as the end
// comes near — straight from the server's messages, the chat's cache is not
// needed. Without a queue handed, a chat song takes the songs the chat has
// loaded. A voice message goes on to the next voice below it when it ends
// (and marks it played), never round. An Eli sound plays alone.
//
// A track that fails to play is tried again before the player gives up: the
// same file in a fresh player (where it was), then the server's copy instead
// of the cached one. `failed` says why it gave up: the file is gone (the
// server answered 404 / 403 / 410), it could not be downloaded, or it could
// not be played — only the first is «فایل در دسترس نیست».
//
// A second track replaces the first; playing a video with sound pauses the
// audio. Volume, the voice speed, repeat and shuffle are remembered in
// Core::Settings.
#pragma once

#include "data/data_media.h"
#include "media/media_playlist.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVector>

#include <memory>

class QAudioOutput;
class QMediaPlayer;

namespace Data {
struct Message;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Media {

struct AudioTrack {
	QString id; // unique for the playable thing
	QString filePath; // a local file, or …
	Data::MediaRef ref; // … media from the session's cache
	Data::MediaVariant variant = Data::MediaVariant::Audio;
	QString title;
	QString performer;
	qint64 durationMs = 0; // known from the message, before the file is open
	bool voice = false;

	// A message's track: where it is (its chat, seq), who sent it.
	QString chat;
	qint64 seq = 0;
	QString sender;
	// An incoming voice nobody played yet: «played» when the queue gets to it.
	bool unplayed = false;

	// The post of an Eli sound ("" otherwise).
	[[nodiscard]] QString postId() const;

	[[nodiscard]] static QString IdForMessage(const QString &chat, qint64 seq);
	[[nodiscard]] static QString IdForPost(
		const QString &postId,
		const QString &mediaId);
	// The track of a voice message or a song of a chat (a local file while
	// it is being sent); an empty id when there is nothing to play.
	[[nodiscard]] static AudioTrack ForMessage(const Data::Message &message);
};

enum class AudioFailure {
	Gone, // the server says the file is not there (404 / 403 / 410)
	Download, // it could not be downloaded now
	Playback, // it is here but could not be played
};

class AudioPlayer final : public QObject {
	Q_OBJECT

public:
	enum class State {
		Stopped,
		Loading, // the file is being fetched
		Playing,
		Paused,
	};

	explicit AudioPlayer(QObject *parent = nullptr);
	~AudioPlayer();

	// Starts the track from the beginning (or from where it was paused when
	// it is the current one). `queue`: the songs it belongs to, in the
	// queue's order (newest first for a chat), the track among them.
	void play(
		const AudioTrack &track,
		Main::Session *session = nullptr,
		QVector<AudioTrack> queue = {});
	// play() / pause() / resume() depending on what the track is doing.
	void toggle(
		const AudioTrack &track,
		Main::Session *session = nullptr,
		QVector<AudioTrack> queue = {});
	void pause();
	void resume();
	// Stops and forgets the queue.
	void stop();
	// 0..1 of the duration.
	void seek(double progress);
	void seekMs(qint64 position);

	// ⏭ / ⏮ (⏮ after the first seconds goes back to the start).
	bool next();
	void previous();

	// The queue in the order it plays.
	[[nodiscard]] int queueSize() const {
		return _order.size();
	}
	[[nodiscard]] const AudioTrack &queueTrack(int position) const;
	[[nodiscard]] int queuePosition() const {
		return _order.position();
	}
	void playAt(int position);
	// Older songs of the chat, page by page.
	[[nodiscard]] bool queueHasMore() const {
		return _queueMore;
	}
	[[nodiscard]] bool queueLoading() const {
		return _queueLoading;
	}
	void loadMoreQueue();

	[[nodiscard]] RepeatMode repeat() const {
		return _repeat;
	}
	void setRepeat(RepeatMode mode);
	[[nodiscard]] bool shuffled() const {
		return _shuffle;
	}
	void setShuffled(bool shuffled);
	// Oldest first instead of newest first.
	[[nodiscard]] bool reversed() const {
		return _reverse;
	}
	void setReversed(bool reversed);

	[[nodiscard]] const AudioTrack &current() const {
		return _track;
	}
	[[nodiscard]] State state() const {
		return _state;
	}
	[[nodiscard]] bool isCurrent(const QString &id) const {
		return (_state != State::Stopped) && (_track.id == id);
	}
	[[nodiscard]] bool isPlaying(const QString &id) const {
		return isCurrent(id) && (_state == State::Playing);
	}
	[[nodiscard]] bool isLoading(const QString &id) const {
		return isCurrent(id) && (_state == State::Loading);
	}
	[[nodiscard]] qint64 positionMs() const;
	[[nodiscard]] qint64 durationMs() const;
	[[nodiscard]] double progress() const;
	// 0..1 of the download while Loading.
	[[nodiscard]] double loadProgress() const;

	// 1, 1.5 or 2. Applies to voice tracks; music always plays at 1×.
	[[nodiscard]] double speed() const {
		return _speed;
	}
	void setSpeed(double speed);
	// 1 -> 1.5 -> 2 -> 1; returns the new value.
	double cycleSpeed();
	[[nodiscard]] double volume() const;
	void setVolume(double volume);

#ifdef ELEZA_DEBUG_TOOLS
	// The track as if it was paused at `progress`, nothing opened (grabs of
	// the bar and the music player without an audio device).
	void debugShow(
		const AudioTrack &track,
		QVector<AudioTrack> queue,
		Main::Session *session);
#endif // ELEZA_DEBUG_TOOLS

Q_SIGNALS:
	// Another track became current (or none).
	void trackChanged();
	void stateChanged();
	void positionChanged(qint64 position, qint64 duration);
	void speedChanged(double speed);
	// The queue, its order or repeat changed.
	void queueChanged();
	// The current track played to its end.
	void finished(const QString &id);
	void failed(const QString &id, Media::AudioFailure reason);

private:
	void ensurePlayer();
	void destroyPlayer();
	void startTrack(const AudioTrack &track, bool advanced);
	void startAt(int queueIndex, bool advanced);
	void request(bool force);
	void start(const QString &filePath);
	void stopPlayback();
	void setState(State state);
	void fail(AudioFailure reason);
	void recover();
	void ended();
	void applyRate();
	void maybeLoadMoreQueue();
	void buildQueue(const AudioTrack &track, QVector<AudioTrack> queue);

	std::unique_ptr<QMediaPlayer> _player;
	std::unique_ptr<QAudioOutput> _output;
	AudioTrack _track;
	Data::MediaHandlePtr _handle;
	QString _temporaryFile;
	QString _path; // what the player has open
	State _state = State::Stopped;
	double _speed = 1.;
	qint64 _pendingSeek = -1;
	int _attempt = 0; // the tries of the current track after a failure
	bool _recovering = false;
	int _generation = 0; // of the current track

	QPointer<Main::Session> _session;
	QVector<AudioTrack> _queue; // the queue's order (newest first for a chat)
	PlayOrder _order;
	RepeatMode _repeat = RepeatMode::Off;
	bool _shuffle = false;
	bool _reverse = false; // oldest first (for this run of the app)
	bool _songs = false; // a chat's songs (paged, repeat, shuffle)
	bool _queueMore = false;
	bool _queueLoading = false;
	int _queueGeneration = 0;

};

// The application's player.
[[nodiscard]] AudioPlayer &Player();

} // namespace Media
