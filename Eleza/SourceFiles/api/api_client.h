// JSON client for the `/v1` API over one QNetworkAccessManager.
//
//   api.request(Api::Post, u"/v1/chats/c1/read"_s)
//       .body(QJsonObject{ { u"seq"_s, 12 } })
//       .guard(this)
//       .done([=](const QJsonObject &result) { ... })
//       .fail([=](const Api::Error &error) { ... })
//       .send();
//
// What every call gets:
//   * bearer auth from the TokenProvider, with one refresh-and-retry on 401
//     (concurrent 401s share a single refresh inside the provider);
//   * `Idempotency-Key` on every mutation and one `X-Request-Id` per call,
//     both reused on the retries;
//   * `X-Eleza-Client: desktop/<version> (<os>)` and `Accept-Language`;
//   * problem+json mapped to Api::Error (with Retry-After on 429 / 503);
//   * up to two short retries on 503 / `server.overloaded`;
//   * 426 -> the upgradeRequired() signal.
//
// Callbacks run on the client's thread and are dropped when the guard object
// (`.guard(this)`) or the client itself is destroyed. Nothing here blocks.
//
// download() fetches raw bytes (media) following redirects by hand so the
// bearer token is never forwarded to the storage host; upload() PUTs bytes or
// a file slice to a presigned URL. Both report progress and can be cancelled.
#pragma once

#include "api/api_error.h"
#include "base/timer.h"
#include "base/weak_guard.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>

#include <map>

class QNetworkReply;

namespace Api {

enum Method {
	Get,
	Post,
	Put,
	Patch,
	Delete,
};

using RequestId = quint64;

// Where access tokens come from (implemented by Main::Account).
class TokenProvider {
public:
	virtual ~TokenProvider() = default;

	// Hands over the current access token, refreshed first when it is missing
	// or about to expire; an empty string when nobody is signed in.
	virtual void accessToken(Fn<void(QString)> done) = 0;
	// A request with `failedToken` got 401: `done(true)` once a usable token
	// exists (possibly after a refresh), `done(false)` when it cannot.
	virtual void refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) = 0;
};

class Client;

struct RequestSpec {
	Method method = Get;
	QString path;
	QVector<QPair<QString, QString>> query;
	QByteArray body;
	QByteArray contentType;
	bool hasBody = false;
	bool authenticated = true;
	bool retryOverload = true;
	bool sendIdempotencyKey = true;
	QString idempotencyKey;
	QString requestId;
	int timeoutMs = 30000;
	base::Guard guard;
	Fn<void(const QJsonValue &)> done;
	Fn<void(const Error &)> fail;
};

class RequestBuilder {
public:
	RequestBuilder(Client *client, Method method, const QString &path);

	// Empty values are skipped (like the web client's query helper).
	RequestBuilder &query(const QString &name, const QString &value);
	RequestBuilder &query(const QString &name, const char *value) = delete;
	RequestBuilder &query(const QString &name, qint64 value);
	RequestBuilder &query(const QString &name, int value);
	RequestBuilder &query(const QVector<QPair<QString, QString>> &pairs);

	RequestBuilder &body(const QJsonObject &json);
	RequestBuilder &body(const QJsonArray &json);
	RequestBuilder &rawBody(const QByteArray &bytes, const QByteArray &contentType);

	// No Authorization header (sign-in, client-config).
	RequestBuilder &unauthenticated();
	// Defaults to a random UUID for every non-GET request.
	RequestBuilder &idempotencyKey(const QString &key);
	RequestBuilder &noIdempotencyKey();
	RequestBuilder &requestId(const QString &id);
	// Do not retry on 503 / overload (the caller has its own policy).
	RequestBuilder &noOverloadRetry();
	RequestBuilder &timeout(int ms);

	// Callbacks are dropped once `object` is destroyed.
	RequestBuilder &guard(QObject *object);
	RequestBuilder &guard(base::Guard guard);

	// The response body as an object ({} for an empty / non-object body).
	RequestBuilder &done(Fn<void(const QJsonObject &)> callback);
	RequestBuilder &done(Fn<void()> callback);
	// The response body whatever JSON it is (arrays).
	RequestBuilder &doneValue(Fn<void(const QJsonValue &)> callback);
	RequestBuilder &fail(Fn<void(const Error &)> callback);

	RequestId send();

private:
	Client *_client = nullptr;
	RequestSpec _spec;

};

struct DownloadResult {
	QByteArray bytes;     // empty when the download went to a file
	QString filePath;
	QString contentType;
	qint64 size = 0;
};

struct DownloadSpec {
	QString target;       // "/v1/..." (API path) or an absolute URL
	bool authenticated = true;
	QString filePath;
	qint64 maxBytes = 0;  // 0: unlimited
	int timeoutMs = 60000;
	base::Guard guard;
	Fn<void(qint64, qint64)> progress;
	Fn<void(const DownloadResult &)> done;
	Fn<void(const Error &)> fail;
};

class DownloadBuilder {
public:
	DownloadBuilder(Client *client, const QString &target);

	DownloadBuilder &unauthenticated();
	// Stream to this file (written as <path>.part, renamed when complete).
	DownloadBuilder &toFile(const QString &path);
	// Abort with code "too_large" beyond this size.
	DownloadBuilder &maxBytes(qint64 bytes);
	DownloadBuilder &timeout(int ms);
	DownloadBuilder &guard(QObject *object);
	DownloadBuilder &guard(base::Guard guard);
	DownloadBuilder &progress(Fn<void(qint64 received, qint64 total)> callback);
	DownloadBuilder &done(Fn<void(const DownloadResult &)> callback);
	DownloadBuilder &fail(Fn<void(const Error &)> callback);

