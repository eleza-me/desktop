// «Eli»: feeds, posts, reactions, replies, topics, sounds, bookmarks and
// reports over HTTP, plus one shared post store so a post shown in the feed,
// on a topic page and on the detail screen is one object patched in one
// place.
//
//   feed.loadFeed(Data::Feed::Tab::ForYou, QString(), this, [=](const Data::FeedPage &page) {
//       for (const auto &item : page.items) { const auto post = feed.post(item.postId); ... }
//   });
//   connect(&feed, &Data::Feed::postChanged, this, [=](const QString &id) { ... });
//
// Optimistic actions (react, repost, bookmark) change the post at once and
// roll back on failure; their local counts are held for 3 s against server
// copies so a counts refresh racing the request does not undo them.
#pragma once

#include "api/api_client.h"
#include "base/timer.h"
#include "data/data_social_types.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

namespace Data {

class Session;

struct CreatePostMedia {
	QString id;  // uploaded media id (Data::Upload, purpose Post)
	QString alt;
};

struct CreatePost {
	QString type;                 // text | media | poll | thread | quote | ...
	QString text;
	QString replyTo;
	QString quoteOf;
	QString replyPolicy;          // everyone | followers | following | mentioned
	QVector<CreatePostMedia> media;
	QString pollQuestion;
	QStringList pollOptions;
	qint64 pollDuration = 0;      // seconds
	QString conversation;         // conversation id for an invite post
	QStringList threadParts;

	[[nodiscard]] QJsonObject toJson() const;
};

class Feed final : public QObject {
	Q_OBJECT

public:
	enum class Tab {
		ForYou,
		Following,
	};
	enum class Reaction {
		None,
		Like,
		Dislike,
	};

	explicit Feed(Session *session);

	// ---- the shared post store
	// Null when the post is not known. Valid until the post changes.
	[[nodiscard]] const Post *post(const QString &id) const;
	// Server copies in (also used by search results and profiles).
	void merge(const QVector<Post> &posts);
	// A reply was published under `id` from here: its counter moves at once
	// (a local change, kept over the server copies that are still on the way).
	void noteReplyAdded(const QString &id);
	// Parses a list of posts, stores them and returns their ids in order.
	QStringList mergeJson(const QJsonValue &list);

	// ---- feeds
	void loadFeed(Tab tab, const QString &cursor, base::Guard guard, Fn<void(const FeedPage &)> done, FailCallback fail = nullptr);
	void loadVideoFeed(const QString &cursor, base::Guard guard, Fn<void(const FeedPage &)> done, FailCallback fail = nullptr);
	// Up to three different authors who posted since `since` (ms), newest first, not me.
	void newAuthors(qint64 since, base::Guard guard, Fn<void(const QStringList &)> done);
	void newCount(qint64 since, base::Guard guard, Fn<void(int)> done);
	// «N new posts» hints over the connection (newPosts() follows); 0 stops them.
	void watchNew(qint64 since);

