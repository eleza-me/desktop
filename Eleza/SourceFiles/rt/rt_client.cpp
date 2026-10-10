#include "rt/rt_client.h"

#include "api/api_client.h"
#include "base/bytes.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "rt/rt_keys.h"
#include "rt/rt_proto.h"

#include <QtCore/QLoggingCategory>
#include <QtNetwork/QNetworkInformation>

namespace Rt {
namespace {

Q_LOGGING_CATEGORY(Log, "eleza.rt.client", QtWarningMsg)

constexpr auto kWatchdogEveryMs = 5000;
constexpr auto kTooManyDelay = 30.;
constexpr auto kAuthFailuresDelay = 15.;
// Close codes that ask for a reconnect "at once" (resync, re-auth, a rotated
// key) are only authenticated by TLS, or not tied to any progress at all: a
// peer that keeps sending them must not turn the client into a tight loop of
// handshakes, token refreshes and /v2/keys requests.
constexpr auto kQuickWindowMs = qint64(10000);
constexpr auto kQuickReconnects = 3;
constexpr auto kQuickPenalty = 2.;

[[nodiscard]] QByteArray EncodeBody(const char *message, const QJsonObject &object) {
	return Proto::Encode(QLatin1StringView(message), object).value_or(QByteArray());
}

[[nodiscard]] QJsonArray ToArray(const QSet<QString> &set) {
	auto result = QJsonArray();
	for (const auto &value : set) {
		result.push_back(value);
	}
	return result;
}

[[nodiscard]] QJsonArray ToArray(const QStringList &list) {
	auto result = QJsonArray();
	for (const auto &value : list) {
		result.push_back(value);
	}
	return result;
}

} // namespace

Client::Client(
	Api::Client *api,
	Storage::KeyValue *cache,
	Delegate *delegate,
	QObject *parent)
: QObject(parent)
, _api(api)
, _delegate(delegate)
, _keys(std::make_unique<Keys>(api, cache))
, _pauseTimer([this] { resumeAfterPause(); })
, _watchdog([this] { watchdogTick(); }) {
	qRegisterMetaType<Rt::Frame>();
	qRegisterMetaType<Rt::State>();
}

Client::~Client() {
	_wanted = false;
	++_generation;
	if (const auto session = _session.data()) {
		session->disconnect(this);
		delete session;
	}
}

void Client::setPtsProvider(Fn<qint64()> provider) {
	_ptsProvider = std::move(provider);
}

void Client::start() {
	if (_wanted) {
		return;
	}
	_wanted = true;
	_backoff.reset();
	_authFailures = 0;
	watchNetwork();
	connectOnce(++_generation);
}

void Client::stop(const QString &reason) {
	_wanted = false;
	++_generation;
	_pauseTimer.cancel();
	_watchdog.cancel();
	if (const auto session = _session.data()) {
		_session = nullptr;
		session->disconnect(this);
		session->close(1000, reason);
		session->deleteLater();
	}
	_connecting = false;
	const auto was = _connected;
	setConnected(false);
	if (was) {
		auto frame = Frame();
		frame.type = Frame::Type::Disconnected;
		Q_EMIT frameReceived(frame);
	}
	updateState();
}

void Client::watchNetwork() {
	if (_networkWatched) {
		return;
	}
	_networkWatched = true;
	// Optional: without a backend plugin the backoff alone drives reconnects.
	if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
		return;
	}
	const auto info = QNetworkInformation::instance();
	if (!info) {
		return;
	}
	connect(
		info,
		&QNetworkInformation::reachabilityChanged,
		this,
		[this](QNetworkInformation::Reachability reachability) {
			if (reachability != QNetworkInformation::Reachability::Disconnected) {
				networkChanged();
			}
		});
}

void Client::networkChanged() {
	if (!_wanted) {
		return;
	}
	_backoff.reset();
	unpause();
	// A socket bound to an interface that is gone looks alive: probe it.
	const auto session = _session.data();
	if (session && session->isOpen()) {
		session->ping();
		const auto weak = QPointer<Session>(session);
		base::call_delayed(5000, this, [=, this] {
			if (weak
				&& _session == weak
				&& (base::unixtime::nowMs() - weak->lastReceivedAt()) > 4000) {
				weak->close(kCloseResync, u"network changed"_s);
			}
		});
	}
}

