// Typed models of the social API («Eli» posts, feeds, notifications, the
// follow graph, topics, search, open conversations). Field names follow the
// server's handlers; see data_types.h for the conventions.
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <QtCore/QHash>

namespace Data {

struct PostCounts {
	qint64 likes = 0;
	qint64 dislikes = 0;
	bool hasDislikes = false; // the server shows the count to this viewer
	qint64 replies = 0;
	qint64 reposts = 0;
	qint64 quotes = 0;
	qint64 views = 0;

	[[nodiscard]] static PostCounts FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct PostMy {
	QString reaction;     // like | dislike | ""
	bool reposted = false;
	bool bookmarked = false;

	[[nodiscard]] static PostMy FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct PostRendition {
	int height = 0;
	int width = 0;
	qint64 bitrate = 0;
};

struct PostMedia {
	QString id;           // fetch through Data::Media (MediaRef::PostMedia(post id, id))
	QString kind;         // image | video | video_note | audio_clip | gif (comments)
	QString alt;
	int width = 0;
	int height = 0;
	qint64 durationMs = 0;
	QString blurhash;
	QVector<PostRendition> hls;

	[[nodiscard]] static PostMedia FromJson(const QJsonObject &json);
};

struct PostPoll {
	QString question;
	QStringList options;
	qint64 endsAt = 0;    // unix seconds
	bool closed = false;
	QVector<qint64> votes; // empty until the viewer may see them
	qint64 total = 0;
	int myVote = -1;      // -1: not voted

	[[nodiscard]] static PostPoll FromJson(const QJsonObject &json);
};

struct PostClip {
	QString media;
	QString source;       // record | device
	QString title;
	QString artist;
	QString cover;
	qint64 startMs = 0;
	qint64 endMs = 0;

	[[nodiscard]] static PostClip FromJson(const QJsonObject &json);
};

// The conversation a `conversation_invite` post points to.
struct PostConversation {
	QString id;
	QString state;
	QString topic;
	QString category;
	qint64 expiresAt = 0; // unix seconds

	[[nodiscard]] static PostConversation FromJson(const QJsonObject &json);
};

struct PostSound {
	QString id;
	QString kind;         // video | audio
	QString owner;
	bool usable = false;
	qint64 uses = 0;
	QString title;
	QString artist;
	QString originPost;
	QString media;
	qint64 durationMs = 0;
	bool mine = false;
	bool reuseAllowed = false;
	qint64 startMs = 0;

	[[nodiscard]] static PostSound FromJson(const QJsonObject &json);
};

struct Post {
	QString id;
	QString author;
	QString type;         // text | short_text | media | video_note | audio | poll | thread | quote | conversation_invite
	QString text;
	qint64 createdAt = 0; // ms
	bool edited = false;
	bool deleted = false;
	QString deletedBy;
	QString replyTo;
	QString threadRoot;
	int partIndex = 0;
	QVector<Post> threadParts;
	std::shared_ptr<Post> quote; // the quoted post (shared: posts nest)
	bool quoteUnavailable = false;
	QVector<PostMedia> media;
	std::optional<PostPoll> poll;
	std::optional<PostClip> clip;
	std::optional<PostConversation> conversation;
	std::optional<PostSound> sound;
	QStringList hashtags;
	QStringList mentions;
	bool pinned = false;
	QString replyPolicy;  // everyone | followers | following | mentioned
	bool canReply = true;
	bool canRepost = true;
	PostCounts counts;
	PostMy my;

	[[nodiscard]] bool valid() const {
		return !id.isEmpty();
	}
	[[nodiscard]] static Post FromJson(const QJsonObject &json);
};

struct FeedItem {
	QString postId;       // the post itself lives in Data::Feed (feed.post(id))
	QString reason;       // repost | follows | ...
	QString actor;        // who reposted / whom I follow
};

struct FeedPage {
	QVector<FeedItem> items;
	QString nextCursor;
};

struct PostsPage {
	QStringList posts;    // ids, in order
	QString nextBefore;
	QString nextCursor;
	// Items the list shows as reposts (`reason: "repost"` of a profile's
	// timeline): post id -> who reposted it.
	QHash<QString, QString> repostedBy;
};

struct RepliesPage {
	QStringList replies;  // ids, in order
	QString nextCursor;
};

struct NotificationEntry {
	QString id;
	QString kind;         // like | reply | follow | mention | conversation_joined | ...
	QStringList actors;
	int actorCount = 0;
	QMap<QString, QString> refs; // post / reply / chat ids by role
	QString tab;
	qint64 updatedAt = 0; // ms
	bool read = false;

	[[nodiscard]] static NotificationEntry FromJson(const QJsonObject &json);
};

struct GraphState {
	QString follow;       // none | requested | following
	bool followsYou = false;
	bool blockedByYou = false;

	[[nodiscard]] static GraphState FromJson(const QJsonObject &json);
};

struct GraphList {
	QVector<Profile> users;
	QString nextCursor;
};

struct TopicInfo {
	QString id;
	QString slug;
	QString title;
	qint64 postsToday = 0;
	qint64 peopleToday = 0;
	qint64 postsTotal = 0;
	bool following = false;
	bool hidden = false;
	int openConversations = 0;

	[[nodiscard]] static TopicInfo FromJson(const QJsonObject &json);
};

struct Trend {
	QString id;
	QString slug;
	QString title;
	qint64 posts = 0;
	qint64 authors = 0;
	int openConversations = 0;
	bool rising = false;

	[[nodiscard]] static Trend FromJson(const QJsonObject &json);
};

// An open anonymous conversation in the topics list.
struct OpenConversation {
	QString id;
	QString topic;
	QString category;
	qint64 expiresAt = 0; // unix seconds
	bool speakerMasked = false;
	QString maskName;
	int maskAvatar = 0;
	QString speakerId;

	[[nodiscard]] static OpenConversation FromJson(const QJsonObject &json);
};

struct OpenConversationsPage {
	QVector<OpenConversation> conversations;
	QString nextCursor;
};

struct SearchResults {
	QVector<Profile> users;
	QStringList posts;    // ids (the posts are in Data::Feed)
	QStringList topics;
	QVector<OpenConversation> conversations;
	int nextOffset = 0;   // 0: no more
};

struct Filters {
	QStringList muted;
	QStringList hidden;
	QStringList mutedWords;
	struct HiddenTopic {
		QString id;
		QString title;
		QString slug;
	};
	QVector<HiddenTopic> hiddenTopics;

	[[nodiscard]] static Filters FromJson(const QJsonObject &json);
};

struct DeviceSession {
	QString id;
	QString deviceName;
	QString platform;
	QString appVersion;
	QString city;
	bool current = false;
	qint64 lastActiveAt = 0;

	[[nodiscard]] static DeviceSession FromJson(const QJsonObject &json);
};

// Somebody I blocked in a masked conversation: only the mask name is known.
struct ContactBan {
	QString token;          // opaque handle for the unblock
	QString label;          // the mask name they had
	qint64 createdAt = 0;   // ms

	[[nodiscard]] static ContactBan FromJson(const QJsonObject &json);
};

// One page of a last-seen exception list (`kind` always | never).
struct LastSeenExceptionsPage {
	QVector<Profile> users;
	QString nextCursor;

	[[nodiscard]] static LastSeenExceptionsPage FromJson(const QJsonObject &json);
};

struct ReportReasons {
	QStringList post;
	QStringList reply;
	QStringList account;
	QStringList conversation;
	QStringList message;      // a message of a chat
	QStringList group;        // a group itself
	QStringList groupMessage; // a message of a group
	QStringList story;
};

using FailCallback = Fn<void(const Api::Error &)>;

// `fail`, invoked only while `guard` is alive (and only when it is set).
[[nodiscard]] inline FailCallback Guarded(base::Guard guard, FailCallback fail) {
	return [guard = std::move(guard), fail = std::move(fail)](const Api::Error &error) {
		if (fail && guard.alive()) {
			fail(error);
		}
	};
}

// A path segment, percent-encoded.
[[nodiscard]] QString PathSegment(const QString &value);

} // namespace Data
