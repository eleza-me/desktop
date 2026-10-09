#include "history/history_downloads.h"

#include "data/data_session.h"
#include "main/main_session.h"
#include "storage/storage_database.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QThreadPool>

namespace History {
namespace {

constexpr auto kMaxRemembered = 400;
const auto kSavedKey = u"history.downloads"_s;

[[nodiscard]] QString ExtensionForMime(const QString &mime, const QString &fallback) {
	if (mime == u"image/png") {
		return u"png"_s;
	} else if (mime == u"image/webp") {
		return u"webp"_s;
	} else if (mime == u"image/gif") {
		return u"gif"_s;
	} else if (mime.startsWith(u"image/")) {
		return u"jpg"_s;
	} else if (mime == u"audio/ogg" || mime == u"audio/opus") {
		return u"ogg"_s;
	} else if (mime == u"audio/mpeg") {
		return u"mp3"_s;
	} else if (mime == u"audio/wav" || mime == u"audio/x-wav") {
		return u"wav"_s;
	} else if (mime == u"video/webm") {
		return u"webm"_s;
	}
	return fallback;
}

[[nodiscard]] QString CleanName(QString name) {
	static const auto kBad = u"\\/:*?\"<>|"_s;
	for (auto &ch : name) {
		if (kBad.contains(ch) || ch.unicode() < 0x20) {
			ch = u'_';
		}
	}
	name = name.trimmed();
	while (name.startsWith(u'.')) {
		name.remove(0, 1);
	}
	return name.isEmpty() ? u"file"_s : name.left(180);
}

[[nodiscard]] QString UniquePath(const QString &directory, const QString &name) {
	const auto info = QFileInfo(name);
	const auto base = info.completeBaseName();
	const auto suffix = info.suffix();
	auto candidate = QDir(directory).filePath(name);
	for (auto i = 1; QFileInfo::exists(candidate) && i < 1000; ++i) {
		candidate = QDir(directory).filePath(suffix.isEmpty()
			? u"%1 (%2)"_s.arg(base).arg(i)
			: u"%1 (%2).%3"_s.arg(base).arg(i).arg(suffix));
	}
	return candidate;
}

} // namespace

Downloads *Downloads::For(Main::Session *session) {
	if (!session) {
		return nullptr;
	}
	if (const auto existing = session->findChild<Downloads*>(
			QString(),
			Qt::FindDirectChildrenOnly)) {
		return existing;
	}
	return new Downloads(session);
}

Downloads::Downloads(Main::Session *session)
: QObject(session)
, _session(session) {
	session->local().value(kSavedKey, this, [this](QJsonValue value) {
		const auto object = value.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			if (!_saved.contains(i.key())) {
				_saved.insert(i.key(), i.value().toString());
			}
		}
		_savedLoaded = true;
	});
}

QString Downloads::Key(const Data::Message &message) {
	return message.chat + u':' + QString::number(message.seq);
}

Data::MediaVariant Downloads::FileVariant(const Data::Message &message) {
	switch (message.kind) {
	case Data::MessageKind::Voice:
	case Data::MessageKind::Audio:
		return Data::MediaVariant::Audio;
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
	case Data::MessageKind::VideoNote:
		return Data::MediaVariant::Mp4;
	case Data::MessageKind::Photo:
		return Data::MediaVariant::Full;
	default:
		return Data::MediaVariant::File;
	}
}

QString Downloads::SuggestedName(const Data::Message &message) {
	const auto mime = message.media ? message.media->mime : QString();
	const auto numbered = [&](const QString &prefix, const QString &extension) {
		return u"%1_%2.%3"_s.arg(prefix).arg(message.seq).arg(extension);
	};
	switch (message.kind) {
	case Data::MessageKind::File:
	case Data::MessageKind::Audio:
		if (!message.text.trimmed().isEmpty()) {
			return CleanName(message.text);
		}
		return numbered(
			(message.kind == Data::MessageKind::Audio) ? u"audio"_s : u"file"_s,
			ExtensionForMime(
				mime,
				(message.kind == Data::MessageKind::Audio) ? u"mp3"_s : u"bin"_s));
	case Data::MessageKind::Photo:
		return numbered(u"photo"_s, ExtensionForMime(mime, u"jpg"_s));
	case Data::MessageKind::Video:
	case Data::MessageKind::VideoNote:
		return numbered(u"video"_s, ExtensionForMime(mime, u"mp4"_s));
	case Data::MessageKind::Gif:
		return numbered(u"animation"_s, u"mp4"_s);
	case Data::MessageKind::Voice:
		return numbered(u"voice"_s, ExtensionForMime(mime, u"m4a"_s));
	default:
		return numbered(u"file"_s, u"bin"_s);
	}
}