void Client::setBackground(bool background) {
	if (_background == background) {
		return;
	}
	_background = background;
	if (!background) {
		_backoff.reset();
		unpause();
	}
	const auto session = _session.data();
	if (!session || !session->isOpen()) {
		return;
	}
	if (!background
		&& (base::unixtime::nowMs() - session->lastReceivedAt()) > qint64(_pingInterval) * 1000) {
		session->ping();
	}
	session->signal(u"state"_s, EncodeBody("StateSignal", QJsonObject{
		{ u"background"_s, background },
	}));
}

void Client::setUpdating(bool updating) {
	if (_updating != updating) {
		_updating = updating;
		updateState();
	}
}

void Client::setConnected(bool connected) {
	if (_connected == connected) {
		return;
	}
	_connected = connected;
	Q_EMIT connectedChanged(connected);
}

void Client::updateState() {
	const auto state = !_wanted
		? State::Stopped
		: _connected
		? (_updating ? State::Updating : State::Connected)
		: _pauseTimer.isActive()
		? State::WaitingForNetwork
		: State::Connecting;
	if (_state != state) {
		_state = state;
		Q_EMIT stateChanged(state);
	}
}

void Client::connectOnce(int generation) {
	if (!current(generation)) {
		return;
	}
	_connecting = true;
	_serverClose = 0;
	updateState();
	_delegate->accessToken(base::guard(this, [=, this](QString token) {
		if (!current(generation)) {
			return;
		}
		if (token.isEmpty()) {
			_connecting = false;
			handleEnd(generation, 1000);
			return;
		}
		if (!_keys->hasKeys() || _keys->stale()) {
			_keys->refresh(this, [=, this] {
				openSession(generation, token);
			});
		} else {
			openSession(generation, token);
		}
	}));
}

void Client::openSession(int generation, const QString &token) {
	if (!current(generation)) {
		return;
	}
	const auto key = _keys->preferredKey();
	if (key.isEmpty()) {
		qCWarning(Log, "no server key (GET /v2/keys failed)");
		_connecting = false;
		handleEnd(generation, 0);
		return;
	}
	const auto session = new Session(
		_api->webSocketUrl(),
		[delegate = _delegate](const QByteArray &nonce) { return delegate->proof(nonce); },
		this);
	_session = session;
	connect(session, &Session::helloReceived, this, [=, this](const Hello &hello) {
		if (_session == session) {
			onHello(hello);
		}
	});
	connect(session, &Session::updateReceived, this, [=, this](const QJsonObject &frame) {
		if (_session == session) {
			onUpdate(frame);
		}
	});
	connect(session, &Session::signalReceived, this, [=, this](
			const QString &type,
			const QJsonObject &body) {
		if (_session == session) {
			onSignal(type, body);
		}
	});
	connect(session, &Session::authFailed, this, [](const ErrorInfo &error) {
		qCWarning(Log, "auth failed: %s", qPrintable(error.code));
	});
	connect(session, &Session::serverClose, this, [=, this](int code, const QString &) {
		if (_session == session) {
			_serverClose = code;
		}
	});
	connect(session, &Session::closed, this, [=, this](int code, const QString &reason) {
#ifdef ELEZA_DEBUG_TOOLS
		qCInfo(Log, "closed: %d %s", code, qPrintable(reason));
#else // ELEZA_DEBUG_TOOLS
		Q_UNUSED(reason);
		qCInfo(Log, "closed: %d", code);
#endif // ELEZA_DEBUG_TOOLS
		sessionClosed(session, code);
	});

	auto auth = AuthParams();
	auth.token = token;
	auth.background = _background;
	auth.pts = _ptsProvider ? _ptsProvider() : 0;
	auth.device = _delegate->deviceId();
	auth.app = _api->clientHeader();
	auth.lang = _api->language();
	session->connectToServer(_keys->preferredId(), key, auth);
	_watchdog.callEach(kWatchdogEveryMs);
}

