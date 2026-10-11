// `GET /v1/client-config`: the limits and feature flags the server exposes
// to clients. Fetched once per start (and on demand), cached in the settings
// file so the previous answer is available before the first response, with
// built-in defaults for a server that does not send a key.
//
// The document is { "version": N, "values": { "<dotted key>": value } }. A
// flat document with the dotted keys at its root and nested objects are
// accepted as well (test servers).
#pragma once

#include "base/weak_guard.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>

namespace Storage {
class KeyValue;
} // namespace Storage

namespace Api {

class Client;

struct ConversationCategory {
	QString slug;
	QString title;
};

class ClientConfig final : public QObject {
	Q_OBJECT

public:
	ClientConfig(Client *api, Storage::KeyValue *cache, QObject *parent = nullptr);

	// Fetches the document (several callers share one request); `done(true)`
	// when a fresh one arrived, `done(false)` when the cached one stays.
	void load(QObject *guard = nullptr, Fn<void(bool)> done = nullptr);
	[[nodiscard]] bool loaded() const {
		return _loaded;
	}

	// ---- raw access
	[[nodiscard]] QJsonValue value(const QString &dotted) const;
	[[nodiscard]] QString string(const QString &dotted, const QString &fallback = QString()) const;
	[[nodiscard]] qint64 integer(const QString &dotted, qint64 fallback = 0) const;
	[[nodiscard]] bool boolean(const QString &dotted, bool fallback = false) const;
	[[nodiscard]] QStringList strings(const QString &dotted) const;
	// Durations come as "24h" / "90s" / "1h30m" strings or as numbers of seconds.
	[[nodiscard]] qint64 seconds(const QString &dotted, qint64 fallback = 0) const;

	// ---- feature flags
	[[nodiscard]] bool realtimeV2Enabled() const;       // feature.realtime_v2
	[[nodiscard]] bool draftsEnabled() const;           // feature.drafts
	[[nodiscard]] bool twoStepEnabled() const;          // feature.two_step
	[[nodiscard]] bool linkPreviewEnabled() const;      // feature.link_preview
	[[nodiscard]] bool stickersEnabled() const;         // feature.stickers
	[[nodiscard]] bool userStickerPacksEnabled() const; // feature.user_sticker_packs
	[[nodiscard]] bool soundsEnabled() const;           // feature.sounds
	[[nodiscard]] bool videoHlsEnabled() const;         // feature.video_hls
	[[nodiscard]] bool groupsEnabled() const;           // feature.groups (absent: off)
	// The second release of groups (admins, invite links, pins, mentions).
	[[nodiscard]] bool groupsAdministration() const;    // groups.max_admins is listed
	[[nodiscard]] bool videoCallsEnabled() const;       // feature.video_calls (absent: off)
	[[nodiscard]] bool deleteChatForBothEnabled() const;

	// ---- sign-in and profile
	[[nodiscard]] int otpCodeLength() const;
	[[nodiscard]] int otpResendCooldown() const;        // seconds
	[[nodiscard]] QStringList allowedCountryPrefixes() const;
	[[nodiscard]] int handleMinLength() const;
	[[nodiscard]] int handleMaxLength() const;
	[[nodiscard]] int displayNameMaxChars() const;
	[[nodiscard]] int bioMaxChars() const;
	[[nodiscard]] int twoStepMinPasswordLength() const;

	// ---- messaging
	[[nodiscard]] int maxMessageLength() const;
	[[nodiscard]] int maxCaptionLength() const;
	[[nodiscard]] int maxEntities() const;
	[[nodiscard]] int maxQuoteLength() const;
	[[nodiscard]] QStringList reactionEmojiSet() const;
	[[nodiscard]] qint64 editWindow() const;            // seconds
	[[nodiscard]] qint64 deleteForEveryoneWindow() const; // seconds, 0 = no limit
	[[nodiscard]] int maxPinnedChats() const;
	[[nodiscard]] int maxPinsPerChat() const;
	[[nodiscard]] QStringList muteDurations() const;
	[[nodiscard]] QStringList liveLocationPeriods() const;
	[[nodiscard]] qint64 activityTtl() const;           // seconds a typing indicator lives
	[[nodiscard]] int presenceMaxSubscriptions() const;
	[[nodiscard]] int cardsBatchMax() const;            // ids per users.cards

	// ---- conversations
	[[nodiscard]] QVector<ConversationCategory> conversationCategories() const;
	[[nodiscard]] QStringList conversationDurations() const;
	[[nodiscard]] QString conversationDefaultDuration() const;
	[[nodiscard]] int conversationTopicMaxChars() const;
	[[nodiscard]] int conversationMaxConcurrent() const;

	// ---- posts
	[[nodiscard]] int postMaxTextChars() const;
	[[nodiscard]] int postMaxThreadParts() const;
	[[nodiscard]] int postMaxAlbumItems() const;
	[[nodiscard]] int pollMinOptions() const;
	[[nodiscard]] int pollMaxOptions() const;
	[[nodiscard]] qint64 postEditWindow() const;        // seconds
	[[nodiscard]] QString dislikeCountVisibility() const;

	// ---- media
	[[nodiscard]] qint64 maxUploadBytes() const;
	[[nodiscard]] qint64 maxImageBytes() const;
	[[nodiscard]] qint64 maxVideoBytes() const;
	[[nodiscard]] qint64 maxAudioBytes() const;
	[[nodiscard]] qint64 maxGifBytes() const;
	[[nodiscard]] qint64 uploadPartSize() const;
	[[nodiscard]] qint64 maxVoiceDuration() const;      // seconds
	[[nodiscard]] qint64 maxVideoDurationChat() const;  // seconds

	// ---- realtime
	[[nodiscard]] qint64 realtimePingInterval() const;  // seconds
	[[nodiscard]] qint64 fallbackSyncInterval() const;  // seconds
	[[nodiscard]] qint64 fallbackSyncAfter() const;     // seconds

	// ---- versions
	// api.min_client_version.<platform>; "0.0.0" when not set.
	[[nodiscard]] QString minClientVersion(const QString &platform = QStringLiteral("desktop")) const;
	[[nodiscard]] QString updateUrl(const QString &platform = QStringLiteral("desktop")) const;
	// True when `version` is older than minClientVersion().
	[[nodiscard]] bool updateRequired(const QString &version) const;

	// "1h30m" -> 5400; -1 when the text is not a duration.
	[[nodiscard]] static qint64 ParseDuration(const QString &text);
	// Compares dotted versions: <0, 0, >0.
	[[nodiscard]] static int CompareVersions(const QString &a, const QString &b);

Q_SIGNALS:
	void changed();

private:
	Client *_api = nullptr;
	Storage::KeyValue *_cache = nullptr;
	QJsonObject _data;
	bool _loaded = false;
	bool _loading = false;
	QVector<std::pair<base::Guard, Fn<void(bool)>>> _waiting;

};

} // namespace Api
