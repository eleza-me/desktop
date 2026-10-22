#include "rt/rt_session.h"

#include "base/bytes.h"
#include "base/unixtime.h"
#include "rt/rt_proto.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtNetwork/QNetworkRequest>

namespace Rt {
namespace {

Q_LOGGING_CATEGORY(Log, "eleza.rt", QtWarningMsg)

constexpr auto kMaxIncomingMessage = quint64(Noise::kMaxMessage) + 1024;

[[nodiscard]] const char *SignalBody(const QString &type) {
	if (type == u"typing") {
		return "TypingSignal";
	} else if (type == u"presence") {
		return "PresenceSignal";
	} else if (type == u"profile") {
		return "ProfileSignal";
	} else if (type == u"live_location") {
		return "LiveLocationSignal";
	} else if (type == u"topics") {
		return "TopicsSignal";
	} else if (type == u"feed_new") {
		return "FeedNewSignal";
	} else if (type == u"state") {
		return "StateSignal";
	} else if (type == u"group_typing") {
		return "GroupTypingSignal";
	} else if (type == u"group_admin") {
		return "GroupAdminSignal";
	}
	return nullptr;
}

// Signals whose body is JSON in the generic carrier (CallSignal).
[[nodiscard]] bool JsonSignal(const QString &type) {
	return type.startsWith(u"call.")
		|| type == u"group"
		|| type == u"config"
		|| type == u"media"
		|| type == u"settings"
		|| type == u"story";
}

[[nodiscard]] ErrorInfo ParseError(const QJsonObject &error) {
	auto result = ErrorInfo();
	result.code = error.value("code"_L1).toString();
	result.title = error.value("title"_L1).toString();
	result.retryable = error.value("retryable"_L1).toBool();
	result.retryAfter = base::SafeInt(error.value("retry_after_s"_L1).toDouble());
	result.httpStatus = base::SafeInt(error.value("http_status"_L1).toDouble());
	return result;
}

[[nodiscard]] QByteArray BytesOf(const QJsonObject &object, QLatin1StringView key) {
	return base::bytes::fromBase64(object.value(key).toString());
}

} // namespace

Session::Session(const QUrl &url, ProofProvider proof, QObject *parent)
: QObject(parent)
, _url(url)
, _proof(std::move(proof))
, _lastReceivedAt(base::unixtime::nowMs()) {
	qRegisterMetaType<Rt::Hello>();
	qRegisterMetaType<Rt::ErrorInfo>();
}

Session::~Session() {
	_closed = true;
	_pending.clear();
	_queuedCalls.clear();
	if (_socket) {
		_socket->disconnect(this);
		_socket->abort();
	}
}

void Session::connectToServer(int keyId, const QByteArray &serverKey, const AuthParams &auth) {
	if (_closed || _socket) {
		return;
	}
	_socket = std::make_unique<QWebSocket>(QString(), QWebSocketProtocol::VersionLatest);
	const auto socket = _socket.get();
	// One message is one Noise record: nothing larger is ever buffered (the
	// defaults would let the other side make us hold 64 MiB per message).
	socket->setMaxAllowedIncomingFrameSize(kMaxIncomingMessage);
	socket->setMaxAllowedIncomingMessageSize(kMaxIncomingMessage);
	connect(socket, &QWebSocket::connected, this, [=, this] {
		start(keyId, serverKey, auth);
	});
	connect(socket, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray &message) {
		receive(message);
	});
	connect(socket, &QWebSocket::disconnected, this, [=, this] {
		socketEnded(int(socket->closeCode()), socket->closeReason());
	});
	const auto onError = [=, this](QAbstractSocket::SocketError) {
#ifdef ELEZA_DEBUG_TOOLS
		qCInfo(Log, "socket error: %s", qPrintable(socket->errorString()));
#else // ELEZA_DEBUG_TOOLS
		qCInfo(Log, "socket error: %d", int(socket->error()));
#endif // ELEZA_DEBUG_TOOLS
		// A refused / unreachable socket or a failed upgrade (an HTTP error
		// instead of 101) never reports `disconnected`; once the session is
		// open, the socket's own close follows an error.
		if (!_open || socket->state() == QAbstractSocket::UnconnectedState) {
			const auto reason = socket->errorString();
			socket->abort();
			socketEnded(0, reason);
		}
	};
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	connect(socket, &QWebSocket::errorOccurred, this, onError);
#else // Qt >= 6.5
	connect(
		socket,
		QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
		this,
		onError);
#endif // Qt >= 6.5
	_lastReceivedAt = base::unixtime::nowMs();
	socket->open(QNetworkRequest(_url));
}

