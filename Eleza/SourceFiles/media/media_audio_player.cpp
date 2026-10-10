#include "media/media_audio_player.h"

#include "base/platform_file.h"
#include "core/core_application.h"
#include "data/data_session.h"
#include "main/main_session.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>

namespace Media {

QString AudioTrack::IdForMessage(const QString &chat, qint64 seq) {
	return QStringLiteral("chat:%1:%2").arg(chat).arg(seq);
}

QString AudioTrack::IdForPost(const QString &postId, const QString &mediaId) {
	return QStringLiteral("post:%1:%2").arg(postId, mediaId);
}

AudioPlayer::AudioPlayer(QObject *parent)
: QObject(parent)
, _speed(Core::Application::Exists()
	? Core::App().settings().voicePlaybackSpeed()
	: 1.) {
}

AudioPlayer::~AudioPlayer() {
	if (!_temporaryFile.isEmpty()) {
		QFile::remove(_temporaryFile);
	}
}

void AudioPlayer::ensurePlayer() {
	if (_player) {
		return;
	}
	_output = std::make_unique<QAudioOutput>();
	_output->setVolume(float(volume()));
	_player = std::make_unique<QMediaPlayer>();
	_player->setAudioOutput(_output.get());
	QObject::connect(
		_player.get(),
		&QMediaPlayer::positionChanged,
		this,
		[this](qint64 position) {
			if (_state == State::Playing || _state == State::Paused) {
				emit positionChanged(position, durationMs());
			}
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::durationChanged,
		this,
		[this](qint64) {
			emit positionChanged(positionMs(), durationMs());
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::mediaStatusChanged,
		this,
		[this](QMediaPlayer::MediaStatus status) {
			if (status == QMediaPlayer::EndOfMedia) {
				const auto id = _track.id;
				stop();
				emit finished(id);
			} else if (status == QMediaPlayer::InvalidMedia) {
				fail();
			} else if ((status == QMediaPlayer::LoadedMedia
					|| status == QMediaPlayer::BufferedMedia)
				&& _pendingSeek >= 0) {
				_player->setPosition(std::exchange(_pendingSeek, -1));
			}
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::errorOccurred,
		this,
		[this](QMediaPlayer::Error error, const QString &text) {
			if (error != QMediaPlayer::NoError) {
				qWarning("Media: audio playback failed: %s", qPrintable(text));
				fail();
			}
		});
}

void AudioPlayer::setState(State state) {
	if (_state != state) {
		_state = state;
		emit stateChanged();
	}
}

void AudioPlayer::fail() {
	if (_state == State::Stopped) {
		return;
	}
	const auto id = _track.id;
	stop();
	emit failed(id);
}

void AudioPlayer::play(const AudioTrack &track, Main::Session *session) {
	if (track.id.isEmpty()) {
		return;
	}
	if (_state != State::Stopped && _track.id == track.id) {
		resume();
		return;
	}
	stop();
	_track = track;
	emit trackChanged();
	if (!track.filePath.isEmpty()) {
		start(track.filePath);
		return;
	} else if (!session || !track.ref.valid()) {
		fail();
		return;
	}
	// The bytes come through the media cache (no decoding: it is not an image).
	auto options = Data::Media::Options();
	options.decode = false;
	_handle = session->data().media().request(track.ref, track.variant, options);
	const auto id = track.id;
	const auto ready = [this, id] {
		if (_track.id != id || !_handle || _state != State::Loading) {
			return;
		}
		auto path = _handle->filePath();
		if (path.isEmpty() && !_handle->bytes().isEmpty()) {
			// No disk cache: the player needs a file.
			path = QDir(base::platform::PrivateTempDirectory()).filePath(
				QStringLiteral("eleza-audio-")
					+ QUuid::createUuid().toString(QUuid::WithoutBraces));
			auto file = QFile(path);
			if (!file.open(QIODevice::WriteOnly)
				|| file.write(_handle->bytes()) != _handle->bytes().size()) {
				fail();
				return;
			}
			file.close();
			if (!_temporaryFile.isEmpty()) {
				QFile::remove(_temporaryFile);
			}
			_temporaryFile = path;
		}
		if (path.isEmpty()) {
			fail();
		} else {
			start(path);
		}
	};
	setState(State::Loading);
	if (_handle->isReady()) {
		ready();
	} else if (_handle->isFailed()) {
		fail();
	} else {
		QObject::connect(_handle.get(), &Data::MediaHandle::ready, this, ready);
		QObject::connect(_handle.get(), &Data::MediaHandle::failed, this, [this, id] {
			if (_track.id == id) {
				fail();
			}
		});
	}
}

void AudioPlayer::start(const QString &filePath) {
	ensurePlayer();
	_player->setSource(QUrl::fromLocalFile(filePath));
	applyRate();
	_player->play();
	setState(State::Playing);
}

void AudioPlayer::toggle(const AudioTrack &track, Main::Session *session) {
	if (_state == State::Stopped || _track.id != track.id) {
		play(track, session);
	} else if (_state == State::Playing) {
		pause();
	} else if (_state == State::Paused) {
		resume();
	} else {
		stop(); // a second click while loading cancels
	}
}

void AudioPlayer::pause() {
	if (_state == State::Playing && _player) {
		_player->pause();
		setState(State::Paused);
	}
}

void AudioPlayer::resume() {
	if (_state == State::Paused && _player) {
		_player->play();
		setState(State::Playing);
	}
}

void AudioPlayer::stop() {
	if (_state == State::Stopped && _track.id.isEmpty()) {
		return;
	}
	if (_player) {
		_player->stop();
		_player->setSource(QUrl());
	}
	_handle = nullptr;
	_pendingSeek = -1;
	const auto had = !_track.id.isEmpty();
	_track = AudioTrack();
	setState(State::Stopped);
	if (had) {
		emit trackChanged();
	}
}

void AudioPlayer::seek(double progress) {
	const auto duration = durationMs();
	if (duration > 0) {
		seekMs(qint64(std::clamp(progress, 0., 1.) * duration));
	}
}

void AudioPlayer::seekMs(qint64 position) {
	if (_state == State::Loading) {
		_pendingSeek = position;
	} else if (_player && _state != State::Stopped) {
		_player->setPosition(std::max(position, qint64(0)));
		emit positionChanged(positionMs(), durationMs());
	}
}

qint64 AudioPlayer::positionMs() const {
	return (_player && _state != State::Stopped && _state != State::Loading)
		? _player->position()
		: 0;
}

qint64 AudioPlayer::durationMs() const {
	const auto real = (_player && _state != State::Stopped)
		? _player->duration()
		: 0;
	return (real > 0) ? real : _track.durationMs;
}

double AudioPlayer::progress() const {
	const auto duration = durationMs();
	return (duration > 0)
		? std::clamp(positionMs() / double(duration), 0., 1.)
		: 0.;
}

void AudioPlayer::applyRate() {
	if (_player) {
		_player->setPlaybackRate(_track.voice ? _speed : 1.);
	}
}

void AudioPlayer::setSpeed(double speed) {
	if (speed != 1.5 && speed != 2.) {
		speed = 1.;
	}
	if (_speed == speed) {
		return;
	}
	_speed = speed;
	if (Core::Application::Exists()) {
		Core::App().settings().setVoicePlaybackSpeed(speed);
	}
	applyRate();
	emit speedChanged(speed);
}

double AudioPlayer::cycleSpeed() {
	setSpeed((_speed == 1.) ? 1.5 : (_speed == 1.5) ? 2. : 1.);
	return _speed;
}

double AudioPlayer::volume() const {
	return Core::Application::Exists()
		? Core::App().settings().playbackVolume()
		: 1.;
}

void AudioPlayer::setVolume(double volume) {
	volume = std::clamp(volume, 0., 1.);
	if (Core::Application::Exists()) {
		Core::App().settings().setPlaybackVolume(volume);
	}
	if (_output) {
		_output->setVolume(float(volume));
	}
}

AudioPlayer &Player() {
	static auto instance = QPointer<AudioPlayer>();
	if (!instance) {
		// Owned by the application object: destroyed before the event loop is.
		instance = new AudioPlayer(QCoreApplication::instance());
	}
	return *instance;
}

} // namespace Media
