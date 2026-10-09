#include "rt/rt_keys.h"

#include "api/api_client.h"
#include "base/bytes.h"
#include "base/unixtime.h"
#include "storage/storage_key_value.h"

#include <QtCore/QJsonArray>

namespace Rt {
namespace {

const auto kCacheKey = u"rt.keys"_s;
constexpr auto kStaleAfterMs = qint64(3600) * 1000;

// Seed for the first connection of a fresh install (production gateway, key id 1).
constexpr auto kBakedId = 1;
const auto kBakedKey = u"6ac378aa14d587c46de508bd15d493c2124f6a6525e73a79481d39ba98ac650d"_s;

} // namespace

Keys::Keys(Api::Client *api, Storage::KeyValue *cache, QObject *parent)
: QObject(parent)
, _api(api)
, _cache(cache) {
	_keys.insert(kBakedId, base::bytes::fromHex(kBakedKey));
	_preferredId = kBakedId;
	if (_cache) {
		apply(_cache->object(kCacheKey), true);
	}
}

bool Keys::stale() const {
	return (base::unixtime::nowMs() - _fetchedAt) > kStaleAfterMs;
}

bool Keys::apply(const QJsonObject &listing, bool fromCache) {
	auto fresh = QMap<int, QByteArray>();
	for (const auto &item : listing.value(u"keys"_s).toArray()) {
		const auto object = item.toObject();
		const auto id = int(object.value(u"id"_s).toDouble());
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
	const auto preferred = int(listing.value(u"preferred"_s).toDouble());
	_keys = fresh;
	_preferredId = fresh.contains(preferred) ? preferred : fresh.firstKey();
	if (!fromCache) {
		_fetchedAt = base::unixtime::nowMs();
	}
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
			qWarning("Rt: GET /v2/keys failed (%s); keeping the cached keys", qPrintable(error.describe()));
			finish();
		})
		.send();
}

} // namespace Rt
