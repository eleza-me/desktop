// The persistent connection: keeps one Rt::Session up while the user is
// signed in, reconnects with a backoff, keeps the server key current
// (GET /v2/keys), turns envelopes into app-level frames, and offers RPC over
// the connection with JSON-shaped bodies
// (`call("chat.send", {...})` -> ChatSendRequest / ChatSendResponse through the schema).
//
// Close codes: 4000 reconnect now; 4001 revoked -> the session ends;
// 4002 / 4004 refresh the token, then reconnect; 4003 wait 30 s; 4005 a
// random share of `reconnect_spread`; 4006 the server rotated its key ->
// refresh /v2/keys and reconnect; anything else: backoff 0.5 s -> 30 s with jitter.
//
// Liveness: the server pings every `ping_interval`; no inbound record for
// 2 x that drops the socket, more than 1 x sends our own ping.
#pragma once

#include "api/api_error.h"
#include "base/backoff.h"
#include "base/timer.h"
#include "base/weak_guard.h"
#include "rt/rt_session.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QSet>

namespace Api {
class Client;
} // namespace Api

namespace Storage {
class KeyValue;
} // namespace Storage

namespace Rt {

class Keys;

// What the title bar shows.
enum class State {
	Stopped,           // nobody is signed in / stop() was called
	WaitingForNetwork, // between attempts (backoff) after a failed connect
	Connecting,        // token, keys, socket, handshake
	Updating,          // connected, catching up with missed updates
	Connected,
};

// One app-level event from the connection.
struct Frame {
	enum class Type {
		Hello,        // pts, resumed
		Resumed,      // the replay after Hello is complete
		Resync,       // the gap is older than the update log: read sync.state / difference
		Disconnected,
		Replayed,     // a difference item: pts, kind, data = { chat, seq, message, chat_info, removed, data }
		Update,       // a live update: pts, kind, data = the kind's payload
		Typing,       // chat, kind ("cancel" ends it), ttl (seconds)
		Presence,     // user, status, lastSeenAt; reset = subscriptions were dropped: re-subscribe
		Profile,      // user: the card changed, refetch it
		Topics,       // category: the open-topics list changed
		FeedNew,      // count, since: new posts in the following feed
		LiveLocation, // data = { chat, seq, sender, location, at, until }
		Call,         // kind = "call.*", data = the voice service's JSON
		Group,        // a change of a group: data = { group, pts, kind, data? }
		GroupTyping,  // chat = the group, user, kind ("cancel" ends it), ttl (seconds)
		GroupAdmin,   // chat = the group, kind ("join_requests"), count, user (a new request)
		Config,       // version: of the client-config the server runs
		Settings,     // version: of my settings document (changed)
		Media,        // data = { media, state: ready | rejected, reason }
		Signal,       // any other signal: kind = its type, data = its JSON body ("story", …)
	};
	Type type = Type::Disconnected;
	qint64 pts = 0;
	bool resumed = false;
	QString kind;
	QJsonObject data;
	QString chat;
	QString user;
	QString status;
	qint64 lastSeenAt = 0;
	bool reset = false;
	int ttl = 0;
	QString category;
	int count = 0;
	qint64 since = 0;
	qint64 version = 0;
};

class Client final : public QObject {
	Q_OBJECT

public:
	// What the connection needs from the account.
	class Delegate {
	public:
		virtual ~Delegate() = default;

		virtual void accessToken(Fn<void(QString)> done) = 0;
		[[nodiscard]] virtual QString currentAccessToken() = 0;
		virtual void refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) = 0;
		[[nodiscard]] virtual bool signedIn() = 0;
		// Close code 4001. Must not destroy the client synchronously.
		virtual void sessionRevoked() = 0;
		[[nodiscard]] virtual QByteArray proof(const QByteArray &nonce) = 0;
		[[nodiscard]] virtual QString deviceId() = 0;
	};

	Client(
		Api::Client *api,
		Storage::KeyValue *cache,
		Delegate *delegate,
		QObject *parent = nullptr);
	~Client();

	// Connects (and keeps reconnecting) until stop().
	void start();
	void stop(const QString &reason = QStringLiteral("stopped"));

	[[nodiscard]] bool isConnected() const {
		return _connected;
	}
	[[nodiscard]] State state() const {
		return _state;
	}
	[[nodiscard]] const Hello &hello() const {
		return _hello;
	}

	// The pts the auth request reports (Data::Sync provides it).
	void setPtsProvider(Fn<qint64()> provider);
	// Data::Sync tells while a replay / difference runs (State::Updating).
	void setUpdating(bool updating);
	// The app went to the background / came back (fewer live frames while hidden).
	void setBackground(bool background);
	// The network came back or changed: reconnect now, probe a socket that may be dead.
	void networkChanged();

