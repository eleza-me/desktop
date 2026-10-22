// Stories (backend docs/stories.md): the tray of the people I follow, the
// stories of one person, one story by id (sent in a chat), seen marks,
// reactions, replies, sending a story to people, my viewers, mute /
// hide-from / close friends / story settings, and posting a story from a file.
// Reads and actions go over the realtime connection (`stories.*` methods,
// HTTP when it is down); the media comes through Data::Media
// (MediaRef::Story, which also answers a fresh URL when a signed one expired).
//
//   auto &stories = session.data().stories();
//   stories.refreshTray();
//   connect(&stories, &Data::Stories::trayChanged, this, [=] { ... stories.tray() ... });
//   stories.loadUser(userId, this, [=](const Data::UserStories &list) { ... });
//   stories.markSeen(story);     // batched: sent every few seconds and by flushSeen()
//
// Rings: ringState(userId) answers from the tray, the person's loaded stories
// and the hints of the feed's author cards (has_story / story_unseen /
// story_close_friends), in that order; ringsChanged() tells whom to repaint.
// The realtime `story` signal refreshes the tray (debounced).
#pragma once

#include "base/timer.h"
#include "data/data_social_types.h"
#include "data/data_story_types.h"
#include "data/data_upload.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

#include <map>

namespace Data {

class Session;

struct CreateStory {
	QString mediaId;  // an upload of purpose Story
	QString postId;   // or an Eli post to share
	QString caption;  // <= 200 characters
	bool closeFriends = false;

	[[nodiscard]] QJsonObject toJson() const;
};

struct PostStoryRequest {
	QString filePath;
	bool video = false;
	QString caption;
	bool closeFriends = false;
	// An Eli post the photo / video was made from: the story carries the
	// post's card too («دیدن الی»).
	QString postId;
};

// My story on its way (upload, processing, create).
struct StoryPosting {
	bool active = false;
	bool failed = false;
	double progress = 0.;   // 0..1
	QString filePath;       // a picture to show while posting
	bool video = false;
	Api::Error error;
};

class Stories final : public QObject {
	Q_OBJECT

public:
	explicit Stories(Session *session);
	~Stories();

	// client-config `feature.stories` (absent: on).
	[[nodiscard]] bool enabled() const;

	// ---- the tray
	[[nodiscard]] const StoryTrayMe &me() const {
		return _me;
	}
	[[nodiscard]] const QVector<StoryTrayItem> &tray() const {
		return _tray;
	}
	[[nodiscard]] bool trayLoaded() const {
		return _trayLoaded;
	}
	[[nodiscard]] bool trayHasMore() const {
		return !_trayCursor.isEmpty();
	}
	// Reads the first page again (the order and the counts may change).
	void refreshTray();
	void loadMoreTray();
	// The people of the tray in its order (the viewer goes on to the next one).
	[[nodiscard]] QStringList trayUserIds() const;
	// Applies an answer of `stories.tray` (also what the tests feed).
	void applyTray(const StoryTrayPage &page, bool first);

	// ---- rings
	[[nodiscard]] StoryRingState ringState(const QString &userId) const;
	[[nodiscard]] StoryRing ring(const QString &userId) const {
		return ringState(userId).ring();
	}
	// The feed's author cards (a list or an id -> card map).
	void applyAuthorHints(const QJsonValue &authors);

	// ---- the stories of one person
	// Null until loaded once.
	[[nodiscard]] const UserStories *cached(const QString &userId) const;
	[[nodiscard]] const Story *story(const QString &storyId) const;
	void loadUser(
		const QString &userId,
		base::Guard guard,
		Fn<void(const UserStories &)> done,
		FailCallback fail = nullptr);
	void applyUserStories(const UserStories &list);

	// ---- seen
	// Marks it seen here at once and queues the mark for the server.
	void markSeen(const QString &storyId);
	// Sends what is queued now (the viewer closed, the session ends).
	void flushSeen();
	[[nodiscard]] QStringList queuedSeen() const {
		return _seenQueue;
	}

