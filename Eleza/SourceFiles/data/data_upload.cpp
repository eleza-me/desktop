#include "data/data_upload.h"

#include "base/bytes.h"
#include "data/data_image_decode.h"
#include "crypto/crypto_primitives.h"
#include "data/data_json.h"
#include "data/data_session.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>

#include <atomic>

namespace Data {
namespace {

constexpr auto kDefaultPartSize = qint64(5) << 20;

// SHA-256 of a whole file, optionally prefixed.
[[nodiscard]] QByteArray HashFile(
		const QString &path,
		const QByteArray &prefix,
		const std::atomic<bool> &cancelled) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	auto hasher = Crypto::Sha256Hasher();
	hasher.feed(prefix);
	auto left = file.size();
	auto buffer = QByteArray(1 << 20, Qt::Uninitialized);
	while (left > 0) {
		if (cancelled.load()) {
			return QByteArray();
		}
		const auto read = file.read(buffer.data(), qMin<qint64>(buffer.size(), left));
		if (read <= 0) {
			return QByteArray();
		}
		hasher.feed(buffer.constData(), read);
		left -= read;
	}
	return hasher.result();
}

// HMAC-SHA256 (RFC 2104) of a whole file, read in pieces like the hash.
[[nodiscard]] QByteArray HmacFile(
		const QString &path,
		QByteArray key,
		const std::atomic<bool> &cancelled) {
	constexpr auto kBlockSize = 64;
	if (key.size() > kBlockSize) {
		key = Crypto::Sha256(key);
	}
	key.append(QByteArray(kBlockSize - key.size(), char(0)));
	auto inner = key;
	auto outer = key;
	for (auto i = 0; i != kBlockSize; ++i) {
		inner[i] = char(inner[i] ^ 0x36);
		outer[i] = char(outer[i] ^ 0x5c);
	}
	const auto digest = HashFile(path, inner, cancelled);
	return digest.isEmpty() ? QByteArray() : Crypto::Sha256(outer + digest);
}

[[nodiscard]] bool StateIs(const QString &state, std::initializer_list<const char*> list) {
	for (const auto item : list) {
		if (state == QLatin1StringView(item)) {
			return true;
		}
	}
	return false;
}

} // namespace

Upload::Upload(Session *session)
: QObject(session)
, _session(session) {
	_pool.setMaxThreadCount(2);
}

Upload::~Upload() {
	for (const auto &[id, job] : _jobs) {
		job->cancelled->store(true);
		for (const auto request : job->requests) {
			_session->api().cancel(request);
		}
		for (const auto &part : job->parts) {
			if (part.request) {
				_session->api().cancel(part.request);
			}
		}
	}
	// Hashing tasks post back to this object: let them end first.
	_pool.waitForDone();
}

QString Upload::KindName(UploadKind kind) {
	switch (kind) {
	case UploadKind::Image: return u"image"_s;
	case UploadKind::Video: return u"video"_s;
	case UploadKind::VideoNote: return u"video_note"_s;
	case UploadKind::Voice: return u"voice"_s;
	case UploadKind::File: return u"file"_s;
	case UploadKind::Gif: return u"gif"_s;
	case UploadKind::Audio: return u"audio"_s;
	}
	return u"file"_s;
}

UploadKind Upload::KindFromName(const QString &name) {
	if (name == u"image") {
		return UploadKind::Image;
	} else if (name == u"video") {
		return UploadKind::Video;
	} else if (name == u"video_note") {
		return UploadKind::VideoNote;
	} else if (name == u"voice") {
		return UploadKind::Voice;
	} else if (name == u"gif") {
		return UploadKind::Gif;
	} else if (name == u"audio") {
		return UploadKind::Audio;
	}
	return UploadKind::File;
}

QString Upload::PurposeName(UploadPurpose purpose) {
	switch (purpose) {
	case UploadPurpose::Avatar: return u"avatar"_s;
	case UploadPurpose::Post: return u"post"_s;
	case UploadPurpose::Chat: return u"chat"_s;
	case UploadPurpose::Conversation: return u"conversation"_s;
	case UploadPurpose::Story: return u"story"_s;
	}
	return u"chat"_s;
}

