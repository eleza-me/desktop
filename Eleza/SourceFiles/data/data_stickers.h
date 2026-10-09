// Sticker packs: the published packs with my installed state
// (`GET /v1/stickers/packs?version=` answers `unchanged` when nothing
// changed, so the cached list is reused), install / remove, one pack by its
// link name, and the recently used stickers of this device.
#pragma once

#include "data/data_social_types.h"

namespace Data {

class Session;

struct StickerPack {
	QString id;
	QString slug;
	QString title;
	QString coverUrl;
	bool isDefault = false;
	bool installed = false;
	QVector<Sticker> stickers;

	[[nodiscard]] static StickerPack FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

class Stickers final : public QObject {
	Q_OBJECT

public:
	explicit Stickers(Session *session);

	[[nodiscard]] bool loaded() const {
		return _loaded;
	}
	[[nodiscard]] const QVector<StickerPack> &packs() const {
		return _packs;
	}
	[[nodiscard]] QVector<StickerPack> installedPacks() const;
	[[nodiscard]] const StickerPack *pack(const QString &id) const;
	// Looks a sticker up in the known packs.
	[[nodiscard]] const Sticker *sticker(const QString &id) const;
	[[nodiscard]] const QVector<Sticker> &recent() const {
		return _recent;
	}

	// The cached list first (database), then the server (only what changed).
	void load(base::Guard guard = base::Guard(), Fn<void()> done = nullptr);
	// A pack by its link name (eleza.me/addstickers/<slug>), also one that is not installed.
	void packBySlug(const QString &slug, base::Guard guard, Fn<void(const StickerPack &)> done, FailCallback fail = nullptr);
	void setInstalled(const QString &packId, bool installed);
	// A sticker was sent: it moves to the front of recent().
	void noteUsed(const Sticker &sticker);

	static constexpr auto kMaxRecent = 30;

Q_SIGNALS:
	void changed();

private:
	void fetch(base::Guard guard, Fn<void()> done);
	void persist();

	Session *_session = nullptr;
	QVector<StickerPack> _packs;
	QVector<Sticker> _recent;
	QString _version;
	bool _loaded = false;
	bool _cacheRead = false;

};

} // namespace Data
