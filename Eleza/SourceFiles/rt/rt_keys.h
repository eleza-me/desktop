// The gateway's static Noise keys. They are read from GET /v2/keys (over TLS,
// from the same host as the API) and cached in the settings file; the baked
// key is only a seed for the very first connection before the listing is
// cached - the listing always wins, so a server-side rotation needs no new
// client build.
//
// Trust decision (the same one the web client makes, src/rt/client.ts): the
// key is NOT pinned to the build. Whoever can answer GET /v2/keys for the
// API host with a certificate this machine accepts - a TLS-intercepting
// proxy whose root the user or an administrator installed, a mis-issued
// certificate - can substitute a key of their own and then terminate the
// Noise session as well. So Noise adds nothing against an attacker who has
// already defeated TLS to the API; what it adds is confidentiality of the
// realtime frames from everything between the TLS terminator and the gateway
// (CDN, load balancers, relays) and padding of record sizes. A hard pin was
// rejected because the gateway key changes with a fresh deploy and a pinned
// build would lose its socket until it is updated. The listing is only ever
// read with full certificate verification (no error is ignored anywhere in
// Api::Client), and the clear-text 0xFF close can do no more than trigger a
// (rate-limited) re-read of it.
#pragma once

#include "base/weak_guard.h"

#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QObject>

namespace Api {
class Client;
} // namespace Api

namespace Storage {
class KeyValue;
} // namespace Storage

namespace Rt {

class Keys final : public QObject {
public:
	// `cache` may be null (tests).
	Keys(Api::Client *api, Storage::KeyValue *cache, QObject *parent = nullptr);

	[[nodiscard]] bool hasKeys() const {
		return !_keys.isEmpty();
	}
	// No successful fetch within the last hour.
	[[nodiscard]] bool stale() const;
	[[nodiscard]] int preferredId() const {
		return _preferredId;
	}
	// The key new connections use; empty when none is known.
	[[nodiscard]] QByteArray preferredKey() const {
		return _keys.value(_preferredId);
	}

	// Re-reads /v2/keys; on failure the cached keys stay. Callers share one request.
	void refresh(QObject *guard, Fn<void()> done);

	// Applies a {keys: [{id, pub}], preferred} document; false when it has no usable key.
	bool apply(const QJsonObject &listing, bool fromCache);

private:
	Api::Client *_api = nullptr;
	Storage::KeyValue *_cache = nullptr;
	QMap<int, QByteArray> _keys;
	int _preferredId = 0;
	qint64 _fetchedAt = 0;
	bool _refreshing = false;
	QVector<std::pair<base::Guard, Fn<void()>>> _waiting;

};

} // namespace Rt