UploadKind Upload::KindFor(const QString &mime, bool asVoice) {
	if (asVoice) {
		return UploadKind::Voice;
	} else if (mime == u"image/gif") {
		return UploadKind::Gif;
	} else if (mime.startsWith(u"image/")) {
		return UploadKind::Image;
	} else if (mime.startsWith(u"video/")) {
		return UploadKind::Video;
	} else if (mime.startsWith(u"audio/")) {
		return UploadKind::Audio;
	}
	return UploadKind::File;
}

QString Upload::MessageKindFor(UploadKind kind) {
	switch (kind) {
	case UploadKind::Image: return u"photo"_s;
	case UploadKind::Video: return u"video"_s;
	case UploadKind::Gif: return u"gif"_s;
	case UploadKind::Audio: return u"audio"_s;
	case UploadKind::Voice: return u"voice"_s;
	case UploadKind::VideoNote: return u"video_note"_s;
	case UploadKind::File: return u"file"_s;
	}
	return u"file"_s;
}

UploadKind Upload::KindForMessageKind(const QString &messageKind) {
	return (messageKind == u"photo") ? UploadKind::Image : KindFromName(messageKind);
}

FileProbe Upload::Probe(const QString &path, bool asVoice) {
	auto result = FileProbe();
	const auto info = QFileInfo(path);
	if (!info.isFile()) {
		return result;
	}
	result.exists = true;
	result.fileName = info.fileName();
	result.size = info.size();
	result.mime = QMimeDatabase().mimeTypeForFile(info).name();
	result.kind = KindFor(result.mime, asVoice);
	if (result.mime.startsWith(u"image/")) {
		// Reads the header only, with the decoder every preview uses.
		const auto size = ImageFileDimensions(path);
		if (!size.isEmpty()) {
			result.width = size.width();
			result.height = size.height();
		} else if (result.kind == UploadKind::Image
			|| result.kind == UploadKind::Gif) {
			// A picture we would not decode ourselves (SVG, TIFF, HEIC, a
			// broken or an oversized one) goes as a plain file.
			result.kind = UploadKind::File;
		}
	}
	return result;
}

Upload::Job *Upload::job(Id id) {
	const auto i = _jobs.find(id);
	return (i != end(_jobs)) ? i->second.get() : nullptr;
}

void Upload::later(Id id, int ms, Fn<void()> callback) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	current->timer = std::make_unique<base::Timer>([=, this] {
		if (job(id)) {
			callback();
		}
	});
	current->timer->callOnce(ms);
}

Upload::Id Upload::start(
		const UploadRequest &request,
		base::Guard guard,
		UploadCallbacks callbacks) {
	const auto id = ++_lastId;
	auto created = std::make_unique<Job>();
	created->request = request;
	created->guard = std::move(guard);
	created->callbacks = std::move(callbacks);
	created->cancelled = std::make_shared<std::atomic<bool>>(false);
	const auto info = QFileInfo(request.filePath);
	created->size = info.isFile() ? info.size() : 0;
	if (created->request.fileName.isEmpty()) {
		created->request.fileName = info.fileName();
	}
	// Recorders report "audio/webm;codecs=opus"; the server matches the bare type.
	auto mime = request.mime.isEmpty()
		? QMimeDatabase().mimeTypeForFile(info).name()
		: request.mime;
	mime = mime.section(u';', 0, 0).trimmed();
	created->mime = mime.isEmpty() ? u"application/octet-stream"_s : mime;
	const auto size = created->size;
	_jobs.emplace(id, std::move(created));
	// Always answer asynchronously.
	QMetaObject::invokeMethod(this, [=, this] {
		if (!job(id)) {
			return;
		} else if (size <= 0) {
			fail(id, Api::Error::Local(400, u"empty"_s, u"this file cannot be sent"_s));
		} else {
			resume(id);
		}
	}, Qt::QueuedConnection);
	return id;
}

void Upload::cancel(Id id) {
	const auto i = _jobs.find(id);
	if (i == end(_jobs)) {
		return;
	}
	const auto current = std::move(i->second);
	_jobs.erase(i);
	current->cancelled->store(true);
	for (const auto request : current->requests) {
		_session->api().cancel(request);
	}
	for (const auto &part : current->parts) {
		if (part.request) {
			_session->api().cancel(part.request);
		}
	}
}

void Upload::abandon(const QString &uploadId) {
	if (uploadId.isEmpty()) {
		return;
	}
	_session->api().request(Api::Delete, u"/v1/uploads/"_s + uploadId)
		.guard(this)
		.send();
}

