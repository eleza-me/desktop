// Media::AudioPlayer — one audio track playing at a time (voice messages, music,
// the sound of a post): play / pause / seek, 1× / 1.5× / 2× speed, position
// updates for the waveform or the seek bar of whoever shows the track.
//
//   auto track = Media::AudioTrack();
//   track.id = Media::AudioTrack::IdForMessage(chatId, seq);
//   track.ref = Data::MediaRef::Attachment(chatId, seq);     // fetched through the media cache
//   track.voice = true;                                       // uses the remembered voice speed
//   Media::Player().toggle(track, &session);                  // play, or pause / resume if current
//
//   track.filePath = path;                                    // … or a local file
//
//   connect(&Media::Player(), &Media::AudioPlayer::stateChanged, this, [=] { update(); });
//   connect(&Media::Player(), &Media::AudioPlayer::positionChanged, this, [=](qint64 position, qint64 duration) { ... });
//   if (Media::Player().isCurrent(track.id)) { progress = Media::Player().progress(); }
//
// A second track replaces the first; playing a video with sound pauses the audio.
// Volume and the voice speed are remembered in Core::Settings.
#pragma once

#include "data/data_media.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <memory>

class QAudioOutput;
class QMediaPlayer;

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

	[[nodiscard]] static QString IdForMessage(const QString &chat, qint64 seq);
	[[nodiscard]] static QString IdForPost(
		const QString &postId,
		const QString &mediaId);
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
	// it is the current one).
	void play(const AudioTrack &track, Main::Session *session = nullptr);
	// play() / pause() / resume() depending on what the track is doing.
	void toggle(const AudioTrack &track, Main::Session *session = nullptr);
	void pause();
	void resume();
	void stop();
	// 0..1 of the duration.
	void seek(double progress);
	void seekMs(qint64 position);

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

	// 1, 1.5 or 2. Applies to voice tracks; music always plays at 1×.
	[[nodiscard]] double speed() const {
		return _speed;
	}
	void setSpeed(double speed);
	// 1 -> 1.5 -> 2 -> 1; returns the new value.
	double cycleSpeed();
	[[nodiscard]] double volume() const;
	void setVolume(double volume);

Q_SIGNALS:
	// Another track became current (or none).
	void trackChanged();
	void stateChanged();
	void positionChanged(qint64 position, qint64 duration);
	void speedChanged(double speed);
	// The current track played to its end.
	void finished(const QString &id);
	void failed(const QString &id);

private:
	void ensurePlayer();
	void start(const QString &filePath);
	void setState(State state);
	void fail();
	void applyRate();

	std::unique_ptr<QMediaPlayer> _player;
	std::unique_ptr<QAudioOutput> _output;
	AudioTrack _track;
	Data::MediaHandlePtr _handle;
	QString _temporaryFile;
	State _state = State::Stopped;
	double _speed = 1.;
	qint64 _pendingSeek = -1;

};

// The application's player.
[[nodiscard]] AudioPlayer &Player();

} // namespace Media
