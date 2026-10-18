#include "data/data_story_types.h"

#include "data/data_json.h"

#include <algorithm>

namespace Data {
namespace {

using namespace Json;

constexpr auto kImageDurationMs = qint64(5000);

[[nodiscard]] User CardOf(const QJsonValue &value) {
	return value.isObject() ? User::FromJson(value.toObject()) : User();
}

} // namespace

const QStringList &StoryReactions() {
	static const auto result = QStringList{
		u"\u2764\uFE0F"_s, // red heart
		u"\U0001F602"_s,    // tears of joy
		u"\U0001F62E"_s,    // open mouth
		u"\U0001F622"_s,    // crying
		u"\U0001F44F"_s,    // clapping
		u"\U0001F525"_s,    // fire
	};
	return result;
}

StoryMedia StoryMedia::FromJson(const QJsonObject &json) {
	auto result = StoryMedia();
	result.url = String(json, "url"_L1);
	result.thumbUrl = String(json, "thumb_url"_L1);
	result.width = Int(json, "width"_L1);
	result.height = Int(json, "height"_L1);
	result.durationMs = Int64(json, "duration_ms"_L1);
	result.blurhash = String(json, "blurhash"_L1);
	return result;
}

StoryPost StoryPost::FromJson(const QJsonObject &json) {
	auto result = StoryPost();
	result.id = String(json, "id"_L1);
	if (const auto author = CardOf(json.value("author"_L1)); author.valid()) {
		result.author = author;
	}
	result.textExcerpt = String(json, "text_excerpt"_L1);
	result.thumbUrl = String(json, "thumb_url"_L1);
	result.available = Bool(json, "available"_L1);
	if (!result.available) {
		// Only the id means something then.
		result.author = std::nullopt;
		result.textExcerpt = QString();
		result.thumbUrl = QString();
	}
	return result;
}

qint64 Story::showMs() const {
	return (media && media->durationMs > 0) ? media->durationMs : kImageDurationMs;
}

Story Story::FromJson(const QJsonObject &json) {
	auto result = Story();
	result.id = String(json, "id"_L1);
	const auto kind = String(json, "kind"_L1);
	result.kind = (kind == u"video")
		? StoryKind::Video
		: (kind == u"post")
		? StoryKind::Post
		: StoryKind::Image;
	result.media = Optional<StoryMedia>(json, "media"_L1);
	result.post = Optional<StoryPost>(json, "post"_L1);
	result.caption = String(json, "caption"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	result.seen = Bool(json, "seen"_L1);
	result.myReaction = String(json, "my_reaction"_L1);
	result.viewerCount = Has(json, "viewer_count"_L1)
		? Int64(json, "viewer_count"_L1)
		: -1;
	result.closeFriends = (String(json, "audience"_L1) == u"close_friends");
	result.canReply = Bool(json, "can_reply"_L1);
	return result;
}

StoryTrayItem StoryTrayItem::FromJson(const QJsonObject &json) {
	auto result = StoryTrayItem();
	result.user = CardOf(json.value("user"_L1));
	result.count = Int(json, "count"_L1);
	result.unseen = Int(json, "unseen"_L1);
	result.latestAt = Int64(json, "latest_at"_L1);
	result.muted = Bool(json, "muted"_L1);
	result.closeFriends = Bool(json, "close_friends"_L1);
	return result;
}

StoryTrayMe StoryTrayMe::FromJson(const QJsonObject &json) {
	auto result = StoryTrayMe();
	result.user = CardOf(json.value("user"_L1));
	result.count = Int(json, "count"_L1);
	result.hasUnseenByMe = Bool(json, "has_unseen_by_me"_L1);
	result.latestAt = Int64(json, "latest_at"_L1);
	result.closeFriends = Bool(json, "close_friends"_L1);
	return result;
}

StoryTrayPage StoryTrayPage::FromJson(const QJsonObject &json) {
	auto result = StoryTrayPage();
	result.me = StoryTrayMe::FromJson(json.value("me"_L1).toObject());
	for (const auto &value : json.value("items"_L1).toArray()) {
		auto item = StoryTrayItem::FromJson(value.toObject());
		if (item.user.valid() && !item.user.deleted && item.count > 0) {
			result.items.push_back(std::move(item));
		}
	}
	result.nextCursor = String(json, "next_cursor"_L1);
	return result;
}

UserStories UserStories::FromJson(const QJsonObject &json, const QString &userId) {
	auto result = UserStories();
	result.user = CardOf(json.value("user"_L1));
	if (result.user.id.isEmpty()) {
		result.user.id = userId;
	}
	for (const auto &value : json.value("stories"_L1).toArray()) {
		auto story = Story::FromJson(value.toObject());
		if (story.valid()) {
			story.owner = result.user.id;
			result.stories.push_back(std::move(story));
		}
	}
	// Oldest first, whatever order arrived.
	std::stable_sort(result.stories.begin(), result.stories.end(), [](
			const Story &a,
			const Story &b) {
		return a.createdAt < b.createdAt;
	});
	return result;
}

StoryViewer StoryViewer::FromJson(const QJsonObject &json) {
	auto result = StoryViewer();
	result.user = CardOf(json.value("user"_L1));
	result.viewedAt = Int64(json, "viewed_at"_L1);
	result.reaction = String(json, "reaction"_L1);
	return result;
}

StoryViewersPage StoryViewersPage::FromJson(const QJsonObject &json) {
	auto result = StoryViewersPage();
	for (const auto &value : json.value("viewers"_L1).toArray()) {
		auto viewer = StoryViewer::FromJson(value.toObject());
		if (viewer.user.valid()) {
			result.viewers.push_back(std::move(viewer));
		}
	}
	result.nextCursor = String(json, "next_cursor"_L1);
	result.total = Int64(json, "total"_L1);
	return result;
}

StoryUsersPage StoryUsersPage::FromJson(const QJsonObject &json) {
	auto result = StoryUsersPage();
	for (const auto &value : json.value("users"_L1).toArray()) {
		auto user = CardOf(value);
		if (user.valid()) {
			result.users.push_back(std::move(user));
		}
	}
	result.nextCursor = String(json, "next_cursor"_L1);
	return result;
}

StorySettings StorySettings::FromJson(const QJsonObject &json) {
	auto result = StorySettings();
	const auto replies = String(json, "replies"_L1);
	result.replies = (replies == u"following" || replies == u"off")
		? replies
		: u"everyone"_s;
	result.hiddenCount = Int(json, "hidden_count"_L1);
	result.closeFriendsCount = Int(json, "close_friends_count"_L1);
	return result;
}

StoryRing StoryRingState::ring() const {
	if (!hasStory) {
		return StoryRing::None;
	} else if (!unseen || muted) {
		return StoryRing::Seen;
	}
	return closeFriends ? StoryRing::CloseFriends : StoryRing::Unseen;
}

StoryRingState RingStateOf(const StoryTrayItem &item) {
	auto result = StoryRingState();
	result.hasStory = (item.count > 0);
	result.unseen = (item.unseen > 0);
	result.closeFriends = item.closeFriends;
	result.muted = item.muted;
	return result;
}

StoryRingState RingStateOf(const StoryTrayMe &me) {
	auto result = StoryRingState();
	result.hasStory = (me.count > 0);
	result.unseen = me.hasUnseenByMe;
	result.closeFriends = me.closeFriends;
	return result;
}

void SortTray(QVector<StoryTrayItem> &items) {
	const auto group = [](const StoryTrayItem &item) {
		return item.muted ? 2 : (item.unseen > 0) ? 0 : 1;
	};
	std::stable_sort(items.begin(), items.end(), [&](
			const StoryTrayItem &a,
			const StoryTrayItem &b) {
		const auto ga = group(a);
		const auto gb = group(b);
		return (ga != gb) ? (ga < gb) : (a.latestAt > b.latestAt);
	});
}

int FirstUnseenIndex(const QVector<Story> &stories) {
	for (auto i = 0; i != int(stories.size()); ++i) {
		if (!stories[i].seen) {
			return i;
		}
	}
	return 0;
}

} // namespace Data
