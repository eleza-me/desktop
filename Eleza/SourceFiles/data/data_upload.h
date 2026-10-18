// Resumable media upload: `POST /v1/uploads` returns presigned part URLs,
// each part is PUT to storage (up to three at a time, three attempts each),
// `.../complete` finishes, then the media is read until it is `ready`.
// Dedup: with `sha256` the server may answer a proof challenge; a successful
// claim skips the transfer.
//
//   const auto id = upload.start(request, this, {
//       .onUploadId = [=](const QString &id) { ... },      // persist for resume
//       .progress = [=](qint64 sent, qint64 total) { ... },
//       .done = [=](const Data::UploadResult &result) { ... result.mediaId ... },
//       .fail = [=](const Api::Error &error) { ... },
//   });
//   upload.cancel(id);
//
// File hashing runs on a worker thread; nothing blocks the caller.
#pragma once

#include "api/api_client.h"
#include "base/timer.h"
#include "base/weak_guard.h"

#include <QtCore/QThreadPool>

#include <atomic>
#include <map>

namespace Data {

class Session;

enum class UploadKind {
	Image,
	Video,
	VideoNote,
	Voice,
	File,
	Gif,
	Audio,
};

enum class UploadPurpose {
	Avatar,
	Post,
	Chat,
	Conversation,
};

struct UploadRequest {
	QString filePath;
	UploadKind kind = UploadKind::File;
	UploadPurpose purpose = UploadPurpose::Chat;
	QString fileName;   // defaults to the file's name
	QString mime;       // defaults to the detected type
	QString resumeId;   // an upload id from an interrupted run
};

struct UploadResult {
	QString uploadId;
	QString mediaId;
};

struct UploadCallbacks {
	Fn<void(const QString &uploadId)> onUploadId;
	Fn<void(qint64 sent, qint64 total)> progress;
	Fn<void(const UploadResult &)> done;
	Fn<void(const Api::Error &)> fail;
};

// What a local file is, read without decoding it fully.
struct FileProbe {
	bool exists = false;
	QString fileName;
	QString mime;
	qint64 size = 0;
	int width = 0;   // images
	int height = 0;
	UploadKind kind = UploadKind::File;
};

class Upload final : public QObject {
public:
	using Id = quint64;

	explicit Upload(Session *session);
	~Upload();

	Id start(const UploadRequest &request, base::Guard guard, UploadCallbacks callbacks);
	// Stops the transfer; no callback is invoked. The server-side upload
	// stays resumable until it expires (abandon() drops it).
	void cancel(Id id);
	// `DELETE /v1/uploads/{id}` (best effort).
	void abandon(const QString &uploadId);
	// The realtime `media` signal: one of my uploads finished processing
	// (state "ready" or "rejected"). An upload waiting for it goes on at
	// once instead of at its next poll.
	void mediaProcessed(const QString &mediaId, const QString &state, const QString &reason);

	[[nodiscard]] static FileProbe Probe(const QString &path, bool asVoice = false);
	[[nodiscard]] static UploadKind KindFor(const QString &mime, bool asVoice = false);
	// "image", "video", ... as the API names upload kinds.
	[[nodiscard]] static QString KindName(UploadKind kind);
	[[nodiscard]] static UploadKind KindFromName(const QString &name);
	[[nodiscard]] static QString PurposeName(UploadPurpose purpose);
	// The message kind a chat message with this upload has ("photo", "video", ...).
	[[nodiscard]] static QString MessageKindFor(UploadKind kind);
	[[nodiscard]] static UploadKind KindForMessageKind(const QString &messageKind);

	static constexpr auto kHashLimit = qint64(64) << 20;
	static constexpr auto kParallelParts = 3;
	static constexpr auto kPartAttempts = 3;
	static constexpr auto kReadyAttempts = 60;
	static constexpr auto kRememberProcessed = 32;

private:
	struct Part {
		int number = 0;
		QUrl url;
		qint64 offset = 0;
		qint64 length = 0;
		qint64 sent = 0;
		int attempt = 0;
		bool finished = false;
		bool started = false;
		Api::RequestId request = 0;
	};
	struct Job {
		UploadRequest request;
		base::Guard guard;
		UploadCallbacks callbacks;
		qint64 size = 0;
		QString mime;
		QString sha;
		QString uploadId;
		QString mediaId;
		QJsonObject proof;
		QString state;
		qint64 partSize = 0;
		QVector<Part> parts;
		int active = 0;
		int readyAttempt = 0;
		QVector<Api::RequestId> requests;
		std::shared_ptr<std::atomic<bool>> cancelled;
		std::unique_ptr<base::Timer> timer;
	};

	[[nodiscard]] Job *job(Id id);
	void resume(Id id);
	void create(Id id);
	void created(Id id, const QJsonObject &upload);
	void claim(Id id);
	void transfer(Id id);
	void pumpParts(Id id);
	void startPart(Id id, int index);
	void reportProgress(Id id);
	void complete(Id id);
	void waitUntilReady(Id id);
	void finish(Id id);
	void fail(Id id, const Api::Error &error);
	void later(Id id, int ms, Fn<void()> callback);
	// Finishes or fails the job by the outcome the signal told; false when
	// none is known.
	bool applyProcessed(Id id);

	Session *_session = nullptr;
	QThreadPool _pool;
	std::map<Id, std::unique_ptr<Job>> _jobs;
	Id _lastId = 0;
	// Outcomes the signal told before the upload asked for them (the
	// signal can overtake the answer of /complete): media id -> state, reason.
	std::vector<std::pair<QString, std::pair<QString, QString>>> _processed;

};

} // namespace Data