	// ---- posts
	void fetchPost(const QString &id, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail = nullptr);
	// `key` makes the creation idempotent across retries (keep it per draft).
	// Retried while the attached media is still processing.
	void createPost(const CreatePost &body, const QString &key, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail = nullptr);
	void editPost(const QString &id, const QString &text, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail = nullptr);
	void deletePost(const QString &id, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void react(const QString &id, Reaction reaction);
	void repost(const QString &id, bool on);
	void bookmark(const QString &id, bool on);
	// `option` < 0 withdraws the vote.
	void vote(const QString &id, int option, base::Guard guard = base::Guard(), Fn<void(const Post &)> done = nullptr, FailCallback fail = nullptr);
	void notInterested(const QString &id);
	// `done` (while `guard` lives) once the server applied it.
	void setReplyPolicy(const QString &id, const QString &policy, base::Guard guard = {}, Fn<void()> done = nullptr);
	void setPinned(const QString &id, bool pinned, base::Guard guard = {}, Fn<void()> done = nullptr);
	// https://eleza.me/s/<slug>
	void shareLink(const QString &id, base::Guard guard, Fn<void(const QString &)> done, FailCallback fail = nullptr);
	// Every share link of my post stops working (new ones can be made).
	void revokeShareLinks(const QString &id, base::Guard guard, Fn<void()> done, FailCallback fail = nullptr);
	void sharedPost(const QString &slug, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail = nullptr);
	void replies(const QString &root, const QString &cursor, base::Guard guard, Fn<void(const RepliesPage &)> done, FailCallback fail = nullptr);
	// A reply under my post (moderation) or my own reply.
	void deleteReply(const QString &root, const QString &reply, bool mine);
	// Who liked / disliked a post, newest first.
	void reactors(const QString &id, Reaction kind, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail = nullptr);

	// ---- view beacons and counts
	// Queues view beacons (once per run per post, never my own); sent every 5 s.
	void reportViews(const QStringList &ids);
	void flushViews();
	// Refreshes counts of posts whose stats are older than a minute (debounced, batched).
	void refreshCounts(const QStringList &ids);

	// ---- topics
	void topic(const QString &slug, base::Guard guard, Fn<void(const TopicInfo &)> done, FailCallback fail = nullptr);
	// tab: "top" | "new".
	void topicPosts(const QString &slug, const QString &tab, const QString &before, base::Guard guard, Fn<void(const PostsPage &)> done, FailCallback fail = nullptr);
	void topicConversations(const QString &slug, base::Guard guard, Fn<void(const QStringList &)> done);
	// `done` / `fail` while `guard` lives (a failure is reported as well).
	void followTopic(const QString &slug, bool on, base::Guard guard = {}, Fn<void()> done = nullptr, Fn<void()> fail = nullptr);
	void hideTopic(const QString &id, bool hidden, base::Guard guard = {}, Fn<void()> done = nullptr, Fn<void()> fail = nullptr);
	void trending(base::Guard guard, Fn<void(const QVector<Trend> &)> done, FailCallback fail = nullptr);

	// ---- lists
	void bookmarks(const QString &cursor, base::Guard guard, Fn<void(const PostsPage &)> done, FailCallback fail = nullptr);
	// tab: "" | "replies" | "media" ...; kind filters by post type.
	void userPosts(const QString &userId, const QString &tab, const QString &before, const QString &kind, base::Guard guard, Fn<void(const PostsPage &)> done, FailCallback fail = nullptr);
	// The posts tab with the person's reposts among their posts, newest
	// first (`with_reposts=1`, ADR-0050; PostsPage::repostedBy), paged by
	// `next_cursor`. A server without it answers the plain tab: its pages go
	// on by `next_before` (`cursorIsBefore`).
	void userTimeline(const QString &userId, const QString &cursor, bool cursorIsBefore, base::Guard guard, Fn<void(const PostsPage &)> done, FailCallback fail = nullptr);
	void sound(const QString &id, base::Guard guard, Fn<void(const PostSound &)> done, FailCallback fail = nullptr);
	void soundPosts(const QString &id, const QString &before, base::Guard guard, Fn<void(const PostsPage &)> done, FailCallback fail = nullptr);
	void updateSound(const QString &id, const QJsonObject &changes);

	// ---- reports
	void reportReasons(base::Guard guard, Fn<void(const ReportReasons &)> done, FailCallback fail = nullptr);
	// targetKind: post | reply | account | conversation | group |
	// group_message (target = the group, messageSeq = the message).
	void sendReport(const QString &targetKind, const QString &target, const QString &reason, const QString &note, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr, qint64 messageSeq = 0);

	// Mention autocomplete (`@abc`): at most five people.
	void searchUsers(const QString &query, base::Guard guard, Fn<void(const QVector<Profile> &)> done);

	// Data::Graph tells when an author was blocked (authorBlocked() follows).
	void noteAuthorBlocked(const QString &userId);

	static constexpr auto kLocalHoldMs = 3000;

Q_SIGNALS:
	void postChanged(const QString &id);
	// A post I just published (feeds insert it at the top when it is not a reply).
	void postPublished(const QString &id);
	// A post that left (deleted, not interested): lists drop the card.
	void postRemoved(const QString &id);
	// An author I blocked: lists drop every card of theirs.
	void authorBlocked(const QString &userId);
	// «N new posts» in the following feed (after watchNew()).
	void newPosts(int count, qint64 since);

private:
	void store(const Post &post, qint64 now);
	void patch(const QString &id, const Fn<void(Post &)> &change);
	void markDeleted(const QString &id);
	void applyCounts(const QJsonObject &map);
	void sendCounts();
	[[nodiscard]] FeedPage parseFeed(const QJsonObject &json, bool videoOnly);
	[[nodiscard]] PostsPage parsePosts(const QJsonObject &json);
	void simple(Api::Method method, const QString &path, const QJsonObject &body, const QString &action, const QString &subject, Fn<void()> onDone = nullptr, Fn<void()> onFail = nullptr);
	void createAttempt(const QJsonObject &body, const QString &key, int attempt, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail);

	Session *_session = nullptr;
	QHash<QString, Post> _posts;
	QHash<QString, qint64> _localUntil;
	QHash<QString, qint64> _statsAt;
	QSet<QString> _viewed;
	QStringList _viewQueue;
	base::Timer _viewTimer;
	QSet<QString> _countsQueue;
	base::Timer _countsTimer;

};

} // namespace Data
