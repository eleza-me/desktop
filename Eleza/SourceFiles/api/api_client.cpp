#include "api/api_client.h"

#include "base/platform_file.h"
#include "base/random.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Api {
namespace {

constexpr auto kMaxRedirects = 5;
constexpr auto kMaxOverloadRetries = 2;
constexpr auto kMaxOverloadWait = 5.;

// A window of a file as an upload body.
class FileSlice final : public QIODevice {
public:
	FileSlice(const QString &path, qint64 offset, qint64 length, QObject *parent)
	: QIODevice(parent)
	, _file(path)
	, _offset(offset)
	, _length(length) {
	}

	[[nodiscard]] bool prepare() {
		if (!_file.open(QIODevice::ReadOnly)) {
			return false;
		}
		const auto available = _file.size() - _offset;
		if (_offset < 0 || available < 0) {
			return false;
		}
		_length = (_length < 0) ? available : qMin(_length, available);
		return _file.seek(_offset) && open(QIODevice::ReadOnly);
	}

	bool isSequential() const override {
		return false;
	}
	qint64 size() const override {
		return _length;
	}
	bool seek(qint64 position) override {
		return QIODevice::seek(position) && _file.seek(_offset + position);
	}
	bool atEnd() const override {
		return pos() >= _length;
	}
	qint64 bytesAvailable() const override {
		return qMax<qint64>(0, _length - pos()) + QIODevice::bytesAvailable();
	}

protected:
	qint64 readData(char *data, qint64 maxSize) override {
		const auto left = _length - pos();
		if (left <= 0) {
			return 0;
		}
		return _file.read(data, qMin(maxSize, left));
	}
	qint64 writeData(const char *, qint64) override {
		return -1;
	}

private:
	QFile _file;
	qint64 _offset = 0;
	qint64 _length = 0;

};

[[nodiscard]] int StatusOf(QNetworkReply *reply) {
	return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

[[nodiscard]] QString ReasonOf(QNetworkReply *reply) {
	return reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
}

} // namespace

struct Client::Call {
	RequestSpec spec;
	QString token;
	QPointer<QNetworkReply> reply;
	std::unique_ptr<base::Timer> retryTimer;
	bool retriedAuth = false;
	int overload = 0;
};

struct Client::Download {
	DownloadSpec spec;
	QString token;
	QUrl current;
	QPointer<QNetworkReply> reply;
	std::unique_ptr<QFile> file;
	QByteArray bytes;
	qint64 received = 0;
	int redirects = 0;
	bool retriedAuth = false;
	bool tooLarge = false;
};

struct Client::Upload {
	UploadSpec spec;
	QPointer<QNetworkReply> reply;
};

RequestBuilder::RequestBuilder(Client *client, Method method, const QString &path)
: _client(client) {
	_spec.method = method;
	_spec.path = path;
}

RequestBuilder &RequestBuilder::query(const QString &name, const QString &value) {
	if (!value.isEmpty()) {
		_spec.query.push_back({ name, value });
	}
	return *this;
}

RequestBuilder &RequestBuilder::query(const QString &name, qint64 value) {
	_spec.query.push_back({ name, QString::number(value) });
	return *this;
}

RequestBuilder &RequestBuilder::query(const QString &name, int value) {
	return query(name, qint64(value));
}

RequestBuilder &RequestBuilder::query(const QVector<QPair<QString, QString>> &pairs) {
	for (const auto &[name, value] : pairs) {
		query(name, value);
	}
	return *this;
}

RequestBuilder &RequestBuilder::body(const QJsonObject &json) {
	_spec.body = QJsonDocument(json).toJson(QJsonDocument::Compact);
	_spec.contentType = "application/json"_ba;
	_spec.hasBody = true;
	return *this;
}

RequestBuilder &RequestBuilder::body(const QJsonArray &json) {
	_spec.body = QJsonDocument(json).toJson(QJsonDocument::Compact);
	_spec.contentType = "application/json"_ba;
	_spec.hasBody = true;
	return *this;
}

RequestBuilder &RequestBuilder::rawBody(const QByteArray &bytes, const QByteArray &contentType) {
	_spec.body = bytes;
	_spec.contentType = contentType;
	_spec.hasBody = true;
	return *this;
}

RequestBuilder &RequestBuilder::unauthenticated() {
	_spec.authenticated = false;
	return *this;
}

RequestBuilder &RequestBuilder::idempotencyKey(const QString &key) {
	_spec.idempotencyKey = key;
	_spec.sendIdempotencyKey = true;
	return *this;
}

RequestBuilder &RequestBuilder::noIdempotencyKey() {
	_spec.sendIdempotencyKey = false;
	return *this;
}

RequestBuilder &RequestBuilder::requestId(const QString &id) {
	_spec.requestId = id;
	return *this;
}

RequestBuilder &RequestBuilder::noOverloadRetry() {
	_spec.retryOverload = false;
	return *this;
}

RequestBuilder &RequestBuilder::timeout(int ms) {
	_spec.timeoutMs = ms;
	return *this;
}

RequestBuilder &RequestBuilder::guard(QObject *object) {
	_spec.guard = base::Guard(object);
	return *this;
}

RequestBuilder &RequestBuilder::guard(base::Guard guard) {
	_spec.guard = std::move(guard);
	return *this;
}

RequestBuilder &RequestBuilder::done(Fn<void(const QJsonObject &)> callback) {
	_spec.done = [callback = std::move(callback)](const QJsonValue &value) {
		callback(value.toObject());
	};
	return *this;
}

RequestBuilder &RequestBuilder::done(Fn<void()> callback) {
	_spec.done = [callback = std::move(callback)](const QJsonValue &) {
		callback();
	};
	return *this;
}

RequestBuilder &RequestBuilder::doneValue(Fn<void(const QJsonValue &)> callback) {
	_spec.done = std::move(callback);
	return *this;
}

RequestBuilder &RequestBuilder::fail(Fn<void(const Error &)> callback) {
	_spec.fail = std::move(callback);
	return *this;
}

RequestId RequestBuilder::send() {
	return _client->start(std::move(_spec));
}

DownloadBuilder::DownloadBuilder(Client *client, const QString &target)
: _client(client) {
	_spec.target = target;
	// Absolute URLs (CDN, presigned storage) never get the bearer token.
	_spec.authenticated = target.startsWith(u'/');
}

DownloadBuilder &DownloadBuilder::unauthenticated() {
	_spec.authenticated = false;
	return *this;
}

DownloadBuilder &DownloadBuilder::toFile(const QString &path) {
	_spec.filePath = path;
	return *this;
}

DownloadBuilder &DownloadBuilder::maxBytes(qint64 bytes) {
	_spec.maxBytes = bytes;
	return *this;
}

DownloadBuilder &DownloadBuilder::timeout(int ms) {
	_spec.timeoutMs = ms;
	return *this;
}

DownloadBuilder &DownloadBuilder::guard(QObject *object) {
	_spec.guard = base::Guard(object);
	return *this;
}

DownloadBuilder &DownloadBuilder::guard(base::Guard guard) {
	_spec.guard = std::move(guard);
	return *this;
}

DownloadBuilder &DownloadBuilder::progress(Fn<void(qint64, qint64)> callback) {
	_spec.progress = std::move(callback);
	return *this;
}

DownloadBuilder &DownloadBuilder::done(Fn<void(const DownloadResult &)> callback) {
	_spec.done = std::move(callback);
	return *this;
}

DownloadBuilder &DownloadBuilder::fail(Fn<void(const Error &)> callback) {
	_spec.fail = std::move(callback);
	return *this;
}

RequestId DownloadBuilder::send() {
	return _client->start(std::move(_spec));
}

UploadBuilder::UploadBuilder(Client *client, const QUrl &url) : _client(client) {
	_spec.url = url;
}

UploadBuilder &UploadBuilder::method(Method method) {
	_spec.method = method;
	return *this;
}

UploadBuilder &UploadBuilder::contentType(const QString &mime) {
	_spec.contentType = mime.toLatin1();
	return *this;
}

UploadBuilder &UploadBuilder::header(const QByteArray &name, const QByteArray &value) {
	_spec.headers.push_back({ name, value });
	return *this;
}

UploadBuilder &UploadBuilder::data(const QByteArray &bytes) {
	_spec.data = bytes;
	_spec.fromFile = false;
	return *this;
}

UploadBuilder &UploadBuilder::file(const QString &path, qint64 offset, qint64 length) {
	_spec.filePath = path;
	_spec.offset = offset;
	_spec.length = length;
	_spec.fromFile = true;
	return *this;
}

UploadBuilder &UploadBuilder::timeout(int ms) {
	_spec.timeoutMs = ms;
	return *this;
}

UploadBuilder &UploadBuilder::guard(QObject *object) {
	_spec.guard = base::Guard(object);
	return *this;
}

UploadBuilder &UploadBuilder::guard(base::Guard guard) {
	_spec.guard = std::move(guard);
	return *this;
}

UploadBuilder &UploadBuilder::progress(Fn<void(qint64, qint64)> callback) {
	_spec.progress = std::move(callback);
	return *this;
}

UploadBuilder &UploadBuilder::done(Fn<void()> callback) {
	_spec.done = std::move(callback);
	return *this;
}

UploadBuilder &UploadBuilder::fail(Fn<void(const Error &)> callback) {
	_spec.fail = std::move(callback);
	return *this;
}

RequestId UploadBuilder::send() {
	return _client->start(std::move(_spec));
}

Client::Client(const QString &base, const QString &version, QObject *parent)
: QObject(parent)
, _base(base.endsWith(u'/') ? base.chopped(1) : base)
, _version(version)
, _clientHeader(u"desktop/%1 (%2)"_s.arg(version, base::platform::SystemDescription())) {
	qRegisterMetaType<Api::Error>();
	_manager.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

Client::~Client() {
	// Replies are children of the manager; make sure no slot runs into a
	// half-destroyed client.
	for (const auto &[id, call] : _calls) {
		if (call->reply) {
			call->reply->disconnect(this);
			call->reply->abort();
		}
	}
	for (const auto &[id, download] : _downloads) {
		if (download->reply) {
			download->reply->disconnect(this);
			download->reply->abort();
		}
		if (download->file) {
			download->file->remove();
		}
	}
	for (const auto &[id, upload] : _uploads) {
		if (upload->reply) {
			upload->reply->disconnect(this);
			upload->reply->abort();
		}
	}
}

void Client::setTokenProvider(TokenProvider *provider) {
	_tokens = provider;
}

void Client::setLanguage(const QString &language) {
	_language = language.isEmpty() ? u"fa"_s : language;
}

QUrl Client::url(const QString &path) const {
	return QUrl(_base + path);
}

QUrl Client::webSocketUrl() const {
	auto result = QUrl(_base);
	result.setScheme((result.scheme() == u"http") ? u"ws"_s : u"wss"_s);
	auto path = result.path();
	if (path.endsWith(u'/')) {
		path.chop(1);
	}
	result.setPath(path + u"/v2/ws"_s);
	return result;
}

RequestBuilder Client::request(Method method, const QString &path) {
	return RequestBuilder(this, method, path);
}

DownloadBuilder Client::download(const QString &pathOrUrl) {
	return DownloadBuilder(this, pathOrUrl);
}

UploadBuilder Client::upload(const QUrl &url) {
	return UploadBuilder(this, url);
}

QString Client::MethodName(Method method) {
	switch (method) {
	case Get: return u"GET"_s;
	case Post: return u"POST"_s;
	case Put: return u"PUT"_s;
	case Patch: return u"PATCH"_s;
	case Delete: return u"DELETE"_s;
	}
	return u"GET"_s;
}

void Client::applyCommonHeaders(QNetworkRequest &request) const {
	request.setRawHeader("X-Eleza-Client"_ba, _clientHeader.toUtf8());
	request.setRawHeader("Accept-Language"_ba, _language.toLatin1());
	request.setHeader(QNetworkRequest::UserAgentHeader, u"Eleza-Desktop/"_s + _version);
}

bool Client::sameOriginAsBase(const QUrl &url) const {
	const auto base = QUrl(_base);
	return url.scheme() == base.scheme()
		&& url.host() == base.host()
		&& url.port(-1) == base.port(-1);
}

void Client::cancel(RequestId id) {
	if (const auto i = _calls.find(id); i != end(_calls)) {
		const auto call = std::move(i->second);
		_calls.erase(i);
		if (call->reply) {
			call->reply->disconnect(this);
			call->reply->abort();
			call->reply->deleteLater();
		}
	} else if (const auto j = _downloads.find(id); j != end(_downloads)) {
		const auto download = std::move(j->second);
		_downloads.erase(j);
		if (download->reply) {
			download->reply->disconnect(this);
			download->reply->abort();
			download->reply->deleteLater();
		}
		if (download->file) {
			download->file->remove();
		}
	} else if (const auto k = _uploads.find(id); k != end(_uploads)) {
		const auto upload = std::move(k->second);
		_uploads.erase(k);
		if (upload->reply) {
			upload->reply->disconnect(this);
			upload->reply->abort();
			upload->reply->deleteLater();
		}
	}
}

void Client::cancelAll(QObject *guard) {
	if (!guard) {
		return;
	}
	auto ids = QVector<RequestId>();
	for (const auto &[id, call] : _calls) {
		if (call->spec.guard.object() == guard) {
			ids.push_back(id);
		}
	}
	for (const auto &[id, download] : _downloads) {
		if (download->spec.guard.object() == guard) {
			ids.push_back(id);
		}
	}
	for (const auto &[id, upload] : _uploads) {
		if (upload->spec.guard.object() == guard) {
			ids.push_back(id);
		}
	}
	for (const auto id : ids) {
		cancel(id);
	}
}

// ---- JSON requests

RequestId Client::start(RequestSpec &&spec) {
	const auto id = ++_lastId;
	auto call = std::make_unique<Call>();
	call->spec = std::move(spec);
	if (call->spec.requestId.isEmpty()) {
		call->spec.requestId = base::RandomUuid();
	}
	if (call->spec.method == Get) {
		call->spec.sendIdempotencyKey = false;
	} else if (call->spec.sendIdempotencyKey && call->spec.idempotencyKey.isEmpty()) {
		call->spec.idempotencyKey = base::RandomUuid();
	}
	const auto authenticated = call->spec.authenticated;
	_calls.emplace(id, std::move(call));
	withToken(id, authenticated, [=, this](QString token) {
		const auto i = _calls.find(id);
		if (i == end(_calls)) {
			return;
		}
		i->second->token = token;
		perform(id);
	});
	return id;
}

void Client::withToken(RequestId id, bool authenticated, Fn<void(QString)> then) {
	Q_UNUSED(id);
	if (!authenticated || !_tokens) {
		// Always asynchronous: send() returns the id before any callback runs.
		QMetaObject::invokeMethod(this, [then = std::move(then)] {
			then(QString());
		}, Qt::QueuedConnection);
		return;
	}
	_tokens->accessToken(base::guard(this, [this, then = std::move(then)](QString token) mutable {
		QMetaObject::invokeMethod(this, [then = std::move(then), token] {
			then(token);
		}, Qt::QueuedConnection);
	}));
}

void Client::perform(RequestId id) {
	const auto i = _calls.find(id);
	if (i == end(_calls)) {
		return;
	}
	const auto call = i->second.get();
	// A dead guard drops the callbacks, never the request: a mutation issued
	// by a screen that closed right away still has to reach the server.
	auto target = url(call->spec.path);
	if (!call->spec.query.isEmpty()) {
		auto query = QUrlQuery(target);
		for (const auto &[name, value] : call->spec.query) {
			query.addQueryItem(
				QString::fromLatin1(QUrl::toPercentEncoding(name)),
				QString::fromLatin1(QUrl::toPercentEncoding(value)));
		}
		target.setQuery(query);
	}
	auto request = QNetworkRequest(target);
	applyCommonHeaders(request);
	request.setRawHeader("Accept"_ba, "application/json"_ba);
	request.setRawHeader("X-Request-Id"_ba, call->spec.requestId.toLatin1());
	if (!call->token.isEmpty()) {
		request.setRawHeader("Authorization"_ba, "Bearer "_ba + call->token.toLatin1());
	}
	if (call->spec.sendIdempotencyKey && !call->spec.idempotencyKey.isEmpty()) {
		request.setRawHeader("Idempotency-Key"_ba, call->spec.idempotencyKey.toLatin1());
	}
	if (call->spec.hasBody) {
		request.setHeader(QNetworkRequest::ContentTypeHeader, call->spec.contentType);
	}
	request.setTransferTimeout(call->spec.timeoutMs);
	const auto verb = MethodName(call->spec.method).toLatin1();
	const auto reply = call->spec.hasBody
		? _manager.sendCustomRequest(request, verb, call->spec.body)
		: _manager.sendCustomRequest(request, verb);
	call->reply = reply;
	connect(reply, &QNetworkReply::finished, this, [=, this] {
		finished(id);
	});
}

void Client::finished(RequestId id) {
	const auto i = _calls.find(id);
	if (i == end(_calls)) {
		return;
	}
	const auto call = i->second.get();
	const auto reply = call->reply.data();
	if (!reply) {
		return;
	}
	call->reply = nullptr;
	reply->deleteLater();

	const auto status = StatusOf(reply);
	const auto body = reply->isOpen() ? reply->readAll() : QByteArray();
	const auto &spec = call->spec;
	if (status == 0) {
		// No HTTP answer at all: DNS, refused, reset, timeout, TLS.
		failCall(id, Error::Offline(reply->errorString()));
		return;
	}
	if (status >= 200 && status < 300) {
		if (body.trimmed().isEmpty()) {
			finishCall(id, QJsonObject());
			return;
		}
		auto parseError = QJsonParseError();
		const auto document = QJsonDocument::fromJson(body, &parseError);
		if (parseError.error != QJsonParseError::NoError) {
			auto error = Error::Local(status, u"decoding"_s, u"bad json"_s);
			error.requestId = spec.requestId;
			failCall(id, error);
			return;
		}
		finishCall(id, document.isArray()
			? QJsonValue(document.array())
			: QJsonValue(document.object()));
		return;
	}
	const auto error = Error::FromResponse(
		status,
		body,
		reply->rawHeader("Retry-After"_ba),
		ReasonOf(reply),
		spec.requestId);
	if (status == 401
		&& spec.authenticated
		&& _tokens
		&& !call->retriedAuth
		&& !spec.path.startsWith(u"/v1/auth/")) {
		call->retriedAuth = true;
		const auto failedToken = call->token;
		_tokens->refreshAfterUnauthorized(failedToken, base::guard(this, [=, this](bool ok) {
			if (_calls.find(id) == end(_calls)) {
				return;
			} else if (!ok) {
				failCall(id, error);
				return;
			}
			withToken(id, true, [=, this](QString token) {
				const auto i = _calls.find(id);
				if (i == end(_calls)) {
					return;
				}
				i->second->token = token;
				perform(id);
			});
		}));
		return;
	}
	if (status == 426) {
		Q_EMIT upgradeRequired();
	}
	if ((status == 503 || error.code == u"server.overloaded")
		&& spec.retryOverload
		&& call->overload < kMaxOverloadRetries) {
		++call->overload;
		const auto wait = error.retryAfter.value_or(0.4 * call->overload);
		if (wait <= kMaxOverloadWait) {
			call->retryTimer = std::make_unique<base::Timer>([=, this] {
				perform(id);
			});
			call->retryTimer->callOnce(int(wait * 1000));
			return;
		}
	}
	failCall(id, error);
}

void Client::finishCall(RequestId id, const QJsonValue &result) {
	const auto i = _calls.find(id);
	if (i == end(_calls)) {
		return;
	}
	const auto call = std::move(i->second);
	_calls.erase(i);
	if (call->spec.done && call->spec.guard.alive()) {
		call->spec.done(result);
	}
}

void Client::failCall(RequestId id, const Error &error) {
	const auto i = _calls.find(id);
	if (i == end(_calls)) {
		return;
	}
	const auto call = std::move(i->second);
	_calls.erase(i);
	Q_EMIT requestFailed(MethodName(call->spec.method), call->spec.path, error);
	if (call->spec.fail && call->spec.guard.alive()) {
		call->spec.fail(error);
	}
}

// ---- downloads

RequestId Client::start(DownloadSpec &&spec) {
	const auto id = ++_lastId;
	auto download = std::make_unique<Download>();
	download->spec = std::move(spec);
	download->current = download->spec.target.startsWith(u'/')
		? url(download->spec.target)
		: QUrl(download->spec.target);
	const auto authenticated = download->spec.authenticated;
	_downloads.emplace(id, std::move(download));
	withToken(id, authenticated, [=, this](QString token) {
		const auto i = _downloads.find(id);
		if (i == end(_downloads)) {
			return;
		}
		i->second->token = token;
		performDownload(id);
	});
	return id;
}

void Client::performDownload(RequestId id) {
	const auto i = _downloads.find(id);
	if (i == end(_downloads)) {
		return;
	}
	const auto download = i->second.get();
	if (!download->spec.guard.alive()) {
		_downloads.erase(i);
		return;
	}
	download->bytes.clear();
	download->received = 0;
	download->tooLarge = false;
	if (download->file) {
		download->file->remove();
		download->file = nullptr;
	}
	auto request = QNetworkRequest(download->current);
	if (sameOriginAsBase(download->current)) {
		applyCommonHeaders(request);
		// The token goes to the API only, never to where it redirects.
		if (download->spec.authenticated && !download->token.isEmpty()) {
			request.setRawHeader("Authorization"_ba, "Bearer "_ba + download->token.toLatin1());
		}
	}
	request.setTransferTimeout(download->spec.timeoutMs);
	const auto reply = _manager.get(request);
	download->reply = reply;
	connect(reply, &QNetworkReply::readyRead, this, [=, this] {
		const auto i = _downloads.find(id);
		if (i == end(_downloads) || i->second->reply != reply) {
			return;
		}
		const auto download = i->second.get();
		const auto status = StatusOf(reply);
		if (status < 200 || status >= 300) {
			return; // error and redirect bodies are read when finished
		}
		const auto chunk = reply->readAll();
		download->received += chunk.size();
		if (download->spec.maxBytes > 0 && download->received > download->spec.maxBytes) {
			download->tooLarge = true;
			reply->abort();
			return;
		}
		if (download->spec.filePath.isEmpty()) {
			download->bytes.append(chunk);
		} else {
			if (!download->file) {
				base::platform::EnsureDirectory(QFileInfo(download->spec.filePath).absolutePath());
				download->file = std::make_unique<QFile>(download->spec.filePath + u".part"_s);
				if (!download->file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					download->file = nullptr;
					reply->abort();
					return;
				}
			}
			download->file->write(chunk);
		}
	});
	connect(reply, &QNetworkReply::downloadProgress, this, [=, this](qint64 received, qint64 total) {
		const auto i = _downloads.find(id);
		if (i == end(_downloads) || i->second->reply != reply) {
			return;
		}
		const auto status = StatusOf(reply);
		const auto &spec = i->second->spec;
		if (status >= 200 && status < 300 && spec.progress && spec.guard.alive()) {
			spec.progress(received, total);
		}
	});
	connect(reply, &QNetworkReply::finished, this, [=, this] {
		downloadFinished(id);
	});
}

void Client::downloadFinished(RequestId id) {
	const auto i = _downloads.find(id);
	if (i == end(_downloads)) {
		return;
	}
	const auto download = i->second.get();
	const auto reply = download->reply.data();
	if (!reply) {
		return;
	}
	download->reply = nullptr;
	reply->deleteLater();

	const auto status = StatusOf(reply);
	if (download->tooLarge) {
		failDownload(id, Error::Local(413, u"too_large"_s, u"the file is larger than allowed"_s));
		return;
	}
	if (status >= 300 && status < 400) {
		// The raw header: Qt's parsed Location is empty for a relative target.
		const auto location = QUrl::fromEncoded(reply->rawHeader("Location"_ba).trimmed());
		if (location.isEmpty() || ++download->redirects > kMaxRedirects) {
			failDownload(id, Error::Local(status, u"redirect"_s, u"bad redirect"_s));
			return;
		}
		download->current = download->current.resolved(location);
		performDownload(id);
		return;
	}
	if (status == 0 || reply->error() != QNetworkReply::NoError) {
		if (status == 0) {
			failDownload(id, Error::Offline(reply->errorString()));
			return;
		}
		const auto error = Error::FromResponse(
			status,
			reply->isOpen() ? reply->readAll() : QByteArray(),
			reply->rawHeader("Retry-After"_ba),
			ReasonOf(reply),
			QString());
		if (status == 401
			&& download->spec.authenticated
			&& _tokens
			&& !download->retriedAuth
			&& sameOriginAsBase(download->current)) {
			download->retriedAuth = true;
			_tokens->refreshAfterUnauthorized(download->token, base::guard(this, [=, this](bool ok) {
				if (_downloads.find(id) == end(_downloads)) {
					return;
				} else if (!ok) {
					failDownload(id, error);
					return;
				}
				withToken(id, true, [=, this](QString token) {
					const auto i = _downloads.find(id);
					if (i == end(_downloads)) {
						return;
					}
					i->second->token = token;
					performDownload(id);
				});
			}));
			return;
		}
		failDownload(id, error);
		return;
	}
	if (status < 200 || status >= 300) {
		failDownload(id, Error::Local(status, u"http"_s, ReasonOf(reply)));
		return;
	}
	auto result = DownloadResult();
	result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
	result.size = download->received;
	if (!download->spec.filePath.isEmpty()) {
		if (!download->file) {
			// An empty body: still produce the (empty) file.
			download->file = std::make_unique<QFile>(download->spec.filePath + u".part"_s);
			base::platform::EnsureDirectory(QFileInfo(download->spec.filePath).absolutePath());
			if (!download->file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				download->file = nullptr;
				failDownload(id, Error::Local(0, u"file"_s, u"could not write the file"_s));
				return;
			}
		}
		download->file->close();
		QFile::remove(download->spec.filePath);
		if (!download->file->rename(download->spec.filePath)) {
			failDownload(id, Error::Local(0, u"file"_s, u"could not write the file"_s));
			return;
		}
		download->file = nullptr;
		result.filePath = download->spec.filePath;
	} else {
		result.bytes = std::move(download->bytes);
	}
	const auto owned = std::move(i->second);
	_downloads.erase(i);
	if (owned->spec.done && owned->spec.guard.alive()) {
		owned->spec.done(result);
	}
}

void Client::failDownload(RequestId id, const Error &error) {
	const auto i = _downloads.find(id);
	if (i == end(_downloads)) {
		return;
	}
	const auto download = std::move(i->second);
	_downloads.erase(i);
	if (download->file) {
		download->file->remove();
	}
	if (download->spec.fail && download->spec.guard.alive()) {
		download->spec.fail(error);
	}
}

// ---- uploads

RequestId Client::start(UploadSpec &&spec) {
	const auto id = ++_lastId;
	auto upload = std::make_unique<Upload>();
	upload->spec = std::move(spec);
	_uploads.emplace(id, std::move(upload));
	QMetaObject::invokeMethod(this, [=, this] {
		performUpload(id);
	}, Qt::QueuedConnection);
	return id;
}

void Client::performUpload(RequestId id) {
	const auto i = _uploads.find(id);
	if (i == end(_uploads)) {
		return;
	}
	const auto upload = i->second.get();
	const auto fail = [=, this](const Error &error) {
		const auto i = _uploads.find(id);
		if (i == end(_uploads)) {
			return;
		}
		const auto owned = std::move(i->second);
		_uploads.erase(i);
		if (owned->spec.fail && owned->spec.guard.alive()) {
			owned->spec.fail(error);
		}
	};
	if (!upload->spec.guard.alive()) {
		_uploads.erase(i);
		return;
	}
	auto request = QNetworkRequest(upload->spec.url);
	if (!upload->spec.contentType.isEmpty()) {
		request.setHeader(QNetworkRequest::ContentTypeHeader, upload->spec.contentType);
	}
	for (const auto &[name, value] : upload->spec.headers) {
		request.setRawHeader(name, value);
	}
	request.setTransferTimeout(upload->spec.timeoutMs);
	const auto verb = MethodName(upload->spec.method).toLatin1();
	QNetworkReply *reply = nullptr;
	if (upload->spec.fromFile) {
		const auto slice = new FileSlice(
			upload->spec.filePath,
			upload->spec.offset,
			upload->spec.length,
			nullptr);
		if (!slice->prepare()) {
			delete slice;
			fail(Error::Local(0, u"file"_s, u"could not read the file"_s));
			return;
		}
		request.setHeader(QNetworkRequest::ContentLengthHeader, slice->size());
		reply = _manager.sendCustomRequest(request, verb, slice);
		slice->setParent(reply);
	} else {
		reply = _manager.sendCustomRequest(request, verb, upload->spec.data);
	}
	upload->reply = reply;
	connect(reply, &QNetworkReply::uploadProgress, this, [=, this](qint64 sent, qint64 total) {
		const auto i = _uploads.find(id);
		if (i == end(_uploads) || total <= 0) {
			return;
		}
		const auto &spec = i->second->spec;
		if (spec.progress && spec.guard.alive()) {
			spec.progress(sent, total);
		}
	});
	connect(reply, &QNetworkReply::finished, this, [=, this] {
		const auto i = _uploads.find(id);
		if (i == end(_uploads)) {
			return;
		}
		reply->deleteLater();
		i->second->reply = nullptr;
		const auto status = StatusOf(reply);
		if (status == 0) {
			fail(Error::Offline(reply->errorString()));
		} else if (status < 200 || status >= 300) {
			// Storage answers XML; 403 means the presigned URL expired.
			fail(Error::Local(status, u"upload.part"_s, ReasonOf(reply), status >= 500 || status == 403));
		} else {
			const auto owned = std::move(i->second);
			_uploads.erase(i);
			if (owned->spec.done && owned->spec.guard.alive()) {
				owned->spec.done();
			}
		}
	});
}

} // namespace Api