void Session::start(int keyId, const QByteArray &serverKey, const AuthParams &auth) {
	if (_closed) {
		return;
	}
	const auto prologue = "eleza-rt-v2/"_ba + QByteArray::number(keyId);
	_handshake = std::make_unique<Noise::Initiator>(serverKey, prologue);
	const auto authBody = Proto::Encode("Auth"_L1, QJsonObject{
		{ u"token"_s, auth.token },
		{ u"background"_s, auth.background },
		{ u"pts"_s, double(auth.pts) },
		{ u"device"_s, auth.device },
		{ u"app"_s, auth.app },
		{ u"lang"_s, auth.lang },
	});
	const auto request = Proto::Encode("Envelope"_L1, QJsonObject{
		{ u"req"_s, QJsonObject{
			{ u"id"_s, double(_nextId++) },
			{ u"method"_s, u"auth"_s },
			{ u"body"_s, base::bytes::toBase64(authBody.value_or(QByteArray())) },
		} },
	});
	const auto message1 = request
		? _handshake->writeMessage1(*request)
		: std::optional<QByteArray>();
	if (!message1) {
		fail(u"handshake: cannot write message 1"_s);
		return;
	}
	_lastReceivedAt = base::unixtime::nowMs();
	_socket->sendBinaryMessage(QByteArray(1, char(keyId & 0xFF)) + *message1);
}

void Session::receive(const QByteArray &record) {
	if (_closed) {
		return;
	}
	_lastReceivedAt = base::unixtime::nowMs();
	if (record.size() > Noise::kMaxMessage) {
		fail(u"oversized record"_s);
		return;
	}
	if (!_noise) {
		// Message 2, or a clear-text Close (unknown key id): 0xFF + Close.
		// The clear-text form is not authenticated by Noise (only by TLS
		// around the socket): all it may do is make the client re-read
		// /v2/keys, which Rt::Client rate-limits. Any other code is an error.
		if (!record.isEmpty() && uchar(record[0]) == 0xFF && record.size() < 48) {
			const auto close = Proto::Decode("Close"_L1, record.mid(1));
			const auto code = close ? base::SafeInt(close->value("code"_L1).toDouble()) : 0;
			if (code != kCloseKeyRotation) {
				fail(u"clear-text close %1"_s.arg(code));
				return;
			}
			Q_EMIT serverClose(code, close->value("reason"_L1).toString());
			if (!_closed) {
				this->close(1000, QString());
			}
			return;
		}
		const auto payload = _handshake
			? _handshake->readMessage2(record)
			: std::optional<QByteArray>();
		if (!payload) {
			// A wrong static key (the server rotated it) ends here.
			fail(u"handshake: message 2 does not decrypt"_s);
			return;
		}
		_noise = &_handshake->session();
		_decoder.setCipher(&_noise->recv);
		_open = true;
		const auto envelope = Proto::Decode("Envelope"_L1, *payload);
		if (!envelope) {
			fail(u"handshake: bad envelope"_s);
			return;
		}
		dispatch(*envelope, 0);
		return;
	}
	const auto result = _decoder.record(record);
	switch (result.status) {
	case Frames::RecordDecoder::Status::Failed:
		fail(u"malformed record"_s);
		return;
	case Frames::RecordDecoder::Status::Partial:
		return;
	case Frames::RecordDecoder::Status::Complete:
		break;
	}
	const auto envelope = Proto::Decode("Envelope"_L1, result.envelope);
	if (!envelope) {
		fail(u"bad envelope"_s);
		return;
	}
	dispatch(*envelope, 0);
}

