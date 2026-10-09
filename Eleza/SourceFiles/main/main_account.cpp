#include "main/main_account.h"

#include "api/api_auth.h"
#include "api/api_client_config.h"
#include "base/bytes.h"
#include "base/platform_file.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"
#include "storage/storage_secure_blob.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

#include <cmath>

#ifndef ELEZA_CORE_VERSION
#define ELEZA_CORE_VERSION "0.1.0"
#endif // !ELEZA_CORE_VERSION

namespace Main {
namespace {

const auto kDeviceIdKey = u"device.id"_s;
const auto kLanguageKey = u"language"_s;

// `access_expires_at` arrives as an RFC 3339 string or a number of
// (milli)seconds; without it assume nine minutes.
[[nodiscard]] qint64 ParseExpiry(const QJsonValue &value) {
	const auto normalize = [](double number) {
		return qint64((number > 1e12) ? (number / 1000.) : number);
	};
	if (value.isDouble()) {
		return normalize(value.toDouble());
	} else if (value.isString()) {
		const auto text = value.toString();
		const auto parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
		if (parsed.isValid()) {
			return parsed.toSecsSinceEpoch();
		}
		auto ok = false;
		const auto number = text.toDouble(&ok);
		if (ok) {
			return normalize(number);
		}
	}
	return base::unixtime::now() + 9 * 60;
}

} // namespace

Tokens Tokens::FromJson(const QJsonObject &json) {
	auto result = Tokens();
	result.access = json.value(u"access"_s).toString();
	result.accessExpiresAt = qint64(json.value(u"access_expires_at"_s).toDouble());
	result.refresh = json.value(u"refresh"_s).toString();
	result.nonce = json.value(u"nonce"_s).toString();
	result.userId = json.value(u"user_id"_s).toString();
	result.sessionId = json.value(u"sid"_s).toString();
	return result;
}

QJsonObject Tokens::toJson() const {
	return QJsonObject{
		{ u"access"_s, access },
		{ u"access_expires_at"_s, double(accessExpiresAt) },
		{ u"refresh"_s, refresh },
		{ u"nonce"_s, nonce },
		{ u"user_id"_s, userId },
		{ u"sid"_s, sessionId },
	};
}

Account::Account(Config config, QObject *parent)
: QObject(parent)
, _config(std::move(config)) {
	qRegisterMetaType<Main::SessionEndReason>();
	if (_config.version.isEmpty()) {
		_config.version = QStringLiteral(ELEZA_CORE_VERSION);
	}
	base::platform::EnsureDirectory(accountDir(), true);
	_settings = std::make_unique<Storage::KeyValue>(accountDir() + u"/settings.json"_s);
	_keyBlob = std::make_unique<Storage::SecureBlob>(accountDir() + u"/device.key"_s);
	_tokensBlob = std::make_unique<Storage::SecureBlob>(accountDir() + u"/session.bin"_s);
	_api = std::make_unique<Api::Client>(_config.apiBase, _config.version);
	_api->setTokenProvider(this);
	_api->setLanguage(_settings->string(kLanguageKey, _config.language));
	_clientConfig = std::make_unique<Api::ClientConfig>(_api.get(), _settings.get());
	_auth = std::make_unique<Api::Auth>(this);
	loadIdentity();
}

Account::~Account() {
	_session = nullptr;
	_api->setTokenProvider(nullptr);
}

QString Account::accountDir() const {
	return QDir(_config.workdir).filePath(u"account"_s);
}

QString Account::userDir(const QString &userId) const {
	// User ids are opaque strings: keep the directory name to a safe alphabet.
	static const auto kUnsafe = QRegularExpression(u"[^A-Za-z0-9_-]"_s);
	auto name = userId;
	name.replace(kUnsafe, u"_"_s);
	return accountDir() + u"/user-"_s + name;
}

void Account::setLanguage(const QString &language) {
	_api->setLanguage(language);
	_settings->set(kLanguageKey, language);
}

QString Account::language() const {
	return _api->language();
}

void Account::loadIdentity() {
	_deviceId = _settings->string(kDeviceIdKey);
	if (_deviceId.isEmpty()) {
		_deviceId = base::RandomUuid();
		_settings->set(kDeviceIdKey, _deviceId);
		_settings->flush();
	}
	if (auto stored = _tokensBlob->read()) {
		_tokens = Tokens::FromJson(QJsonDocument::fromJson(*stored).object());
		base::bytes::wipe(*stored);
	}
	if (auto stored = _keyBlob->read()) {
		_deviceKey = Crypto::DeviceKey::FromPrivate(*stored);
		base::bytes::wipe(*stored);
	}
	if (_tokens.signedIn() && !_deviceKey.valid()) {
		// Tokens without the key that proves them cannot be refreshed.
		qWarning("Main: the device key is missing; the stored session is dropped.");
		_tokens = Tokens();
		_tokensBlob->remove();
	}
}

const Crypto::DeviceKey &Account::deviceKey() {
	if (!_deviceKey.valid()) {
		_deviceKey = Crypto::DeviceKey::Generate();
		if (!_keyBlob->write(_deviceKey.privateKey())) {
			qWarning("Main: could not store the device key.");
		}
	}
	return _deviceKey;
}

void Account::resetDeviceKey() {
	if (signedIn()) {
		return; // the key in use belongs to the live session
	}
	_deviceKey = Crypto::DeviceKey();
	_keyBlob->remove();
}

QByteArray Account::proof(const QByteArray &nonce) {
	return deviceKey().sign(u"ws_resume"_s, nonce);
}

void Account::start() {
	if (signedIn() && !_session) {
		createSession();
		Q_EMIT sessionChanged();
	}
}

QJsonObject Account::JwtClaims(const QString &token) {
	const auto parts = token.split(u'.');
	if (parts.size() < 2) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(base::bytes::fromBase64(parts[1])).object();
}

void Account::persistTokens() {
	auto bytes = QJsonDocument(_tokens.toJson()).toJson(QJsonDocument::Compact);
	if (!_tokensBlob->write(bytes)) {
		qWarning("Main: could not store the session.");
	}
	base::bytes::wipe(bytes);
}

void Account::storeTokens(const QJsonObject &tokens) {
	const auto access = tokens.value(u"access_token"_s).toString();
	const auto claims = JwtClaims(access);
	const auto previousUser = _tokens.userId;
	auto next = Tokens();
	next.access = access;
	next.accessExpiresAt = ParseExpiry(tokens.value(u"access_expires_at"_s));
	next.refresh = tokens.value(u"refresh_token"_s).toString();
	next.nonce = tokens.value(u"device_nonce"_s).toString();
	next.userId = tokens.value(u"user_id"_s).toString();
	if (next.userId.isEmpty()) {
		next.userId = claims.value(u"sub"_s).toString();
	}
	if (next.userId.isEmpty()) {
		next.userId = _tokens.userId;
	}
	next.sessionId = claims.value(u"sid"_s).toString();
	if (next.sessionId.isEmpty()) {
		next.sessionId = _tokens.sessionId;
	}
	_tokens = next;
	_refreshFailures = 0;
	_refreshRetryAt = 0;
	persistTokens();

	if (!_tokens.signedIn()) {
		return;
	}
	if (_session && previousUser != _tokens.userId) {
		// Another account signed in over the old one.
		destroySession(false);
	}
	if (!_session) {
		createSession();
		Q_EMIT sessionChanged();
	}
}

void Account::createSession() {
	_session = std::make_unique<Session>(this, _tokens.userId);
}

void Account::destroySession(bool wipeData) {
	if (!_session) {
		return;
	}
	const auto dir = _session->userDir();
	// Screens observing the session must let go before its services die.
	auto session = std::move(_session);
	Q_EMIT sessionChanged();
	session->shutdown();
	session = nullptr;
	if (wipeData) {
		base::platform::RemoveRecursively(dir);
	}
}

void Account::endSession(SessionEndReason reason) {
	if (!signedIn() && !_session) {
		return;
	}
	const auto userId = _tokens.userId;
	_tokens = Tokens();
	_tokensBlob->remove();
	finishRefresh(false);
	const auto wipe = (reason != SessionEndReason::Expired);
	if (_session) {
		destroySession(wipe);
	} else if (wipe && !userId.isEmpty()) {
		base::platform::RemoveRecursively(userDir(userId));
	}
	Q_EMIT sessionEnded(reason);
}

void Account::logout(QObject *guard, Fn<void()> done) {
	const auto finish = [this, guard = guard ? base::Guard(guard) : base::Guard(), done] {
		endSession(SessionEndReason::SignedOut);
		if (done && guard.alive()) {
			done();
		}
	};
	if (!signedIn()) {
		finish();
		return;
	}
	// Best effort: the server learns about it now or the token expires.
	_api->request(Api::Post, u"/v1/auth/logout"_s)
		.timeout(5000)
		.noOverloadRetry()
		.guard(this)
		.done([=] { finish(); })
		.fail([=](const Api::Error &) { finish(); })
		.send();
}

bool Account::stale(int marginSeconds) const {
	if (_tokens.access.isEmpty() || !_tokens.accessExpiresAt) {
		return true;
	}
	return (_tokens.accessExpiresAt - base::unixtime::now()) < marginSeconds;
}

void Account::accessToken(Fn<void(QString)> done) {
	if (signedIn() && stale(60)) {
		refreshShared(false, [this, done = std::move(done)](bool) {
			done(_tokens.access);
		});
		return;
	}
	done(_tokens.access);
}

void Account::refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) {
	if (!_tokens.access.isEmpty() && _tokens.access != failedToken && !stale(30)) {
		done(true);
		return;
	}
	refreshShared(false, std::move(done));
}