	// ---- reactions and replies
	void react(const QString &storyId, const QString &emoji, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void unreact(const QString &storyId, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void reply(const QString &storyId, const QString &text, base::Guard guard, Fn<void(const QString &chat, qint64 seq)> done, FailCallback fail = nullptr);

	// ---- sending a story to people («ارسال به…»)
	// `recipients`: user ids (mine is Saved Messages) and group ids, at most
	// shareCap() together. One of my own stories counts the messages it
	// became (shareCount).
	void share(const QString &storyId, const QStringList &recipients, const QString &text, base::Guard guard, Fn<void(const StoryShareResult &)> done, FailCallback fail = nullptr);
	// client-config `stories.share_max_recipients`.
	[[nodiscard]] int shareCap() const;

	// ---- one story by id (a story sent in a chat), decided for me
	// `story.not_found` (404) when it expired, was deleted or is not shown to
	// me; the story is then remembered as gone (storyGone).
	void get(const QString &storyId, base::Guard guard, Fn<void(const User &owner, const Story &story)> done, FailCallback fail = nullptr);
	// Stories found gone in this session: a 404 of the story or of its
	// thumbnail. Chat messages that point at one draw it so.
	void markGone(const QString &storyId);
	[[nodiscard]] bool isGone(const QString &storyId) const {
		return _gone.contains(storyId);
	}

	// ---- my stories
	void viewers(const QString &storyId, const QString &cursor, base::Guard guard, Fn<void(const StoryViewersPage &)> done, FailCallback fail = nullptr);
	void deleteStory(const QString &storyId, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	// Retried while the uploaded media is still processing.
	void create(const CreateStory &request, base::Guard guard, Fn<void(const Story &)> done, FailCallback fail = nullptr);

	// Uploads a photo / video (purpose story) and creates the story; the
	// state is posting(), the end posted() or postingChanged() with failed.
	// False when another story is on its way.
	bool post(const PostStoryRequest &request);
	void retryPosting();
	void cancelPosting();
	[[nodiscard]] const StoryPosting &posting() const {
		return _posting;
	}

	// ---- mute, hide-from, close friends, settings
	void setMuted(const QString &userId, bool muted, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void hidden(const QString &cursor, base::Guard guard, Fn<void(const StoryUsersPage &)> done, FailCallback fail = nullptr);
	void setHidden(const QString &userId, bool hidden, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void closeFriends(const QString &cursor, base::Guard guard, Fn<void(const StoryUsersPage &)> done, FailCallback fail = nullptr);
	void setCloseFriend(const QString &userId, bool on, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	// At most `stories.close_friends_bulk_max` ids together (split by the caller).
	void updateCloseFriends(const QStringList &add, const QStringList &remove, base::Guard guard, Fn<void()> done, FailCallback fail = nullptr);
	void settings(base::Guard guard, Fn<void(const StorySettings &)> done, FailCallback fail = nullptr);
	// replies: everyone | following | off.
	void updateSettings(const QString &replies, base::Guard guard, Fn<void(const StorySettings &)> done, FailCallback fail = nullptr);
	void setAllowSharing(bool allow, base::Guard guard, Fn<void(const StorySettings &)> done, FailCallback fail = nullptr);
	[[nodiscard]] const std::optional<StorySettings> &cachedSettings() const {
		return _settings;
	}

	static constexpr auto kSeenFlushMs = 3000;
	static constexpr auto kSeenBatch = 100;
	static constexpr auto kSignalDebounceMs = 2000;
	static constexpr auto kCaptionMax = 200;

Q_SIGNALS:
	void enabledChanged();
	void trayChanged();
	void ringsChanged(const QStringList &userIds);
	// The loaded stories of a person changed (seen, reaction, deleted, new).
	void userStoriesChanged(const QString &userId);
	void postingChanged();
	void posted(const Data::Story &story);
	void storyGone(const QString &storyId);

private:
	struct UserEntry {
		UserStories data;
		bool fresh = true; // false after a `story` signal named them
	};

	void handleSignal(const QString &userId);
	void patchStory(const QString &storyId, const Fn<void(Story &)> &change);
	[[nodiscard]] StoryTrayItem *trayItem(const QString &userId);
	void refreshRingOf(const QString &userId);
	void emitRings(const QStringList &ids);
	void sendSeen(const QStringList &ids);
	void createAttempt(const QJsonObject &body, const QString &key, int attempt, base::Guard guard, Fn<void(const Story &)> done, FailCallback fail);
	void startPosting();
	void addMine(const Story &story);
	void simple(const QString &method, const QJsonObject &body, const QString &action, const QString &subject, base::Guard guard, Fn<void()> done, FailCallback fail);
	void usersPage(const QString &method, const QString &cursor, base::Guard guard, Fn<void(const StoryUsersPage &)> done, FailCallback fail);
	void settingsUpdate(const QJsonObject &body, base::Guard guard, Fn<void(const StorySettings &)> done, FailCallback fail);
	void removeStory(const QString &storyId);

	Session *_session = nullptr;
	bool _wasEnabled = true;

	StoryTrayMe _me;
	QVector<StoryTrayItem> _tray;
	QString _trayCursor;
	bool _trayLoaded = false;
	bool _trayLoading = false;
	int _trayRequest = 0;

	QHash<QString, StoryRingState> _hints;
	QHash<QString, StoryRingState> _rings; // what was told last (ringsChanged)
	std::map<QString, UserEntry> _users;

	QStringList _seenQueue;
	QSet<QString> _seenSent;
	base::Timer _seenTimer;
	base::Timer _signalTimer;
	QSet<QString> _signalUsers;

	StoryPosting _posting;
	PostStoryRequest _postingRequest;
	QString _postingMediaId;
	QString _postingKey;
	Upload::Id _postingUpload = 0;
	int _postingGeneration = 0;

	std::optional<StorySettings> _settings;
	QSet<QString> _gone;

};

} // namespace Data
