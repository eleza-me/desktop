#include "data/data_social_types.h"

#include "data/data_json.h"

#include <QtCore/QDateTime>
#include <QtCore/QUrl>

namespace Data {
namespace {

using namespace Json;

} // namespace

QString PathSegment(const QString &value) {
	return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

PostCounts PostCounts::FromJson(const QJsonObject &json) {
	auto result = PostCounts();
	result.likes = Int64(json, "likes"_L1);
	result.hasDislikes = Has(json, "dislikes"_L1);
	result.dislikes = Int64(json, "dislikes"_L1);
	result.replies = Int64(json, "replies"_L1);
	result.reposts = Int64(json, "reposts"_L1);
	result.quotes = Int64(json, "quotes"_L1);
	result.views = Int64(json, "views"_L1);
	return result;
}

QJsonObject PostCounts::toJson() const {
	auto result = QJsonObject{
		{ u"likes"_s, double(likes) },
		{ u"replies"_s, double(replies) },
		{ u"reposts"_s, double(reposts) },
		{ u"quotes"_s, double(quotes) },
		{ u"views"_s, double(views) },
	};
	if (hasDislikes) {
		result.insert("dislikes"_L1, double(dislikes));
	}
	return result;
}

PostMy PostMy::FromJson(const QJsonObject &json) {
	auto result = PostMy();
	result.reaction = String(json, "reaction"_L1);
	if (result.reaction == u"none") {
		result.reaction = QString();
	}
	result.reposted = Bool(json, "reposted"_L1);
	result.bookmarked = Bool(json, "bookmarked"_L1);
	return result;
}

QJsonObject PostMy::toJson() const {
	auto result = QJsonObject();
	Put(result, "reaction"_L1, reaction);
	Put(result, "reposted"_L1, reposted);
	Put(result, "bookmarked"_L1, bookmarked);
	return result;
}

PostMedia PostMedia::FromJson(const QJsonObject &json) {
	auto result = PostMedia();
	result.id = String(json, "id"_L1);
	result.kind = String(json, "kind"_L1);
	result.alt = String(json, "alt"_L1);
	result.width = Int(json, "width"_L1);
	result.height = Int(json, "height"_L1);
	result.durationMs = Int64(json, "duration_ms"_L1);
	result.blurhash = String(json, "blurhash"_L1);
	for (const auto &item : json.value("hls"_L1).toObject().value("renditions"_L1).toArray()) {
		const auto object = item.toObject();
		result.hls.push_back({
			Int(object, "height"_L1),
			Int(object, "width"_L1),
			Int64(object, "bitrate"_L1),
		});
	}
	return result;
}

PostPoll PostPoll::FromJson(const QJsonObject &json) {
	auto result = PostPoll();
	result.question = String(json, "question"_L1);
	result.options = Strings(json.value("options"_L1));
	result.endsAt = Int64(json, "ends_at"_L1);
	result.closed = Bool(json, "closed"_L1);
	result.votes = Int64s(json.value("votes"_L1));
	result.total = Int64(json, "total"_L1);
	result.myVote = Has(json, "my_vote"_L1) ? Int(json, "my_vote"_L1, -1) : -1;
	return result;
}

PostClip PostClip::FromJson(const QJsonObject &json) {
	auto result = PostClip();
	result.media = String(json, "media"_L1);
	result.source = String(json, "source"_L1);
	result.title = String(json, "title"_L1);
	result.artist = String(json, "artist"_L1);
	result.cover = String(json, "cover"_L1);
	result.startMs = Int64(json, "start_ms"_L1);
	result.endMs = Int64(json, "end_ms"_L1);
	return result;
}

PostConversation PostConversation::FromJson(const QJsonObject &json) {
	auto result = PostConversation();
	result.id = String(json, "id"_L1);
	result.state = String(json, "state"_L1);
	result.topic = String(json, "topic"_L1);
	result.category = String(json, "category"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	return result;
}

PostSound PostSound::FromJson(const QJsonObject &json) {
	auto result = PostSound();
	result.id = String(json, "id"_L1);
	result.kind = String(json, "kind"_L1);
	result.owner = String(json, "owner"_L1);
	result.usable = Bool(json, "usable"_L1);
	result.uses = Int64(json, "uses"_L1);
	result.title = String(json, "title"_L1);
	result.artist = String(json, "artist"_L1);
	result.originPost = String(json, "origin_post"_L1);
	result.media = String(json, "media"_L1);
	result.durationMs = Int64(json, "duration_ms"_L1);
	result.mine = Bool(json, "mine"_L1);
	result.reuseAllowed = Bool(json, "reuse_allowed"_L1);
	result.startMs = Int64(json, "start_ms"_L1);
	return result;
}

Post Post::FromJson(const QJsonObject &json) {
	auto result = Post();
	result.id = String(json, "id"_L1);
	result.author = String(json, "author"_L1);
	result.type = String(json, "type"_L1);
	result.text = String(json, "text"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.edited = Bool(json, "edited"_L1);
	result.deleted = Bool(json, "deleted"_L1);
	result.deletedBy = String(json, "deleted_by"_L1);
	result.replyTo = String(json, "reply_to"_L1);
	result.threadRoot = String(json, "thread_root"_L1);
	result.partIndex = Int(json, "part_index"_L1);
	result.threadParts = List<Post>(json.value("thread_parts"_L1));
	if (const auto quote = json.value("quote"_L1).toObject(); !String(quote, "id"_L1).isEmpty()) {
		result.quote = std::make_shared<Post>(Post::FromJson(quote));
	}
	result.quoteUnavailable = Bool(json, "quote_unavailable"_L1);
	result.media = List<PostMedia>(json.value("media"_L1));
	result.poll = Optional<PostPoll>(json, "poll"_L1);
	result.clip = Optional<PostClip>(json, "clip"_L1);
	result.conversation = Optional<PostConversation>(json, "conversation"_L1);
	result.sound = Optional<PostSound>(json, "sound"_L1);
	result.hashtags = Strings(json.value("hashtags"_L1));
	result.mentions = Strings(json.value("mentions"_L1));
	result.pinned = Bool(json, "pinned"_L1);
	result.replyPolicy = String(json, "reply_policy"_L1);
	if (result.replyPolicy.isEmpty()) {
		result.replyPolicy = u"everyone"_s;
	}
	result.canReply = Bool(json, "can_reply"_L1, true);
	result.canRepost = Bool(json, "can_repost"_L1, true);
	result.counts = PostCounts::FromJson(json.value("counts"_L1).toObject());
	result.my = PostMy::FromJson(json.value("my"_L1).toObject());
	return result;
}

NotificationEntry NotificationEntry::FromJson(const QJsonObject &json) {
	auto result = NotificationEntry();
	result.id = String(json, "id"_L1);
	result.kind = String(json, "kind"_L1);
	result.actors = Strings(json.value("actors"_L1));
	result.actorCount = Int(json, "actor_count"_L1);
	result.refs = StringMap(json.value("refs"_L1));
	result.tab = String(json, "tab"_L1);
	result.updatedAt = Int64(json, "updated_at"_L1);
	result.read = Bool(json, "read"_L1);
	return result;
}

GraphState GraphState::FromJson(const QJsonObject &json) {
	auto result = GraphState();
	result.follow = String(json, "follow"_L1);
	if (result.follow.isEmpty()) {
		result.follow = u"none"_s;
	}
	result.followsYou = Bool(json, "follows_you"_L1);
	result.blockedByYou = Bool(json, "blocked_by_you"_L1);
	return result;
}

TopicInfo TopicInfo::FromJson(const QJsonObject &json) {
	auto result = TopicInfo();
	result.id = String(json, "id"_L1);
	result.slug = String(json, "slug"_L1);
	result.title = String(json, "title"_L1);
	result.postsToday = Int64(json, "posts_today"_L1);
	result.peopleToday = Int64(json, "people_today"_L1);
	result.postsTotal = Int64(json, "posts_total"_L1);
	result.following = Bool(json, "following"_L1);
	result.hidden = Bool(json, "hidden"_L1);
	result.openConversations = Int(json, "open_conversations"_L1);
	return result;
}

Trend Trend::FromJson(const QJsonObject &json) {
	auto result = Trend();
	result.id = String(json, "id"_L1);
	result.slug = String(json, "slug"_L1);
	result.title = String(json, "title"_L1);
	result.posts = Int64(json, "posts"_L1);
	result.authors = Int64(json, "authors"_L1);
	result.openConversations = Int(json, "open_conversations"_L1);
	result.rising = Bool(json, "rising"_L1);
	return result;
}

OpenConversation OpenConversation::FromJson(const QJsonObject &json) {
	auto result = OpenConversation();
	result.id = String(json, "id"_L1);
	result.topic = String(json, "topic"_L1);
	result.category = String(json, "category"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	// Search results name the flag `masked`.
	result.speakerMasked = Bool(json, "speaker_masked"_L1) || Bool(json, "masked"_L1);
	result.maskName = String(json, "mask_name"_L1);
	result.maskAvatar = Int(json, "mask_avatar"_L1);
	result.speakerId = String(json, "speaker_id"_L1);
	return result;
}

Filters Filters::FromJson(const QJsonObject &json) {
	auto result = Filters();
	result.muted = Strings(json.value("muted"_L1));
	result.hidden = Strings(json.value("hidden"_L1));
	result.mutedWords = Strings(json.value("muted_words"_L1));
	for (const auto &item : json.value("hidden_topics"_L1).toArray()) {
		auto topic = HiddenTopic();
		if (item.isString()) {
			topic.id = item.toString();
		} else {
			const auto object = item.toObject();
			topic.id = String(object, "id"_L1);
			topic.title = String(object, "title"_L1);
			topic.slug = String(object, "slug"_L1);
		}
		if (!topic.id.isEmpty()) {
			result.hiddenTopics.push_back(topic);
		}
	}
	return result;
}

ContactBan ContactBan::FromJson(const QJsonObject &json) {
	auto result = ContactBan();
	result.token = String(json, "token"_L1);
	result.label = Name(json, "label"_L1);
	const auto created = json.value("created_at"_L1);
	if (created.isString()) {
		// An RFC 3339 string (a Go time).
		const auto time = QDateTime::fromString(created.toString(), Qt::ISODateWithMs);
		result.createdAt = time.isValid() ? time.toMSecsSinceEpoch() : 0;
	} else {
		result.createdAt = ToMs(Int64(json, "created_at"_L1));
	}
	return result;
}

LastSeenExceptionsPage LastSeenExceptionsPage::FromJson(const QJsonObject &json) {
	auto result = LastSeenExceptionsPage();
	for (const auto &item : json.value("items"_L1).toArray()) {
		const auto user = Profile::FromJson(item.toObject().value("user"_L1).toObject());
		if (!user.id.isEmpty()) {
			result.users.push_back(user);
		}
	}
	result.nextCursor = String(json, "next_cursor"_L1);
	return result;
}

DeviceSession DeviceSession::FromJson(const QJsonObject &json) {
	auto result = DeviceSession();
	result.id = String(json, "session_id"_L1);
	if (result.id.isEmpty()) {
		result.id = String(json, "id"_L1);
	}
	result.deviceName = String(json, "device_name"_L1);
	result.platform = String(json, "platform"_L1);
	result.appVersion = String(json, "app_version"_L1);
	result.city = String(json, "city"_L1);
	result.current = Bool(json, "current"_L1);
	result.lastActiveAt = Int64(json, "last_active_at"_L1);
	if (result.lastActiveAt <= 0) {
		// The server names it `last_seen_at` and sends an RFC 3339 string.
		const auto seen = json.value("last_seen_at"_L1);
		if (seen.isString()) {
			const auto time = QDateTime::fromString(
				seen.toString(),
				Qt::ISODateWithMs);
			if (time.isValid()) {
				result.lastActiveAt = time.toMSecsSinceEpoch();
			}
		} else if (seen.isDouble()) {
			result.lastActiveAt = base::SafeInt64(seen.toDouble());
		}
	}
	return result;
}

} // namespace Data
