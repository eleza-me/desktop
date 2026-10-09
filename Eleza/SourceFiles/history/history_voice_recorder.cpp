#include "history/history_voice_recorder.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioInput>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QMediaFormat>
#include <QtMultimedia/QMediaRecorder>

namespace History {

VoiceRecorder::VoiceRecorder(QObject *parent)
: QObject(parent) {
}

VoiceRecorder::~VoiceRecorder() {
	if (_recording) {
		_keep = false;
		if (_recorder) {
			_recorder->disconnect(this);
			_recorder->stop();
		}
		cleanup(true);
	}
}

bool VoiceRecorder::Available() {
	return !QMediaDevices::audioInputs().isEmpty();
}

qint64 VoiceRecorder::durationMs() const {
	if (!_recording) {
		return _duration;
	}
	// The recorder reports its own clock; until the first report use ours.
	return std::max(
		_duration,
		QDateTime::currentMSecsSinceEpoch() - _startedAt);
}

bool VoiceRecorder::start() {
	if (_recording) {
		return true;
	}
	const auto device = QMediaDevices::defaultAudioInput();
	if (device.isNull()) {
		return false;
	}
	_session = std::make_unique<QMediaCaptureSession>();
	_input = std::make_unique<QAudioInput>(device);
	_recorder = std::make_unique<QMediaRecorder>();
	_session->setAudioInput(_input.get());
	_session->setRecorder(_recorder.get());

	auto format = QMediaFormat();
	auto extension = QStringLiteral("m4a");
	const auto supported = QMediaFormat().supportedAudioCodecs(QMediaFormat::Encode);
	if (supported.contains(QMediaFormat::AudioCodec::Opus)
		&& QMediaFormat().supportedFileFormats(QMediaFormat::Encode).contains(
			QMediaFormat::Ogg)) {
		format.setFileFormat(QMediaFormat::Ogg);
		format.setAudioCodec(QMediaFormat::AudioCodec::Opus);
		extension = QStringLiteral("ogg");
	} else {
		format.setFileFormat(QMediaFormat::Mpeg4Audio);
		format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
	}
	_recorder->setMediaFormat(format);
	_recorder->setAudioChannelCount(1);
	_recorder->setAudioSampleRate(48000);
	_recorder->setAudioBitRate(32000);
	_recorder->setQuality(QMediaRecorder::NormalQuality);

	auto directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (directory.isEmpty()) {
		directory = QDir::tempPath();
	}
	_path = QDir(directory).filePath(QStringLiteral("eleza-voice-%1.%2").arg(
		QUuid::createUuid().toString(QUuid::WithoutBraces),
		extension));
	_recorder->setOutputLocation(QUrl::fromLocalFile(_path));

	connect(_recorder.get(), &QMediaRecorder::durationChanged, this, [this](qint64 value) {
		_duration = value;
		Q_EMIT durationChanged(durationMs());
	});
	connect(
		_recorder.get(),
		&QMediaRecorder::errorOccurred,
		this,
		[this](QMediaRecorder::Error, const QString &) {
			if (!_recording) {
				return;
			}
			cleanup(true);
			Q_EMIT failed();
		});
	connect(
		_recorder.get(),
		&QMediaRecorder::recorderStateChanged,
		this,
		[this](QMediaRecorder::RecorderState state) {
			if (state != QMediaRecorder::StoppedState || !_stopping) {
				return;
			}
			const auto keep = _keep;
			const auto duration = durationMs();
			// The recorder may have chosen another name / extension.
			const auto actual = _recorder->actualLocation();
			const auto path = actual.isLocalFile() ? actual.toLocalFile() : _path;
			_path = path;
			cleanup(!keep);
			if (keep) {
				Q_EMIT finished(path, duration);
			}
		});
	_duration = 0;
	_startedAt = QDateTime::currentMSecsSinceEpoch();
	_stopping = false;
	_recording = true;
	_recorder->record();
	if (_recorder->error() != QMediaRecorder::NoError) {
		cleanup(true);
		return false;
	}
	return true;
}

void VoiceRecorder::stop(bool keep) {
	if (!_recording || !_recorder) {
		return;
	}
	_keep = keep;
	_stopping = true;
	_duration = durationMs();
	_recording = false;
	if (_recorder->recorderState() == QMediaRecorder::StoppedState) {
		// Never really started.
		cleanup(true);
		return;
	}
	_recorder->stop();
}

void VoiceRecorder::cleanup(bool removeFile) {
	_recording = false;
	_stopping = false;
	if (removeFile && !_path.isEmpty()) {
		QFile::remove(_path);
	}
	// The objects may be in the middle of emitting: let them finish.
	if (_recorder) {
		_recorder->disconnect(this);
		_recorder.release()->deleteLater();
	}
	if (_session) {
		_session.release()->deleteLater();
	}
	if (_input) {
		_input.release()->deleteLater();
	}
}

} // namespace History
