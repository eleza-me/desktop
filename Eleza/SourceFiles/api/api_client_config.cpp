#include "api/api_client_config.h"

#include "api/api_client.h"
#include "storage/storage_key_value.h"

#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>

#include <algorithm>

namespace Api {
namespace {

const auto kCacheKey = u"client_config"_s;

} // namespace

ClientConfig::ClientConfig(Client *api, Storage::KeyValue *cache, QObject *parent)
: QObject(parent)
, _api(api)
, _cache(cache)
, _data(cache ? cache->object(kCacheKey) : QJsonObject()) {
}

void ClientConfig::load(QObject *guard, Fn<void(bool)> done) {
	if (done) {
		_waiting.push_back({ guard ? base::Guard(guard) : base::Guard(), std::move(done) });
	}
	if (_loading) {
		return;
	}
	_loading = true;
	const auto finish = [this](bool fresh) {
		_loading = false;
		const auto waiting = std::exchange(_waiting, {});
		for (const auto &[guard, callback] : waiting) {
			if (guard.alive()) {
				callback(fresh);
			}
		}
	};
	_api->request(Get, u"/v1/client-config"_s)
		.unauthenticated()
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			_loaded = true;
			if (_data != result) {
				_data = result;
				if (_cache) {
					_cache->set(kCacheKey, result);
				}
				Q_EMIT changed();
			}
			finish(true);
		})
		.fail([=](const Error &) {
			finish(false);
		})
		.send();
}

QJsonValue ClientConfig::value(const QString &dotted) const {
	// The server's document is { "version": N, "values": { "<key>": … } };
	// a flat document (test servers) is read as well.
	if (const auto values = _data.value("values"_L1); values.isObject()) {
		const auto found = values.toObject().value(dotted);
		if (!found.isUndefined()) {
			return found;
		}
	}
	if (const auto direct = _data.value(dotted); !direct.isUndefined()) {
		return direct;
	}
	auto current = QJsonValue(_data);
	for (const auto &part : dotted.split(u'.')) {
		if (!current.isObject()) {
			return QJsonValue(QJsonValue::Undefined);
		}
		current = current.toObject().value(part);
	}
	return current;
}

QString ClientConfig::string(const QString &dotted, const QString &fallback) const {
	const auto found = value(dotted);
	if (found.isString()) {
		return found.toString();
	} else if (found.isDouble()) {
		return QString::number(base::SafeInt64(found.toDouble()));
	}
	return fallback;
}

qint64 ClientConfig::integer(const QString &dotted, qint64 fallback) const {
	const auto found = value(dotted);
	if (found.isDouble()) {
		return base::SafeInt64(found.toDouble());
	} else if (found.isString()) {
		auto ok = false;
		const auto result = found.toString().toLongLong(&ok);
		return ok ? result : fallback;
	}
	return fallback;
}

bool ClientConfig::boolean(const QString &dotted, bool fallback) const {
	const auto found = value(dotted);
	return found.isBool() ? found.toBool() : fallback;
}

QStringList ClientConfig::strings(const QString &dotted) const {
	auto result = QStringList();
	const auto found = value(dotted);
	if (found.isString()) {
		// Some string lists are stored comma separated.
		for (const auto &part : found.toString().split(u',', Qt::SkipEmptyParts)) {
			result.push_back(part.trimmed());
		}
		return result;
	}
	for (const auto &item : found.toArray()) {
		if (item.isString()) {
			result.push_back(item.toString());
		} else if (item.isDouble()) {
			result.push_back(QString::number(base::SafeInt64(item.toDouble())));
		}
	}
	return result;
}

