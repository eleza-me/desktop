#include "data/data_media.h"

#include "base/platform_file.h"
#include "data/data_session.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtGui/QImageReader>

#include <algorithm>

namespace Data {
namespace {

constexpr auto kDefaultMemoryLimit = qint64(64) << 20;
constexpr auto kPruneTo = 0.8;

[[nodiscard]] QString Encoded(const QString &segment) {
	return QString::fromLatin1(QUrl::toPercentEncoding(segment));
}

[[nodiscard]] int CacheCost(const QImage &image) {
	return int(std::max<qint64>(1, image.sizeInBytes() / 1024));
}

[[nodiscard]] QString OptionsKey(const QString &cacheKey, const Media::Options &options) {
	return cacheKey
		+ (options.decode ? u"|d"_s : u"|f"_s)
		+ QString::number(options.fit.isValid() ? options.fit.width() : 0)
		+ u'x'
		+ QString::number(options.fit.isValid() ? options.fit.height() : 0);
}

struct DiskEntry {
	QString path;
	qint64 size = 0;
	qint64 used = 0;
};

[[nodiscard]] QVector<DiskEntry> ListDisk(const QString &directory) {
	auto result = QVector<DiskEntry>();
	auto i = QDirIterator(directory, QDir::Files, QDirIterator::Subdirectories);
	while (i.hasNext()) {
		i.next();
		const auto info = i.fileInfo();
		result.push_back({
			info.absoluteFilePath(),
			info.size(),
			info.lastModified().toMSecsSinceEpoch(),
		});
	}
	return result;
}

} // namespace

QString MediaVariantName(MediaVariant variant) {
	switch (variant) {
	case MediaVariant::Thumb: return u"thumb"_s;
	case MediaVariant::Feed: return u"feed"_s;
	case MediaVariant::Full: return u"full"_s;
	case MediaVariant::Poster: return u"poster"_s;
	case MediaVariant::Mp4: return u"mp4"_s;
	case MediaVariant::Audio: return u"audio"_s;
	case MediaVariant::File: return u"file"_s;
	case MediaVariant::Cover: return u"cover"_s;
	}
	return u"thumb"_s;
}

MediaRef MediaRef::Avatar(const QString &userId, const QString &avatarId) {
	auto result = MediaRef();
	if (!userId.isEmpty() && !avatarId.isEmpty()) {
		result.type = Type::Avatar;
		result.id = userId;
		result.version = avatarId;
	}
	return result;
}

MediaRef MediaRef::Attachment(const QString &chat, qint64 seq) {
	auto result = MediaRef();
	if (!chat.isEmpty() && seq > 0) {
		result.type = Type::Attachment;
		result.id = chat;
		result.seq = seq;
	}
	return result;
}

MediaRef MediaRef::PostMedia(const QString &postId, const QString &mediaId) {
	auto result = MediaRef();
	if (!postId.isEmpty() && !mediaId.isEmpty()) {
		result.type = Type::PostMedia;
		result.id = postId;
		result.version = mediaId;
	}
	return result;
}

MediaRef MediaRef::Url(const QString &url) {
	auto result = MediaRef();
	if (url.startsWith(u"https://") || url.startsWith(u"http://") || url.startsWith(u'/')) {
		result.type = Type::Url;
		result.id = url;
	}
	return result;
}

bool MediaRef::valid() const {
	return type != Type::None;
}

QString MediaRef::location(MediaVariant variant) const {
	const auto name = MediaVariantName(variant);
	switch (type) {
	case Type::Avatar:
		return u"/v1/users/%1/avatar?variant=%2"_s.arg(Encoded(id), name);
	case Type::Attachment:
		return u"/v1/chats/%1/messages/%2/attachment?variant=%3"_s.arg(Encoded(id)).arg(seq).arg(name);
	case Type::PostMedia:
		return u"/v1/posts/%1/media/%2?variant=%3"_s.arg(Encoded(id), Encoded(version), name);
	case Type::Url:
		return id;
	case Type::None:
		break;
	}
	return QString();
}

QString MediaRef::cacheKey(MediaVariant variant) const {
	const auto name = MediaVariantName(variant);
	switch (type) {
	case Type::Avatar: return u"avatar:%1:%2:%3"_s.arg(id, version, name);
	case Type::Attachment: return u"attachment:%1:%2:%3"_s.arg(id).arg(seq).arg(name);
	case Type::PostMedia: return u"post:%1:%2:%3"_s.arg(id, version, name);
	case Type::Url: return u"url:"_s + id;
	case Type::None: break;
	}
	return QString();
}

Media::Media(Session *session)
: QObject(session)
, _session(session)
, _directory(session->config().cacheDir)
, _memory(int(kDefaultMemoryLimit / 1024))
, _diskLimit(session->config().mediaCacheLimit) {
	_pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, 4));
	if (!_directory.isEmpty()) {
		base::platform::EnsureDirectory(_directory, true);
		scanDisk();
	}
}

