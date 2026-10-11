// Media of other people comes from viewer-scoped routes that need the
// bearer token and answer with a 302 to a short-lived storage URL. This store
// fetches the bytes (the token goes to the API only, never to the storage
// host), keeps them in a size-capped disk cache keyed by the logical address
// (avatar id / attachment / variant), decodes images off the main thread and
// keeps the decoded ones in a small in-memory LRU. People's and groups'
// pictures have a store of their own next to it (see picture()), so a screen
// that opens paints them at once instead of the initial first.
//
//   _avatar = media.request(Data::MediaRef::Avatar(userId, avatarId), Data::MediaVariant::Thumb);
//   if (_avatar->isReady()) {
//       update();
//   } else {
//       connect(_avatar.get(), &Data::MediaHandle::ready, this, [=] { update(); });
//   }
//   ... painter.drawImage(rect, _avatar->image());
//
// Keep the handle while the image is on screen: requests for the same media
// share one download, and a handle nobody holds any more stops it.
#pragma once

#include "api/api_client.h"

#include <QtCore/QCache>
#include <QtCore/QHash>
#include <QtCore/QSize>
#include <QtCore/QThreadPool>
#include <QtGui/QImage>

namespace Data {

class Session;
class Media;

enum class MediaVariant {
	Thumb,  // small preview (lists, bubbles)
	Feed,   // medium (feed cards, large avatars)
	Full,   // the original image
	Poster, // a video's still frame
	Mp4,    // a playable video
	Audio,  // a playable voice / music track
	File,   // the uploaded file as is
	Cover,  // a music file's cover art
};
[[nodiscard]] QString MediaVariantName(MediaVariant variant);

// The logical address of a piece of media.
struct MediaRef {
	enum class Type {
		None,
		Avatar,     // id = user id, version = avatar id
		Attachment, // id = chat or group id, seq = message seq
		PostMedia,  // id = post id, version = media id
		GroupPhoto, // id = group id, version = photo id
		InvitePhoto, // id = an invite token, version = photo id (not a member yet)
		Url,        // id = an absolute URL (stickers, link previews): fetched without the token
	};
	Type type = Type::None;
	QString id;
	QString version;
	qint64 seq = 0;

	[[nodiscard]] static MediaRef Avatar(const QString &userId, const QString &avatarId);
	[[nodiscard]] static MediaRef Attachment(const QString &chat, qint64 seq);
	[[nodiscard]] static MediaRef PostMedia(const QString &postId, const QString &mediaId);
	[[nodiscard]] static MediaRef GroupPhoto(const QString &groupId, const QString &photoId);
	[[nodiscard]] static MediaRef InvitePhoto(const QString &token, const QString &photoId);
	[[nodiscard]] static MediaRef Url(const QString &url);

	[[nodiscard]] bool valid() const;
	// The API path (or the absolute URL) the bytes come from.
	[[nodiscard]] QString location(MediaVariant variant) const;
	// Stable across runs; changes when the content does (a new avatar id).
	[[nodiscard]] QString cacheKey(MediaVariant variant) const;
};

class MediaHandle final : public QObject {
	Q_OBJECT

public:
	enum class State {
		Loading,
		Ready,
		Failed,
	};

	[[nodiscard]] State state() const {
		return _state;
	}
	[[nodiscard]] bool isReady() const {
		return _state == State::Ready;
	}
	[[nodiscard]] bool isFailed() const {
		return _state == State::Failed;
	}
	// The decoded image; null while loading, for non-image media, or when
	// the request asked for the file only.
	[[nodiscard]] const QImage &image() const {
		return _image;
	}
	// The file in the disk cache (empty when the cache is memory only).
	[[nodiscard]] QString filePath() const {
		return _filePath;
	}
	// The raw bytes when there is no disk cache.
	[[nodiscard]] const QByteArray &bytes() const {
		return _bytes;
	}
	// 0..1 while downloading.
	[[nodiscard]] double progress() const {
		return _progress;
	}
	[[nodiscard]] const Api::Error &error() const {
		return _error;
	}
	[[nodiscard]] const MediaRef &ref() const {
		return _ref;
	}
	[[nodiscard]] MediaVariant variant() const {
		return _variant;
	}

Q_SIGNALS:
	void ready();
	void failed();
	void progressChanged();

private:
	friend class Media;

	MediaHandle() = default;