void Account::refreshShared(bool ignoreBackoff, Fn<void(bool)> done) {
	if (_refreshing) {
		_refreshWaiters.push_back(std::move(done));
		return;
	}
	if (!ignoreBackoff && _refreshRetryAt > base::unixtime::nowMs()) {
		done(false);
		return;
	}
	_refreshing = true;
	_refreshWaiters.push_back(std::move(done));
	performRefresh();
}

void Account::finishRefresh(bool ok) {
	if (!_refreshing) {
		return;
	}
	_refreshing = false;
	const auto waiters = std::exchange(_refreshWaiters, {});
	for (const auto &waiter : waiters) {
		waiter(ok);
	}
}

void Account::failedInTransport() {
	++_refreshFailures;
	const auto delay = std::min(30., std::pow(2., std::min(_refreshFailures, 5)));
	_refreshRetryAt = base::unixtime::nowMs() + qint64(delay * 1000);
}

void Account::performRefresh() {
	const auto refresh = _tokens.refresh;
	if (refresh.isEmpty()) {
		finishRefresh(false);
		return;
	}
	refreshAttempt(refresh, false);
}

void Account::refreshAttempt(const QString &refresh, bool reissued) {
	// Signed out (or signed in anew) while a request was in flight.
	const auto replaced = [=, this] {
		if (_tokens.refresh != refresh) {
			finishRefresh(signedIn());
			return true;
		}
		return false;
	};
	const auto sessionGone = [=, this](const Api::Error &error) {
		// Only `auth.session_revoked` means the session was ended on purpose
		// (wipe the local data); every other code keeps it for the next sign-in.
		endSession((error.code == u"auth.session_revoked")
			? SessionEndReason::Revoked
			: SessionEndReason::Expired);
		finishRefresh(false);
	};
	refreshRequest(refresh, _tokens.nonce, [=, this](const QJsonObject &tokens) {
		if (replaced()) {
			return;
		}
		storeTokens(tokens);
		finishRefresh(true);
	}, [=, this](const Api::Error &error) {
		static const auto kProof = QRegularExpression(u"proof|nonce"_s);
		const auto proofProblem = (error.status == 401) && error.code.contains(kProof);
		if (proofProblem && !reissued) {
			reissueNonce(refresh, [=, this](const QString &nonce) {
				if (replaced()) {
					return;
				}
				_tokens.nonce = nonce;
				persistTokens();
				refreshAttempt(refresh, true);
			}, [=, this](const Api::Error &nonceError) {
				if (nonceError.status == 401 && !nonceError.code.isEmpty()) {
					if (!replaced()) {
						sessionGone(nonceError);
					}
					return;
				}
				failedInTransport();
				finishRefresh(false);
			});
			return;
		}
		// A 401 with the API's own problem body: the session is gone.
		if (error.status == 401 && !error.code.isEmpty() && error.code != u"offline") {
			if (!replaced()) {
				sessionGone(error);
			}
			return;
		}
		failedInTransport();
		finishRefresh(false);
	});
}