void Upload::fail(Id id, const Api::Error &error) {
	const auto i = _jobs.find(id);
	if (i == end(_jobs)) {
		return;
	}
	const auto current = std::move(i->second);
	_jobs.erase(i);
	current->cancelled->store(true);
	for (const auto &part : current->parts) {
		if (part.request) {
			_session->api().cancel(part.request);
		}
	}
	if (current->callbacks.fail && current->guard.alive()) {
		current->callbacks.fail(error);
	}
}

void Upload::finish(Id id) {
	const auto i = _jobs.find(id);
	if (i == end(_jobs)) {
		return;
	}
	const auto current = std::move(i->second);
	_jobs.erase(i);
	if (current->callbacks.done && current->guard.alive()) {
		current->callbacks.done({ current->uploadId, current->mediaId });
	}
}

void Upload::resume(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	} else if (current->request.resumeId.isEmpty()) {
		create(id);
		return;
	}
	current->requests.push_back(_session->api().request(
		Api::Get,
		u"/v1/uploads/"_s + current->request.resumeId)
		.guard(this)
		.done([=, this](const QJsonObject &upload) {
			const auto state = Json::String(upload, "state"_L1);
			if (StateIs(state, { "expired", "aborted", "failed" })
				|| Json::String(upload, "upload_id"_L1).isEmpty()) {
				create(id);
			} else {
				created(id, upload);
			}
		})
		.fail([=, this](const Api::Error &error) {
			// Gone or refused: start over. A transient failure is the caller's to retry.
			if (error.notFound() || !error.retryable) {
				create(id);
			} else {
				fail(id, error);
			}
		})
		.send());
}

void Upload::create(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	const auto post = [=, this] {
		const auto current = job(id);
		if (!current) {
			return;
		}
		auto body = QJsonObject{
			{ u"purpose"_s, PurposeName(current->request.purpose) },
			{ u"kind"_s, KindName(current->request.kind) },
			{ u"size"_s, double(current->size) },
			{ u"mime"_s, current->mime },
		};
		Json::Put(body, "filename"_L1, current->request.fileName);
		Json::Put(body, "sha256"_L1, current->sha);
		current->requests.push_back(_session->api().request(Api::Post, u"/v1/uploads"_s)
			.body(body)
			.guard(this)
			.done([=, this](const QJsonObject &upload) {
				created(id, upload);
			})
			.fail([=, this](const Api::Error &error) {
				fail(id, error);
			})
			.send());
	};
	if (current->size > kHashLimit || !current->request.dedup) {
		post();
		return;
	}
	// Hash on a worker thread; the answer hops back here.
	const auto path = current->request.filePath;
	const auto cancelled = current->cancelled;
	_pool.start([=, this] {
		const auto hash = HashFile(path, QByteArray(), *cancelled);
		QMetaObject::invokeMethod(this, [=, this] {
			if (const auto current = job(id)) {
				current->sha = base::bytes::toHex(hash);
				post();
			}
		}, Qt::QueuedConnection);
	});
}

void Upload::created(Id id, const QJsonObject &upload) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	current->uploadId = Json::String(upload, "upload_id"_L1);
	current->mediaId = Json::String(upload, "media_id"_L1);
	current->state = Json::String(upload, "state"_L1);
	current->partSize = Json::Int64(upload, "part_size"_L1, kDefaultPartSize);
	if (current->partSize <= 0) {
		current->partSize = kDefaultPartSize;
	}
	current->proof = upload.value("proof"_L1).toObject();
	const auto uploaded = Json::Int64s(upload.value("uploaded"_L1));
	current->parts.clear();
	for (const auto &item : upload.value("parts"_L1).toArray()) {
		const auto object = item.toObject();
		auto part = Part();
		part.number = Json::Int(object, "number"_L1);
		part.url = QUrl(Json::String(object, "url"_L1));
		part.offset = Json::Has(object, "offset"_L1)
			? Json::Int64(object, "offset"_L1)
			: (qint64(part.number) - 1) * current->partSize;
		part.length = Json::Has(object, "length"_L1)
			? Json::Int64(object, "length"_L1)
			: std::min(current->partSize, current->size - part.offset);
		if (uploaded.contains(part.number)) {
			part.finished = true;
			part.sent = part.length;
		}
		current->parts.push_back(part);
	}
	std::sort(current->parts.begin(), current->parts.end(), [](const Part &a, const Part &b) {
		return a.number < b.number;
	});
	if (current->callbacks.onUploadId && current->guard.alive()) {
		current->callbacks.onUploadId(current->uploadId);
		if (!job(id)) {
			return;
		}
	}
	// Dedup claim: prove we hold the bytes the server already has.
	if (!current->sha.isEmpty() && current->proof.contains("nonce"_L1)) {
		claim(id);
	} else {
		transfer(id);
	}
}