qint64 ClientConfig::ParseDuration(const QString &text) {
	static const auto kPart = QRegularExpression(u"(\\d+(?:\\.\\d+)?)(ns|us|µs|ms|s|m|h|d)"_s);
	const auto trimmed = text.trimmed();
	if (trimmed.isEmpty()) {
		return -1;
	}
	auto total = 0.;
	auto consumed = qsizetype(0);
	auto i = kPart.globalMatch(trimmed);
	while (i.hasNext()) {
		const auto match = i.next();
		if (match.capturedStart() != consumed) {
			return -1;
		}
		consumed = match.capturedEnd();
		const auto number = match.captured(1).toDouble();
		const auto unit = match.captured(2);
		total += number * ((unit == u"d")
			? 86400.
			: (unit == u"h")
			? 3600.
			: (unit == u"m")
			? 60.
			: (unit == u"s")
			? 1.
			: (unit == u"ms")
			? 0.001
			: (unit == u"ns")
			? 1e-9
			: 1e-6);
	}
	return (consumed == trimmed.size()) ? qint64(total) : -1;
}

qint64 ClientConfig::seconds(const QString &dotted, qint64 fallback) const {
	const auto found = value(dotted);
	if (found.isString()) {
		const auto text = found.toString();
		const auto parsed = ParseDuration(text);
		if (parsed >= 0) {
			return parsed;
		}
		auto ok = false;
		const auto number = text.toLongLong(&ok);
		return ok ? number : fallback;
	} else if (found.isDouble()) {
		const auto number = found.toDouble();
		// A Go duration serialized as a number is nanoseconds.
		return (number >= 1e9) ? base::SafeInt64(number / 1e9) : base::SafeInt64(number);
	}
	return fallback;
}

bool ClientConfig::realtimeV2Enabled() const {
	return boolean(u"feature.realtime_v2"_s, true);
}

bool ClientConfig::draftsEnabled() const {
	return boolean(u"feature.drafts"_s, true);
}

bool ClientConfig::twoStepEnabled() const {
	return boolean(u"feature.two_step"_s, true);
}

bool ClientConfig::linkPreviewEnabled() const {
	return boolean(u"feature.link_preview"_s, true);
}

bool ClientConfig::stickersEnabled() const {
	return boolean(u"feature.stickers"_s, true);
}

bool ClientConfig::userStickerPacksEnabled() const {
	return boolean(u"feature.user_sticker_packs"_s, false);
}

bool ClientConfig::soundsEnabled() const {
	return boolean(u"feature.sounds"_s, false);
}

bool ClientConfig::videoHlsEnabled() const {
	return boolean(u"feature.video_hls"_s, false);
}

bool ClientConfig::groupsEnabled() const {
	return boolean(u"feature.groups"_s, false);
}

bool ClientConfig::groupsAdministration() const {
	// Defined by the second release of groups: a server that lists it has
	// admins, invite links, pins, mentions and group drafts.
	const auto found = value(u"groups.max_admins"_s);
	return !found.isUndefined() && !found.isNull();
}

bool ClientConfig::videoCallsEnabled() const {
	// Absent means a server older than video calls, which rejects the
	// `video` field of a call: never send it then.
	return boolean(u"feature.video_calls"_s, false);
}

bool ClientConfig::deleteChatForBothEnabled() const {
	return boolean(u"messaging.delete_chat_for_both_enabled"_s, true);
}

int ClientConfig::otpCodeLength() const {
	return int(integer(u"otp.code_length"_s, 6));
}

int ClientConfig::otpResendCooldown() const {
	return int(seconds(u"otp.resend_cooldown"_s, 60));
}

QStringList ClientConfig::allowedCountryPrefixes() const {
	return strings(u"otp.allowed_country_prefixes"_s);
}

int ClientConfig::handleMinLength() const {
	return int(integer(u"profile.handle_min_len"_s, 3));
}

int ClientConfig::handleMaxLength() const {
	return int(integer(u"profile.handle_max_len"_s, 24));
}

int ClientConfig::displayNameMaxChars() const {
	return int(integer(u"profile.display_name_max_chars"_s, 50));
}

int ClientConfig::bioMaxChars() const {
	return int(integer(u"profile.bio_max_chars"_s, 160));
}