void Account::refreshRequest(
		const QString &refresh,
		const QString &nonce,
		Fn<void(const QJsonObject &)> done,
		Fn<void(const Api::Error &)> fail) {
	if (nonce.isEmpty()) {
		fail(Api::Error::Local(401, u"auth.device_proof_invalid"_s, u"no nonce"_s));
		return;
	}
	const auto signature = base::bytes::toBase64(
		deviceKey().sign(u"refresh"_s, base::bytes::fromBase64(nonce)));
	const auto body = QJsonObject{
		{ u"refresh_token"_s, refresh },
		{ u"device_nonce"_s, nonce },
		{ u"signature"_s, signature },
	};
	const auto requestId = base::RandomUuid();
	const auto send = [=, this](Fn<void(const Api::Error &)> onFail) {
		_api->request(Api::Post, u"/v1/auth/token/refresh"_s)
			.unauthenticated()
			.noIdempotencyKey()
			.noOverloadRetry()
			.requestId(requestId)
			.body(body)
			.guard(this)
			.done(done)
			.fail(std::move(onFail))
			.send();
	};
	send([=](const Api::Error &error) {
		if (error.offline()) {
			// The server may already have rotated the token: the identical
			// request once more (its reuse grace covers it).
			send(fail);
		} else {
			fail(error);
		}
	});
}

void Account::reissueNonce(
		const QString &refresh,
		Fn<void(const QString &)> done,
		Fn<void(const Api::Error &)> fail) {
	_api->request(Api::Post, u"/v1/auth/nonce"_s)
		.unauthenticated()
		.noOverloadRetry()
		.body(QJsonObject{ { u"refresh_token"_s, refresh } })
		.guard(this)
		.done([=](const QJsonObject &result) {
			done(result.value(u"device_nonce"_s).toString());
		})
		.fail(std::move(fail))
		.send();
}

} // namespace Main
