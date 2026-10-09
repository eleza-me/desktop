// Recording a voice message with QtMultimedia: the default microphone into a
// temporary file (Ogg / Opus when the platform can, AAC otherwise).
//
//   if (!VoiceRecorder::Available()) { toast; return; }
//   _recorder->start();                       // false when it could not start
//   _recorder->stop(true);  ->  finished(path, durationMs)   // false: cancel, the file is removed
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

class QAudioInput;
class QMediaCaptureSession;
class QMediaRecorder;

namespace History {

class VoiceRecorder final : public QObject {
	Q_OBJECT

public:
	explicit VoiceRecorder(QObject *parent = nullptr);
	~VoiceRecorder();

	// A microphone is connected.
	[[nodiscard]] static bool Available();

	bool start();
	// keep = false throws the recording away.
	void stop(bool keep);
	[[nodiscard]] bool recording() const {
		return _recording;
	}
	[[nodiscard]] qint64 durationMs() const;

Q_SIGNALS:
	void durationChanged(qint64 durationMs);
	// The file is complete.
	void finished(const QString &path, qint64 durationMs);
	// The device went away / the recorder reported an error.
	void failed();

private:
	void cleanup(bool removeFile);

	std::unique_ptr<QMediaCaptureSession> _session;
	std::unique_ptr<QAudioInput> _input;
	std::unique_ptr<QMediaRecorder> _recorder;
	QString _path;
	qint64 _startedAt = 0;
	qint64 _duration = 0;
	bool _recording = false;
	bool _keep = false;
	bool _stopping = false;

};

} // namespace History
