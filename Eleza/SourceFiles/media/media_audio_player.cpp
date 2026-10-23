#include "media/media_audio_player.h"

#include "base/platform_file.h"
#include "core/core_application.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "data/data_shared_media.h"
#include "data/data_types.h"
#include "lang_auto.h"
#include "main/main_session.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>

#include <limits>

namespace Media {
namespace {

using namespace Qt::StringLiterals;

// ⏮ within the first seconds goes to the song before, later to the start.
constexpr auto kPreviousRestartMs = 3000;
// Older songs are asked for while this few are left to play.
constexpr auto kQueuePreload = 2;

[[nodiscard]] bool Gone(const Api::Error &error) {
	return (error.status == 404) || (error.status == 403) || (error.status == 410);
}

// The songs the chat has loaded, newest first.
[[nodiscard]] QVector<AudioTrack> ChatSongs(
		Main::Session *session,
		const QString &chat) {
	auto result = QVector<AudioTrack>();
	const auto &items = session->data().messages().history(chat).items;
	for (auto i = items.size(); i != 0;) {
		const auto &message = items[--i];
		if (message.kind == Data::MessageKind::Audio
			&& message.seq > 0
			&& message.visible()) {
			result.push_back(AudioTrack::ForMessage(message));
		}
	}
	return result;
}

// The voice and the voices below it in the chat, in their order.
[[nodiscard]] QVector<AudioTrack> VoicesFrom(
		Main::Session *session,
		const AudioTrack &track) {
	auto result = QVector<AudioTrack>{ track };
	const auto &items = session->data().messages().history(track.chat).items;
	for (const auto &message : items) {
		if (message.kind == Data::MessageKind::Voice
			&& message.seq > track.seq
			&& message.visible()) {
			result.push_back(AudioTrack::ForMessage(message));
		}
	}
	return result;
}

} // namespace

QString AudioTrack::IdForMessage(const QString &chat, qint64 seq) {
	return QStringLiteral("chat:%1:%2").arg(chat).arg(seq);
}

QString AudioTrack::IdForPost(const QString &postId, const QString &mediaId) {
	return QStringLiteral("post:%1:%2").arg(postId, mediaId);
}

QString AudioTrack::postId() const {
	return id.startsWith(u"post:"_s) ? id.section(u':', 1, 1) : QString();
}

AudioTrack AudioTrack::ForMessage(const Data::Message &message) {
	auto track = AudioTrack();
	const auto local = !message.localPath.isEmpty()
		&& QFileInfo::exists(message.localPath);
	if (message.seq > 0) {
		track.id = IdForMessage(message.chat, message.seq);
		track.ref = Data::MediaRef::Attachment(message.chat, message.seq);
		track.chat = message.chat;
		track.seq = message.seq;
	} else if (local) {
		track.id = u"local:"_s + message.clientMessageId;
	} else {
		return track;
	}
	if (local) {
		// My own file plays from the disk, not downloaded back.
		track.filePath = message.localPath;
	}
	track.voice = (message.kind == Data::MessageKind::Voice);
	track.durationMs = message.media ? message.media->durationMs : 0;
	track.sender = message.sender;
	track.unplayed = track.voice && !message.mine && !message.played;
	if (message.audio) {
		track.title = message.audio->title;
		track.performer = message.audio->performer;
	}
	if (track.title.isEmpty()) {
		track.title = track.voice
			? tr::lng_media_voice(tr::now)
			: message.text.trimmed();
	}
	return track;
}

AudioPlayer::AudioPlayer(QObject *parent)
: QObject(parent)
, _speed(Core::Application::Exists()
	? Core::App().settings().voicePlaybackSpeed()
	: 1.) {
	if (Core::Application::Exists()) {
		const auto &settings = Core::App().settings();
		_repeat = RepeatMode(std::clamp(settings.musicRepeat(), 0, 2));
		_shuffle = settings.musicShuffle();
		_reverse = settings.musicReversed();
	}
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
	const auto raw = _player.get();
	QObject::connect(
		raw,
		&QMediaPlayer::positionChanged,
		this,
		[this](qint64 position) {
			if (_state == State::Playing || _state == State::Paused) {
				emit positionChanged(position, durationMs());
			}
		});
	QObject::connect(
		raw,
		&QMediaPlayer::durationChanged,
		this,
		[this](qint64) {
			emit positionChanged(positionMs(), durationMs());
		});
	QObject::connect(
		raw,
		&QMediaPlayer::mediaStatusChanged,
		this,
		[this](QMediaPlayer::MediaStatus status) {
			if (status == QMediaPlayer::EndOfMedia) {
				// Not inside the player's own signal: the next track may
				// replace the player.
				const auto generation = _generation;
				QTimer::singleShot(0, this, [=, this] {
					if (_generation == generation) {
						ended();
					}
				});
			} else if (status == QMediaPlayer::InvalidMedia) {
				recover();
			} else if ((status == QMediaPlayer::LoadedMedia
					|| status == QMediaPlayer::BufferedMedia)
				&& _pendingSeek >= 0) {
				_player->setPosition(std::exchange(_pendingSeek, -1));
			}
		});
	QObject::connect(
		raw,
		&QMediaPlayer::errorOccurred,
		this,
		[this](QMediaPlayer::Error error, const QString &text) {
			if (error != QMediaPlayer::NoError) {
				// The text may name the file or its address.
#ifdef ELEZA_DEBUG_TOOLS
				qWarning("Media: audio playback failed: %s", qPrintable(text));
#else // ELEZA_DEBUG_TOOLS
				Q_UNUSED(text);
				qWarning("Media: audio playback failed: %d", int(error));
#endif // ELEZA_DEBUG_TOOLS
				recover();
			}
		});
}

void AudioPlayer::destroyPlayer() {
	if (_player) {
		_player->disconnect(this);
		_player->stop();
		// Deleting it inside one of its own signals is not safe.
		const auto player = _player.release();
		const auto output = _output.release();
		QTimer::singleShot(0, [=] {
			delete player;
			delete output;
		});
	}
}

void AudioPlayer::setState(State state) {
	if (_state != state) {
		_state = state;
		emit stateChanged();
	}
}

void AudioPlayer::fail(AudioFailure reason) {
	if (_state == State::Stopped) {
		return;
	}
	const auto id = _track.id;
	stop();
	emit failed(id, reason);
}

void AudioPlayer::recover() {
	if (_state == State::Stopped || _path.isEmpty() || _recovering) {
		// (InvalidMedia and the error of one failure come together.)
		return;
	}
	_recovering = true;
	const auto generation = _generation;
	const auto at = (_player && _player->position() > 0)
		? _player->position()
		: std::max(_pendingSeek, qint64(0));
	++_attempt;
	// Out of the failing player's signal first.
	QTimer::singleShot(0, this, [=, this] {
		_recovering = false;
		if (_generation != generation || _state == State::Stopped) {
			return;
		}
		destroyPlayer();
		if (_attempt == 1) {
			// The same file in a fresh player, where it was: a decoder that
			// was lost, a media service that restarted while paused.
			const auto path = _path;
			_pendingSeek = (at > 0) ? at : -1;
			start(path);
		} else if (_attempt == 2
			&& _track.filePath.isEmpty()
			&& _track.ref.valid()
			&& _session) {
			// A broken copy in the cache would come back every time: drop
			// it and fetch the server's.
			_session->data().media().evict(_track.ref, _track.variant);
			_pendingSeek = (at > 0) ? at : -1;
			request(true);
		} else {
			fail(AudioFailure::Playback);
		}
	});
}

void AudioPlayer::play(
		const AudioTrack &track,
		Main::Session *session,
		QVector<AudioTrack> queue) {
	if (track.id.isEmpty()) {
		return;
	}
	if (_state != State::Stopped && _track.id == track.id) {
		resume();
		return;
	}
	if (session) {
		_session = session;
	}
	buildQueue(track, std::move(queue));
	startTrack(track, false);
	maybeLoadMoreQueue();
}

void AudioPlayer::buildQueue(const AudioTrack &track, QVector<AudioTrack> queue) {
	++_queueGeneration;
	_queueLoading = false;
	const auto chat = !track.chat.isEmpty() && track.seq > 0 && _session;
	_songs = chat && !track.voice;
	if (queue.isEmpty() && chat) {
		queue = track.voice
			? VoicesFrom(_session, track)
			: ChatSongs(_session, track.chat);
	}
	auto index = -1;
	for (auto i = 0; i != int(queue.size()); ++i) {
		if (queue[i].id == track.id) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		queue.push_front(track);
		index = 0;
	}
	_queue = std::move(queue);
	_queueMore = _songs;
	_order = PlayOrder();
	// Reverse and shuffle are for a chat's songs only.
	_order.setReversed(_songs && _reverse);
	_order.setShuffled(_songs && _shuffle);
	_order.reset(int(_queue.size()), index);
	emit queueChanged();
}

void AudioPlayer::startTrack(const AudioTrack &track, bool advanced) {
	stopPlayback();
	_track = track;
	_attempt = 0;
	++_generation;
	emit trackChanged();
	if (advanced && track.unplayed && _session && track.seq > 0) {
		// The queue got to a voice nobody played yet.
		_session->data().messages().markPlayed(track.chat, track.seq);
		for (auto &entry : _queue) {
			if (entry.id == track.id) {
				entry.unplayed = false;
			}
		}
		_track.unplayed = false;
	}
	if (!track.filePath.isEmpty() && QFileInfo::exists(track.filePath)) {
		start(track.filePath);
		return;
	} else if (!_session || !track.ref.valid()) {
		setState(State::Loading);
		fail(AudioFailure::Gone);
		return;
	}
	request(false);
}

void AudioPlayer::request(bool force) {
	// The bytes come through the media cache (no decoding: it is not an image).
	auto options = Data::Media::Options();
	options.decode = false;
	options.force = force;
	_handle = _session->data().media().request(_track.ref, _track.variant, options);
	const auto generation = _generation;
	const auto handle = _handle.get();
	const auto ready = [this, generation, handle] {
		if (_generation != generation
			|| _handle.get() != handle
			|| _state != State::Loading) {
			return;
		}
		auto path = handle->filePath();
		if (path.isEmpty() && !handle->bytes().isEmpty()) {
			// No disk cache: the player needs a file.
			path = QDir(base::platform::PrivateTempDirectory()).filePath(
				QStringLiteral("eleza-audio-")
					+ QUuid::createUuid().toString(QUuid::WithoutBraces));
			auto file = QFile(path);
			if (!file.open(QIODevice::WriteOnly)
				|| file.write(handle->bytes()) != handle->bytes().size()) {
				fail(AudioFailure::Playback);
				return;
			}
			file.close();
			if (!_temporaryFile.isEmpty()) {
				QFile::remove(_temporaryFile);
			}
			_temporaryFile = path;
		}
		if (path.isEmpty()) {
			fail(AudioFailure::Download);
		} else {
			start(path);
		}
	};
	const auto failed = [this, generation, handle] {
		if (_generation == generation && _handle.get() == handle) {
			// Only the server saying so makes it «unavailable».
			fail(Gone(handle->error())
				? AudioFailure::Gone
				: AudioFailure::Download);
		}
	};
	setState(State::Loading);
	if (handle->isReady()) {
		ready();
	} else if (handle->isFailed()) {
		failed();
	} else {
		QObject::connect(handle, &Data::MediaHandle::ready, this, ready);
		QObject::connect(handle, &Data::MediaHandle::failed, this, failed);
		QObject::connect(handle, &Data::MediaHandle::progressChanged, this, [=, this] {
			if (_generation == generation && _state == State::Loading) {
				emit positionChanged(0, durationMs());
			}
		});
	}
}

void AudioPlayer::start(const QString &filePath) {
	ensurePlayer();
	_path = filePath;
	_player->setSource(QUrl::fromLocalFile(filePath));
	applyRate();
	_player->play();
	setState(State::Playing);
}

void AudioPlayer::toggle(
		const AudioTrack &track,
		Main::Session *session,
		QVector<AudioTrack> queue) {
	if (_state == State::Stopped || _track.id != track.id) {
		play(track, session, std::move(queue));
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
	if (_state != State::Paused) {
		return;
	} else if (!_player || _player->error() != QMediaPlayer::NoError) {
		// A player that broke while paused: what recover() does.
		recover();
		return;
	}
	_player->play();
	setState(State::Playing);
}

void AudioPlayer::stopPlayback() {
	if (_player) {
		_player->stop();
		_player->setSource(QUrl());
	}
	_handle = nullptr;
	_pendingSeek = -1;
	_path = QString();
}

void AudioPlayer::stop() {
	if (_state == State::Stopped && _track.id.isEmpty()) {
		return;
	}
	stopPlayback();
	++_generation;
	++_queueGeneration;
	const auto had = !_track.id.isEmpty();
	_track = AudioTrack();
	_queue.clear();
	_order.reset(0, -1);
	_queueMore = _queueLoading = false;
	setState(State::Stopped);
	if (had) {
		emit trackChanged();
		emit queueChanged();
	}
}

void AudioPlayer::ended() {
	const auto id = _track.id;
	const auto next = _order.next(
		PlayOrder::Advance::Ended,
		_songs ? _repeat : RepeatMode::Off);
	emit finished(id);
	if (next && *next >= 0 && *next < int(_queue.size())) {
		if (_queue[*next].id == id) {
			// Repeat one: from the start.
			if (_player) {
				_player->setPosition(0);
				_player->play();
			}
			return;
		}
		startAt(*next, true);
	} else {
		stop();
	}
}

void AudioPlayer::startAt(int queueIndex, bool advanced) {
	if (queueIndex < 0 || queueIndex >= int(_queue.size())) {
		return;
	}
	_order.setCurrent(queueIndex);
	startTrack(_queue[queueIndex], advanced);
	emit queueChanged();
	maybeLoadMoreQueue();
}

bool AudioPlayer::next() {
	if (_state == State::Stopped || _order.size() < 2) {
		return false;
	}
	// A voice goes on downwards only; a song goes round at the end.
	const auto index = _songs
		? _order.next(PlayOrder::Advance::Manual, _repeat)
		: _order.next(PlayOrder::Advance::Ended, RepeatMode::Off);
	if (!index) {
		return false;
	}
	startAt(*index, !_songs);
	return true;
}

void AudioPlayer::previous() {
	if (_state == State::Stopped) {
		return;
	} else if (positionMs() > kPreviousRestartMs || _order.size() < 2) {
		seekMs(0);
		return;
	}
	const auto was = _order.current();
	const auto index = _order.previous(_songs ? _repeat : RepeatMode::Off);
	if (index == was) {
		seekMs(0);
	} else {
		startAt(index, false);
	}
}

const AudioTrack &AudioPlayer::queueTrack(int position) const {
	static const auto kEmpty = AudioTrack();
	const auto index = _order.at(position);
	return (index >= 0 && index < int(_queue.size())) ? _queue[index] : kEmpty;
}

void AudioPlayer::playAt(int position) {
	const auto index = _order.at(position);
	if (index >= 0 && index != _order.current()) {
		startAt(index, false);
	} else if (index >= 0 && _state == State::Paused) {
		resume();
	}
}

void AudioPlayer::maybeLoadMoreQueue() {
	if (_songs && _queueMore && !_queueLoading && _order.remaining() <= kQueuePreload) {
		loadMoreQueue();
	}
}

void AudioPlayer::loadMoreQueue() {
	if (!_songs || !_queueMore || _queueLoading || !_session || _queue.isEmpty()) {
		return;
	}
	auto oldest = std::numeric_limits<qint64>::max();
	for (const auto &track : std::as_const(_queue)) {
		if (track.seq > 0) {
			oldest = std::min(oldest, track.seq);
		}
	}
	if (oldest == std::numeric_limits<qint64>::max()) {
		_queueMore = false;
		return;
	}
	_queueLoading = true;
	const auto generation = _queueGeneration;
	const auto chat = _track.chat;
	emit queueChanged();
	Data::LoadSharedMedia(
		&_session->data(),
		chat,
		Data::SharedMediaKinds(Data::SharedMediaTab::Music),
		oldest,
		0,
		base::Guard(this),
		[=, this](const Data::SharedMediaPage &page) {
			if (_queueGeneration != generation) {
				return;
			}
			_queueLoading = false;
			_queueMore = page.hasMoreBefore;
			auto known = QSet<QString>();
			for (const auto &track : std::as_const(_queue)) {
				known.insert(track.id);
			}
			auto added = 0;
			for (const auto &message : page.messages) {
				auto track = AudioTrack::ForMessage(message);
				if (!track.id.isEmpty()
					&& message.seq < oldest
					&& !known.contains(track.id)) {
					known.insert(track.id);
					_queue.push_back(std::move(track));
					++added;
				}
			}
			_order.append(added);
			if (!added) {
				_queueMore = false;
			}
			emit queueChanged();
		},
		[=, this](const Api::Error &) {
			if (_queueGeneration != generation) {
				return;
			}
			// An older server, an error: the queue stays as it is.
			_queueLoading = false;
			_queueMore = false;
			emit queueChanged();
		});
}

void AudioPlayer::setRepeat(RepeatMode mode) {
	if (_repeat == mode) {
		return;
	}
	_repeat = mode;
	if (Core::Application::Exists()) {
		Core::App().settings().setMusicRepeat(int(mode));
	}
	emit queueChanged();
}

void AudioPlayer::setShuffled(bool shuffled) {
	if (_shuffle == shuffled) {
		return;
	}
	_shuffle = shuffled;
	if (_songs) {
		_order.setShuffled(shuffled);
	}
	if (Core::Application::Exists()) {
		Core::App().settings().setMusicShuffle(shuffled);
	}
	emit queueChanged();
}

void AudioPlayer::setReversed(bool reversed) {
	if (_reverse == reversed) {
		return;
	}
	_reverse = reversed;
	if (Core::Application::Exists()) {
		Core::App().settings().setMusicReversed(reversed);
	}
	if (_songs) {
		_order.setReversed(reversed);
	}
	emit queueChanged();
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

double AudioPlayer::loadProgress() const {
	return (_state == State::Loading && _handle) ? _handle->progress() : 0.;
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

#ifdef ELEZA_DEBUG_TOOLS
void AudioPlayer::debugShow(
		const AudioTrack &track,
		QVector<AudioTrack> queue,
		Main::Session *session) {
	stop();
	_session = session;
	buildQueue(track, std::move(queue));
	_queueMore = false;
	_track = track;
	++_generation;
	emit trackChanged();
	setState(State::Paused);
	emit queueChanged();
}
#endif // ELEZA_DEBUG_TOOLS

AudioPlayer &Player() {
	static auto instance = QPointer<AudioPlayer>();
	if (!instance) {
		// Owned by the application object: destroyed before the event loop is.
		instance = new AudioPlayer(QCoreApplication::instance());
	}
	return *instance;
}

} // namespace Media