void Client::sessionClosed(Session *session, int code) {
	session->deleteLater();
	if (_session != session) {
		return;
	}
	_session = nullptr;
	_watchdog.cancel();
	const auto generation = _generation;
	const auto finalCode = _serverClose ? _serverClose : code;
	const auto proceed = [=, this] {
		if (!current(generation)) {
			return;
		}
		if (_connected) {
			setConnected(false);
			auto frame = Frame();
			frame.type = Frame::Type::Disconnected;
			Q_EMIT frameReceived(frame);
		}
		_connecting = false;
		handleEnd(generation, finalCode);
	};
	// The socket opened but closed before `hello`: a wrong static key after a
	// server redeploy ends the handshake like this. Re-read /v2/keys so the
	// next attempt uses the current key.
	const auto handshakeFailed = !_connected
		&& session->handshakeSent()
		&& finalCode != kCloseKeyRotation;
	if (handshakeFailed && _wanted) {
		_keys->refresh(this, proceed);
	} else {
		proceed();
	}
}

void Client::handleEnd(int generation, int code) {
	if (!current(generation)) {
		return;
	}
	switch (code) {
	case kCloseResync:
		scheduleReconnect(generation, 0.);
		break;
	case kCloseRevoked: {
		const auto delegate = _delegate;
		stop(u"revoked"_s);
		delegate->sessionRevoked();
	} break;
	case kCloseReauth:
	case kCloseAuth:
		++_authFailures;
		_delegate->refreshAfterUnauthorized(
			_delegate->currentAccessToken(),
			base::guard(this, [=, this](bool) {
				if (!current(generation)) {
					return;
				} else if (!_delegate->signedIn()) {
					stop(u"signed out"_s);
					return;
				}
				scheduleReconnect(
					generation,
					(_authFailures <= 2) ? 0. : std::max(_backoff.next(), kAuthFailuresDelay));
			}));
		break;
	case kCloseTooMany:
		scheduleReconnect(generation, kTooManyDelay);
		break;
	case kCloseGoAway:
		scheduleReconnect(generation, base::RandomUnit() * _reconnectSpread);
		break;
	case kCloseKeyRotation: {
		const auto before = _keys->preferredId();
		_keys->refresh(this, [=, this] {
			scheduleReconnect(
				generation,
				(_keys->preferredId() != before) ? 0. : _backoff.next());
		});
	} break;
	default:
		scheduleReconnect(generation, _backoff.next());
		break;
	}
}

void Client::scheduleReconnect(int generation, double seconds) {
	if (!current(generation)) {
		return;
	}
	_pausedGeneration = generation;
	if (seconds <= 0.) {
		const auto now = base::unixtime::nowMs();
		if (now - _quickWindowStart > kQuickWindowMs || now < _quickWindowStart) {
			_quickWindowStart = now;
			_quickReconnects = 0;
		}
		if (++_quickReconnects > kQuickReconnects) {
			seconds = std::max(_backoff.next(), kQuickPenalty);
		}
	}
	if (seconds <= 0.) {
		// "Now" still goes through the event loop: never reconnect from
		// inside a closing session's callbacks.
		_connecting = true;
		updateState();
		QMetaObject::invokeMethod(this, [=, this] {
			if (current(generation) && !_session) {
				connectOnce(generation);
			}
		}, Qt::QueuedConnection);
		return;
	}
	_pauseTimer.callOnce(int(seconds * 1000));
	updateState();
}

void Client::unpause() {
	if (_pauseTimer.isActive()) {
		_pauseTimer.cancel();
		resumeAfterPause();
	}
}

void Client::resumeAfterPause() {
	const auto generation = _pausedGeneration;
	if (current(generation) && !_session && !_connecting) {
		connectOnce(generation);
	}
}