void Upload::claim(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	// The challenge (docs/client-media-dedup-forward.md): the nonce in hex,
	// the proof hex(HMAC-SHA256(key = the nonce's bytes, the whole file)).
	const auto alg = Json::String(current->proof, "alg"_L1);
	const auto nonce = base::bytes::fromHex(Json::String(current->proof, "nonce"_L1));
	if (nonce.isEmpty() || (!alg.isEmpty() && alg != u"hmac-sha256")) {
		transfer(id);
		return;
	}
	const auto path = current->request.filePath;
	const auto cancelled = current->cancelled;
	_pool.start([=, this] {
		const auto proof = HmacFile(path, nonce, *cancelled);
		QMetaObject::invokeMethod(this, [=, this] {
			if (!job(id)) {
				return;
			} else if (proof.isEmpty()) {
				transfer(id);
				return;
			}
			sendClaim(id, base::bytes::toHex(proof), 0);
		}, Qt::QueuedConnection);
	});
}

void Upload::sendClaim(Id id, const QString &proof, int attempt) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	current->requests.push_back(_session->api().request(
		Api::Post,
		u"/v1/uploads/%1/claim"_s.arg(QString::fromLatin1(QUrl::toPercentEncoding(current->uploadId))))
		.body(QJsonObject{ { u"proof"_s, proof } })
		.noOverloadRetry()
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto current = job(id);
			if (!current) {
				return;
			}
			if (const auto media = Json::String(result, "media_id"_L1); !media.isEmpty()) {
				current->mediaId = media;
			}
			if (current->callbacks.progress && current->guard.alive()) {
				current->callbacks.progress(current->size, current->size);
			}
			finish(id);
		})
		.fail([=, this](const Api::Error &error) {
			// Dedup only saves the transfer and never stands in the way of
			// sending: a lost answer or a server error is asked once more,
			// anything else (the content is not known) sends the bytes.
			if (attempt < kClaimRetries && (error.offline() || error.serverSide())) {
				sendClaim(id, proof, attempt + 1);
			} else {
				transfer(id);
			}
		})
		.send());
}

void Upload::transfer(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	if (StateIs(current->state, { "complete", "completed", "processing", "ready" })) {
		waitUntilReady(id);
		return;
	}
	if (current->parts.isEmpty()) {
		fail(id, Api::Error::Local(500, u"upload.no_parts"_s, u"the upload has no parts"_s));
		return;
	}
	reportProgress(id);
	pumpParts(id);
}

void Upload::reportProgress(Id id) {
	const auto current = job(id);
	if (!current || !current->callbacks.progress || !current->guard.alive()) {
		return;
	}
	auto sent = qint64(0);
	for (const auto &part : current->parts) {
		sent += part.finished ? part.length : part.sent;
	}
	current->callbacks.progress(std::min(sent, current->size), current->size);
}

void Upload::pumpParts(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	auto left = false;
	for (auto i = 0; i != current->parts.size(); ++i) {
		const auto &part = current->parts[i];
		if (part.finished) {
			continue;
		}
		left = true;
		if (!part.started && current->active < kParallelParts) {
			startPart(id, i);
		}
	}
	if (!left && current->active == 0) {
		complete(id);
	}
}