void Session::dispatch(const QJsonObject &envelope, int depth) {
	if (_closed) {
		return;
	}
	if (const auto batch = envelope.value("batch"_L1); batch.isObject()) {
		if (depth > 4) {
			fail(u"nested batches"_s);
			return;
		}
		for (const auto &item : batch.toObject().value("items"_L1).toArray()) {
			dispatch(item.toObject(), depth + 1);
			if (_closed) {
				return;
			}
		}
	} else if (const auto hello = envelope.value("hello"_L1); hello.isObject()) {
		const auto h = hello.toObject();
		_hello.version = base::SafeInt(h.value("v"_L1).toDouble());
		_hello.connection = h.value("conn"_L1).toString();
		_hello.pingInterval = base::SafeInt(h.value("ping_interval_s"_L1).toDouble());
		_hello.reconnectSpread = base::SafeInt(h.value("reconnect_spread_s"_L1).toDouble());
		_hello.pts = base::SafeInt64(h.value("pts"_L1).toDouble());
		_hello.resumed = h.value("resumed"_L1).toBool();
		_hello.serverTimeMs = base::SafeInt64(h.value("server_time_ms"_L1).toDouble());
		Q_EMIT helloReceived(_hello);
	} else if (const auto response = envelope.value("res"_L1); response.isObject()) {
		onResponse(response.toObject());
	} else if (const auto update = envelope.value("update"_L1); update.isObject()) {
		Q_EMIT updateReceived(update.toObject());
	} else if (const auto signal = envelope.value("signal"_L1); signal.isObject()) {
		const auto s = signal.toObject();
		const auto type = s.value("t"_L1).toString();
		const auto raw = BytesOf(s, "body"_L1);
		auto body = QJsonObject();
		if (const auto name = SignalBody(type); name && !raw.isEmpty()) {
			body = Proto::Decode(QLatin1StringView(name), raw).value_or(QJsonObject());
		} else if (JsonSignal(type) && !raw.isEmpty()) {
			// The voice service's JSON travels as is, in the generic JSON
			// carrier; so do a change of a group, config / settings / media
			// / story hints.
			if (const auto call = Proto::Decode("CallSignal"_L1, raw)) {
				body = QJsonDocument::fromJson(BytesOf(*call, "json"_L1)).object();
			}
		}
		Q_EMIT signalReceived(type, body);
	} else if (const auto ping = envelope.value("ping"_L1); ping.isObject()) {
		enqueue(QJsonObject{
			{ u"pong"_s, QJsonObject{ { u"nonce"_s, ping.toObject().value("nonce"_L1) } } },
		});
	} else if (const auto close = envelope.value("close"_L1); close.isObject()) {
		const auto c = close.toObject();
		Q_EMIT serverClose(base::SafeInt(c.value("code"_L1).toDouble()), c.value("reason"_L1).toString());
	}
	// pong, ack: nothing to do (any record refreshes lastReceivedAt).
}