void Client::onHello(const Hello &hello) {
	_hello = hello;
	_authFailures = 0;
	_backoff.reset();
	if (hello.pingInterval > 0) {
		_pingInterval = hello.pingInterval;
	}
	if (hello.reconnectSpread > 0) {
		_reconnectSpread = hello.reconnectSpread;
	}
	_connecting = false;
	setConnected(true);
	updateState();
	auto frame = Frame();
	frame.type = Frame::Type::Hello;
	frame.pts = hello.pts;
	frame.resumed = hello.resumed;
	Q_EMIT frameReceived(frame);
	resendSubscriptions();
}

void Client::onUpdate(const QJsonObject &update) {
	auto frame = Frame();
	frame.pts = base::SafeInt64(update.value("pts"_L1).toDouble());
	frame.kind = update.value("kind"_L1).toString();
	if (update.contains("chat"_L1)
		|| update.contains("message"_L1)
		|| update.contains("chat_info"_L1)
		|| update.value("removed"_L1).toBool()) {
		// A replayed difference item (chat, seq, message, chat_info, removed;
		// data for the kinds with a stored view).
		frame.type = Frame::Type::Replayed;
		frame.data = update;
		frame.chat = update.value("chat"_L1).toString();
	} else {
		frame.type = Frame::Type::Update;
		const auto data = update.value("data"_L1).toObject();
		const auto field = Proto::DataFieldOfKind(frame.kind);
		frame.data = (!field.isEmpty() && data.value(field).isObject())
			? data.value(field).toObject()
			: Proto::UnwrapData(data);
		frame.chat = frame.data.value("chat"_L1).toString();
	}
	Q_EMIT frameReceived(frame);
}

void Client::onSignal(const QString &type, const QJsonObject &body) {
	auto frame = Frame();
	if (type == u"typing") {
		frame.type = Frame::Type::Typing;
		frame.chat = body.value("chat"_L1).toString();
		frame.kind = body.value("kind"_L1).toString(u"typing"_s);
		if (frame.kind.isEmpty()) {
			frame.kind = u"typing"_s;
		}
		const auto ttl = base::SafeInt(body.value("ttl_s"_L1).toDouble());
		frame.ttl = (ttl > 0) ? ttl : 6;
	} else if (type == u"presence") {
		frame.type = Frame::Type::Presence;
		frame.user = body.value("user"_L1).toString();
		frame.status = body.value("status"_L1).toString();
		frame.lastSeenAt = base::SafeInt64(body.value("last_seen_at"_L1).toDouble());
		frame.reset = body.value("reset"_L1).toBool();
	} else if (type == u"presence_reset") {
		frame.type = Frame::Type::Presence;
		frame.reset = true;
	} else if (type == u"profile") {
		frame.type = Frame::Type::Profile;
		frame.user = body.value("user"_L1).toString();
	} else if (type == u"topics") {
		frame.type = Frame::Type::Topics;
		frame.category = body.value("category"_L1).toString();
	} else if (type == u"feed_new") {
		frame.type = Frame::Type::FeedNew;
		frame.count = base::SafeInt(body.value("count"_L1).toDouble());
		frame.since = base::SafeInt64(body.value("since"_L1).toDouble());
	} else if (type == u"live_location") {
		frame.type = Frame::Type::LiveLocation;
		frame.data = body;
		frame.chat = body.value("chat"_L1).toString();
	} else if (type == u"resumed") {
		frame.type = Frame::Type::Resumed;
	} else if (type == u"resync") {
		frame.type = Frame::Type::Resync;
	} else if (type.startsWith(u"call.")) {
		frame.type = Frame::Type::Call;
		frame.kind = type;
		frame.data = body;
	} else {
		return;
	}
	Q_EMIT frameReceived(frame);
	if (frame.type == Frame::Type::Presence && frame.reset && frame.user.isEmpty()) {
		// The server dropped our presence subscriptions (visibility changed).
		const auto session = _session.data();
		if (session && session->isOpen() && !_presenceSubs.isEmpty()) {
			session->signal(u"presence_sub"_s, EncodeBody("PresenceSub", QJsonObject{
				{ u"users"_s, ToArray(_presenceSubs) },
			}));
		}
	}
}