void Upload::startPart(Id id, int index) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	auto &part = current->parts[index];
	part.started = true;
	part.sent = 0;
	++current->active;
	part.request = _session->api().upload(part.url)
		.contentType(current->mime)
		.file(current->request.filePath, part.offset, part.length)
		.guard(this)
		.progress([=, this](qint64 sent, qint64) {
			if (const auto current = job(id)) {
				current->parts[index].sent = sent;
				reportProgress(id);
			}
		})
		.done([=, this] {
			const auto current = job(id);
			if (!current) {
				return;
			}
			auto &part = current->parts[index];
			part.finished = true;
			part.sent = part.length;
			part.request = 0;
			--current->active;
			reportProgress(id);
			pumpParts(id);
		})
		.fail([=, this](const Api::Error &error) {
			const auto current = job(id);
			if (!current) {
				return;
			}
			auto &part = current->parts[index];
			part.request = 0;
			part.sent = 0;
			--current->active;
			if (++part.attempt >= kPartAttempts) {
				fail(id, error);
				return;
			}
			reportProgress(id);
			// The part is tried again after attempt x 1 s, others keep going.
			const auto attempt = part.attempt;
			QTimer::singleShot(attempt * 1000, this, [=, this] {
				if (const auto current = job(id)) {
					current->parts[index].started = false;
					pumpParts(id);
				}
			});
			pumpParts(id);
		})
		.send();
}

void Upload::complete(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	current->requests.push_back(_session->api().request(
		Api::Post,
		u"/v1/uploads/%1/complete"_s.arg(QString::fromLatin1(QUrl::toPercentEncoding(current->uploadId))))
		.body(QJsonObject())
		.guard(this)
		.done([=, this] {
			waitUntilReady(id);
		})
		.fail([=, this](const Api::Error &error) {
			fail(id, error);
		})
		.send());
}

void Upload::mediaProcessed(
		const QString &mediaId,
		const QString &state,
		const QString &reason) {
	if (mediaId.isEmpty() || (state != u"ready" && state != u"rejected")) {
		return;
	}
	_processed.erase(std::remove_if(_processed.begin(), _processed.end(), [&](const auto &entry) {
		return entry.first == mediaId;
	}), _processed.end());
	_processed.push_back({ mediaId, { state, reason } });
	if (_processed.size() > size_t(kRememberProcessed)) {
		_processed.erase(_processed.begin());
	}
	for (const auto &[id, current] : _jobs) {
		if (current->mediaId == mediaId && current->readyAttempt > 0) {
			// Waiting between polls (or for one): the answer is here.
			applyProcessed(id);
			return;
		}
	}
}

bool Upload::applyProcessed(Id id) {
	const auto current = job(id);
	if (!current || current->mediaId.isEmpty()) {
		return false;
	}
	const auto i = std::find_if(_processed.begin(), _processed.end(), [&](const auto &entry) {
		return entry.first == current->mediaId;
	});
	if (i == _processed.end()) {
		return false;
	}
	const auto [state, reason] = i->second;
	_processed.erase(i);
	if (state == u"ready") {
		finish(id);
	} else {
		qInfo("Data: media %s rejected: %s", qPrintable(current->mediaId), qPrintable(reason));
		fail(id, Api::Error::Local(422, u"media.rejected"_s, u"this file cannot be sent"_s));
	}
	return true;
}

void Upload::waitUntilReady(Id id) {
	const auto current = job(id);
	if (!current) {
		return;
	}
	const auto known = std::any_of(_processed.cbegin(), _processed.cend(), [&](const auto &entry) {
		return entry.first == current->mediaId;
	});
	if (known) {
		// The signal came first. Not from inside the poll timer's own
		// callback: finishing destroys the job and its timer.
		++current->readyAttempt;
		QTimer::singleShot(0, this, [=, this] {
			applyProcessed(id);
		});
		return;
	}
	if (current->readyAttempt >= kReadyAttempts) {
		fail(id, Api::Error::Local(504, u"media.not_ready"_s, u"processing the file took too long"_s, true));
		return;
	}
	const auto attempt = current->readyAttempt++;
	const auto delay = (attempt < 10) ? 500 : (attempt < 25) ? 1000 : 4000;
	current->requests.push_back(_session->api().request(
		Api::Get,
		u"/v1/media/"_s + current->mediaId)
		.guard(this)
		.done([=, this](const QJsonObject &info) {
			const auto state = info.value("state"_L1).toString(u"ready"_s);
			if (state == u"ready" || state.isEmpty()) {
				finish(id);
			} else if (StateIs(state, { "failed", "rejected", "deleted", "expired", "aborted" })) {
				fail(id, Api::Error::Local(422, u"media."_s + state, u"this file cannot be sent"_s));
			} else {
				later(id, delay, [=, this] { waitUntilReady(id); });
			}
		})
		.fail([=, this](const Api::Error &) {
			// The state cannot be read: the send itself will tell.
			finish(id);
		})
		.send());
}

} // namespace Data