void Session::onResponse(const QJsonObject &response) {
	const auto wideId = base::SafeInt64(response.value("id"_L1).toDouble());
	const auto id = (wideId > 0 && wideId <= qint64(0xFFFFFFFFLL)) ? quint32(wideId) : quint32(0);
	const auto ok = response.value("ok"_L1).toBool();
	const auto error = ParseError(response.value("error"_L1).toObject());
	const auto body = BytesOf(response, "body"_L1);

	auto pending = Pending();
	const auto i = _pending.find(id);
	const auto found = (i != end(_pending));
	if (found) {
		pending = std::move(i->second);
		_pending.erase(i);
	}
	if (id == 1 && !ok && error.code == u"auth.proof_required") {
		// An expired token inside the resume grace: sign the server's nonce
		// with the device key.
		const auto challenge = Proto::Decode("Challenge"_L1, body);
		const auto signature = (_proof && challenge)
			? _proof(BytesOf(*challenge, "nonce"_L1))
			: QByteArray();
		if (signature.isEmpty()) {
			Q_EMIT authFailed(error);
			return;
		}
		const auto proof = Proto::Encode("Proof"_L1, QJsonObject{
			{ u"sig"_s, base::bytes::toBase64(signature) },
		});
		enqueue(QJsonObject{
			{ u"req"_s, QJsonObject{
				{ u"id"_s, double(_nextId++) },
				{ u"method"_s, u"proof"_s },
				{ u"body"_s, base::bytes::toBase64(proof.value_or(QByteArray())) },
			} },
		});
		return;
	}
	if (id <= 2 && !found) {
		// The answers to `auth` (1) and `proof` (2).
		if (!ok) {
			Q_EMIT authFailed(error);
		}
		return;
	}
	if (found && pending.call) {
		--_callsInFlight;
	}
	if (found && pending.done) {
		auto result = Response();
		result.status = ok ? Response::Status::Ok : Response::Status::Error;
		result.body = body;
		result.error = error;
		pending.done(result);
	}
	if (found && pending.call) {
		callFinished();
	}
}

void Session::enqueue(const QJsonObject &envelope) {
	const auto bytes = Proto::Encode("Envelope"_L1, envelope);
	if (bytes) {
		transmit(*bytes);
	}
}

void Session::transmit(const QByteArray &envelope) {
	if (_closed
		|| !_noise
		|| !_socket
		|| _socket->state() != QAbstractSocket::ConnectedState) {
		return;
	}
	// The Noise nonce is a strict sequence: encrypt and hand over in one go.
	const auto records = Frames::EncodeRecords(_noise->send, envelope);
	if (!records) {
		qCWarning(Log, "an envelope of %lld bytes was not sent", qlonglong(envelope.size()));
		if (envelope.size() <= Frames::kMaxFrame) {
			fail(u"cipher failure"_s);
		}
		return;
	}
	for (const auto &record : *records) {
		_socket->sendBinaryMessage(record);
	}
}

quint32 Session::addPending(Fn<void(const Response &)> done, bool call) {
	const auto id = _nextId++;
	auto pending = Pending();
	pending.done = std::move(done);
	pending.call = call;
	if (call) {
		++_callsInFlight;
	}
	pending.timer = std::make_unique<base::Timer>([=, this] {
		drop(id);
	});
	pending.timer->callOnce(kRequestTimeoutMs);
	_pending.emplace(id, std::move(pending));
	return id;
}

void Session::call(
		const QString &method,
		const QByteArray &body,
		const QString &idempotencyKey,
		Fn<void(const Response &)> done) {
	if (!isOpen()) {
		if (done) {
			auto result = Response();
			result.status = Response::Status::NotOpen;
			done(result);
		}
		return;
	}
	auto queued = QueuedCall{ method, body, idempotencyKey, std::move(done) };
	if (_callsInFlight >= kMaxCallsInFlight) {
		_queuedCalls.push_back(std::move(queued));
		return;
	}
	sendCall(std::move(queued));
}

void Session::callFinished() {
	while (isOpen()
		&& !_queuedCalls.empty()
		&& _callsInFlight < kMaxCallsInFlight) {
		auto next = std::move(_queuedCalls.front());
		_queuedCalls.pop_front();
		sendCall(std::move(next));
	}
}

void Session::sendCall(QueuedCall &&call) {
	const auto id = addPending(std::move(call.done), true);
	auto request = QJsonObject{
		{ u"id"_s, double(id) },
		{ u"method"_s, call.method },
	};
	if (!call.body.isEmpty()) {
		request.insert(u"body"_s, base::bytes::toBase64(call.body));
	}
	if (!call.idempotencyKey.isEmpty()) {
		request.insert(u"idempotency_key"_s, call.idempotencyKey);
	}
	const auto envelope = Proto::Encode("Envelope"_L1, QJsonObject{ { u"req"_s, request } });
	if (!envelope || envelope->size() > Frames::kMaxFrame) {
		// Too large for one envelope: the caller sends it over HTTP instead.
		drop(id);
		return;
	}
	transmit(*envelope);
}