int ClientConfig::twoStepMinPasswordLength() const {
	return int(integer(u"auth.two_step.min_password_length"_s, 6));
}

int ClientConfig::maxMessageLength() const {
	return int(integer(u"messaging.max_message_length"_s, 4096));
}

int ClientConfig::maxCaptionLength() const {
	return int(integer(u"messaging.max_caption_length"_s, 1024));
}

int ClientConfig::maxEntities() const {
	return int(integer(u"messaging.max_entities"_s, 100));
}

int ClientConfig::maxQuoteLength() const {
	return int(integer(u"messaging.max_quote_length"_s, 1024));
}

QStringList ClientConfig::reactionEmojiSet() const {
	auto result = strings(u"messaging.reaction_emoji_set"_s);
	if (result.isEmpty()) {
		result = { u"❤️"_s, u"👍"_s, u"😂"_s, u"😮"_s, u"😢"_s, u"🙏"_s };
	}
	return result;
}

qint64 ClientConfig::editWindow() const {
	return seconds(u"messaging.edit_window"_s, 48 * 3600);
}

qint64 ClientConfig::deleteForEveryoneWindow() const {
	return seconds(u"messaging.delete_for_everyone_window"_s, 0);
}

int ClientConfig::maxPinnedChats() const {
	return int(integer(u"chat.inbox.max_pinned"_s, 5));
}

int ClientConfig::maxPinsPerChat() const {
	return int(integer(u"chat.pins.max_per_chat"_s, 100));
}

QStringList ClientConfig::muteDurations() const {
	auto result = strings(u"chat.mute.durations"_s);
	if (result.isEmpty()) {
		result = { u"1h"_s, u"8h"_s, u"2d"_s, u"forever"_s };
	}
	return result;
}

QStringList ClientConfig::liveLocationPeriods() const {
	auto result = strings(u"chat.live_location.periods"_s);
	if (result.isEmpty()) {
		result = { u"15m"_s, u"1h"_s, u"8h"_s };
	}
	return result;
}

qint64 ClientConfig::activityTtl() const {
	return seconds(u"realtime.activity_ttl"_s, 6);
}

int ClientConfig::presenceMaxSubscriptions() const {
	return int(integer(u"presence.max_subscriptions_per_connection"_s, 200));
}

int ClientConfig::cardsBatchMax() const {
	return int(std::clamp<qint64>(integer(u"profile.cards_batch_max"_s, 50), 1, 200));
}

QVector<ConversationCategory> ClientConfig::conversationCategories() const {
	auto result = QVector<ConversationCategory>();
	for (const auto &item : value(u"conversation.categories"_s).toArray()) {
		const auto object = item.toObject();
		const auto slug = object.value(u"slug"_s).toString();
		if (!slug.isEmpty()) {
			const auto title = object.value(u"title"_s).toString();
			result.push_back({ slug, title.isEmpty() ? slug : title });
		}
	}
	if (result.isEmpty()) {
		// The server's defaults, for a deployment that does not expose the list.
		result = {
			{ u"heart"_s, u"دلی"_s },
			{ u"cinema"_s, u"سینما"_s },
			{ u"study_work"_s, u"تحصیل و کار"_s },
			{ u"migration"_s, u"مهاجرت"_s },
			{ u"family"_s, u"خانواده"_s },
			{ u"sport"_s, u"ورزش"_s },
			{ u"music"_s, u"موسیقی"_s },
			{ u"books"_s, u"کتاب"_s },
			{ u"tech"_s, u"تکنولوژی"_s },
			{ u"other"_s, u"سایر"_s },
		};
	}
	return result;
}

QStringList ClientConfig::conversationDurations() const {
	auto result = strings(u"conversation.durations"_s);
	if (result.isEmpty()) {
		result = { u"1h"_s, u"6h"_s, u"24h"_s };
	}
	return result;
}

QString ClientConfig::conversationDefaultDuration() const {
	return string(u"conversation.default_duration"_s, u"24h"_s);
}

