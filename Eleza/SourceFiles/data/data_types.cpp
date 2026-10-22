#include "data/data_types.h"

#include "base/bytes.h"
#include "data/data_group_rules.h"
#include "data/data_json.h"

#include <QtCore/QRegularExpression>

namespace Data {
namespace {

using namespace Json;

} // namespace

User User::FromJson(const QJsonObject &json) {
	auto result = User();
	result.id = String(json, "id"_L1);
	result.handle = Name(json, "handle"_L1);
	result.displayName = Name(json, "display_name"_L1);
	result.avatarId = String(json, "avatar_id"_L1);
	result.verified = Bool(json, "verified"_L1);
	result.deleted = Bool(json, "deleted"_L1);
	result.isBot = Bool(json, "is_bot"_L1);
	return result;
}

QJsonObject User::toJson() const {
	auto result = QJsonObject();
	result.insert("id"_L1, id);
	Put(result, "handle"_L1, handle);
	result.insert("display_name"_L1, displayName);
	Put(result, "avatar_id"_L1, avatarId);
	Put(result, "verified"_L1, verified);
	Put(result, "deleted"_L1, deleted);
	Put(result, "is_bot"_L1, isBot);
	return result;
}

Birthday Birthday::FromJson(const QJsonObject &json) {
	auto result = Birthday();
	result.calendar = String(json, "calendar"_L1);
	result.month = Int(json, "month"_L1);
	result.day = Int(json, "day"_L1);
	result.year = Int(json, "year"_L1);
	result.today = Bool(json, "today"_L1);
	return result;
}

QJsonObject Birthday::toJson() const {
	auto result = QJsonObject();
	result.insert("calendar"_L1, calendar);
	result.insert("month"_L1, month);
	result.insert("day"_L1, day);
	Put(result, "year"_L1, year);
	Put(result, "today"_L1, today);
	return result;
}

Profile Profile::FromJson(const QJsonObject &json) {
	auto result = Profile();
	static_cast<User&>(result) = User::FromJson(json);
	result.bio = String(json, "bio"_L1);
	result.link = String(json, "link"_L1);
	result.metaLine = String(json, "meta_line"_L1);
	result.province = String(json, "province"_L1);
	result.isPrivate = Bool(json, "is_private"_L1) || Bool(json, "private"_L1);
	result.followers = Int64(json, "followers"_L1);
	result.following = Int64(json, "following"_L1);
	result.posts = Int64(json, "posts"_L1);
	result.visibility = String(json, "visibility"_L1);
	result.followState = String(json, "follow_state"_L1);
	result.followsYou = Bool(json, "follows_you"_L1);
	result.blockedByYou = Bool(json, "blocked_by_you"_L1);
	result.me = Bool(json, "me"_L1);
	result.since = Int64(json, "since"_L1);
	result.mutuals = List<User>(json.value("mutuals"_L1));
	result.mutualsTotal = Int(json, "mutuals_total"_L1);
	result.birthday = Optional<Birthday>(json, "birthday"_L1);
	result.reason = String(json, "reason"_L1);
	result.mutualCount = Int(json, "mutual_count"_L1);
	return result;
}

QJsonObject Profile::toJson() const {
	auto result = User::toJson();
	Put(result, "bio"_L1, bio);
	Put(result, "link"_L1, link);
	Put(result, "meta_line"_L1, metaLine);
	Put(result, "province"_L1, province);
	Put(result, "is_private"_L1, isPrivate);
	Put(result, "followers"_L1, followers);
	Put(result, "following"_L1, following);
	Put(result, "posts"_L1, posts);
	Put(result, "visibility"_L1, visibility);
	Put(result, "follow_state"_L1, followState);
	Put(result, "follows_you"_L1, followsYou);
	Put(result, "blocked_by_you"_L1, blockedByYou);
	Put(result, "me"_L1, me);
	Put(result, "since"_L1, since);
	Put(result, "mutuals"_L1, Array(mutuals));
	Put(result, "mutuals_total"_L1, mutualsTotal);
	if (birthday) {
		result.insert("birthday"_L1, birthday->toJson());
	}
	Put(result, "reason"_L1, reason);
	Put(result, "mutual_count"_L1, mutualCount);
	return result;
}

Participant Participant::FromJson(const QJsonObject &json) {
	auto result = Participant();
	result.ref = String(json, "ref"_L1);
	result.role = String(json, "role"_L1);
	result.masked = Bool(json, "masked"_L1);
	result.maskName = Name(json, "mask_name"_L1);
	result.maskAvatar = Int(json, "mask_avatar"_L1);
	result.userId = String(json, "user_id"_L1);
	result.me = Bool(json, "me"_L1);
	result.bot = Bool(json, "bot"_L1);
	result.deleted = Bool(json, "deleted"_L1);
	return result;
}

QJsonObject Participant::toJson() const {
	auto result = QJsonObject();
	result.insert("ref"_L1, ref);
	Put(result, "role"_L1, role);
	Put(result, "masked"_L1, masked);
	Put(result, "mask_name"_L1, maskName);
	Put(result, "mask_avatar"_L1, maskAvatar);
	Put(result, "user_id"_L1, userId);
	Put(result, "me"_L1, me);
	Put(result, "bot"_L1, bot);
	Put(result, "deleted"_L1, deleted);
	return result;
}

Conversation Conversation::FromJson(const QJsonObject &json) {
	auto result = Conversation();
	result.id = String(json, "id"_L1);
	result.topic = String(json, "topic"_L1);
	result.category = String(json, "category"_L1);
	result.state = String(json, "state"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	result.speaker = Participant::FromJson(json.value("speaker"_L1).toObject());
	// The realtime codec restores `speaker` as an always-key but never invents a listener.
	if (const auto listener = json.value("listener"_L1).toObject(); !listener.isEmpty()) {
		result.listener = Participant::FromJson(listener);
	}
	result.myRole = String(json, "my_role"_L1);
	result.readonlyUntil = Int64(json, "readonly_until"_L1);
	result.canContinue = Bool(json, "can_continue"_L1);
	result.continueRequested = Bool(json, "continue_requested"_L1);
	result.continuedInto = String(json, "continued_into"_L1);
	result.endReason = String(json, "end_reason"_L1);
	return result;
}

QJsonObject Conversation::toJson() const {
	auto result = QJsonObject();
	result.insert("id"_L1, id);
	result.insert("topic"_L1, topic);
	Put(result, "category"_L1, category);
	result.insert("state"_L1, state);
	Put(result, "expires_at"_L1, expiresAt);
	result.insert("speaker"_L1, speaker.toJson());
	if (listener) {
		result.insert("listener"_L1, listener->toJson());
	}
	Put(result, "my_role"_L1, myRole);
	Put(result, "readonly_until"_L1, readonlyUntil);
	Put(result, "can_continue"_L1, canContinue);
	Put(result, "continue_requested"_L1, continueRequested);
	Put(result, "continued_into"_L1, continuedInto);
	Put(result, "end_reason"_L1, endReason);
	return result;
}

Entity Entity::FromJson(const QJsonObject &json) {
	auto result = Entity();
	result.type = String(json, "type"_L1);
	result.offset = Int(json, "offset"_L1);
	result.length = Int(json, "length"_L1);
	result.url = String(json, "url"_L1);
	result.language = String(json, "language"_L1);
	result.user = String(json, "user"_L1);
	return result;
}

QJsonObject Entity::toJson() const {
	auto result = QJsonObject();
	result.insert("type"_L1, type);
	result.insert("offset"_L1, offset);
	result.insert("length"_L1, length);
	Put(result, "url"_L1, url);
	Put(result, "language"_L1, language);
	Put(result, "user"_L1, user);
	return result;
}

Draft Draft::FromJson(const QJsonObject &json) {
	auto result = Draft();
	result.text = String(json, "text"_L1);
	result.replyToSeq = Int64(json, "reply_to_seq"_L1);
	result.entities = List<Entity>(json.value("entities"_L1));
	result.updatedAt = Int64(json, "updated_at"_L1);
	return result;
}

QJsonObject Draft::toJson() const {
	auto result = QJsonObject();
	result.insert("text"_L1, text);
	Put(result, "reply_to_seq"_L1, replyToSeq);
	Put(result, "entities"_L1, Array(entities));
	Put(result, "updated_at"_L1, updatedAt);
	return result;
}

const Participant *ChatInfo::peer() const {
	if (conversation) {
		if (conversation->speaker.me) {
			return conversation->listener ? &*conversation->listener : nullptr;
		}
		return &conversation->speaker;
	}
	for (const auto &participant : participants) {
		if (!participant.me) {
			return &participant;
		}
	}
	return participants.isEmpty() ? nullptr : &participants.front();
}

QStringList ChatInfo::myRefs(const QString &myUserId) const {
	auto result = QStringList();
	if (!myUserId.isEmpty()) {
		result.push_back(myUserId);
	}
	for (const auto &participant : participants) {
		if (participant.me) {
			if (!participant.ref.isEmpty() && !result.contains(participant.ref)) {
				result.push_back(participant.ref);
			}
			if (!participant.userId.isEmpty() && !result.contains(participant.userId)) {
				result.push_back(participant.userId);
			}
		}
	}
	return result;
}

QStringList ChatInfo::peopleIds() const {
	auto result = QStringList();
	const auto add = [&](const QString &id) {
		if (!id.isEmpty() && !result.contains(id)) {
			result.push_back(id);
		}
	};
	for (const auto &participant : participants) {
		add(participant.userId);
	}
	if (conversation) {
		add(conversation->speaker.userId);
		if (conversation->listener) {
			add(conversation->listener->userId);
		}
	}
	return result;
}

ChatInfo ChatInfo::FromJson(const QJsonObject &json) {
	auto result = ChatInfo();
	result.chat = String(json, "chat"_L1);
	result.kind = String(json, "kind"_L1);
	result.state = String(json, "state"_L1);
	result.lastSeq = Int64(json, "last_seq"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	result.participants = List<Participant>(json.value("participants"_L1));
	result.lastReadSeq = Int64(json, "last_read_seq"_L1);
	result.peerReadSeq = Int64(json, "peer_read_seq"_L1);
	result.muted = Bool(json, "muted"_L1);
	result.mutedUntil = Int64(json, "muted_until"_L1);
	result.archived = Bool(json, "archived"_L1);
	result.request = String(json, "request"_L1);
	result.canSend = Bool(json, "can_send"_L1, true);
	result.peerDeleted = Bool(json, "peer_deleted"_L1);
	if (const auto conversation = json.value("conversation"_L1).toObject();
		!String(conversation, "id"_L1).isEmpty()) {
		result.conversation = Conversation::FromJson(conversation);
	}
	result.continuedInto = String(json, "continued_into"_L1);
	result.theme = String(json, "theme"_L1);
	result.saved = Bool(json, "saved"_L1);
	result.draft = Optional<Draft>(json, "draft"_L1);
	result.pins = Int64s(json.value("pins"_L1));
	result.liveShares = Int(json, "live_shares"_L1);
	return result;
}

QJsonObject ChatInfo::toJson() const {
	auto result = QJsonObject();
	result.insert("chat"_L1, chat);
	result.insert("kind"_L1, kind);
	Put(result, "state"_L1, state);
	Put(result, "last_seq"_L1, lastSeq);
	Put(result, "expires_at"_L1, expiresAt);
	result.insert("participants"_L1, Array(participants));
	Put(result, "last_read_seq"_L1, lastReadSeq);
	Put(result, "peer_read_seq"_L1, peerReadSeq);
	Put(result, "muted"_L1, muted);
	Put(result, "muted_until"_L1, mutedUntil);
	Put(result, "archived"_L1, archived);
	Put(result, "request"_L1, request);
	result.insert("can_send"_L1, canSend);
	Put(result, "peer_deleted"_L1, peerDeleted);
	if (conversation) {
		result.insert("conversation"_L1, conversation->toJson());
	}
	Put(result, "continued_into"_L1, continuedInto);
	Put(result, "theme"_L1, theme);
	Put(result, "saved"_L1, saved);
	if (draft) {
		result.insert("draft"_L1, draft->toJson());
	}
	Put(result, "pins"_L1, Array(pins));
	Put(result, "live_shares"_L1, liveShares);
	return result;
}

MediaMeta MediaMeta::FromJson(const QJsonObject &json) {
	auto result = MediaMeta();
	result.width = Int(json, "w"_L1);
	result.height = Int(json, "h"_L1);
	result.durationMs = Int64(json, "duration_ms"_L1);
	result.blurhash = String(json, "blurhash"_L1);
	result.mime = String(json, "mime"_L1);
	result.size = Int64(json, "size"_L1);
	result.waveform = base::bytes::fromBase64(String(json, "waveform"_L1));
	return result;
}

QJsonObject MediaMeta::toJson() const {
	auto result = QJsonObject();
	Put(result, "w"_L1, width);
	Put(result, "h"_L1, height);
	Put(result, "duration_ms"_L1, durationMs);
	Put(result, "blurhash"_L1, blurhash);
	Put(result, "mime"_L1, mime);
	Put(result, "size"_L1, size);
	if (!waveform.isEmpty()) {
		result.insert("waveform"_L1, base::bytes::toBase64(waveform));
	}
	return result;
}

Sticker Sticker::FromJson(const QJsonObject &json) {
	auto result = Sticker();
	result.id = String(json, "id"_L1);
	result.pack = String(json, "pack"_L1);
	result.emoji = String(json, "emoji"_L1);
	result.emojis = Strings(json.value("emojis"_L1));
	result.url = String(json, "url"_L1);
	result.width = Int(json, "width"_L1);
	result.height = Int(json, "height"_L1);
	return result;
}

QJsonObject Sticker::toJson() const {
	auto result = QJsonObject();
	result.insert("id"_L1, id);
	Put(result, "pack"_L1, pack);
	Put(result, "emoji"_L1, emoji);
	Put(result, "emojis"_L1, Array(emojis));
	Put(result, "url"_L1, url);
	Put(result, "width"_L1, width);
	Put(result, "height"_L1, height);
	return result;
}

LinkPreview LinkPreview::FromJson(const QJsonObject &json) {
	auto result = LinkPreview();
	result.url = String(json, "url"_L1);
	result.siteName = String(json, "site_name"_L1);
	result.title = String(json, "title"_L1);
	result.description = String(json, "description"_L1);
	result.imageUrl = String(json, "image_url"_L1);
	result.imageWidth = Int(json, "image_width"_L1);
	result.imageHeight = Int(json, "image_height"_L1);
	return result;
}

QJsonObject LinkPreview::toJson() const {
	auto result = QJsonObject();
	result.insert("url"_L1, url);
	Put(result, "site_name"_L1, siteName);
	Put(result, "title"_L1, title);
	Put(result, "description"_L1, description);
	Put(result, "image_url"_L1, imageUrl);
	Put(result, "image_width"_L1, imageWidth);
	Put(result, "image_height"_L1, imageHeight);
	return result;
}

Location Location::FromJson(const QJsonObject &json) {
	auto result = Location();
	result.lat = Double(json, "lat"_L1);
	result.lng = Double(json, "lng"_L1);
	result.accuracyM = Double(json, "accuracy_m"_L1);
	result.label = String(json, "label"_L1);
	result.address = String(json, "address"_L1);
	return result;
}

QJsonObject Location::toJson() const {
	auto result = QJsonObject();
	result.insert("lat"_L1, lat);
	result.insert("lng"_L1, lng);
	Put(result, "accuracy_m"_L1, accuracyM);
	Put(result, "label"_L1, label);
	Put(result, "address"_L1, address);
	return result;
}

LiveInfo LiveInfo::FromJson(const QJsonObject &json) {
	auto result = LiveInfo();
	result.period = Int64(json, "period"_L1);
	result.until = Int64(json, "until"_L1);
	result.stopped = Bool(json, "stopped"_L1);
	return result;
}

QJsonObject LiveInfo::toJson() const {
	auto result = QJsonObject();
	result.insert("period"_L1, double(period));
	result.insert("until"_L1, double(until));
	Put(result, "stopped"_L1, stopped);
	return result;
}

AudioMeta AudioMeta::FromJson(const QJsonObject &json) {
	auto result = AudioMeta();
	result.title = String(json, "title"_L1);
	result.performer = String(json, "performer"_L1);
	return result;
}

QJsonObject AudioMeta::toJson() const {
	auto result = QJsonObject();
	Put(result, "title"_L1, title);
	Put(result, "performer"_L1, performer);
	return result;
}

CallRecord CallRecord::FromJson(const QJsonObject &json) {
	auto result = CallRecord();
	result.outcome = String(json, "outcome"_L1);
	result.durationS = Int64(json, "duration_s"_L1);
	result.video = Bool(json, "video"_L1);
	return result;
}

QJsonObject CallRecord::toJson() const {
	auto result = QJsonObject();
	result.insert("outcome"_L1, outcome);
	Put(result, "duration_s"_L1, durationS);
	Put(result, "video"_L1, video);
	return result;
}

Quote Quote::FromJson(const QJsonObject &json) {
	auto result = Quote();
	result.text = String(json, "text"_L1);
	result.offset = Int(json, "offset"_L1);
	return result;
}

QJsonObject Quote::toJson() const {
	auto result = QJsonObject();
	result.insert("text"_L1, text);
	result.insert("offset"_L1, offset);
	return result;
}

StoryRef StoryRef::FromJson(const QJsonObject &json) {
	auto result = StoryRef();
	result.storyId = String(json, "story_id"_L1);
	result.owner = String(json, "owner"_L1);
	result.thumbUrl = String(json, "thumb_url"_L1);
	result.kind = String(json, "kind"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	result.emoji = String(json, "emoji"_L1);
	return result;
}

QJsonObject StoryRef::toJson() const {
	auto result = QJsonObject();
	result.insert("story_id"_L1, storyId);
	Put(result, "owner"_L1, owner);
	Put(result, "thumb_url"_L1, thumbUrl);
	Put(result, "kind"_L1, kind);
	Put(result, "expires_at"_L1, expiresAt);
	Put(result, "emoji"_L1, emoji);
	return result;
}

ViaBot ViaBot::FromJson(const QJsonObject &json) {
	auto result = ViaBot();
	result.id = String(json, "id"_L1);
	result.handle = String(json, "handle"_L1);
	result.name = String(json, "name"_L1);
	return result;
}

QJsonObject ViaBot::toJson() const {
	auto result = QJsonObject();
	result.insert("id"_L1, id);
	Put(result, "handle"_L1, handle);
	Put(result, "name"_L1, name);
	return result;
}

MarkupButton MarkupButton::FromJson(const QJsonObject &json) {
	auto result = MarkupButton();
	result.text = String(json, "text"_L1);
	result.kind = String(json, "kind"_L1);
	result.url = String(json, "url"_L1);
	if (json.value("query"_L1).isString()) {
		result.query = json.value("query"_L1).toString();
	}
	return result;
}

QJsonObject MarkupButton::toJson() const {
	auto result = QJsonObject();
	result.insert("text"_L1, text);
	result.insert("kind"_L1, kind);
	Put(result, "url"_L1, url);
	if (query) {
		result.insert("query"_L1, *query);
	}
	return result;
}

ReplyMarkup ReplyMarkup::FromJson(const QJsonObject &json) {
	auto result = ReplyMarkup();
	for (const auto &row : json.value("inline_keyboard"_L1).toArray()) {
		// HTTP and the realtime codec give nested lists; tolerate the wrapped rows too.
		const auto items = row.isArray() ? row : row.toObject().value("items"_L1);
		result.inlineKeyboard.push_back(List<MarkupButton>(items));
	}
	for (const auto &row : json.value("keyboard"_L1).toArray()) {
		const auto items = row.isArray() ? row.toArray() : row.toObject().value("items"_L1).toArray();
		auto texts = QStringList();
		for (const auto &item : items) {
			texts.push_back(item.isString() ? item.toString() : String(item.toObject(), "text"_L1));
		}
		result.keyboard.push_back(texts);
	}
	result.resize = Bool(json, "resize"_L1);
	result.oneTime = Bool(json, "one_time"_L1);
	result.persistent = Bool(json, "persistent"_L1);
	result.placeholder = String(json, "placeholder"_L1);
	result.removeKeyboard = Bool(json, "remove_keyboard"_L1);
	return result;
}

QJsonObject ReplyMarkup::toJson() const {
	auto result = QJsonObject();
	auto inlineRows = QJsonArray();
	for (const auto &row : inlineKeyboard) {
		inlineRows.push_back(Array(row));
	}
	Put(result, "inline_keyboard"_L1, inlineRows);
	auto rows = QJsonArray();
	for (const auto &row : keyboard) {
		auto items = QJsonArray();
		for (const auto &text : row) {
			items.push_back(QJsonObject{ { u"text"_s, text } });
		}
		rows.push_back(items);
	}
	Put(result, "keyboard"_L1, rows);
	Put(result, "resize"_L1, resize);
	Put(result, "one_time"_L1, oneTime);
	Put(result, "persistent"_L1, persistent);
	Put(result, "placeholder"_L1, placeholder);
	Put(result, "remove_keyboard"_L1, removeKeyboard);
	return result;
}

MessageKind MessageKindFromString(const QString &kind) {
	if (kind == u"text" || kind.isEmpty()) {
		return MessageKind::Text;
	} else if (kind == u"photo") {
		return MessageKind::Photo;
	} else if (kind == u"video") {
		return MessageKind::Video;
	} else if (kind == u"gif") {
		return MessageKind::Gif;
	} else if (kind == u"voice") {
		return MessageKind::Voice;
	} else if (kind == u"video_note") {
		return MessageKind::VideoNote;
	} else if (kind == u"audio") {
		return MessageKind::Audio;
	} else if (kind == u"file") {
		return MessageKind::File;
	} else if (kind == u"sticker") {
		return MessageKind::Sticker;
	} else if (kind == u"location") {
		return MessageKind::Location;
	} else if (kind == u"live_location") {
		return MessageKind::LiveLocation;
	} else if (kind == u"forwarded_post" || kind == u"post") {
		return MessageKind::ForwardedPost;
	} else if (kind == u"system") {
		return MessageKind::System;
	} else if (kind == u"call") {
		return MessageKind::Call;
	} else if (kind == u"story_reply") {
		return MessageKind::StoryReply;
	} else if (kind == u"story_reaction") {
		return MessageKind::StoryReaction;
	}
	return MessageKind::Unknown;
}

QString MessageKindToString(MessageKind kind) {
	switch (kind) {
	case MessageKind::Text: return u"text"_s;
	case MessageKind::Photo: return u"photo"_s;
	case MessageKind::Video: return u"video"_s;
	case MessageKind::Gif: return u"gif"_s;
	case MessageKind::Voice: return u"voice"_s;
	case MessageKind::VideoNote: return u"video_note"_s;
	case MessageKind::Audio: return u"audio"_s;
	case MessageKind::File: return u"file"_s;
	case MessageKind::Sticker: return u"sticker"_s;
	case MessageKind::Location: return u"location"_s;
	case MessageKind::LiveLocation: return u"live_location"_s;
	case MessageKind::ForwardedPost: return u"forwarded_post"_s;
	case MessageKind::System: return u"system"_s;
	case MessageKind::Call: return u"call"_s;
	case MessageKind::StoryReply: return u"story_reply"_s;
	case MessageKind::StoryReaction: return u"story_reaction"_s;
	case MessageKind::Unknown: return QString();
	}
	return QString();
}

bool MessageKindHasMedia(MessageKind kind) {
	switch (kind) {
	case MessageKind::Photo:
	case MessageKind::Video:
	case MessageKind::Gif:
	case MessageKind::Voice:
	case MessageKind::VideoNote:
	case MessageKind::Audio:
	case MessageKind::File:
		return true;
	default:
		return false;
	}
}

QString Message::key() const {
	return (seq > 0) ? KeyForSeq(seq) : KeyForLocal(clientMessageId);
}

QString Message::KeyForSeq(qint64 seq) {
	return u"seq:"_s + QString::number(seq);
}

QString Message::KeyForLocal(const QString &clientMessageId) {
	return u"local:"_s + clientMessageId;
}

ServiceEvent Message::serviceEvent() const {
	if (IsGroupId(chat)) {
		// The notes of a group have codes of their own («member_joined» is
		// not a listener joining a conversation).
		return (GroupRules::NoteOf(system) != GroupRules::Note::None)
			? ServiceEvent::Group
			: ServiceEvent::None;
	} else if (!text.isEmpty()) {
		return ServiceEvent::Text;
	}
	static const auto kSeparators = QRegularExpression(u"[._-]"_s);
	auto name = system.section(u':', 0, 0);
	name.remove(kSeparators);
	if (name.isEmpty()) {
		return ServiceEvent::None;
	} else if (name.contains(u"joined")) {
		return ServiceEvent::Joined;
	} else if (name.contains(u"ended") || name.contains(u"closed") || name.contains(u"expired")) {
		return ServiceEvent::Ended;
	} else if (name.contains(u"pinned")) {
		return ServiceEvent::Pinned;
	} else if (name.contains(u"theme")) {
		return ServiceEvent::ThemeChanged;
	} else if (name.contains(u"continued")) {
		return ServiceEvent::Continued;
	}
	return ServiceEvent::None;
}

bool Message::visible() const {
	if (deleted) {
		return false;
	} else if (underReview) {
		return IsGroupId(chat);
	} else if (kind == MessageKind::System) {
		return call.has_value() || serviceEvent() != ServiceEvent::None;
	}
	return true;
}

Message Message::FromJson(const QJsonObject &json) {
	auto result = Message();
	result.chat = String(json, "chat"_L1);
	result.seq = Int64(json, "seq"_L1);
	result.id = String(json, "id"_L1);
	result.sender = String(json, "sender"_L1);
	// Ids are compared with the ones this client generated (lowercase UUIDs).
	result.clientMessageId = String(json, "client_message_id"_L1).toLower();
	result.kindName = String(json, "kind"_L1);
	result.kind = MessageKindFromString(result.kindName);
	result.text = String(json, "text"_L1);
	result.replyToSeq = Int64(json, "reply_to_seq"_L1);
	result.attachment = String(json, "attachment"_L1);
	result.media = Optional<MediaMeta>(json, "media"_L1);
	result.forwardedPost = String(json, "forwarded_post"_L1);
	result.forwarded = Bool(json, "forwarded"_L1);
	result.system = String(json, "system"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.edited = Bool(json, "edited"_L1);
	result.deleted = Bool(json, "deleted"_L1);
	result.underReview = Bool(json, "under_review"_L1);
	result.played = Bool(json, "played"_L1);
	result.call = Optional<CallRecord>(json, "call"_L1);
	result.location = Optional<Location>(json, "location"_L1);
	result.live = Optional<LiveInfo>(json, "live"_L1);
	result.audio = Optional<AudioMeta>(json, "audio"_L1);
	result.reactions = CountMap(json.value("reactions"_L1));
	for (auto i = result.reactions.begin(); i != result.reactions.end();) {
		i = (i.value() <= 0) ? result.reactions.erase(i) : std::next(i);
	}
	result.myReaction = String(json, "my_reaction"_L1);
	result.entities = List<Entity>(json.value("entities"_L1));
	result.mediaSpoiler = Bool(json, "media_spoiler"_L1);
	// Every item of an album compares equal in one spelling.
	result.album = String(json, "album"_L1).toLower();
	result.captionAbove = Bool(json, "caption_above"_L1);
	result.quote = Optional<Quote>(json, "quote"_L1);
	result.linkPreview = Optional<LinkPreview>(json, "link_preview"_L1);
	result.sticker = Optional<Sticker>(json, "sticker"_L1);
	if (auto markup = Optional<ReplyMarkup>(json, "reply_markup"_L1); markup && !markup->empty()) {
		result.replyMarkup = std::move(markup);
	}
	result.viaBot = Optional<ViaBot>(json, "via_bot"_L1);
	if (auto story = Optional<StoryRef>(json, "story"_L1); story && !story->storyId.isEmpty()) {
		result.story = std::move(story);
	}
	result.senderTag = Name(json, "sender_tag"_L1);
	result.senderRole = String(json, "sender_role"_L1);
	return result;
}

QJsonObject Message::toJson() const {
	auto result = QJsonObject();
	result.insert("chat"_L1, chat);
	result.insert("seq"_L1, double(seq));
	result.insert("id"_L1, id);
	result.insert("sender"_L1, sender);
	Put(result, "client_message_id"_L1, clientMessageId);
	result.insert("kind"_L1, kindName.isEmpty() ? MessageKindToString(kind) : kindName);
	Put(result, "text"_L1, text);
	Put(result, "reply_to_seq"_L1, replyToSeq);
	Put(result, "attachment"_L1, attachment);
	if (media) {
		result.insert("media"_L1, media->toJson());
	}
	Put(result, "forwarded_post"_L1, forwardedPost);
	Put(result, "forwarded"_L1, forwarded);
	Put(result, "system"_L1, system);
	result.insert("created_at"_L1, double(createdAt));
	Put(result, "edited"_L1, edited);
	Put(result, "deleted"_L1, deleted);
	Put(result, "under_review"_L1, underReview);
	Put(result, "played"_L1, played);
	if (call) {
		result.insert("call"_L1, call->toJson());
	}
	if (location) {
		result.insert("location"_L1, location->toJson());
	}
	if (live) {
		result.insert("live"_L1, live->toJson());
	}
	if (audio) {
		result.insert("audio"_L1, audio->toJson());
	}
	Put(result, "reactions"_L1, Object(reactions));
	Put(result, "my_reaction"_L1, myReaction);
	Put(result, "entities"_L1, Array(entities));
	Put(result, "media_spoiler"_L1, mediaSpoiler);
	Put(result, "album"_L1, album);
	Put(result, "caption_above"_L1, captionAbove);
	if (quote) {
		result.insert("quote"_L1, quote->toJson());
	}
	if (linkPreview) {
		result.insert("link_preview"_L1, linkPreview->toJson());
	}
	if (sticker) {
		result.insert("sticker"_L1, sticker->toJson());
	}
	if (replyMarkup) {
		result.insert("reply_markup"_L1, replyMarkup->toJson());
	}
	if (viaBot) {
		result.insert("via_bot"_L1, viaBot->toJson());
	}
	if (story) {
		result.insert("story"_L1, story->toJson());
	}
	Put(result, "sender_tag"_L1, senderTag);
	Put(result, "sender_role"_L1, senderRole);
	return result;
}

Message Message::FromStored(const QJsonObject &json) {
	auto result = FromJson(json);
	result.mine = Bool(json, "_mine"_L1);
	const auto status = String(json, "_status"_L1);
	result.status = (status == u"read")
		? MessageStatus::Read
		: (status == u"failed")
		? MessageStatus::Failed
		: (status == u"sending")
		? MessageStatus::Sending
		: (status == u"queued")
		? MessageStatus::Queued
		: MessageStatus::Sent;
	return result;
}

QJsonObject Message::toStored() const {
	auto result = toJson();
	Put(result, "_mine"_L1, mine);
	switch (status) {
	case MessageStatus::Read: result.insert("_status"_L1, u"read"_s); break;
	case MessageStatus::Failed: result.insert("_status"_L1, u"failed"_s); break;
	case MessageStatus::Sending: result.insert("_status"_L1, u"sending"_s); break;
	case MessageStatus::Queued: result.insert("_status"_L1, u"queued"_s); break;
	case MessageStatus::Sent: break;
	}
	return result;
}

void Message::resolveLocal(const ChatInfo *info, const QString &myUserId) {
	const auto refs = info ? info->myRefs(myUserId) : QStringList{ myUserId };
	mine = !sender.isEmpty() && refs.contains(sender);
	const auto peerRead = info ? info->peerReadSeq : 0;
	status = (mine && seq > 0 && seq <= peerRead)
		? MessageStatus::Read
		: MessageStatus::Sent;
}

bool MessageLess(const Message &a, const Message &b) {
	if (a.seq > 0 && b.seq > 0) {
		return a.seq < b.seq;
	} else if (a.seq > 0) {
		return true;
	} else if (b.seq > 0) {
		return false;
	} else if (a.createdAt != b.createdAt) {
		return a.createdAt < b.createdAt;
	}
	return a.clientMessageId < b.clientMessageId;
}

InboxEntry InboxEntry::FromJson(const QJsonObject &json) {
	auto result = InboxEntry();
	result.chat = String(json, "chat"_L1);
	result.section = String(json, "section"_L1);
	result.state = String(json, "state"_L1);
	result.lastSeq = Int64(json, "last_seq"_L1);
	result.unread = Int(json, "unread"_L1);
	result.unreadCapped = Bool(json, "unread_capped"_L1);
	if (Has(json, "muted"_L1)) {
		result.muted = Bool(json, "muted"_L1);
	}
	result.mutedUntil = Int64(json, "muted_until"_L1);
	if (Has(json, "pinned"_L1)) {
		result.pinned = Bool(json, "pinned"_L1);
	}
	if (Has(json, "archived"_L1)) {
		result.archived = Bool(json, "archived"_L1);
	}
	result.timeRingExpiresAt = Int64(json, "time_ring_expires_at"_L1);
	result.lastActivityAt = Int64(json, "last_activity_at"_L1);
	if (const auto last = json.value("last_message"_L1).toObject(); Int64(last, "seq"_L1) > 0) {
		result.lastMessage = Message::FromJson(last);
	}
	if (const auto info = json.value("chat_info"_L1).toObject(); !String(info, "chat"_L1).isEmpty()) {
		result.chatInfo = ChatInfo::FromJson(info);
	}
	result.peerDeleted = Bool(json, "peer_deleted"_L1);
	if (const auto draft = json.value("draft"_L1).toObject(); !String(draft, "text"_L1).isEmpty()) {
		result.draft = Draft::FromJson(draft);
	}
	return result;
}

bool Chat::waitingForListener() const {
	return conversation
		&& !conversation->listener
		&& conversation->state == u"open";
}

Chat Chat::FromStored(const QJsonObject &json) {
	auto result = Chat();
	result.id = String(json, "id"_L1);
	result.isConversation = Bool(json, "is_conversation"_L1);
	result.section = String(json, "section"_L1);
	result.title = String(json, "title"_L1);
	result.peerUserId = String(json, "peer_user_id"_L1);
	result.peerRef = String(json, "peer_ref"_L1);
	result.anonymous = Bool(json, "anonymous"_L1);
	result.saved = Bool(json, "saved"_L1);
	result.isBot = Bool(json, "is_bot"_L1);
	result.verified = Bool(json, "verified"_L1);
	result.peerDeleted = Bool(json, "peer_deleted"_L1);
	if (json.value("last_message"_L1).isObject()) {
		result.lastMessage = Message::FromStored(json.value("last_message"_L1).toObject());
	}
	result.lastMine = Bool(json, "last_mine"_L1);
	result.lastActivityAt = Int64(json, "last_activity_at"_L1);
	result.unread = Int(json, "unread"_L1);
	result.unreadCapped = Bool(json, "unread_capped"_L1);
	result.muted = Bool(json, "muted"_L1);
	result.mutedUntil = Int64(json, "muted_until"_L1);
	result.pinned = Bool(json, "pinned"_L1);
	result.archived = Bool(json, "archived"_L1);
	result.request = String(json, "request"_L1);
	result.canSend = Bool(json, "can_send"_L1, true);
	result.state = String(json, "state"_L1);
	result.draft = Optional<Draft>(json, "draft"_L1);
	result.lastSeq = Int64(json, "last_seq"_L1);
	result.peerReadSeq = Int64(json, "peer_read_seq"_L1);
	result.lastReadSeq = Int64(json, "last_read_seq"_L1);
	result.conversation = Optional<Conversation>(json, "conversation"_L1);
	result.theme = String(json, "theme"_L1);
	result.timeRingExpiresAt = Int64(json, "time_ring_expires_at"_L1);
	result.info = ChatInfo::FromJson(json.value("info"_L1).toObject());
	if (const auto group = json.value("group"_L1); group.isObject()) {
		result.isGroup = true;
		result.group = Group::FromJson(group.toObject());
		result.groupPts = Int64(json, "group_pts"_L1, -1);
		result.groupCachePts = Int64(json, "group_cache_pts"_L1, -1);
		result.unreadMentions = Int(json, "unread_mentions"_L1);
	}
	return result;
}

QJsonObject Chat::toStored() const {
	auto result = QJsonObject();
	result.insert("id"_L1, id);
	Put(result, "is_conversation"_L1, isConversation);
	result.insert("section"_L1, section);
	Put(result, "title"_L1, title);
	Put(result, "peer_user_id"_L1, peerUserId);
	Put(result, "peer_ref"_L1, peerRef);
	Put(result, "anonymous"_L1, anonymous);
	Put(result, "saved"_L1, saved);
	Put(result, "is_bot"_L1, isBot);
	Put(result, "verified"_L1, verified);
	Put(result, "peer_deleted"_L1, peerDeleted);
	if (lastMessage && lastMessage->seq > 0) {
		result.insert("last_message"_L1, lastMessage->toStored());
	}
	Put(result, "last_mine"_L1, lastMine);
	Put(result, "last_activity_at"_L1, lastActivityAt);
	Put(result, "unread"_L1, unread);
	Put(result, "unread_capped"_L1, unreadCapped);
	Put(result, "muted"_L1, muted);
	Put(result, "muted_until"_L1, mutedUntil);
	Put(result, "pinned"_L1, pinned);
	Put(result, "archived"_L1, archived);
	Put(result, "request"_L1, request);
	result.insert("can_send"_L1, canSend);
	Put(result, "state"_L1, state);
	if (draft && !draft->empty()) {
		result.insert("draft"_L1, draft->toJson());
	}
	Put(result, "last_seq"_L1, lastSeq);
	Put(result, "peer_read_seq"_L1, peerReadSeq);
	Put(result, "last_read_seq"_L1, lastReadSeq);
	if (conversation) {
		result.insert("conversation"_L1, conversation->toJson());
	}
	Put(result, "theme"_L1, theme);
	Put(result, "time_ring_expires_at"_L1, timeRingExpiresAt);
	result.insert("info"_L1, info.toJson());
	if (isGroup && group) {
		result.insert("group"_L1, group->toJson());
		result.insert("group_pts"_L1, double(groupPts));
		result.insert("group_cache_pts"_L1, double(groupCachePts));
		Put(result, "unread_mentions"_L1, unreadMentions);
	}
	return result;
}

bool ChatLess(const Chat &a, const Chat &b) {
	if (a.pinned != b.pinned) {
		return a.pinned;
	} else if (a.lastActivityAt != b.lastActivityAt) {
		return a.lastActivityAt > b.lastActivityAt;
	}
	return a.id < b.id;
}

Presence Presence::FromJson(const QJsonObject &json) {
	auto result = Presence();
	result.user = String(json, "user"_L1);
	result.status = String(json, "status"_L1);
	result.lastSeenAt = Int64(json, "last_seen_at"_L1);
	return result;
}

PinnedMessage PinnedMessage::FromJson(const QJsonObject &json) {
	auto result = PinnedMessage();
	result.message = Message::FromJson(json.value("message"_L1).toObject());
	result.pinnedAt = Int64(json, "pinned_at"_L1);
	result.scope = String(json, "scope"_L1);
	result.pinnedBy = String(json, "pinned_by"_L1);
	if (result.message.seq <= 0) {
		// A group's pin whose message the viewer cannot see (cleared, or
		// from before they joined): only its place is known.
		result.message.seq = Int64(json, "seq"_L1);
		result.hidden = true;
	}
	return result;
}

OutboxItem OutboxItem::FromJson(const QJsonObject &json) {
	auto result = OutboxItem();
	result.clientMessageId = String(json, "client_message_id"_L1);
	result.chat = String(json, "chat"_L1);
	result.body = json.value("body"_L1).toObject();
	result.kind = String(json, "kind"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.attempts = Int(json, "attempts"_L1);
	result.filePath = String(json, "file_path"_L1);
	result.fileName = String(json, "file_name"_L1);
	result.mime = String(json, "mime"_L1);
	result.fileSize = Int64(json, "file_size"_L1);
	result.durationMs = Int64(json, "duration_ms"_L1);
	result.width = Int(json, "width"_L1);
	result.height = Int(json, "height"_L1);
	result.uploadId = String(json, "upload_id"_L1);
	result.mediaId = String(json, "media_id"_L1);
	result.failed = Bool(json, "failed"_L1);
	result.errorCode = String(json, "error_code"_L1);
	result.errorText = String(json, "error_text"_L1);
	result.previewText = String(json, "preview_text"_L1);
	return result;
}

QJsonObject OutboxItem::toJson() const {
	auto result = QJsonObject();
	result.insert("client_message_id"_L1, clientMessageId);
	result.insert("chat"_L1, chat);
	result.insert("body"_L1, body);
	result.insert("kind"_L1, kind);
	result.insert("created_at"_L1, double(createdAt));
	Put(result, "attempts"_L1, attempts);
	Put(result, "file_path"_L1, filePath);
	Put(result, "file_name"_L1, fileName);
	Put(result, "mime"_L1, mime);
	Put(result, "file_size"_L1, fileSize);
	Put(result, "duration_ms"_L1, durationMs);
	Put(result, "width"_L1, width);
	Put(result, "height"_L1, height);
	Put(result, "upload_id"_L1, uploadId);
	Put(result, "media_id"_L1, mediaId);
	Put(result, "failed"_L1, failed);
	Put(result, "error_code"_L1, errorCode);
	Put(result, "error_text"_L1, errorText);
	Put(result, "preview_text"_L1, previewText);
	return result;
}

} // namespace Data