	RequestId send();

private:
	Client *_client = nullptr;
	DownloadSpec _spec;

};

struct UploadSpec {
	QUrl url;
	Method method = Put;
	QByteArray contentType;
	QVector<QPair<QByteArray, QByteArray>> headers;
	QByteArray data;
	QString filePath;
	qint64 offset = 0;
	qint64 length = -1;   // -1: to the end of the file
	bool fromFile = false;
	int timeoutMs = 120000; // of inactivity
	base::Guard guard;
	Fn<void(qint64, qint64)> progress;
	Fn<void()> done;
	Fn<void(const Error &)> fail;
};

class UploadBuilder {
public:
	UploadBuilder(Client *client, const QUrl &url);

	UploadBuilder &method(Method method);
	UploadBuilder &contentType(const QString &mime);
	UploadBuilder &header(const QByteArray &name, const QByteArray &value);
	UploadBuilder &data(const QByteArray &bytes);
	UploadBuilder &file(const QString &path, qint64 offset = 0, qint64 length = -1);
	UploadBuilder &timeout(int ms);
	UploadBuilder &guard(QObject *object);
	UploadBuilder &guard(base::Guard guard);
	UploadBuilder &progress(Fn<void(qint64 sent, qint64 total)> callback);
	UploadBuilder &done(Fn<void()> callback);
	UploadBuilder &fail(Fn<void(const Error &)> callback);

	RequestId send();

private:
	Client *_client = nullptr;
	UploadSpec _spec;

};

class Client final : public QObject {
	Q_OBJECT

public:
	// `base` is the API origin ("https://api.eleza.me", "http://127.0.0.1:8000");
	// `version` goes into the X-Eleza-Client header.
	Client(const QString &base, const QString &version, QObject *parent = nullptr);
	~Client();

	void setTokenProvider(TokenProvider *provider);
	// "fa" / "en": sent as Accept-Language.
	void setLanguage(const QString &language);
	[[nodiscard]] QString language() const {
		return _language;
	}

	[[nodiscard]] QString base() const {
		return _base;
	}
	// "desktop/0.1.0 (ubuntu 24.04)".
	[[nodiscard]] QString clientHeader() const {
		return _clientHeader;
	}
	[[nodiscard]] QString version() const {
		return _version;
	}
	// The absolute URL of an API path.
	[[nodiscard]] QUrl url(const QString &path) const;
	// ws(s)://<host>/v2/ws
	[[nodiscard]] QUrl webSocketUrl() const;
	[[nodiscard]] QNetworkAccessManager &manager() {
		return _manager;
	}
	// False after a JSON request got no HTTP answer at all (no network, the
	// server is unreachable), true again once one did.
	[[nodiscard]] bool reachable() const {
		return _reachable;
	}

	[[nodiscard]] RequestBuilder request(Method method, const QString &path);
	[[nodiscard]] DownloadBuilder download(const QString &pathOrUrl);
	[[nodiscard]] UploadBuilder upload(const QUrl &url);

	// Stops a request / download / upload; its callbacks are not invoked.
	void cancel(RequestId id);
	// Stops everything issued with this guard object.
	void cancelAll(QObject *guard);
	// The signed-in user is changing: stops every request that carries (or is
	// waiting for) the bearer token and every upload, without callbacks. A
	// request retried after a 401 or a token refresh must never go out with
	// the next account's token.
	void abortAuthenticated();

	// An API origin we are willing to send tokens to: https, or http on the
	// loopback interface (test servers). `allowInsecure` lifts the loopback
	// rule (debug builds only pass it).
	[[nodiscard]] static bool IsAcceptableBase(const QString &base, bool allowInsecure = false);

	[[nodiscard]] static QString MethodName(Method method);

Q_SIGNALS:
	// The server answered 426: this build is too old.
	void upgradeRequired();
	void reachableChanged(bool reachable);
	// Every failed JSON request, for logging / diagnostics.
	void requestFailed(const QString &method, const QString &path, const Api::Error &error);

private:
	friend class RequestBuilder;
	friend class DownloadBuilder;
	friend class UploadBuilder;

	struct Call;
	struct Download;
	struct Upload;

	RequestId start(RequestSpec &&spec);
	RequestId start(DownloadSpec &&spec);
	RequestId start(UploadSpec &&spec);

	void withToken(RequestId id, bool authenticated, Fn<void(QString)> then);
	void perform(RequestId id);
	void finished(RequestId id);
	void finishCall(RequestId id, const QJsonValue &result);
	void failCall(RequestId id, const Error &error);

	void performDownload(RequestId id);
	void downloadFinished(RequestId id);
	void failDownload(RequestId id, const Error &error);

	void performUpload(RequestId id);

	void setReachable(bool reachable);
	void applyCommonHeaders(QNetworkRequest &request) const;
	[[nodiscard]] bool sameOriginAsBase(const QUrl &url) const;
	[[nodiscard]] bool acceptableRemote(const QUrl &url) const;

	QString _base;
	QString _version;
	QString _clientHeader;
	QString _language = QStringLiteral("fa");
	QNetworkAccessManager _manager;
	TokenProvider *_tokens = nullptr;
	bool _reachable = true;
	RequestId _lastId = 0;
	std::map<RequestId, std::unique_ptr<Call>> _calls;
	std::map<RequestId, std::unique_ptr<Download>> _downloads;
	std::map<RequestId, std::unique_ptr<Upload>> _uploads;

};

} // namespace Api
