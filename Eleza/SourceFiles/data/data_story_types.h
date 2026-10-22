// The wire shapes of stories (backend docs/stories.md) and the small rules
// that go with them: the order of the tray, which ring a circle gets, which
// story of a person to open first. Plain structs, parsed once.
#pragma once

#include "data/data_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <optional>

namespace Data {

// The fixed set of quick reactions (`stories.reactions`).
[[nodiscard]] const QStringList &StoryReactions();

enum class StoryKind {
	Image,
	Video,
	Post,
};

struct StoryMedia {
	QString url;       // a signed delivery URL (the photo's `full` or the MP4)
	QString thumbUrl;  // the photo's `thumb` or the video's `poster`
	int width = 0;
	int height = 0;
	qint64 durationMs = 0;
	QString blurhash;

	[[nodiscard]] static StoryMedia FromJson(const QJsonObject &json);
};

// A shared Eli post, as the viewer may see it now.
struct StoryPost {
	QString id;
	std::optional<User> author;
	QString textExcerpt;
	QString thumbUrl;
	bool available = false;

	[[nodiscard]] static StoryPost FromJson(const QJsonObject &json);
};

struct Story {
	QString id;
	QString owner;     // not on the wire: the user the list belongs to
	StoryKind kind = StoryKind::Image;
	std::optional<StoryMedia> media;
	std::optional<StoryPost> post;
	QString caption;
	qint64 createdAt = 0;  // unix seconds
	qint64 expiresAt = 0;  // unix seconds
	bool seen = false;
	QString myReaction;
	qint64 viewerCount = -1; // my own stories only; -1 when absent
	bool closeFriends = false; // audience "close_friends"
	bool canReply = false;
	bool canShare = false;   // I may send it to people («ارسال به…»)
	qint64 shareCount = -1;  // my own stories only; -1 when absent

	[[nodiscard]] bool valid() const {
		return !id.isEmpty();
	}
	// A photo / video made from an Eli post (both media_id and post_id):
	// «دیدن الی» opens the post.
	[[nodiscard]] bool linksPost() const {
		return (kind != StoryKind::Post)
			&& post
			&& post->available
			&& !post->id.isEmpty();
	}
	[[nodiscard]] bool expired(qint64 now) const {
		return expiresAt > 0 && now >= expiresAt;
	}
	// How long it is shown: a photo's (or a post's) duration, a video's own.
	[[nodiscard]] qint64 showMs() const;
	[[nodiscard]] static Story FromJson(const QJsonObject &json);
};

struct StoryTrayItem {
	User user;
	int count = 0;
	int unseen = 0;
	qint64 latestAt = 0;
	bool muted = false;
	bool closeFriends = false;

	[[nodiscard]] static StoryTrayItem FromJson(const QJsonObject &json);
};

struct StoryTrayMe {
	User user;
	int count = 0;
	bool hasUnseenByMe = false;
	qint64 latestAt = 0;
	bool closeFriends = false;

	[[nodiscard]] static StoryTrayMe FromJson(const QJsonObject &json);
};

struct StoryTrayPage {
	StoryTrayMe me;
	QVector<StoryTrayItem> items;
	QString nextCursor;

	[[nodiscard]] static StoryTrayPage FromJson(const QJsonObject &json);
};

struct UserStories {
	User user;
	QVector<Story> stories; // oldest -> newest

	[[nodiscard]] static UserStories FromJson(const QJsonObject &json, const QString &userId);
};

struct StoryViewer {
	User user;
	qint64 viewedAt = 0;
	QString reaction;

	[[nodiscard]] static StoryViewer FromJson(const QJsonObject &json);
};

struct StoryViewersPage {
	QVector<StoryViewer> viewers;
	QString nextCursor;
	qint64 total = 0;
	qint64 shares = -1; // how many messages sending it became; -1 when absent

	[[nodiscard]] static StoryViewersPage FromJson(const QJsonObject &json);
};

// The hide-from and close-friends lists.
struct StoryUsersPage {
	QVector<User> users;
	QString nextCursor;

	[[nodiscard]] static StoryUsersPage FromJson(const QJsonObject &json);
};

struct StorySettings {
	QString replies = QStringLiteral("everyone"); // everyone | following | off
	int hiddenCount = 0;
	int closeFriendsCount = 0;
	// People who see my stories may send them on; absent from a server
	// that cannot share stories (then the switch is not offered).
	std::optional<bool> allowSharing;

	[[nodiscard]] static StorySettings FromJson(const QJsonObject &json);
};

// ---- sending a story to people («ارسال به…», docs/stories.md §7)

// Why somebody did not get it. An unknown reason of a newer server reads as
// Failed («try again» is the safe thing to say).
enum class StoryShareReason {
	Privacy,     // no longer given by the server, kept for older answers
	NotAllowed,  // their message setting, a block, a read-only membership
	Limit,       // a message limit, slow mode: later
	Unavailable, // a gone account or group, not a member
	Failed,      // a passing failure
};
[[nodiscard]] StoryShareReason StoryShareReasonFromString(const QString &reason);

struct StoryShareSent {
	QString to;    // the person or the group
	QString chat;  // the 1:1 chat, or the group
	qint64 seq = 0;
};

struct StoryShareNotSent {
	QString to;
	StoryShareReason reason = StoryShareReason::Failed;
};

// {sent: [{to, chat, seq}], not_sent: [{to, reason}]}.
struct StoryShareResult {
	QVector<StoryShareSent> sent;
	QVector<StoryShareNotSent> notSent;

	// The reasons that came, each once, in the answer's order.
	[[nodiscard]] QVector<StoryShareReason> reasons() const;
	// The ids left out for `reason`, in the answer's order.
	[[nodiscard]] QStringList notSentFor(StoryShareReason reason) const;

	[[nodiscard]] static StoryShareResult FromJson(const QJsonObject &json);
};

// `stories.share_max_recipients`: people and groups of one share together.
inline constexpr auto kStoryShareMaxRecipients = 15;
// The configured cap, or the default when the config says nothing sensible.
[[nodiscard]] int StoryShareCap(qint64 configured);

// The body of `stories.share` (POST /v1/stories/{id}/share): the picked ids
// split into people (`to`) and groups (`groups`, ids `j…`), duplicates once,
// in the order they were picked; the message trimmed and left out when empty.
[[nodiscard]] QJsonObject StoryShareBody(
	const QString &storyId,
	const QStringList &recipients,
	const QString &text);

// What the circle around a face shows.
enum class StoryRing {
	None,
	Seen,          // grey
	Unseen,        // brand
	CloseFriends,  // amber, an unseen close-friends story
};

// The ring hints of one person: from the tray, or from the feed's author
// cards (has_story / story_unseen / story_close_friends).
struct StoryRingState {
	bool hasStory = false;
	bool unseen = false;
	bool closeFriends = false;
	bool muted = false;

	[[nodiscard]] StoryRing ring() const;
	friend bool operator==(const StoryRingState &, const StoryRingState &) = default;
};

[[nodiscard]] StoryRingState RingStateOf(const StoryTrayItem &item);
[[nodiscard]] StoryRingState RingStateOf(const StoryTrayMe &me);

// The groups of the tray: accounts with unseen stories first, then the fully
// seen ones, muted accounts last. The server orders each group (the people
// whose stories I watch most first) and that order is kept: this only moves
// an account whose group changed here (seen, muted).
void SortTray(QVector<StoryTrayItem> &items);

// Where the viewer starts in a person's stories: the first one I have not
// seen, or the first one when all are seen.
[[nodiscard]] int FirstUnseenIndex(const QVector<Story> &stories);

} // namespace Data