	// ---- RPC
	// Calls a method with a JSON-shaped body; the answer is the method's JSON
	// body (unwrapped). Fails with Api::Error: the server's problem, or
	// status 0 and code "rt.not_open" / "rt.dropped" when the call got no
	// answer (the caller may then use HTTP - see Rt::Methods).
	void call(
		const QString &method,
		const QJsonObject &body,
		const QString &idempotencyKey,
		base::Guard guard,
		Fn<void(const QJsonValue &)> done,
		Fn<void(const Api::Error &)> fail);

	// ---- ephemeral signals and subscriptions (re-sent after every reconnect)
	// `cap` is the chat's capability token (chat.capability).
	bool sendTyping(const QString &cap, const QString &kind);
	// An activity indicator in a group (the connection's subscription to
	// the group is the proof of membership).
	bool sendGroupTyping(const QString &group, const QString &kind);
	// Subscribes to presence changes; `done` gets the current PresenceOut list
	// ([] when not connected - use POST /v1/presence then).
	void subscribePresence(
		const QStringList &users,
		base::Guard guard = base::Guard(),
		Fn<void(const QJsonArray &)> done = nullptr);
	void unsubscribePresence(const QStringList &users);
	// Presence subscriptions are counted per user: two screens showing the
	// same person need two unsubscribes. At most kPresenceSubMax are sent.
	static constexpr auto kPresenceSubMax = 200; // presence.max_subscriptions_per_connection
	// Card change notifications (Frame::Type::Profile) for these users. The
	// most recently shown kProfileSubMax are followed (the gateway's cap);
	// both kinds of subscription expire on the server and are renewed while
	// connected.
	void followProfiles(const QStringList &users);
	static constexpr auto kProfileSubMax = 300;  // cardSubMax of the gateway
	static constexpr auto kProfileSubKeep = 200; // what a full resend keeps
	[[nodiscard]] QStringList followedProfiles() const {
		return _profileSubs;
	}
	// Open-topics hints for a category ("" = all).
	void subscribeTopics(const QString &category);
	void unsubscribeTopics();
	// «N new posts» hints for the following feed since `since` (ms).
	void subscribeFeed(qint64 since);
	void unsubscribeFeed();
	void sendLiveLocation(const QString &chat, qint64 seq, const QJsonObject &location);

	void ack(qint64 pts);

	// "chat.not_found" style errors of the realtime Response as Api::Error.
	[[nodiscard]] static Api::Error ToApiError(const Response &response);

Q_SIGNALS:
	void frameReceived(const Rt::Frame &frame);
	void connectedChanged(bool connected);
	void stateChanged(Rt::State state);

private:
	void connectOnce(int generation);
	void openSession(int generation, const QString &token);
	void sessionClosed(Session *session, int code);
	void handleEnd(int generation, int code);
	void scheduleReconnect(int generation, double seconds);
	void unpause();
	void resumeAfterPause();
	void onHello(const Hello &hello);
	void onUpdate(const QJsonObject &frame);
	void onSignal(const QString &type, const QJsonObject &body);
	void resendSubscriptions();
	void renewSubscriptions();
	void sendPresenceSubs(Session *session);
	void sendProfileSubs(Session *session, const QStringList &users);
	void watchdogTick();
	void setConnected(bool connected);
	void updateState();
	[[nodiscard]] bool current(int generation) const {
		return _wanted && _generation == generation;
	}
	void watchNetwork();

	Api::Client *_api = nullptr;
	Delegate *_delegate = nullptr;
	std::unique_ptr<Keys> _keys;
	QPointer<Session> _session;
	Hello _hello;
	Fn<qint64()> _ptsProvider;
	base::ExponentialBackoff _backoff;
	base::Timer _pauseTimer;
	base::Timer _watchdog;
	base::Timer _renewTimer;
	State _state = State::Stopped;
	int _generation = 0;
	int _pausedGeneration = 0;
	int _pingInterval = 25;
	int _reconnectSpread = 5;
	int _authFailures = 0;
	// Reconnects "at once" inside the current window (see scheduleReconnect).
	qint64 _quickWindowStart = 0;
	int _quickReconnects = 0;
	int _serverClose = 0;
	bool _wanted = false;
	bool _connected = false;
	bool _connecting = false;
	bool _updating = false;
	bool _background = false;
	bool _networkWatched = false;

	QHash<QString, int> _presenceSubs;  // user -> subscribers here
	int _presenceTtl = 300;             // seconds (PresenceSubResult.ttl_s)
	QStringList _profileSubs;           // most recently shown first
	QSet<QString> _profileSubsOnServer; // what the gateway holds, at most
	std::optional<QString> _topicsCategory;
	std::optional<qint64> _feedSince;

};

} // namespace Rt

Q_DECLARE_METATYPE(Rt::Frame)
Q_DECLARE_METATYPE(Rt::State)
