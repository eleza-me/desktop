#include "rt/rt_keys.h"

#include "api/api_client.h"
#include "base/bytes.h"
#include "base/unixtime.h"
#include "storage/storage_key_value.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUrl>

namespace Rt {
namespace {

const auto kCacheKey = u"rt.keys"_s;
constexpr auto kStaleAfterMs = qint64(3600) * 1000;

// id -> X25519 public key (hex), copied from the production GET /v2/keys
// (the public half of the gateway's rt_static_<id>.key). The first entry is
// preferred until the server names another baked id. To rotate: add the
// next key here, ship, and only then switch the server (the same list as
// RtKeys.BAKED on Android and RtKeys.baked on iOS).
struct BakedKey {
	int id = 0;
	const char *hex = nullptr;
};
constexpr BakedKey kBaked[] = {
	// Production, Noise_NK_25519_ChaChaPoly_SHA256, id 1.
	{ 1, "6ac378aa14d587c46de508bd15d493c2124f6a6525e73a79481d39ba98ac650d" },
};

// The API host the baked keys belong to. It comes from the build or from the
// user's own command line / environment, never from the network.
const auto kPinnedHost = u"api.eleza.me"_s;

} // namespace

Keys::Keys(Api::Client *api, Storage::KeyValue *cache, QObject *parent)
: QObject(parent)
, _api(api)
, _cache(cache)
, _pinned(api && IsPinnedBase(api->base())) {
	if (_pinned) {
		for (const auto &baked : kBaked) {
			const auto key = base::bytes::fromHex(QString::fromLatin1(baked.hex));
			if (key.size() == 32) {
				_keys.insert(baked.id, key);
			}
		}
		_preferredId = kBaked[0].id;
	}
	if (_cache) {
		apply(_cache->object(kCacheKey), true);
	}
}

bool Keys::IsPinnedBase(const QString &apiBase) {
	const auto url = QUrl(apiBase, QUrl::StrictMode);
	return url.isValid()
		&& url.scheme() == u"https"_s
		&& url.host().compare(kPinnedHost, Qt::CaseInsensitive) == 0
		&& (url.port(443) == 443);
}

bool Keys::stale() const {
	return !_pinned && (base::unixtime::nowMs() - _fetchedAt) > kStaleAfterMs;
}

bool Keys::apply(const QJsonObject &listing, bool fromCache) {
	auto fresh = QMap<int, QByteArray>();
	for (const auto &item : listing.value(u"keys"_s).toArray()) {
		const auto object = item.toObject();
		const auto id = base::SafeInt(object.value(u"id"_s).toDouble());
		const auto hex = object.value(u"pub"_s).toString();
		if (id >= 1 && id <= 255 && hex.size() == 64) {
			const auto key = base::bytes::fromHex(hex);
			if (key.size() == 32) {
				fresh.insert(id, key);
			}
		}
	}
	if (fresh.isEmpty()) {
		return false;
	}
	const auto preferred = base::SafeInt(listing.value(u"preferred"_s).toDouble());
	if (!fromCache) {
		_fetchedAt = base::unixtime::nowMs();
	}
	if (_pinned) {
		// The listing may only point at a key this build has, byte for byte.
		const auto known = [&](int id) {
			return _keys.contains(id) && fresh.value(id) == _keys.value(id);
		};
		if (known(preferred)) {
			_preferredId = preferred;
			return true;
		}
		auto any = false;
		for (auto i = _keys.cbegin(); i != _keys.cend(); ++i) {
			if (known(i.key())) {
				any = true;
				if (!known(_preferredId)) {
					_preferredId = i.key();
				}
				break;
			}
		}
		if (!any && !fromCache) {
			qWarning("Rt: GET /v2/keys lists none of the built-in keys; keeping them");
		}
		return any;
	}
	_keys = fresh;
	_preferredId = fresh.contains(preferred) ? preferred : fresh.firstKey();
	return true;
}

void Keys::refresh(QObject *guard, Fn<void()> done) {
	if (done) {
		_waiting.push_back({ guard ? base::Guard(guard) : base::Guard(), std::move(done) });
	}
	if (_refreshing) {
		return;
	}
	_refreshing = true;
	const auto finish = [this] {
		_refreshing = false;
		const auto waiting = std::exchange(_waiting, {});
		for (const auto &[alive, callback] : waiting) {
			if (alive.alive()) {
				callback();
			}
		}
	};
	_api->request(Api::Get, u"/v2/keys"_s)
		.unauthenticated()
		.noOverloadRetry()
		.timeout(10000)
		.guard(this)
		.done([=, this](const QJsonObject &listing) {
			if (apply(listing, false) && _cache) {
				_cache->set(kCacheKey, listing);
			}
			finish();
		})
		.fail([=](const Api::Error &error) {
			qWarning("Rt: GET /v2/keys failed (%s); keeping the cached keys", qPrintable(error.forLog()));
			finish();
		})
		.send();
}

} // namespace Rt