bool Downloads::IsExecutable(const QString &path) {
	static const auto kExtensions = QStringList{
		u"exe"_s, u"bat"_s, u"cmd"_s, u"com"_s, u"scr"_s, u"msi"_s,
		u"ps1"_s, u"vbs"_s, u"js"_s, u"jar"_s, u"lnk"_s, u"reg"_s,
		u"sh"_s, u"run"_s, u"desktop"_s, u"appimage"_s, u"deb"_s,
		u"rpm"_s, u"py"_s, u"pl"_s, u"hta"_s, u"cpl"_s, u"pif"_s,
	};
	return kExtensions.contains(QFileInfo(path).suffix().toLower());
}

View::DownloadState Downloads::state(const Data::Message &message) const {
	auto result = View::DownloadState();
	if (message.seq <= 0) {
		return result;
	}
	const auto key = Key(message);
	static const auto kVariants = {
		Data::MediaVariant::File,
		Data::MediaVariant::Mp4,
		Data::MediaVariant::Audio,
		Data::MediaVariant::Full,
	};
	for (const auto variant : kVariants) {
		const auto i = _jobs.constFind(key + u':' + Data::MediaVariantName(variant));
		if (i != _jobs.cend()) {
			result.loading = true;
			result.progress = i->handle ? i->handle->progress() : 0.;
			return result;
		}
	}
	result.path = savedPath(message);
	return result;
}

QString Downloads::savedPath(const Data::Message &message) const {
	const auto i = _saved.constFind(Key(message));
	return (i != _saved.cend() && QFileInfo::exists(*i)) ? *i : QString();
}

void Downloads::save(
		const Data::Message &message,
		QObject *guard,
		Fn<void(const QString &path)> done) {
	const auto existing = savedPath(message);
	if (!existing.isEmpty()) {
		if (done) {
			done(existing);
		}
		return;
	}
	start(message, FileVariant(message), true, guard, std::move(done));
}

void Downloads::fetch(
		const Data::Message &message,
		Data::MediaVariant variant,
		QObject *guard,
		Fn<void(const QString &path)> done) {
	start(message, variant, false, guard, std::move(done));
}

void Downloads::start(
		const Data::Message &message,
		Data::MediaVariant variant,
		bool copy,
		QObject *guard,
		Fn<void(const QString &)> done) {
	if (message.seq <= 0) {
		if (done) {
			done(QString());
		}
		return;
	}
	const auto key = Key(message) + u':' + Data::MediaVariantName(variant);
	const auto chat = message.chat;
	const auto seq = message.seq;
	auto &job = _jobs[key];
	if (done) {
		job.waiting.push_back({
			QPointer<QObject>(guard),
			(guard != nullptr),
			std::move(done),
		});
	}
	job.copy = job.copy || copy;
	if (job.handle) {
		return;
	}
	job.name = SuggestedName(message);
	auto options = Data::Media::Options();
	options.decode = false;
	job.handle = _session->data().media().request(
		Data::MediaRef::Attachment(chat, seq),
		variant,
		options);
	const auto handle = job.handle.get();
	if (handle->isReady()) {
		// Deferred: callers expect the answer after this call returned.
		QMetaObject::invokeMethod(this, [=, this] {
			finished(key, chat, seq);
		}, Qt::QueuedConnection);
		Q_EMIT changed(chat, seq);
		return;
	} else if (handle->isFailed()) {
		QMetaObject::invokeMethod(this, [=, this] {
			finish(key, chat, seq, QString());
		}, Qt::QueuedConnection);
		return;
	}
	connect(handle, &Data::MediaHandle::ready, this, [=, this] {
		finished(key, chat, seq);
	});
	connect(handle, &Data::MediaHandle::failed, this, [=, this] {
		finish(key, chat, seq, QString());
		Q_EMIT failed(chat, seq);
	});
	connect(handle, &Data::MediaHandle::progressChanged, this, [=, this] {
		Q_EMIT changed(chat, seq);
	});
	Q_EMIT changed(chat, seq);
}