Media::~Media() {
	for (const auto &job : std::as_const(_jobs)) {
		if (job->download) {
			_session->api().cancel(job->download);
		}
	}
	_jobs.clear();
	// Worker tasks post back to this object: let them end first.
	_pool.waitForDone();
}

void Media::setMemoryLimit(qint64 bytes) {
	_memory.setMaxCost(int(std::max<qint64>(1, bytes / 1024)));
}

void Media::setDiskLimit(qint64 bytes) {
	_diskLimit = bytes;
	prune();
}

void Media::clearMemory() {
	_memory.clear();
}

QString Media::pathForKey(const QString &cacheKey) const {
	if (_directory.isEmpty()) {
		return QString();
	}
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(
		cacheKey.toUtf8(),
		QCryptographicHash::Sha256).toHex().left(40));
	return _directory + u'/' + hash.left(2) + u'/' + hash;
}

QString Media::filePathFor(const MediaRef &ref, MediaVariant variant) const {
	return pathForKey(ref.cacheKey(variant));
}

QImage Media::cached(const MediaRef &ref, MediaVariant variant, QSize fit) const {
	auto options = Options();
	options.fit = fit;
	const auto found = _memory.object(OptionsKey(ref.cacheKey(variant), options));
	return found ? *found : QImage();
}

void Media::evict(const MediaRef &ref, MediaVariant variant) {
	const auto cacheKey = ref.cacheKey(variant);
	const auto prefix = cacheKey + u'|';
	for (const auto &key : _memory.keys()) {
		if (key.startsWith(prefix)) {
			_memory.remove(key);
		}
	}
	const auto path = pathForKey(cacheKey);
	if (!path.isEmpty()) {
		const auto size = QFileInfo(path).size();
		if (QFile::remove(path)) {
			_diskUsage = std::max<qint64>(0, _diskUsage - size);
			Q_EMIT diskUsageChanged();
		}
	}
}

MediaHandlePtr Media::request(const MediaRef &ref, MediaVariant variant) {
	return request(ref, variant, Options());
}

MediaHandlePtr Media::request(const MediaRef &ref, MediaVariant variant, const Options &options) {
	const auto cacheKey = ref.cacheKey(variant);
	const auto key = OptionsKey(cacheKey, options);
	if (!options.force) {
		if (const auto i = _handles.constFind(key); i != _handles.cend()) {
			if (const auto alive = i->lock()) {
				return alive;
			}
		}
	}
	const auto weak = QPointer<Media>(this);
	const auto handle = MediaHandlePtr(new MediaHandle(), [weak, key](MediaHandle *raw) {
		if (weak) {
			weak->released(key);
		}
		raw->deleteLater();
	});
	handle->_ref = ref;
	handle->_variant = variant;
	if (!ref.valid()) {
		handle->_state = MediaHandle::State::Failed;
		handle->_error = Api::Error::Local(404, u"media.none"_s, u"no media"_s);
		return handle;
	}
	const auto filePath = pathForKey(cacheKey);
	if (!options.force && options.decode) {
		if (const auto found = _memory.object(key)) {
			handle->_image = *found;
			handle->_filePath = filePath;
			handle->_state = MediaHandle::State::Ready;
			handle->_progress = 1.;
			_handles.insert(key, handle);
			return handle;
		}
	}
	_handles.insert(key, handle);
	const auto job = std::make_shared<Job>();
	job->key = key;
	job->cacheKey = cacheKey;
	job->location = ref.location(variant);
	job->filePath = filePath;
	job->options = options;
	job->authenticated = (ref.type != MediaRef::Type::Url);
	job->handle = handle;
	_jobs.insert(key, job);
	startJob(job);
	return handle;
}

