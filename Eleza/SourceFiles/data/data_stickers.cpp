#include "data/data_stickers.h"

#include "api/api_client.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "storage/storage_database.h"

namespace Data {
namespace {

const auto kCacheKey = u"stickers"_s;
const auto kRecentKey = u"stickers.recent"_s;

} // namespace

StickerPack StickerPack::FromJson(const QJsonObject &json) {
	auto result = StickerPack();
	result.id = Json::String(json, "id"_L1);
	result.slug = Json::String(json, "slug"_L1);
	result.title = Json::String(json, "title"_L1);
	result.coverUrl = Json::String(json, "cover_url"_L1);
	result.isDefault = Json::Bool(json, "default"_L1);
	result.installed = Json::Bool(json, "installed"_L1);
	result.stickers = Json::List<Sticker>(json.value("stickers"_L1));
	for (auto &sticker : result.stickers) {
		if (sticker.pack.isEmpty()) {
			sticker.pack = result.id;
		}
		if (sticker.emoji.isEmpty() && !sticker.emojis.isEmpty()) {
			sticker.emoji = sticker.emojis.front();
		}
	}
	return result;
}

QJsonObject StickerPack::toJson() const {
	auto result = QJsonObject{
		{ u"id"_s, id },
		{ u"slug"_s, slug },
		{ u"title"_s, title },
		{ u"default"_s, isDefault },
		{ u"installed"_s, installed },
		{ u"stickers"_s, Json::Array(stickers) },
	};
	Json::Put(result, "cover_url"_L1, coverUrl);
	return result;
}

Stickers::Stickers(Session *session)
: QObject(session)
, _session(session) {
}

QVector<StickerPack> Stickers::installedPacks() const {
	auto result = QVector<StickerPack>();
	for (const auto &pack : _packs) {
		if (pack.installed) {
			result.push_back(pack);
		}
	}
	return result;
}

const StickerPack *Stickers::pack(const QString &id) const {
	for (const auto &pack : _packs) {
		if (pack.id == id) {
			return &pack;
		}
	}
	return nullptr;
}

const Sticker *Stickers::sticker(const QString &id) const {
	for (const auto &pack : _packs) {
		for (const auto &sticker : pack.stickers) {
			if (sticker.id == id) {
				return &sticker;
			}
		}
	}
	for (const auto &sticker : _recent) {
		if (sticker.id == id) {
			return &sticker;
		}
	}
	return nullptr;
}

void Stickers::persist() {
	if (const auto db = _session->local()) {
		db->setValue(kCacheKey, QJsonObject{
			{ u"version"_s, _version },
			{ u"packs"_s, Json::Array(_packs) },
		});
	}
}

void Stickers::load(base::Guard guard, Fn<void()> done) {
	const auto db = _session->local();
	if (_cacheRead || !db) {
		fetch(std::move(guard), std::move(done));
		return;
	}
	_cacheRead = true;
	db->value(kRecentKey, this, [=, this](QJsonValue value) {
		if (_recent.isEmpty()) {
			_recent = Json::List<Sticker>(value);
		}
	});
	db->value(kCacheKey, this, [=, this](QJsonValue value) {
		const auto cached = value.toObject();
		if (!_loaded && cached.value("packs"_L1).isArray()) {
			_packs = Json::List<StickerPack>(cached.value("packs"_L1));
			_version = Json::String(cached, "version"_L1);
			_loaded = true;
			Q_EMIT changed();
		}
		fetch(guard, done);
	});
}

void Stickers::fetch(base::Guard guard, Fn<void()> done) {
	_session->api().request(Api::Get, u"/v1/stickers/packs"_s)
		.query(u"version"_s, _version)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (!Json::Bool(result, "unchanged"_L1)) {
				_packs = Json::List<StickerPack>(result.value("packs"_L1));
				_version = Json::String(result, "version"_L1);
				persist();
				_loaded = true;
				Q_EMIT changed();
			}
			_loaded = true;
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=](const Api::Error &) {
			// The cached list (if any) stays.
			if (done && guard.alive()) {
				done();
			}
		})
		.send();
}

void Stickers::packBySlug(
		const QString &slug,
		base::Guard guard,
		Fn<void(const StickerPack &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/stickers/packs/by-slug/"_s + PathSegment(slug))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(StickerPack::FromJson(result));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Stickers::setInstalled(const QString &packId, bool installed) {
	const auto apply = [=, this](bool value) {
		for (auto &pack : _packs) {
			if (pack.id == packId && pack.installed != value) {
				pack.installed = value;
				Q_EMIT changed();
			}
		}
	};
	apply(installed);
	auto builder = _session->api().request(
		installed ? Api::Put : Api::Delete,
		u"/v1/stickers/packs/%1/installed"_s.arg(PathSegment(packId)));
	if (installed) {
		builder.body(QJsonObject());
	}
	builder
		.guard(this)
		.done([=, this] {
			// The list's version moved: read it again (packs made by people
			// appear in the list only once installed).
			fetch(base::Guard(), nullptr);
		})
		.fail([=, this](const Api::Error &error) {
			apply(!installed);
			_session->reportFailure(u"stickers.install"_s, packId, error);
		})
		.send();
}

void Stickers::noteUsed(const Sticker &sticker) {
	if (sticker.id.isEmpty()) {
		return;
	}
	_recent.erase(std::remove_if(_recent.begin(), _recent.end(), [&](const Sticker &existing) {
		return existing.id == sticker.id;
	}), _recent.end());
	_recent.push_front(sticker);
	if (_recent.size() > kMaxRecent) {
		_recent.resize(kMaxRecent);
	}
	if (const auto db = _session->local()) {
		db->setValue(kRecentKey, Json::Array(_recent));
	}
	Q_EMIT changed();
}

} // namespace Data
