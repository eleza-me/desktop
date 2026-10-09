// The gateway's static Noise keys. They are read from GET /v2/keys (over TLS,
// from the same host as the API) and cached in the settings file; the baked
// key is only a seed for the very first connection before the listing is
// cached - the listing always wins, so a server-side rotation needs no new
// client build.
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
