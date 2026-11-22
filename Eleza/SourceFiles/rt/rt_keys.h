// The gateway's static Noise keys.
//
// Trust decision (the same as the Android and iOS apps, RtKeys there): a
// build ships the production gateway's key(s) baked in, and against the
// production API host those are the only keys it will ever handshake with.
// GET /v2/keys is authenticated by nothing but TLS, so its answer may only
// choose *which* baked key to use (the server's preferred id, e.g. after a
// 4006 close): a key it lists that is not baked, or whose bytes differ from
// the baked ones, is ignored, and so is any key material in the cached
// listing. A TLS-intercepting proxy (a root the user or an administrator
// installed, a mis-issued certificate) therefore cannot talk the client into
// a key of its own: without the private half of a baked key it cannot answer
// the handshake. A failed handshake or a clear-text close only re-reads the
// listing, which again can only point at a baked key.
//
// Rotating the gateway key needs a release that bakes the next key first (as
// a second entry, ahead of the server switching to it).
//
// Only against another API host (a developer's local stack or a test peer,
// `--api` / ELEZA_API) are keys taken from that host's /v2/keys and cached;
// such a connection has no protection beyond TLS.
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

	// True for the API origin the baked keys belong to ("https://api.eleza.me").
	[[nodiscard]] static bool IsPinnedBase(const QString &apiBase);

	// Only baked keys are used (see the top of the file).
	[[nodiscard]] bool pinned() const {
		return _pinned;
	}
	[[nodiscard]] bool hasKeys() const {
		return !_keys.isEmpty();
	}
	// No successful fetch within the last hour; never for a pinned client,
	// whose keys do not come from the network.
	[[nodiscard]] bool stale() const;
	[[nodiscard]] int preferredId() const {
		return _preferredId;
	}
	// The key new connections use; empty when none is known.
	[[nodiscard]] QByteArray preferredKey() const {
		return _keys.value(_preferredId);
	}

	// Re-reads /v2/keys; on failure the keys stay. Callers share one request.
	void refresh(QObject *guard, Fn<void()> done);

	// Applies a {keys: [{id, pub}], preferred} document; false when it has no
	// usable key. A pinned client only takes the preferred id from it, and
	// only when that id is baked with the same bytes.
	bool apply(const QJsonObject &listing, bool fromCache);

private:
	Api::Client *_api = nullptr;
	Storage::KeyValue *_cache = nullptr;
	bool _pinned = false;
	QMap<int, QByteArray> _keys;
	int _preferredId = 0;
	qint64 _fetchedAt = 0;
	bool _refreshing = false;
	QVector<std::pair<base::Guard, Fn<void()>>> _waiting;

};

} // namespace Rt