void Media::released(const QString &key) {
	if (const auto i = _handles.find(key); i != _handles.end() && i->expired()) {
		_handles.erase(i);
	}
	// Nobody waits for it any more: stop the download.
	if (const auto job = _jobs.value(key); job && job->handle.expired()) {
		if (job->download) {
			_session->api().cancel(job->download);
		}
		_jobs.remove(key);
	}
}

void Media::startJob(const std::shared_ptr<Job> &job) {
	if (!job->options.force && !job->filePath.isEmpty() && QFile::exists(job->filePath)) {
		decode(job, QByteArray());
	} else {
		download(job);
	}
}

void Media::download(const std::shared_ptr<Job> &job) {
	auto builder = _session->api().download(job->location);
	if (!job->authenticated) {
		builder.unauthenticated();
	}
	if (!job->filePath.isEmpty()) {
		builder.toFile(job->filePath);
	}
	const auto weak = std::weak_ptr<Job>(job);
	job->download = builder
		.guard(this)
		.progress([=](qint64 received, qint64 total) {
			const auto job = weak.lock();
			const auto handle = job ? job->handle.lock() : nullptr;
			if (handle && total > 0) {
				handle->_progress = double(received) / double(total);
				Q_EMIT handle->progressChanged();
			}
		})
		.done([=, this](const Api::DownloadResult &result) {
			const auto job = weak.lock();
			if (!job) {
				return;
			}
			job->download = 0;
			if (!result.filePath.isEmpty()) {
				added(result.size);
			}
			decode(job, result.bytes);
		})
		.fail([=, this](const Api::Error &error) {
			if (const auto job = weak.lock()) {
				job->download = 0;
				fail(job, error);
			}
		})
		.send();
}

