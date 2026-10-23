#include "calls/calls_video.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtMultimedia/QCamera>
#include <QtMultimedia/QCameraDevice>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

#include <algorithm>

namespace Calls::Video {

struct Camera::Private {
	std::unique_ptr<QCamera> camera;
	QMediaCaptureSession session;
	QVideoSink sink;
};

QVector<CameraInfo> Cameras() {
	auto result = QVector<CameraInfo>();
	const auto preferred = QMediaDevices::defaultVideoInput();
	for (const auto &device : QMediaDevices::videoInputs()) {
		result.push_back({
			device.id(),
			device.description(),
			(!preferred.isNull() && device.id() == preferred.id()),
		});
	}
	return result;
}

bool HasCamera() {
	return !QMediaDevices::videoInputs().isEmpty();
}

QSize OutputSize(QSize captured, QSize max) {
	if (captured.width() <= 0 || captured.height() <= 0) {
		return QSize();
	}
	auto size = captured;
	if (size.width() > max.width() || size.height() > max.height()) {
		size = size.scaled(max, Qt::KeepAspectRatio);
	}
	// Encoders want even sides.
	return QSize(
		std::max(size.width() & ~1, 2),
		std::max(size.height() & ~1, 2));
}

QImage PrepareFrame(const QImage &captured, QSize max) {
	const auto size = OutputSize(captured.size(), max);
	if (size.isEmpty()) {
		return QImage();
	}
	auto result = (captured.size() == size)
		? captured
		: captured.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
	if (result.format() != QImage::Format_RGBA8888) {
		result = result.convertToFormat(QImage::Format_RGBA8888);
	}
	return result;
}

QImage PatternFrame(int index, QSize size) {
	auto result = QImage(size, QImage::Format_RGBA8888);
	result.fill(QColor(0x06, 0x41, 0x2C));
	auto p = QPainter(&result);
	// Colour bars, a bar that travels across them and the frame number:
	// enough to see on the other side that pictures arrive and move.
	static const QColor kBars[] = {
		QColor(0xD8, 0x78, 0x6E),
		QColor(0xD9, 0x9A, 0x3C),
		QColor(0x8F, 0xA8, 0x4A),
		QColor(0x3A, 0xA3, 0xA8),
		QColor(0x5C, 0x8F, 0xD6),
		QColor(0x8E, 0x7B, 0xD0),
	};
	const auto count = int(std::size(kBars));
	const auto barWidth = size.width() / count;
	for (auto i = 0; i != count; ++i) {
		p.fillRect(
			i * barWidth,
			size.height() / 4,
			barWidth,
			size.height() / 2,
			kBars[i]);
	}
	const auto travel = std::max(size.width() - 40, 1);
	p.fillRect((index * 12) % travel, 0, 40, size.height(), QColor(255, 255, 255, 200));
	auto font = p.font();
	font.setPixelSize(std::max(size.height() / 8, 12));
	font.setBold(true);
	p.setFont(font);
	p.setPen(Qt::white);
	p.drawText(
		QRect(0, size.height() * 3 / 4, size.width(), size.height() / 4),
		Qt::AlignCenter,
		QString::number(index));
	return result;
}

Camera::Camera(QObject *parent)
: QObject(parent) {
}

Camera::~Camera() {
	stop();
}

bool Camera::start(const QByteArray &deviceId) {
	stop();
	auto chosen = QCameraDevice();
	if (!deviceId.isEmpty()) {
		for (const auto &device : QMediaDevices::videoInputs()) {
			if (device.id() == deviceId) {
				chosen = device;
				break;
			}
		}
	}
	if (chosen.isNull()) {
		chosen = QMediaDevices::defaultVideoInput();
	}
	if (chosen.isNull()) {
		return false;
	}
	_private = std::make_unique<Private>();
	_private->camera = std::make_unique<QCamera>(chosen);
	_private->session.setCamera(_private->camera.get());
	_private->session.setVideoSink(&_private->sink);
	connect(&_private->sink, &QVideoSink::videoFrameChanged, this, [this](
			const QVideoFrame &frame) {
		if (!_active || !frame.isValid()) {
			return;
		}
		// The rate first: a frame that is not sent is not converted.
		const auto now = QDateTime::currentMSecsSinceEpoch();
		if (now - _lastFrameMs < 1000 / kMaxFps) {
			return;
		}
		_lastFrameMs = now;
		deliver(frame.toImage());
	});
	connect(_private->camera.get(), &QCamera::errorOccurred, this, [this](
			QCamera::Error error,
			const QString &) {
		if (error != QCamera::NoError && _active) {
			failLater();
		}
	});
	connect(_private->camera.get(), &QCamera::activeChanged, this, [this](
			bool active) {
		// Stopped under us (unplugged, the system took it away).
		if (!active && _active) {
			failLater();
		}
	});
	_deviceId = chosen.id();
	_active = true;
	_private->camera->start();
	// An error signal from inside start() went to failLater(): out of
	// that signal now, so stop here and report it as the result alone.
	if (!_private || !_active || _private->camera->error() != QCamera::NoError) {
		stop();
		return false;
	}
	return true;
}

void Camera::failLater() {
	// Not from inside the camera's own signal: stop() destroys it.
	_active = false;
	const auto failing = _private.get();
	QTimer::singleShot(0, this, [this, failing] {
		if (_private.get() == failing && !_active) {
			stop();
			Q_EMIT failed();
		}
	});
}

void Camera::startPattern() {
	stop();
	_active = true;
	_patternIndex = 0;
	_pattern = new QTimer(this);
	_pattern->setInterval(1000 / kMaxFps);
	connect(_pattern, &QTimer::timeout, this, [this] {
		Q_EMIT frame(PatternFrame(_patternIndex++));
	});
	_pattern->start();
}

void Camera::stop() {
	_active = false;
	_deviceId = QByteArray();
	_lastFrameMs = 0;
	if (_pattern) {
		delete std::exchange(_pattern, nullptr);
	}
	if (_private) {
		if (_private->camera) {
			_private->camera->stop();
		}
		_private = nullptr;
	}
}

void Camera::deliver(const QImage &image) {
	const auto prepared = PrepareFrame(image);
	if (!prepared.isNull()) {
		Q_EMIT frame(prepared);
	}
}

} // namespace Calls::Video
