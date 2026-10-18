#include "main/main_session.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "base/platform_file.h"
#include "base/random.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "rt/rt_methods.h"
#include "storage/storage_database.h"
#include "storage/storage_key_value.h"

namespace Main {
namespace {

[[nodiscard]] std::optional<Api::Method> VerbToMethod(const QString &verb) {
	if (verb == u"GET") {
		return Api::Get;
	} else if (verb == u"POST") {
		return Api::Post;
	} else if (verb == u"PUT") {
		return Api::Put;
	} else if (verb == u"PATCH") {
		return Api::Patch;
	} else if (verb == u"DELETE") {
		return Api::Delete;
	}
	return std::nullopt;
}

// The call got no usable answer over the connection: the same request over HTTP.
[[nodiscard]] bool ShouldFallBack(const Api::Error &error) {
	return error.status == 0
		|| error.code == u"response.too_large"
		|| error.code == u"rt.method_unknown"
		|| error.code == u"rt.body_invalid";
}

} // namespace

class Session::RtDelegate final : public Rt::Client::Delegate {
public:
	explicit RtDelegate(Account *account) : _account(account) {
	}

	void accessToken(Fn<void(QString)> done) override {
		_account->accessToken(std::move(done));
	}
	QString currentAccessToken() override {
		return _account->currentAccessToken();
	}
	void refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) override {
		_account->refreshAfterUnauthorized(failedToken, std::move(done));
	}
	bool signedIn() override {
		return _account->signedIn();
	}
	void sessionRevoked() override {
		// Ending the session destroys the connection that reports it: do it
		// from the event loop, with the account (which outlives it) as context.
		const auto account = _account;
		QMetaObject::invokeMethod(account, [account] {
			account->endSession(SessionEndReason::Revoked);
		}, Qt::QueuedConnection);
	}
	QByteArray proof(const QByteArray &nonce) override {
		return _account->proof(nonce);
	}
	QString deviceId() override {
		return _account->deviceId();
	}

private:
	Account *_account = nullptr;

};

class Session::Transport final : public Data::Transport {
public:
	Transport(Api::Client *api, Rt::Client *rt)
	: _api(api)
	, _rt(rt) {
		connect(rt, &Rt::Client::frameReceived, this, &Data::Transport::frameReceived);
		connect(rt, &Rt::Client::connectedChanged, this, &Data::Transport::connectedChanged);
	}

	void call(
			const QString &method,
			const QJsonObject &body,
			const QString &idempotencyKey,
			base::Guard guard,
			Fn<void(const QJsonValue &)> done,
			Fn<void(const Api::Error &)> fail) override {
		const auto call = Rt::Methods::ToHttp(method, body);
		if (!call) {
			QMetaObject::invokeMethod(this, [=] {
				if (fail && guard.alive()) {
					fail(Api::Error::Local(
						400,
						Rt::Methods::Known(method) ? u"rt.param_missing"_s : u"rt.method_unknown"_s,
						u"cannot send "_s + method));
				}
			}, Qt::QueuedConnection);
			return;
		}
		// One key for both transports: a repeat is answered, not executed twice.
		const auto key = (!idempotencyKey.isEmpty() || call->verb == u"GET")
			? idempotencyKey
			: base::RandomUuid();
		const auto http = [=, this, call = *call] {
			const auto verb = VerbToMethod(call.verb);
			if (!verb) {
				if (fail && guard.alive()) {
					fail(Api::Error::Local(400, u"rt.method_unknown"_s, u"cannot send "_s + method));
				}
				return;
			}
			auto builder = _api->request(*verb, call.path);
			builder.query(call.query);
			if (call.hasBody) {
				builder.body(call.body);
			}
			if (!key.isEmpty()) {
				builder.idempotencyKey(key);
			}
			builder
				.guard(guard)
				.doneValue(done)
				.fail(fail)
				.send();
		};
		if (!_rt->isConnected()) {
			http();
			return;
		}
		_rt->call(method, body, key, base::Guard(this), [=](const QJsonValue &result) {
			if (done && guard.alive()) {
				done(result);
			}
		}, [=](const Api::Error &error) {
			if (!guard.alive()) {
				return;
			} else if (ShouldFallBack(error)) {
				http();
			} else if (fail) {
				fail(error);
			}
		});
	}

	bool connected() const override {
		return _rt->isConnected();
	}
	void setPtsProvider(Fn<qint64()> provider) override {
		_rt->setPtsProvider(std::move(provider));
	}
	void setUpdating(bool updating) override {
		_rt->setUpdating(updating);
	}
	void ack(qint64 pts) override {
		_rt->ack(pts);
	}
	bool sendTyping(const QString &cap, const QString &kind) override {
		return _rt->sendTyping(cap, kind);
	}
	bool sendGroupTyping(const QString &group, const QString &kind) override {
		return _rt->sendGroupTyping(group, kind);
	}
	void subscribePresence(
			const QStringList &users,
			base::Guard guard,
			Fn<void(const QJsonArray &)> done) override {
		_rt->subscribePresence(users, std::move(guard), std::move(done));
	}
	void unsubscribePresence(const QStringList &users) override {
		_rt->unsubscribePresence(users);
	}
	void followProfiles(const QStringList &users) override {
		_rt->followProfiles(users);
	}
	void subscribeTopics(const QString &category) override {
		_rt->subscribeTopics(category);
	}
	void unsubscribeTopics() override {
		_rt->unsubscribeTopics();
	}
	void subscribeFeed(qint64 since) override {
		_rt->subscribeFeed(since);
	}
	void unsubscribeFeed() override {
		_rt->unsubscribeFeed();
	}

private:
	Api::Client *_api = nullptr;
	Rt::Client *_rt = nullptr;

};

Session::Session(Account *account, const QString &userId)
: _account(account)
, _userId(userId)
, _userDir(account->userDir(userId)) {
	base::platform::EnsureDirectory(_userDir, true);
	_database = std::make_unique<Storage::Database>(_userDir + u"/cache.db"_s);
	_rtDelegate = std::make_unique<RtDelegate>(account);
	_rt = std::make_unique<Rt::Client>(&account->api(), &account->settings(), _rtDelegate.get());
	_transport = std::make_unique<Transport>(&account->api(), _rt.get());
	connect(_rt.get(), &Rt::Client::stateChanged, this, &Session::connectionStateChanged);

	auto config = Data::Session::Config();
	config.userId = userId;
	config.transport = _transport.get();
	config.api = &account->api();
	config.database = _database.get();
	config.clientConfig = &account->clientConfig();
	config.cacheDir = _userDir + u"/media"_s;
	_data = std::make_unique<Data::Session>(std::move(config));

	account->clientConfig().load();
	_data->start([this] {
		if (_shutdown) {
			return;
		}
		_ready = true;
		Q_EMIT ready();
		// The stored pts is known now: the auth request can report it.
		if (_account->config().realtime) {
			_rt->start();
		}
	});
}

Session::~Session() {
	shutdown();
	// The stores go first, then what they use.
	_data = nullptr;
	_transport = nullptr;
	_rt = nullptr;
	_rtDelegate = nullptr;
	_database = nullptr;
}

Api::Client &Session::api() const {
	return _account->api();
}

Data::Transport &Session::transport() const {
	return *_transport;
}

Data::RpcBuilder Session::rpc(const QString &method) const {
	return _transport->rpc(method);
}

void Session::shutdown() {
	if (_shutdown) {
		return;
	}
	_shutdown = true;
	_rt->stop(u"signed out"_s);
	_data->stop();
	// Requests issued by the stores must not call into them any more; their
	// guards take care of that as the stores are destroyed.
}

} // namespace Main
