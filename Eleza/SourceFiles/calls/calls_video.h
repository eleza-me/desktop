// The camera of a video call: frames of the chosen device (or of the
// system's default one) as RGBA images of a size and rate a call can afford.
//
//   auto camera = Calls::Video::Camera();
//   connect(&camera, &Calls::Video::Camera::frame, ...);   // QImage, Format_RGBA8888
//   if (!camera.start(deviceId)) { /* no camera, or it cannot be opened */ }
//   camera.stop();
//
// Frames are scaled down to fit kMaxSize (even width and height, as video
// encoders want) and limited to kMaxFps. Lives on the thread that creates it
// (the GUI thread: Qt's capture session belongs there).
#pragma once

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QImage>

#include <memory>

class QTimer;

namespace Calls::Video {

inline constexpr auto kMaxSize = QSize(640, 480);
inline constexpr auto kMaxFps = 15;

struct CameraInfo {
	QByteArray id;
	QString name;
	bool isDefault = false;
};
[[nodiscard]] QVector<CameraInfo> Cameras();
[[nodiscard]] bool HasCamera();

// What goes out for a captured picture: at most kMaxSize, the aspect kept,
// both sides even (never below 2), RGBA.
[[nodiscard]] QSize OutputSize(QSize captured, QSize max = kMaxSize);
[[nodiscard]] QImage PrepareFrame(const QImage &captured, QSize max = kMaxSize);
// The generated picture of a debug run: frame `index` of an endless film.
[[nodiscard]] QImage PatternFrame(int index, QSize size = kMaxSize);

class Camera final : public QObject {
	Q_OBJECT

public:
	explicit Camera(QObject *parent = nullptr);
	~Camera();

	// False: there is no such camera and no default one either.
	bool start(const QByteArray &deviceId);
	// Generated frames, without any hardware.
	void startPattern();
	void stop();
	[[nodiscard]] bool active() const {
		return _active;
	}
	// The device in use ("" for the pattern, or when stopped).
	[[nodiscard]] QByteArray deviceId() const {
		return _deviceId;
	}

Q_SIGNALS:
	void frame(const QImage &rgba);
	// The camera stopped by itself (unplugged, taken by another program).
	void failed();

private:
	struct Private;

	void deliver(const QImage &image);

	std::unique_ptr<Private> _private;
	QTimer *_pattern = nullptr;
	QByteArray _deviceId;
	qint64 _lastFrameMs = 0;
	int _patternIndex = 0;
	bool _active = false;

};

} // namespace Calls::Video
