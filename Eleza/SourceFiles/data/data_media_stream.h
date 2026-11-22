// A file that is still being downloaded into the media cache, as a random
// access device for a player: a chat video or a voice message plays while
// the rest of it arrives.
//
//   if (Data::MediaStream::Streamable(*handle, Data::MediaStream::kVideoStart)) {
//       _stream = std::make_unique<Data::MediaStream>(handle);
//       _stream->open(QIODevice::ReadOnly);
//       player->setSourceDevice(_stream.get(), QUrl(u"video.mp4"_s));
//   }
//   ...
//   _stream->abort(); // before the player lets it go
//
// size() is the size of the whole file. A read of bytes that are not on
// disk yet waits for them a moment on a player thread (the FFmpeg backend of
// Qt 6.5+ demuxes on threads of its own and seeks without waiting for them)
// and not at all on the stream's own thread; then it reads as the end of the
// file (ranOut() tells that from the real end). A player that stops there is
// opened again at its position by its owner once enough more has arrived
// (RebufferBytes), or on the finished file. The stream follows its handle:
// when the download is complete it reads the finished file, when it fails
// every read fails.
//
// Reads come from the player's threads, everything else from the stream's
// thread; the state they share is behind a mutex.
#pragma once

#include "data/data_media.h"

#include <QtCore/QIODevice>
#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>

#include <memory>

class QFile;

namespace Data {

class MediaStream final : public QIODevice {
public:
	// What must be on disk before playing starts: the header of a video
	// (the server's MP4 have it first, "faststart") and a few seconds of
	// it; a voice message or a song needs less.
	static constexpr auto kVideoStart = qint64(1) << 20;
	static constexpr auto kAudioStart = qint64(128) << 10;

	// The handle's download can be played from: its first `minimum` bytes
	// (or the whole file, if smaller) are on disk and the size is known.
	[[nodiscard]] static bool Streamable(const MediaHandle &handle, qint64 minimum);
	// How much more must arrive before a stream that ran out is opened
	// again: a few seconds of the file by its average rate.
	[[nodiscard]] static qint64 RebufferBytes(qint64 total, qint64 durationMs);

	// Follows the handle (kept alive, so the download goes on).
	explicit MediaStream(MediaHandlePtr handle, QObject *parent = nullptr);
	// Driven by hand (tests): `total` bytes, `path` filled from the start.
	MediaStream(const QString &path, qint64 total, QObject *parent = nullptr);
	~MediaStream();

	bool open(OpenMode mode) override;
	void close() override;
	[[nodiscard]] bool isSequential() const override;
	[[nodiscard]] qint64 size() const override;
	[[nodiscard]] qint64 bytesAvailable() const override;
	[[nodiscard]] bool atEnd() const override;
	bool seek(qint64 pos) override;

	// Bytes from the start that can be read now (all of them once complete).
	[[nodiscard]] qint64 available() const;
	// available() / size(), 0..1.
	[[nodiscard]] double availableShare() const;
	[[nodiscard]] bool complete() const;
	[[nodiscard]] bool failed() const;
	// A read asked for bytes that had not arrived (the player saw an end
	// that is not the end of the file).
	[[nodiscard]] bool ranOut() const;
	// Every read from now on fails, a waiting one at once (before the player
	// lets go of it).
	void abort();

	// What the handle tells (public for the tests).
	void setAvailable(qint64 bytes);
	void setMoving(); // the part is about to be renamed: closed until setComplete
	void setComplete(const QString &path);
	void setFailed();

protected:
	qint64 readData(char *data, qint64 maxSize) override;
	qint64 writeData(const char *data, qint64 maxSize) override;

private:
	[[nodiscard]] bool ensureFileLocked();

	MediaHandlePtr _handle;
	const qint64 _total = 0;

	mutable QMutex _mutex;
	QWaitCondition _arrived;
	QString _path;
	std::unique_ptr<QFile> _file;
	qint64 _available = 0;
	bool _moving = false;
	bool _complete = false;
	bool _failed = false;
	bool _aborted = false;
	bool _ranOut = false;

};

} // namespace Data