Api::Error Client::ToApiError(const Response &response) {
	switch (response.status) {
	case Response::Status::NotOpen: {
		auto error = Api::Error::Offline(u"the connection is not open"_s);
		error.code = u"rt.not_open"_s;
		return error;
	}
	case Response::Status::Dropped: {
		auto error = Api::Error::Offline(u"the connection dropped the request"_s);
		error.code = u"rt.dropped"_s;
		return error;
	}
	default:
		break;
	}
	auto error = Api::Error();
	error.status = response.error.httpStatus ? response.error.httpStatus : 500;
	error.code = response.error.code;
	error.message = response.error.title.isEmpty() ? response.error.code : response.error.title;
	error.retryable = response.error.retryable;
	if (response.error.retryAfter > 0) {
		error.retryAfter = double(response.error.retryAfter);
	}
	return error;
}

void Client::call(
		const QString &method,
		const QJsonObject &body,
		const QString &idempotencyKey,
		base::Guard guard,
		Fn<void(const QJsonValue &)> done,
		Fn<void(const Api::Error &)> fail) {
	const auto failWith = [=](Response::Status status) {
		if (fail && guard.alive()) {
			auto response = Response();
			response.status = status;
			fail(ToApiError(response));
		}
	};
	const auto session = _session.data();
	const auto names = Proto::NamesOfMethod(method);
	const auto request = Schema::FindMessage(QLatin1StringView(names.request));
	const auto responseMessage = Schema::FindMessage(QLatin1StringView(names.response));
	if (!session || !session->isOpen() || !request || !responseMessage) {
		// Asynchronous like every other outcome.
		QMetaObject::invokeMethod(this, [=] {
			failWith(Response::Status::NotOpen);
		}, Qt::QueuedConnection);
		return;
	}
	const auto bytes = body.isEmpty() ? QByteArray() : Proto::Encode(*request, body);
	session->call(method, bytes, idempotencyKey, [=](const Response &response) {
		if (!guard.alive()) {
			return;
		} else if (!response.ok()) {
			const auto error = ToApiError(response);
			if (!response.transport()) {
				qCInfo(Log, "%s -> %s", qPrintable(method), qPrintable(error.forLog()));
			}
			if (fail) {
				fail(error);
			}
			return;
		}
		const auto decoded = response.body.isEmpty()
			? std::make_optional(QJsonObject())
			: Proto::Decode(*responseMessage, response.body);
		if (!decoded) {
			if (fail) {
				fail(Api::Error::Local(500, u"decoding"_s, u"bad protobuf"_s));
			}
			return;
		}
		if (done) {
			done(Proto::UnwrapBody(*decoded));
		}
	});
}

bool Client::sendTyping(const QString &cap, const QString &kind) {
	const auto session = _session.data();
	if (!session || !session->isOpen() || cap.isEmpty()) {
		return false;
	}
	session->signal(u"typing"_s, EncodeBody("TypingSignal", QJsonObject{
		{ u"cap"_s, cap },
		{ u"kind"_s, kind },
	}));
	return true;
}

void Client::subscribePresence(
		const QStringList &users,
		base::Guard guard,
		Fn<void(const QJsonArray &)> done) {
	for (const auto &user : users) {
		_presenceSubs.insert(user);
	}
	const auto session = _session.data();
	if (!session || !session->isOpen() || users.isEmpty()) {
		if (done) {
			QMetaObject::invokeMethod(this, [=] {
				if (guard.alive()) {
					done(QJsonArray());
				}
			}, Qt::QueuedConnection);
		}
		return;
	}
	const auto body = EncodeBody("PresenceSub", QJsonObject{ { u"users"_s, ToArray(users) } });
	if (!done) {
		session->signal(u"presence_sub"_s, body);
		return;
	}
	session->signal(u"presence_sub"_s, body, [=](const Response &response) {
		if (!guard.alive()) {
			return;
		}
		const auto decoded = response.ok()
			? Proto::Decode("PresenceSubResult"_L1, response.body)
			: std::nullopt;
		done(decoded ? decoded->value("presence"_L1).toArray() : QJsonArray());
	});
}