void Session::signal(
		const QString &type,
		const QByteArray &body,
		Fn<void(const Response &)> done) {
	if (!isOpen()) {
		if (done) {
			auto result = Response();
			result.status = Response::Status::NotOpen;
			done(result);
		}
		return;
	}
	auto signal = QJsonObject{ { u"t"_s, type } };
	if (!body.isEmpty()) {
		signal.insert(u"body"_s, base::bytes::toBase64(body));
	}
	if (done) {
		signal.insert(u"id"_s, double(addPending(std::move(done))));
	}
	enqueue(QJsonObject{ { u"signal"_s, signal } });
}

void Session::ack(qint64 pts) {
	if (isOpen()) {
		enqueue(QJsonObject{ { u"ack"_s, QJsonObject{ { u"pts"_s, double(pts) } } } });
	}
}

void Session::ping() {
	if (isOpen()) {
		enqueue(QJsonObject{
			{ u"ping"_s, QJsonObject{ { u"nonce"_s, double(quint32(base::unixtime::now())) } } },
		});
	}
}

void Session::reauth(const QString &token, Fn<void(const Response &)> done) {
	const auto body = Proto::Encode("Auth"_L1, QJsonObject{ { u"token"_s, token } });
	call(u"auth"_s, body.value_or(QByteArray()), QString(), std::move(done));
}

void Session::drop(quint32 id) {
	const auto i = _pending.find(id);
	if (i == end(_pending)) {
		return;
	}
	auto pending = std::move(i->second);
	_pending.erase(i);
	if (pending.timer) {
		// drop() runs inside this timer's own callback when a request times out.
		pending.timer->cancel();
		pending.timer.release()->deleteLater();
	}
	if (pending.call) {
		--_callsInFlight;
	}
	if (pending.done) {
		pending.done(Response());
	}
	if (pending.call) {
		callFinished();
	}
}

void Session::dropPending() {
	const auto pending = std::exchange(_pending, {});
	const auto queued = std::exchange(_queuedCalls, {});
	_callsInFlight = 0;
	for (const auto &[id, entry] : pending) {
		if (entry.done) {
			entry.done(Response());
		}
	}
	for (const auto &call : queued) {
		if (call.done) {
			call.done(Response());
		}
	}
}

void Session::close(int code, const QString &reason) {
	if (_closed) {
		return;
	}
	_closed = true;
	_open = false;
	if (_socket) {
		_socket->disconnect(this);
		if (_socket->state() == QAbstractSocket::ConnectedState) {
			const auto valid = (code >= 1000 && code < 5000) ? code : 1000;
			_socket->close(QWebSocketProtocol::CloseCode(valid), reason.left(120));
		} else {
			_socket->abort();
		}
	}
	dropPending();
	Q_EMIT closed(code, reason);
}

void Session::socketEnded(int code, const QString &reason) {
	if (_closed) {
		return;
	}
	_closed = true;
	_open = false;
	if (_socket) {
		_socket->disconnect(this);
	}
	dropPending();
	Q_EMIT closed(code, reason);
}

void Session::fail(const QString &error) {
	if (_closed) {
		return;
	}
	qCInfo(Log, "session failed: %s", qPrintable(error));
	_closed = true;
	_open = false;
	if (_socket) {
		_socket->disconnect(this);
		if (_socket->state() == QAbstractSocket::ConnectedState) {
			_socket->close(QWebSocketProtocol::CloseCodeProtocolError, u"protocol"_s);
		} else {
			_socket->abort();
		}
	}
	dropPending();
	Q_EMIT closed(0, error);
}

} // namespace Rt
