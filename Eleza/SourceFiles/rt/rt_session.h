// The realtime v2 protocol core: one WebSocket = one Noise session; protobuf
// envelopes; RPC with ids; typed update frames; ephemeral signals. Reconnect
// policy, token refresh and persistence belong to the owner (Rt::Client).
//
//   client -> [key id:1][noise message 1 carrying Envelope{req: auth}]
//   server -> [noise message 2 carrying Envelope{res}], then records:
//             Hello, the replay of missed updates, live frames
//   server -> 0xFF + Close{4006} in clear text when the key id is unknown
#pragma once

#include "base/timer.h"
#include "rt/rt_frames.h"
#include "rt/rt_noise.h"

#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtWebSockets/QWebSocket>

#include <deque>
#include <map>

namespace Rt {

inline constexpr auto kCloseResync = 4000;
inline constexpr auto kCloseRevoked = 4001;
inline constexpr auto kCloseReauth = 4002;
inline constexpr auto kCloseTooMany = 4003;
inline constexpr auto kCloseAuth = 4004;
inline constexpr auto kCloseGoAway = 4005;
inline constexpr auto kCloseKeyRotation = 4006;

struct Hello {
	int version = 0;
	QString connection;
	int pingInterval = 0;    // seconds
	int reconnectSpread = 0; // seconds
	qint64 pts = 0;
	bool resumed = false;
	qint64 serverTimeMs = 0;
};

struct AuthParams {
	QString token;
	bool background = false;
	qint64 pts = 0;
	QString device;
	QString app;
	QString lang;
};

struct ErrorInfo {
	QString code;
	QString title;
	bool retryable = false;
	int retryAfter = 0; // seconds
	int httpStatus = 0;
};

struct Response {
	enum class Status {
		Ok,
		Error,   // the server answered with `error`
		NotOpen, // the call was made while the session was not open
		Dropped, // the connection ended or 10 s passed without an answer
	};
	Status status = Status::Dropped;
	QByteArray body;
	ErrorInfo error;

	[[nodiscard]] bool ok() const {
		return status == Status::Ok;
	}
	// The call never produced an answer: safe to repeat over HTTP.
	[[nodiscard]] bool transport() const {
		return status == Status::NotOpen || status == Status::Dropped;
	}
};

class Session final : public QObject {
	Q_OBJECT

public:
	// Signs a challenge nonce with the device key (purpose "ws_resume");
	// an empty result fails the authentication.
	using ProofProvider = Fn<QByteArray(const QByteArray &nonce)>;

	Session(const QUrl &url, ProofProvider proof, QObject *parent = nullptr);
	~Session();

	// Opens the socket and performs the handshake with the auth request on message 1.
	void connectToServer(int keyId, const QByteArray &serverKey, const AuthParams &auth);

	[[nodiscard]] bool isOpen() const {
		return _open && !_closed;
	}
	[[nodiscard]] bool isClosed() const {
		return _closed;
	}
	// The socket connected and message 1 went out.
	[[nodiscard]] bool handshakeSent() const {
		return _handshake != nullptr;
	}
	[[nodiscard]] const Hello &hello() const {
		return _hello;
	}
	// Local clock (ms) of the last record received.
	[[nodiscard]] qint64 lastReceivedAt() const {
		return _lastReceivedAt;
	}

	// Calls an RPC method with a protobuf-encoded body. `done` always runs
	// (unless the session is destroyed): with the answer, or with NotOpen /
	// Dropped when the connection ends or the request times out. At most
	// kMaxCallsInFlight are sent at once (the gateway refuses more with
	// rate_limited); the rest wait in order for an answer to make room.
	void call(
		const QString &method,
		const QByteArray &body,
		const QString &idempotencyKey,
		Fn<void(const Response &)> done);

	// Sends an ephemeral signal; with `done` the server answers with a Response.
	void signal(
		const QString &type,
		const QByteArray &body,
		Fn<void(const Response &)> done = nullptr);

	void ack(qint64 pts);
	void ping();
	// Re-authenticates on the open connection with a fresh token of the same session.
	void reauth(const QString &token, Fn<void(const Response &)> done);

	// Closes from our side; closed() follows synchronously.
	void close(int code, const QString &reason);

	static constexpr auto kRequestTimeoutMs = 10000;
	static constexpr auto kMaxCallsInFlight = 64; // v2MaxInflight of the gateway

Q_SIGNALS:
	void helloReceived(const Rt::Hello &hello);
	// An UpdateFrame as JSON (live or replayed shape).
	void updateReceived(const QJsonObject &frame);
	// An ephemeral signal with its decoded body ({} when it has none).
	void signalReceived(const QString &type, const QJsonObject &body);
	void authFailed(const Rt::ErrorInfo &error);
	// The server announced why it is closing (close() of the socket follows).
	void serverClose(int code, const QString &reason);
	// The session ended: `code` is the WebSocket close code, 0 after a failure.
	void closed(int code, const QString &reason);

private:
	struct Pending {
		Fn<void(const Response &)> done;
		std::unique_ptr<base::Timer> timer;
		bool call = false; // counted in _callsInFlight
	};
	struct QueuedCall {
		QString method;
		QByteArray body;
		QString idempotencyKey;
		Fn<void(const Response &)> done;
	};

	void start(int keyId, const QByteArray &serverKey, const AuthParams &auth);
	void receive(const QByteArray &record);
	void dispatch(const QJsonObject &envelope, int depth);
	void onResponse(const QJsonObject &response);
	void enqueue(const QJsonObject &envelope);
	void transmit(const QByteArray &envelope);
	quint32 addPending(Fn<void(const Response &)> done, bool call = false);
	void sendCall(QueuedCall &&call);
	void callFinished();
	void drop(quint32 id);
	void dropPending();
	void socketEnded(int code, const QString &reason);
	void fail(const QString &error);

	QUrl _url;
	ProofProvider _proof;
	std::unique_ptr<QWebSocket> _socket;
	std::unique_ptr<Noise::Initiator> _handshake;
	Noise::Session *_noise = nullptr;
	Frames::RecordDecoder _decoder;
	Hello _hello;
	std::map<quint32, Pending> _pending;
	std::deque<QueuedCall> _queuedCalls;
	int _callsInFlight = 0;
	quint32 _nextId = 1;
	qint64 _lastReceivedAt = 0;
	bool _open = false;
	bool _closed = false;

};

} // namespace Rt

Q_DECLARE_METATYPE(Rt::Hello)
Q_DECLARE_METATYPE(Rt::ErrorInfo)