QImage Media::Decode(const QString &filePath, const QByteArray &bytes, QSize fit) {
	auto buffer = QBuffer();
	auto reader = QImageReader();
	if (!filePath.isEmpty()) {
		reader.setFileName(filePath);
	} else {
		buffer.setData(bytes);
		buffer.open(QIODevice::ReadOnly);
		reader.setDevice(&buffer);
	}
	reader.setAutoTransform(true);
	reader.setDecideFormatFromContent(true);
	if (!reader.canRead()) {
		return QImage();
	}
	if (fit.isValid() && !fit.isEmpty()) {
		const auto size = reader.size();
		if (size.isValid() && (size.width() > fit.width() || size.height() > fit.height())) {
			// Scaled by the decoder where the format can (JPEG): cheaper than a full decode.
			auto bounds = fit;
			if (reader.transformation() & QImageIOHandler::TransformationRotate90) {
				bounds.transpose();
			}
			reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
		}
	}
	auto image = reader.read();
	if (image.isNull()) {
		return QImage();
	}
	if (fit.isValid() && !fit.isEmpty()
		&& (image.width() > fit.width() || image.height() > fit.height())) {
		image = image.scaled(fit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}
	const auto format = image.hasAlphaChannel()
		? QImage::Format_ARGB32_Premultiplied
		: QImage::Format_RGB32;
	if (image.format() != format) {
		image = image.convertToFormat(format);
	}
	return image;
}

void Media::decode(const std::shared_ptr<Job> &job, const QByteArray &bytes) {
	if (!job->options.decode) {
		finish(job, QImage(), bytes);
		return;
	}
	const auto filePath = job->filePath;
	const auto fit = job->options.fit;
	_pool.start([=, this] {
		auto image = Decode(filePath, bytes, fit);
		if (!filePath.isEmpty()) {
			// The modification time doubles as "last used" for pruning.
			auto file = QFile(filePath);
			if (file.open(QIODevice::ReadWrite)) {
				file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
			}
		}
		QMetaObject::invokeMethod(this, [=, this] {
			if (_jobs.value(job->key) == job) {
				finish(job, image, bytes);
			}
		}, Qt::QueuedConnection);
	});
}

void Media::finish(
		const std::shared_ptr<Job> &job,
		const QImage &image,
		const QByteArray &bytes) {
	_jobs.remove(job->key);
	if (!image.isNull()) {
		_memory.insert(job->key, new QImage(image), CacheCost(image));
	}
	if (const auto handle = job->handle.lock()) {
		handle->_image = image;
		handle->_filePath = job->filePath;
		if (job->filePath.isEmpty()) {
			handle->_bytes = bytes;
		}
		handle->_progress = 1.;
		handle->_state = MediaHandle::State::Ready;
		Q_EMIT handle->ready();
	}
}

void Media::fail(const std::shared_ptr<Job> &job, const Api::Error &error) {
	_jobs.remove(job->key);
	if (const auto handle = job->handle.lock()) {
		handle->_error = error;
		handle->_state = MediaHandle::State::Failed;
		// A failed handle is not handed out again: the next request retries.
		_handles.remove(job->key);
		Q_EMIT handle->failed();
	}
}

void Media::scanDisk() {
	const auto directory = _directory;
	_pool.start([=, this] {
		auto total = qint64(0);
		for (const auto &entry : ListDisk(directory)) {
			if (entry.path.endsWith(u".part")) {
				// Left over from an interrupted download (not one that just began).
				if (entry.used < QDateTime::currentMSecsSinceEpoch() - 3600 * 1000) {
					QFile::remove(entry.path);
				}
			} else {
				total += entry.size;
			}
		}
		QMetaObject::invokeMethod(this, [=, this] {
			_diskUsage += total;
			Q_EMIT diskUsageChanged();
			prune();
		}, Qt::QueuedConnection);
	});
}

void Media::added(qint64 bytes) {
	_diskUsage += bytes;
	if (_pruning) {
		// The running prune measured the directory before this file landed.
		_addedWhilePruning += bytes;
	}
	Q_EMIT diskUsageChanged();
	prune();
}

void Media::prune() {
	if (_pruning || _directory.isEmpty() || _diskLimit <= 0 || _diskUsage <= _diskLimit) {
		return;
	}
	_pruning = true;
	const auto directory = _directory;
	const auto target = qint64(double(_diskLimit) * kPruneTo);
	_pool.start([=, this] {
		auto entries = ListDisk(directory);
		std::sort(entries.begin(), entries.end(), [](const DiskEntry &a, const DiskEntry &b) {
			return a.used < b.used;
		});
		auto total = qint64(0);
		for (const auto &entry : entries) {
			total += entry.size;
		}
		for (const auto &entry : entries) {
			if (total <= target) {
				break;
			} else if (entry.path.endsWith(u".part")) {
				continue; // a download in progress
			} else if (QFile::remove(entry.path)) {
				total -= entry.size;
			}
		}
		QMetaObject::invokeMethod(this, [=, this] {
			_pruning = false;
			_diskUsage = total + std::exchange(_addedWhilePruning, 0);
			Q_EMIT diskUsageChanged();
			prune();
		}, Qt::QueuedConnection);
	});
}

void Media::clearDisk() {
	if (_directory.isEmpty()) {
		return;
	}
	const auto directory = _directory;
	_pool.start([=, this] {
		for (const auto &entry : ListDisk(directory)) {
			if (!entry.path.endsWith(u".part")) {
				QFile::remove(entry.path);
			}
		}
		QMetaObject::invokeMethod(this, [=, this] {
			_diskUsage = 0;
			Q_EMIT diskUsageChanged();
		}, Qt::QueuedConnection);
	});
}

} // namespace Data
