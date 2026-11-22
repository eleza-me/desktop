#include "api/api_client.h"

#include "base/platform_file.h"
#include "base/random.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Api {
namespace {

constexpr auto kMaxRedirects = 5;
constexpr auto kMaxOverloadRetries = 2;
constexpr auto kMaxOverloadWait = 5.;
// A JSON answer larger than this is not an answer of ours (the realtime
// transport caps an envelope at 256 KiB; HTTP pages are a few hundred KiB).
constexpr auto kMaxJsonResponse = qint64(32) << 20;
// What a download may take when the caller set no limit of its own.
constexpr auto kMaxDownloadDefault = qint64(4) << 30;
// What is kept of the body of an error / redirect answer to a download or an
// upload (a problem document), and how much of one is tolerated at all: the
// network reply buffers whatever nobody reads, without a limit of its own.
constexpr auto kKeptErrorBody = qint64(64) << 10;
constexpr auto kMaxErrorBody = qint64(1) << 20;

[[nodiscard]] bool LoopbackHost(const QString &host) {
	if (host.compare(u"localhost", Qt::CaseInsensitive) == 0) {
		return true;
	}
	// A literal address only: "127.0.0.1.evil.example" is a name.
	auto address = QHostAddress();
	return address.setAddress(host) && address.isLoopback();
}

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
	bool oversized = false;
	int overload = 0;
};

struct Client::Download {
	DownloadSpec spec;
	QString token;
	QUrl current;
	QPointer<QNetworkReply> reply;
	std::unique_ptr<QFile> file;
	QByteArray bytes;
	QByteArray errorBody;
	qint64 errorReceived = 0;
	qint64 received = 0;
	int redirects = 0;
	bool retriedAuth = false;
	bool tooLarge = false;
	bool writeFailed = false;
	// ---- to a file
	QString partPath;
	bool stablePart = false; // <path>.part of a resumable download
	qint64 offset = 0;       // what the part held when this request went out
	QByteArray tag;          // its entity tag, sent as If-Range
	qint64 total = -1;       // the size of the whole file, once known
	bool fresh = false;      // no range any more (the server did not continue it)
	bool restart = false;    // this answer does not continue the part
};

namespace {

// Next to the target, and never shared by two downloads of the same file.
[[nodiscard]] QString PartPath(const QString &filePath, RequestId id) {
	return filePath + u'.' + QString::number(id) + u".part"_s;
}

[[nodiscard]] QString StablePartPath(const QString &filePath) {
	return filePath + u".part"_s;
}

[[nodiscard]] QString TagPath(const QString &partPath) {
	return partPath + u".tag"_s;
}

// A strong entity tag (`"…"`); a weak one (W/"…") never makes a range
// conditional (RFC 9110 §13.1.5), so it is no use for resuming.
[[nodiscard]] QByteArray StrongTag(const QByteArray &value) {
	const auto tag = value.trimmed();
	if (tag.size() < 2 || tag.size() > 256 || !tag.startsWith('"') || !tag.endsWith('"')) {
		return QByteArray();
	}
	for (const auto c : tag) {
		if (uchar(c) < 0x21 || uchar(c) > 0x7e) {
			return QByteArray();
		}
	}
	return tag;
}

[[nodiscard]] QByteArray ReadTag(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? StrongTag(file.read(512)) : QByteArray();
}

void WriteTag(const QString &path, const QByteArray &tag) {
	if (tag.isEmpty()) {
		QFile::remove(path);
		return;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| file.write(tag) != tag.size()
		|| !file.flush()) {
		file.close();
		QFile::remove(path);
	}
}

struct ContentRange {
	qint64 first = -1; // -1: `bytes */<total>`
	qint64 last = -1;
	qint64 total = -1; // -1: `/*`
};

// `bytes <first>-<last>/<total>`, `bytes */<total>`, `bytes <first>-<last>/*`.
[[nodiscard]] std::optional<ContentRange> ParseContentRange(const QByteArray &value) {
	const auto trimmed = value.trimmed();
	if (!trimmed.startsWith("bytes ")) {
		return std::nullopt;
	}
	const auto spec = trimmed.mid(6).trimmed();
	const auto slash = spec.indexOf('/');
	if (slash <= 0) {
		return std::nullopt;
	}
	const auto number = [](const QByteArray &text) -> qint64 {
		if (text.isEmpty() || text.size() > 18) {
			return -1;
		}
		for (const auto c : text) {
			if (c < '0' || c > '9') {
				return -1;
			}
		}
		return text.toLongLong();
	};
	auto result = ContentRange();
	const auto total = spec.mid(slash + 1);
	if (total != "*") {
		result.total = number(total);
		if (result.total < 0) {
			return std::nullopt;
		}
	}
	const auto range = spec.left(slash);
	if (range == "*") {
		return (result.total >= 0) ? std::make_optional(result) : std::nullopt;
	}
	const auto dash = range.indexOf('-');
	if (dash <= 0) {
		return std::nullopt;
	}
	result.first = number(range.left(dash));
	result.last = number(range.mid(dash + 1));
	if (result.first < 0
		|| result.last < result.first
		|| (result.total >= 0 && result.last >= result.total)) {
		return std::nullopt;
	}
	return result;
}

} // namespace