void Client::unsubscribePresence(const QStringList &users) {
	for (const auto &user : users) {
		_presenceSubs.remove(user);
	}
	const auto session = _session.data();
	if (session && session->isOpen() && !users.isEmpty()) {
		session->signal(u"presence_unsub"_s, EncodeBody("PresenceSub", QJsonObject{
			{ u"users"_s, ToArray(users) },
		}));
	}
}

void Client::followProfiles(const QStringList &users) {
	auto changed = false;
	for (const auto &user : users) {
		if (!user.isEmpty() && !_profileSubs.contains(user)) {
			_profileSubs.insert(user);
			changed = true;
		}
	}
	const auto session = _session.data();
	if (changed && session && session->isOpen()) {
		session->signal(u"profile_sub"_s, EncodeBody("ProfileSub", QJsonObject{
			{ u"users"_s, ToArray(_profileSubs) },
		}));
	}
}

void Client::subscribeTopics(const QString &category) {
	_topicsCategory = category;
	const auto session = _session.data();
	if (session && session->isOpen()) {
		session->signal(u"topics_sub"_s, EncodeBody("TopicsSub", QJsonObject{
			{ u"category"_s, category },
		}));
	}
}

void Client::unsubscribeTopics() {
	// The gateway has no unsubscribe: the hint simply is not re-requested.
	_topicsCategory = std::nullopt;
}

void Client::subscribeFeed(qint64 since) {
	_feedSince = since;
	const auto session = _session.data();
	if (session && session->isOpen()) {
		session->signal(u"feed_sub"_s, EncodeBody("FeedSub", QJsonObject{
			{ u"since"_s, double(since) },
		}));
	}
}

void Client::unsubscribeFeed() {
	_feedSince = std::nullopt;
}

void Client::sendLiveLocation(const QString &chat, qint64 seq, const QJsonObject &location) {
	const auto session = _session.data();
	if (session && session->isOpen()) {
		session->signal(u"live_location"_s, EncodeBody("LiveLocationSignal", QJsonObject{
			{ u"chat"_s, chat },
			{ u"seq"_s, double(seq) },
			{ u"location"_s, location },
		}));
	}
}

void Client::resendSubscriptions() {
	const auto session = _session.data();
	if (!session || !session->isOpen()) {
		return;
	}
	if (!_presenceSubs.isEmpty()) {
		session->signal(u"presence_sub"_s, EncodeBody("PresenceSub", QJsonObject{
			{ u"users"_s, ToArray(_presenceSubs) },
		}));
	}
	if (!_profileSubs.isEmpty()) {
		session->signal(u"profile_sub"_s, EncodeBody("ProfileSub", QJsonObject{
			{ u"users"_s, ToArray(_profileSubs) },
		}));
	}
	if (_topicsCategory) {
		session->signal(u"topics_sub"_s, EncodeBody("TopicsSub", QJsonObject{
			{ u"category"_s, *_topicsCategory },
		}));
	}
	if (_feedSince) {
		session->signal(u"feed_sub"_s, EncodeBody("FeedSub", QJsonObject{
			{ u"since"_s, double(*_feedSince) },
		}));
	}
}

void Client::ack(qint64 pts) {
	if (const auto session = _session.data()) {
		session->ack(pts);
	}
}

void Client::watchdogTick() {
	const auto session = _session.data();
	if (!session || !session->isOpen()) {
		// Still opening: a handshake that hangs is dropped too.
		if (session
			&& (base::unixtime::nowMs() - session->lastReceivedAt()) > qint64(2) * _pingInterval * 1000) {
			session->close(kCloseResync, u"watchdog"_s);
		}
		return;
	}
	const auto idle = double(base::unixtime::nowMs() - session->lastReceivedAt()) / 1000.;
	if (idle > 2. * _pingInterval) {
		qCInfo(Log, "no record for %d s, dropping", 2 * _pingInterval);
		session->close(kCloseResync, u"watchdog"_s);
	} else if (idle > _pingInterval) {
		session->ping();
	}
}

} // namespace Rt