	MediaRef _ref;
	MediaVariant _variant = MediaVariant::Thumb;
	State _state = State::Loading;
	QImage _image;
	QString _filePath;
	QByteArray _bytes;
	double _progress = 0.;
	Api::Error _error;

};

using MediaHandlePtr = std::shared_ptr<MediaHandle>;

class Media final : public QObject {
	Q_OBJECT

public:
	struct Options {
		// Decode the bytes as an image (off the main thread).
		bool decode = true;
		// Scale down while decoding so the image fits this box (aspect kept);
		// empty: the original size.
		QSize fit;
		// Ignore what is cached and fetch again.
		bool force = false;
		// The size the message / media metadata declares for these bytes
		// (0: unknown). The download is stopped and fails with the code
		// "too_large" once it goes beyond DownloadLimit(expectedBytes).
		qint64 expectedBytes = 0;
	};

	explicit Media(Session *session);
	~Media();

	// Never null. Already Ready when the image was in memory.
	[[nodiscard]] MediaHandlePtr request(const MediaRef &ref, MediaVariant variant = MediaVariant::Thumb);
	[[nodiscard]] MediaHandlePtr request(const MediaRef &ref, MediaVariant variant, const Options &options);
	// The decoded image if it is in memory right now (no I/O).
	[[nodiscard]] QImage cached(const MediaRef &ref, MediaVariant variant, QSize fit = QSize()) const;
	// A person's or a group's picture, ready to paint right now: from the
	// pictures' own store (which the photos and videos of the chats never push
	// anything out of), or decoded here and now from its small file in the disk
	// cache. Null only when the picture was never fetched (then request() it).
	// The call that makes a screen opened for the second time, a list created
	// for another tab, or the first screen after a restart show the faces in
	// their first frame.
	[[nodiscard]] QImage picture(const MediaRef &ref, MediaVariant variant = MediaVariant::Thumb);
	// Whether picture() keeps this kind of media.
	[[nodiscard]] static bool IsPicture(const MediaRef &ref, MediaVariant variant);
	// Drops one item from memory and disk (the media changed or is broken).
	void evict(const MediaRef &ref, MediaVariant variant);

	void setMemoryLimit(qint64 bytes);
	void setDiskLimit(qint64 bytes);
	[[nodiscard]] qint64 diskLimit() const {
		return _diskLimit;
	}
	// Bytes in the disk cache (known once the startup scan finished).
	[[nodiscard]] qint64 diskUsage() const {
		return _diskUsage;
	}
	void clearMemory();
	// Deletes every cached file.
	void clearDisk();
	[[nodiscard]] QString directory() const {
		return _directory;
	}

	// Where an item lives in the disk cache.
	[[nodiscard]] QString filePathFor(const MediaRef &ref, MediaVariant variant) const;

	// How many bytes a download whose metadata declares `declared` bytes may
	// bring before it is stopped: a small margin over the declared size,
	// max(declared × 1.05, declared + 64 KiB), never above the global cap of
	// its kind (`decode`: pictures, otherwise files). Nothing declared
	// (<= 0): the global cap.
	[[nodiscard]] static qint64 DownloadLimit(qint64 declared, bool decode);

	// Synchronous decode helper (used by the worker threads; exposed for tests).
	[[nodiscard]] static QImage Decode(const QString &filePath, const QByteArray &bytes, QSize fit);

Q_SIGNALS:
	void diskUsageChanged();

private:
	struct Job {
		QString key;       // cache key + decode options
		QString cacheKey;
		QString location;
		QString filePath;
		Options options;
		bool authenticated = true;
		bool picture = false; // goes to the pictures' store too
		std::weak_ptr<MediaHandle> handle;
		Api::RequestId download = 0;
	};

	[[nodiscard]] QString pathForKey(const QString &cacheKey) const;
	void startJob(const std::shared_ptr<Job> &job);
	void download(const std::shared_ptr<Job> &job);
	void decode(const std::shared_ptr<Job> &job, const QByteArray &bytes);
	void finish(const std::shared_ptr<Job> &job, const QImage &image, const QByteArray &bytes);
	void fail(const std::shared_ptr<Job> &job, const Api::Error &error);
	void released(const QString &key);
	void scanDisk();
	void added(qint64 bytes);
	void prune();

	Session *_session = nullptr;
	QString _directory;
	QThreadPool _pool;
	QHash<QString, std::shared_ptr<Job>> _jobs;
	QHash<QString, std::weak_ptr<MediaHandle>> _handles;
	mutable QCache<QString, QImage> _memory;
	// Decoded avatars and group photos by cache key (cost in KiB).
	QCache<QString, QImage> _pictures;
	qint64 _diskLimit = 0;
	qint64 _diskUsage = 0;
	qint64 _addedWhilePruning = 0;
	bool _pruning = false;

};

} // namespace Data