struct Client::Upload {
	UploadSpec spec;
	QPointer<QNetworkReply> reply;
	qint64 answered = 0;
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

DownloadBuilder &DownloadBuilder::resumable() {
	_spec.resumable = true;
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

DownloadBuilder &DownloadBuilder::written(
		Fn<void(const QString &partPath, qint64 written, qint64 total)> callback) {
	_spec.written = std::move(callback);
	return *this;
}

DownloadBuilder &DownloadBuilder::finishing(Fn<void()> callback) {
	_spec.finishing = std::move(callback);
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

bool Client::IsAcceptableBase(const QString &base, bool allowInsecure) {
	const auto url = QUrl(base, QUrl::StrictMode);
	if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) {
		return false;
	}
	const auto scheme = url.scheme().toLower();
	if (scheme == u"https") {
		return true;
	}
	// Clear text carries the tokens: a local test server only.
	return (scheme == u"http") && (allowInsecure || LoopbackHost(url.host()));
}

bool Client::acceptableRemote(const QUrl &url) const {
	// Addresses come from the server (media, presigned uploads, redirects):
	// nothing but the web may be reached through them - never "file:",
	// "qrc:", "data:" or "ftp:", which the network manager would serve too -
	// and never in clear text unless the API itself is a clear-text test server.
	if (!url.isValid() || url.host().isEmpty()) {
		return false;
	}
	const auto scheme = url.scheme().toLower();
	return (scheme == u"https")
		|| (scheme == u"http" && QUrl(_base).scheme().toLower() == u"http");
}

void Client::abortAuthenticated() {
	auto ids = QVector<RequestId>();
	for (const auto &[id, call] : _calls) {
		if (call->spec.authenticated) {
			ids.push_back(id);
		}
	}
	for (const auto &[id, download] : _downloads) {
		if (download->spec.authenticated) {
			ids.push_back(id);
		}
	}
	for (const auto &[id, upload] : _uploads) {
		ids.push_back(id);
	}
	for (const auto id : ids) {
		cancel(id);
	}
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
		// A resumable part stays for the next download of the file.
		releasePart(download.get(), download->spec.resumable);
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
	connect(reply, &QNetworkReply::downloadProgress, this, [=, this](qint64 received, qint64) {
		const auto i = _calls.find(id);
		if (received > kMaxJsonResponse
			&& i != end(_calls)
			&& i->second->reply == reply
			&& !i->second->oversized) {
			// Never buffer an endless body: finished() reports it.
			i->second->oversized = true;
			reply->abort();
		}
	});
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
	const auto &spec = call->spec;
	if (std::exchange(call->oversized, false)) {
		auto error = Error::Local(status, u"decoding"_s, u"the response is too large"_s);
		error.requestId = spec.requestId;
		failCall(id, error);
		return;
	}
	const auto body = reply->isOpen() ? reply->readAll() : QByteArray();
	if (status == 0) {
		// No HTTP answer at all: DNS, refused, reset, timeout, TLS.
		setReachable(false);
		failCall(id, Error::Offline(reply->errorString()));
		return;
	}
	setReachable(true);
	if (status >= 200 && status < 300 && reply->error() != QNetworkReply::NoError) {
		// The headers arrived, the body did not (reset, timeout mid-transfer):
		// not an answer, and the request may be tried again.
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

void Client::setReachable(bool reachable) {
	if (_reachable != reachable) {
		_reachable = reachable;
		Q_EMIT reachableChanged(reachable);
	}
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
	if (!download->spec.filePath.isEmpty()) {
		const auto stable = StablePartPath(download->spec.filePath);
		if (download->spec.resumable && !_partsInUse.contains(stable)) {
			download->partPath = stable;
			download->stablePart = true;
			_partsInUse.insert(stable);
		} else {
			// Not resumable, or the file is being downloaded already.
			download->partPath = PartPath(download->spec.filePath, id);
		}
	}
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

void Client::releasePart(Download *download, bool keep) {
	download->file = nullptr;
	if (download->partPath.isEmpty()) {
		return;
	}
	const auto part = std::exchange(download->partPath, QString());
	if (download->stablePart) {
		_partsInUse.remove(part);
		// Only what a later request can continue: bytes and their tag.
		if (keep && QFileInfo(part).size() > 0 && QFileInfo::exists(TagPath(part))) {
			return;
		}
		QFile::remove(TagPath(part));
	}
	QFile::remove(part);
}

void Client::performDownload(RequestId id) {
	const auto i = _downloads.find(id);
	if (i == end(_downloads)) {
		return;
	}
	const auto download = i->second.get();
	if (!download->spec.guard.alive()) {
		releasePart(download, download->spec.resumable);
		_downloads.erase(i);
		return;
	}
	download->bytes.clear();
	download->errorBody.clear();
	download->errorReceived = 0;
	download->received = 0;
	download->tooLarge = false;
	download->writeFailed = false;
	download->restart = false;
	download->file = nullptr;
	download->offset = 0;
	download->tag.clear();
	download->total = -1;
	if (!download->partPath.isEmpty()) {
		if (!download->stablePart) {
			QFile::remove(download->partPath);
		} else {
			// What an earlier download left: continued only with its tag.
			const auto size = QFileInfo(download->partPath).size();
			const auto tag = ReadTag(TagPath(download->partPath));
			if (!download->fresh && size > 0 && !tag.isEmpty()) {
				download->offset = size;
				download->tag = tag;
			} else if (size > 0 || QFileInfo::exists(TagPath(download->partPath))) {
				QFile::remove(download->partPath);
				QFile::remove(TagPath(download->partPath));
			}
		}
	}
	if (!acceptableRemote(download->current)) {
		// Asynchronous like every other outcome.
		QMetaObject::invokeMethod(this, [=, this] {
			failDownload(id, Error::Local(0, u"url"_s, u"the address is not allowed"_s));
		}, Qt::QueuedConnection);
		return;
	}
	auto request = QNetworkRequest(download->current);
	if (sameOriginAsBase(download->current)) {
		applyCommonHeaders(request);
		// The token goes to the API only, never to where it redirects.
		if (download->spec.authenticated && !download->token.isEmpty()) {
			request.setRawHeader("Authorization"_ba, "Bearer "_ba + download->token.toLatin1());
		}
	}
	if (download->offset > 0) {
		// On every hop: the API's 302 ignores it, the storage answers it.
		request.setRawHeader("Range"_ba, "bytes="_ba + QByteArray::number(download->offset) + '-');
		request.setRawHeader("If-Range"_ba, download->tag);
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
			// The body of an error or a redirect: the beginning is kept for
			// the problem document, the rest is dropped as it arrives, and a
			// body that does not end is cut off.
			const auto chunk = reply->readAll();
			download->errorReceived += chunk.size();
			if (download->errorBody.size() < kKeptErrorBody) {
				download->errorBody.append(
					chunk.left(kKeptErrorBody - download->errorBody.size()));
			}
			if (download->errorReceived > kMaxErrorBody) {
				reply->abort();
			}
			return;
		} else if (download->restart) {
			reply->readAll();
			return;
		}
		const auto toFile = !download->spec.filePath.isEmpty();
		if (toFile && !download->file) {
			// The first bytes of the body: does it continue the part?
			auto append = false;
			if (status == 206) {
				const auto range = ParseContentRange(reply->rawHeader("Content-Range"_ba));
				const auto tag = reply->rawHeader("ETag"_ba).trimmed();
				const auto continues = range
					&& range->first == download->offset
					&& (range->total < 0 || range->last + 1 == range->total)
					&& (download->offset == 0 || tag.isEmpty() || tag == download->tag);
				if (!continues) {
					download->restart = true;
					reply->abort();
					return;
				}
				download->total = range->total;
				append = (download->offset > 0);
			} else {
				// The whole body: what the part held (if anything) is dropped.
				download->offset = 0;
				const auto length = reply->header(QNetworkRequest::ContentLengthHeader);
				download->total = length.isValid() ? length.toLongLong() : -1;
			}
			base::platform::EnsureDirectory(QFileInfo(download->spec.filePath).absolutePath());
			download->file = std::make_unique<QFile>(download->partPath);
			// Unbuffered: what was written is on disk for a reader of the part.
			const auto mode = QIODevice::WriteOnly
				| QIODevice::Unbuffered
				| (append ? QIODevice::Append : QIODevice::Truncate);
			if (!download->file->open(mode)) {
				download->file = nullptr;
				download->writeFailed = true;
				reply->abort();
				return;
			}
			if (download->stablePart && !append) {
				WriteTag(TagPath(download->partPath), StrongTag(reply->rawHeader("ETag"_ba)));
			}
		}
		const auto chunk = reply->readAll();
		download->received += chunk.size();
		const auto limit = (download->spec.maxBytes > 0)
			? download->spec.maxBytes
			: kMaxDownloadDefault;
		if (download->offset + download->received > limit) {
			download->tooLarge = true;
			reply->abort();
			return;
		}
		if (!toFile) {
			download->bytes.append(chunk);
		} else if (download->file->write(chunk) != chunk.size()) {
			// A full disk: never reported as a finished (truncated) file.
			download->writeFailed = true;
			reply->abort();
		} else if (download->spec.written && download->spec.guard.alive()) {
			download->spec.written(
				download->partPath,
				download->offset + download->received,
				download->total);
		}
	});
	connect(reply, &QNetworkReply::downloadProgress, this, [=, this](qint64 received, qint64 total) {
		const auto i = _downloads.find(id);
		if (i == end(_downloads) || i->second->reply != reply) {
			return;
		}
		const auto download = i->second.get();
		const auto status = StatusOf(reply);
		const auto &spec = download->spec;
		if (status >= 200 && status < 300
			&& !download->restart
			&& spec.progress
			&& spec.guard.alive()) {
			// Of the whole file, the part continued included.
			const auto offset = (status == 206) ? download->offset : 0;
			spec.progress(offset + received, (total > 0) ? (offset + total) : total);
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
	const auto startOver = [&] {
		// Once: then without a range, the part dropped.
		if (download->fresh) {
			failDownload(id, Error::Local(status, u"range"_s, u"the file was not continued"_s));
			return;
		}
		download->fresh = true;
		download->file = nullptr;
		QFile::remove(download->partPath);
		QFile::remove(TagPath(download->partPath));
		performDownload(id);
	};
	if (download->restart) {
		startOver();
		return;
	} else if (download->tooLarge) {
		failDownload(id, Error::Local(413, u"too_large"_s, u"the file is larger than allowed"_s));
		return;
	} else if (download->writeFailed) {
		failDownload(id, Error::Local(0, u"file"_s, u"could not write the file"_s));
		return;
	}
	if (status >= 300 && status < 400) {
		// The raw header: Qt's parsed Location is empty for a relative target.
		const auto location = QUrl::fromEncoded(reply->rawHeader("Location"_ba).trimmed());
		if (location.isEmpty() || ++download->redirects > kMaxRedirects) {
			failDownload(id, Error::Local(status, u"redirect"_s, u"bad redirect"_s));
			return;
		}
		const auto next = download->current.resolved(location);
		if (!acceptableRemote(next)
			|| (download->current.scheme() == u"https" && next.scheme() != u"https")) {
			// A redirect never leaves the web and never drops TLS.
			failDownload(id, Error::Local(status, u"redirect"_s, u"bad redirect"_s));
			return;
		}
		download->current = next;
		performDownload(id);
		return;
	}
	// 416 to a range from the end of the part: it was the whole file.
	auto complete = false;
	if (status == 416 && download->offset > 0) {
		const auto range = ParseContentRange(reply->rawHeader("Content-Range"_ba));
		if (range && range->first < 0 && range->total == download->offset) {
			complete = true;
		} else {
			startOver();
			return;
		}
	}
	if (!complete && (status == 0 || reply->error() != QNetworkReply::NoError)) {
		if (status == 0 || (status >= 200 && status < 300)) {
			// No answer, or a body that broke off (a dropped connection, a
			// timeout): the transfer failed, not the request.
			failDownload(id, Error::Offline(reply->errorString()));
			return;
		}
		if (reply->isOpen() && download->errorBody.size() < kKeptErrorBody) {
			download->errorBody.append(
				reply->read(kKeptErrorBody - download->errorBody.size()));
		}
		const auto error = Error::FromResponse(
			status,
			download->errorBody,
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
	if (!complete && (status < 200 || status >= 300)) {
		failDownload(id, Error::Local(status, u"http"_s, ReasonOf(reply)));
		return;
	}
	if (!complete && status != 206) {
		download->offset = 0; // a whole body (an empty one too) replaced the part
	} else if (!complete && !download->file && !download->spec.filePath.isEmpty()) {
		startOver(); // a 206 without a body continues nothing
		return;
	}
	auto result = DownloadResult();
	result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
	result.size = complete ? download->offset : (download->offset + download->received);
	if (!download->spec.filePath.isEmpty()) {
		if (!download->file && !complete) {
			// An empty body: still produce the (empty) file.
			base::platform::EnsureDirectory(QFileInfo(download->spec.filePath).absolutePath());
			download->file = std::make_unique<QFile>(download->partPath);
			if (!download->file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				download->file = nullptr;
				failDownload(id, Error::Local(0, u"file"_s, u"could not write the file"_s));
				return;
			}
		}
		if (download->file) {
			const auto flushed = download->file->flush();
			download->file->close();
			const auto failed = !flushed || download->file->error() != QFileDevice::NoError;
			download->file = nullptr;
			if (failed) {
				failDownload(id, Error::Local(0, u"file"_s, u"could not write the file"_s));
				return;
			}
		}
		if (download->spec.finishing && download->spec.guard.alive()) {
			download->spec.finishing();
		}
		QFile::remove(download->spec.filePath);
		if (!QFile::rename(download->partPath, download->spec.filePath)) {
			failDownload(id, Error::Local(0, u"file"_s, u"could not write the file"_s));
			return;
		}
		if (download->stablePart) {
			QFile::remove(TagPath(download->partPath));
			_partsInUse.remove(download->partPath);
		}
		download->partPath.clear();
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
	// Kept for the next attempt when only the transfer failed; not when
	// what arrived was wrong (too large, unwritable, the wrong file).
	const auto keep = download->spec.resumable
		&& error.transient()
		&& error.code != u"file"
		&& error.code != u"too_large"
		&& error.code != u"range";
	releasePart(download.get(), keep);
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
	if (!acceptableRemote(upload->spec.url)) {
		// A presigned address that is not a web address: the file stays here.
		fail(Error::Local(0, u"url"_s, u"the address is not allowed"_s));
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
	connect(reply, &QNetworkReply::readyRead, this, [=, this] {
		// Nothing in the answer of the storage is used: it is not kept, and
		// one that does not end is cut off.
		const auto i = _uploads.find(id);
		if (i == end(_uploads) || i->second->reply != reply) {
			return;
		}
		i->second->answered += reply->readAll().size();
		if (i->second->answered > kMaxErrorBody) {
			reply->abort();
		}
	});
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