int ClientConfig::conversationTopicMaxChars() const {
	const auto result = int(integer(u"conversation.topic_max_chars"_s, 120));
	return (result > 0) ? result : 120;
}

int ClientConfig::conversationMaxConcurrent() const {
	const auto result = int(integer(u"conversation.max_concurrent_per_user"_s, 3));
	return (result > 0) ? result : 3;
}

int ClientConfig::postMaxTextChars() const {
	return int(integer(u"post.max_text_chars"_s, 2000));
}

int ClientConfig::postMaxThreadParts() const {
	return int(integer(u"post.max_thread_parts"_s, 10));
}

int ClientConfig::postMaxAlbumItems() const {
	return int(integer(u"post.max_album_items"_s, 10));
}

int ClientConfig::pollMinOptions() const {
	return int(integer(u"post.poll_min_options"_s, 2));
}

int ClientConfig::pollMaxOptions() const {
	return int(integer(u"post.poll_max_options"_s, 4));
}

qint64 ClientConfig::postEditWindow() const {
	return seconds(u"post.edit_window"_s, 15 * 60);
}

QString ClientConfig::dislikeCountVisibility() const {
	return string(u"post.dislike_count_visibility"_s, u"public"_s);
}

qint64 ClientConfig::maxUploadBytes() const {
	return integer(u"media.max_upload_bytes"_s, qint64(2) << 30);
}

qint64 ClientConfig::maxImageBytes() const {
	return integer(u"media.max_image_bytes"_s, qint64(20) << 20);
}

qint64 ClientConfig::maxVideoBytes() const {
	return integer(u"media.max_video_bytes"_s, qint64(512) << 20);
}

qint64 ClientConfig::maxAudioBytes() const {
	return integer(u"media.max_audio_bytes"_s, qint64(50) << 20);
}

qint64 ClientConfig::maxGifBytes() const {
	return integer(u"media.max_gif_bytes"_s, qint64(20) << 20);
}

qint64 ClientConfig::uploadPartSize() const {
	return integer(u"media.part_size_bytes"_s, qint64(5) << 20);
}

qint64 ClientConfig::maxVoiceDuration() const {
	return seconds(u"media.max_voice_duration"_s, 15 * 60);
}

qint64 ClientConfig::maxVideoDurationChat() const {
	return seconds(u"media.max_video_duration_chat"_s, 60 * 60);
}

qint64 ClientConfig::realtimePingInterval() const {
	return seconds(u"realtime.ping_interval"_s, 25);
}

qint64 ClientConfig::fallbackSyncInterval() const {
	const auto result = seconds(u"realtime.fallback_sync_interval"_s, 30);
	return (result > 0) ? result : 30;
}

qint64 ClientConfig::fallbackSyncAfter() const {
	return seconds(u"realtime.fallback_sync_after"_s, 30);
}

QString ClientConfig::minClientVersion(const QString &platform) const {
	auto found = value(u"api.min_client_version."_s + platform);
	if (!found.isString()) {
		found = value(u"api.min_client_version"_s).toObject().value(platform);
	}
	return found.isString() ? found.toString() : u"0.0.0"_s;
}

QString ClientConfig::updateUrl(const QString &platform) const {
	return string(u"api.update_url."_s + platform);
}

int ClientConfig::CompareVersions(const QString &a, const QString &b) {
	const auto partsA = a.split(u'.');
	const auto partsB = b.split(u'.');
	const auto count = qMax(partsA.size(), partsB.size());
	for (auto i = qsizetype(0); i != count; ++i) {
		const auto left = (i < partsA.size()) ? partsA[i].toInt() : 0;
		const auto right = (i < partsB.size()) ? partsB[i].toInt() : 0;
		if (left != right) {
			return (left < right) ? -1 : 1;
		}
	}
	return 0;
}

bool ClientConfig::updateRequired(const QString &version) const {
	return CompareVersions(version, minClientVersion()) < 0;
}

} // namespace Api
