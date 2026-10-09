// The device's identity and the signed-in account.
//
// Main::Account exists for the whole run of the application: it owns the
// HTTP client, the device id and device key (a fresh install generates them),
// the stored tokens, and the token refresh flow. When somebody is signed in
// it also owns the Main::Session with every per-user service; `session()` is
// null otherwise and `sessionChanged()` tells when that flips.
//
// Files (under <workdir>/account/):
//   settings.json            device id, cached client-config and realtime keys
//   device.key, session.bin  Storage::SecureBlob (DPAPI on Windows, 0600 on Linux)
//   user-<id>/               the signed-in user's cache.db and media cache
//
// Token refresh (the web client's net/session.ts): refreshing proves
// possession of the device key by signing the nonce handed out with the
// previous tokens; concurrent callers share one refresh; a proof / nonce
// problem re-issues the nonce once; only the refresh or nonce endpoint
// answering 401 with the API's own problem code ends the session - network
// errors and 5xx keep it and back off (2 s .. 30 s).
#pragma once

#include "api/api_client.h"
#include "crypto/crypto_device_key.h"

namespace Storage {
class KeyValue;
class SecureBlob;
} // namespace Storage

namespace Api {
class Auth;
class ClientConfig;
} // namespace Api

namespace Main {

class Session;

enum class SessionEndReason {
	SignedOut,      // the user signed out here
	Expired,        // the refresh token is no longer accepted: sign in again, local data stays
	Revoked,        // ended on purpose elsewhere (another device, an operator): local data is wiped
	AccountDeleted,
};

struct Tokens {
	QString access;
	qint64 accessExpiresAt = 0; // unix seconds
	QString refresh;
	QString nonce;              // base64 device nonce for the next refresh
	QString userId;
	QString sessionId;          // the `sid` claim

	[[nodiscard]] bool signedIn() const {
		return !refresh.isEmpty();
	}
	[[nodiscard]] static Tokens FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

class Account final : public QObject, public Api::TokenProvider {
	Q_OBJECT

public:
	struct Config {
		QString workdir;                                   // data directory
		QString apiBase = QStringLiteral("https://api.eleza.me");
		QString version;                                   // empty: the build's version
		QString language = QStringLiteral("fa");
		bool realtime = true;                              // false: HTTP only (tests, tools)
	};

	explicit Account(Config config, QObject *parent = nullptr);
	~Account();

	// Creates the session when stored tokens exist. Call once after
	// connecting to sessionChanged() / sessionEnded().
	void start();

	[[nodiscard]] const Config &config() const {
		return _config;
	}
	[[nodiscard]] Api::Client &api() {
		return *_api;
	}
	[[nodiscard]] Api::Auth &auth() {
		return *_auth;
	}
	[[nodiscard]] Api::ClientConfig &clientConfig() {
		return *_clientConfig;
	}
	// App-level preferences and caches (not per user).
	[[nodiscard]] Storage::KeyValue &settings() {
		return *_settings;
	}
	[[nodiscard]] QString accountDir() const;
	[[nodiscard]] QString userDir(const QString &userId) const;

	void setLanguage(const QString &language);
	[[nodiscard]] QString language() const;

	// ---- device identity
	[[nodiscard]] QString deviceId() const {
		return _deviceId;
	}
	[[nodiscard]] const Crypto::DeviceKey &deviceKey();
	// A new sign-in may register a fresh key.
	void resetDeviceKey();
	// Signature for a realtime challenge (purpose "ws_resume").
	[[nodiscard]] QByteArray proof(const QByteArray &nonce);

	// ---- who is signed in
	[[nodiscard]] bool signedIn() const {
		return _tokens.signedIn();
	}
	[[nodiscard]] QString userId() const {
		return _tokens.userId;
	}
	[[nodiscard]] QString sessionId() const {
		return _tokens.sessionId;
	}
	[[nodiscard]] Session *session() const {
		return _session.get();
	}

	// ---- tokens
	// Stores the tokens of a sign-in / refresh answer (the API's tokens
	// document) and brings the session up if it is not.
	void storeTokens(const QJsonObject &tokens);
	void accessToken(Fn<void(QString)> done) override;
	// The raw token even if it is about to expire (the realtime gateway takes a proof then).
	[[nodiscard]] QString currentAccessToken() const {
		return _tokens.access;
	}
	void refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) override;

	// `POST /v1/auth/logout` (best effort, at most 5 s), then the local
	// session and its data are dropped. `done` runs after sessionEnded().
	void logout(QObject *guard = nullptr, Fn<void()> done = nullptr);
	// Ends the local session without telling the server.
	void endSession(SessionEndReason reason);

	// The claims of a JWT's payload ({} when it does not parse).
	[[nodiscard]] static QJsonObject JwtClaims(const QString &token);

Q_SIGNALS:
	// session() appeared (signed in) or went away.
	void sessionChanged();
	void sessionEnded(Main::SessionEndReason reason);

private:
	enum class RefreshResult {
		Refreshed,
		NotSignedIn,
		Failed,
	};

	void loadIdentity();
	void persistTokens();
	void createSession();
	void destroySession(bool wipeData);
	[[nodiscard]] bool stale(int marginSeconds) const;

	void refreshShared(bool ignoreBackoff, Fn<void(bool)> done);
	void performRefresh();
	void refreshAttempt(const QString &refresh, bool reissued);
	void refreshRequest(
		const QString &refresh,
		const QString &nonce,
		Fn<void(const QJsonObject &)> done,
		Fn<void(const Api::Error &)> fail);
	void reissueNonce(
		const QString &refresh,
		Fn<void(const QString &)> done,
		Fn<void(const Api::Error &)> fail);
	void failedInTransport();
	void finishRefresh(bool ok);

	Config _config;
	std::unique_ptr<Storage::KeyValue> _settings;
	std::unique_ptr<Storage::SecureBlob> _keyBlob;
	std::unique_ptr<Storage::SecureBlob> _tokensBlob;
	std::unique_ptr<Api::Client> _api;
	std::unique_ptr<Api::ClientConfig> _clientConfig;
	std::unique_ptr<Api::Auth> _auth;
	std::unique_ptr<Session> _session;

	QString _deviceId;
	Crypto::DeviceKey _deviceKey;
	Tokens _tokens;

	bool _refreshing = false;
	QVector<Fn<void(bool)>> _refreshWaiters;
	qint64 _refreshRetryAt = 0; // ms
	int _refreshFailures = 0;

};

} // namespace Main

Q_DECLARE_METATYPE(Main::SessionEndReason)