void Downloads::finished(const QString &key, const QString &chat, qint64 seq) {
	const auto i = _jobs.find(key);
	if (i == _jobs.end() || !i->handle) {
		return;
	}
	auto cached = i->handle->filePath();
	const auto bytes = cached.isEmpty() ? i->handle->bytes() : QByteArray();
	if (cached.isEmpty() && bytes.isEmpty()) {
		finish(key, chat, seq, QString());
		Q_EMIT failed(chat, seq);
		return;
	}
	const auto copy = i->copy;
	const auto name = i->name;
	if (!copy && !cached.isEmpty()) {
		finish(key, chat, seq, cached);
		return;
	}
	// Copy on a worker thread: files may be large.
	auto directory = copy
		? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
		: QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (directory.isEmpty()) {
		directory = QDir::homePath();
	}
	if (copy) {
		directory = QDir(directory).filePath(u"Eleza"_s);
	}
	const auto weak = QPointer<Downloads>(this);
	QThreadPool::globalInstance()->start([=] {
		QDir().mkpath(directory);
		const auto target = UniquePath(directory, name);
		auto ok = false;
		if (!cached.isEmpty()) {
			ok = QFile::copy(cached, target);
		} else {
			auto file = QFile(target);
			ok = file.open(QIODevice::WriteOnly)
				&& (file.write(bytes) == bytes.size());
		}
		QMetaObject::invokeMethod(qApp, [=] {
			if (!weak) {
				return;
			}
			if (ok && copy) {
				weak->_saved.insert(chat + u':' + QString::number(seq), target);
				weak->persist();
			}
			weak->finish(key, chat, seq, ok ? target : QString());
			if (!ok) {
				Q_EMIT weak->failed(chat, seq);
			}
		}, Qt::QueuedConnection);
	});
}

void Downloads::finish(
		const QString &key,
		const QString &chat,
		qint64 seq,
		const QString &path) {
	const auto i = _jobs.find(key);
	if (i == _jobs.end()) {
		return;
	}
	const auto job = std::move(*i);
	_jobs.erase(i);
	Q_EMIT changed(chat, seq);
	for (const auto &waiter : job.waiting) {
		// A callback without a guard always runs; one whose guard died does not.
		if (waiter.callback && (waiter.guard || !waiter.guarded)) {
			waiter.callback(path);
		}
	}
}

void Downloads::cancel(const Data::Message &message) {
	const auto prefix = Key(message) + u':';
	auto removed = false;
	for (auto i = _jobs.begin(); i != _jobs.end();) {
		if (i.key().startsWith(prefix)) {
			i = _jobs.erase(i);
			removed = true;
		} else {
			++i;
		}
	}
	if (removed) {
		Q_EMIT changed(message.chat, message.seq);
	}
}

void Downloads::persist() {
	if (_saved.size() > kMaxRemembered) {
		// Forget entries whose files are gone, then the rest at random.
		for (auto i = _saved.begin(); i != _saved.end();) {
			i = QFileInfo::exists(i.value()) ? std::next(i) : _saved.erase(i);
		}
		while (_saved.size() > kMaxRemembered) {
			_saved.erase(_saved.begin());
		}
	}
	auto object = QJsonObject();
	for (auto i = _saved.cbegin(); i != _saved.cend(); ++i) {
		object.insert(i.key(), i.value());
	}
	_session->local().setValue(kSavedKey, object);
}

} // namespace History
