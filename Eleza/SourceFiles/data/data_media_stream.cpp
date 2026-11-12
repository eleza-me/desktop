#include "data/data_media_stream.h"

#include <QtCore/QDeadlineTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <algorithm>

namespace Data {
namespace {

// How long a read on a player thread waits for bytes that are not on disk
// yet before it reports the end of the file. Short: a backend that held the
// main thread on its reading thread would freeze the window this long.
constexpr auto kWaitLimitMs = 1000;

[[nodiscard]] qint64 TotalOf(const MediaHandlePtr &handle) {
	if (!handle) {
		return 0;
	} else if (handle->isReady()) {
		return QFileInfo(handle->filePath()).size();
	}
	return std::max(handle->totalBytes(), qint64(0));
}

} // namespace

bool MediaStream::Streamable(const MediaHandle &handle, qint64 minimum) {
	const auto total = handle.totalBytes();
	return handle.state() == MediaHandle::State::Loading
		&& !handle.partialPath().isEmpty()
		&& total > 0
		&& handle.partialBytes() >= std::min(minimum, total);
}

qint64 MediaStream::RebufferBytes(qint64 total, qint64 durationMs) {
	// About this many seconds of it, by its average rate.
	constexpr auto kSeconds = 8;
	constexpr auto kFallback = qint64(1) << 20;
	constexpr auto kAtLeast = qint64(128) << 10;
	if (total <= 0 || durationMs <= 0) {
		return kFallback;
	}
	return std::max(total * kSeconds * 1000 / std::max(durationMs, qint64(1000)), kAtLeast);
}

MediaStream::MediaStream(MediaHandlePtr handle, QObject *parent)
: QIODevice(parent)
, _handle(std::move(handle))
, _total(TotalOf(_handle)) {
	if (!_handle) {
		_failed = true;
		return;
	}
	const auto raw = _handle.get();
	if (raw->isReady()) {
		_path = raw->filePath();
		_available = _total;
		_complete = !_path.isEmpty();
		_failed = !_complete;
		return;
	} else if (raw->isFailed()) {
		_failed = true;
		return;
	}
	_path = raw->partialPath();
	_available = std::min(raw->partialBytes(), _total);
	QObject::connect(raw, &MediaHandle::partialChanged, this, [=, this] {
		setAvailable(raw->partialBytes());
	});
	// Direct: the part is renamed right after the signal returns.
	QObject::connect(raw, &MediaHandle::finishing, this, [=, this] {
		setMoving();
	}, Qt::DirectConnection);
	QObject::connect(raw, &MediaHandle::ready, this, [=, this] {
		if (raw->filePath().isEmpty()) {
			setFailed();
		} else {
			setComplete(raw->filePath());
		}
	});
	QObject::connect(raw, &MediaHandle::failed, this, [=, this] {
		setFailed();
	});
}

MediaStream::MediaStream(const QString &path, qint64 total, QObject *parent)
: QIODevice(parent)
, _total(std::max(total, qint64(0)))
, _path(path) {
}

MediaStream::~MediaStream() {
	abort();
	QIODevice::close();
}

bool MediaStream::open(OpenMode mode) {
	if ((mode & QIODevice::WriteOnly) || !(mode & QIODevice::ReadOnly)) {
		return false;
	}
	// Unbuffered: pos() is where the player is, nothing is read ahead.
	return QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

void MediaStream::close() {
	abort();
	QIODevice::close();
}

bool MediaStream::isSequential() const {
	return false;
}

qint64 MediaStream::size() const {
	return _total;
}

qint64 MediaStream::bytesAvailable() const {
	return std::max(available() - pos(), qint64(0));
}

bool MediaStream::atEnd() const {
	return !isOpen() || pos() >= _total;
}

bool MediaStream::seek(qint64 pos) {
	if (pos < 0 || pos > _total) {
		return false;
	}
	return QIODevice::seek(pos);
}

qint64 MediaStream::available() const {
	QMutexLocker lock(&_mutex);
	return _complete ? _total : _available;
}

double MediaStream::availableShare() const {
	return (_total > 0) ? (double(available()) / double(_total)) : 0.;
}

bool MediaStream::complete() const {
	QMutexLocker lock(&_mutex);
	return _complete;
}

bool MediaStream::failed() const {
	QMutexLocker lock(&_mutex);
	return _failed;
}

bool MediaStream::ranOut() const {
	QMutexLocker lock(&_mutex);
	return _ranOut;
}

void MediaStream::abort() {
	{
		QMutexLocker lock(&_mutex);
		_aborted = true;
		_file = nullptr;
	}
	_arrived.wakeAll();
}

void MediaStream::setAvailable(qint64 bytes) {
	{
		QMutexLocker lock(&_mutex);
		if (_complete || bytes <= _available) {
			return;
		}
		_available = std::min(bytes, _total);
	}
	_arrived.wakeAll();
	Q_EMIT readyRead();
}

void MediaStream::setMoving() {
	QMutexLocker lock(&_mutex);
	// Taken once no read is in progress: nothing holds the part any more.
	_moving = true;
	_file = nullptr;
}

void MediaStream::setComplete(const QString &path) {
	{
		QMutexLocker lock(&_mutex);
		_moving = false;
		_complete = true;
		_available = _total;
		if (_path != path) {
			_path = path;
			_file = nullptr;
		}
	}
	_arrived.wakeAll();
	Q_EMIT readyRead();
}

void MediaStream::setFailed() {
	{
		QMutexLocker lock(&_mutex);
		_failed = true;
		_file = nullptr;
	}
	_arrived.wakeAll();
}

bool MediaStream::ensureFileLocked() {
	if (_file) {
		return true;
	}
	auto file = std::make_unique<QFile>(_path);
	if (!file->open(QIODevice::ReadOnly)) {
		return false;
	}
	_file = std::move(file);
	return true;
}

qint64 MediaStream::readData(char *data, qint64 maxSize) {
	const auto position = pos();
	if (maxSize <= 0) {
		return 0;
	}
	// The thread the stream lives on never waits: it paints the window
	// (and the bytes come through it).
	const auto mayWait = (QThread::currentThread() != thread());
	const auto deadline = QDeadlineTimer(kWaitLimitMs);
	QMutexLocker lock(&_mutex);
	for (;;) {
		if (_aborted || _failed) {
			return -1;
		} else if (position >= _total) {
			return 0; // the end of the file
		}
		const auto end = _complete ? _total : _available;
		if (position < end && !_moving) {
			break;
		} else if (!mayWait || deadline.hasExpired()) {
			// Not here (yet): "the end" for the player; its owner opens the
			// stream again once more has arrived.
			_ranOut = true;
			return 0;
		}
		_arrived.wait(&_mutex, deadline);
	}
	const auto end = _complete ? _total : _available;
	if (!ensureFileLocked() || !_file->seek(position)) {
		_file = nullptr;
		return -1;
	}
	const auto read = _file->read(data, std::min(maxSize, end - position));
	if (read <= 0) {
		// Shorter than it said it was: not the file we were told of.
		_file = nullptr;
		return -1;
	}
	return read;
}

qint64 MediaStream::writeData(const char *data, qint64 maxSize) {
	Q_UNUSED(data);
	Q_UNUSED(maxSize);
	return -1;
}

} // namespace Data
