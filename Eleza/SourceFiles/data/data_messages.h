// Message timelines. One Data::History per chat: the window of messages the
// chat screen shows (oldest first), local pending messages at its end, and
// the chat's header as last seen.
//
//   const auto &history = messages.history(chatId);     // never null; empty until loaded
//   messages.loadCached(chatId, this, [=](int count) { ... });   // instant, from the database
//   messages.loadLatest(chatId, this, [=] { ... });               // then the server
//   messages.loadOlder(chatId, this, [=](int added) { ... });     // scrolling up
//   connect(&messages, &Data::Messages::historyChanged, this, [=](const QString &chat) { ... });
//
// Windows: normally the window ends at the newest message and live updates
// extend it. `loadAround(seq)` (jump to a reply / pinned message) replaces it
// with a window around that message (`hasMoreAfter` becomes true);
// `loadNewer` walks forward and `loadLatest` returns to the end.
//
// Sending lives in Data::Outbox; this store applies what the server reports.
//
// A group (an id that starts with "j") has the same timeline; its pages and
// actions go to the group routes (`groups.history`, `groups.read`, …), and
// what changes in it arrives through Data::Groups.
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <map>

namespace Data {

class Session;

struct History {
	QString chat;
	// Oldest first. Server messages (seq > 0) by seq, then local pending ones.
	QVector<Message> items;
	bool loaded = false;        // the newest page came from the server in this run
	bool hasMoreBefore = true;
	bool hasMoreAfter = false;  // the window does not reach the newest message
	bool stale = false;         // may hold messages that are gone: read again
	std::optional<ChatInfo> info;
	QVector<PinnedMessage> pins; // after loadPins()
	bool pinsLoaded = false;

	[[nodiscard]] qint64 firstSeq() const;
	[[nodiscard]] qint64 lastSeq() const;
	[[nodiscard]] int indexOfKey(const QString &key) const;
	[[nodiscard]] int indexOfSeq(qint64 seq) const;
	[[nodiscard]] const Message *find(qint64 seq) const;
};

// The unread count of a row after my read mark moved to `readSeq` (here or
// on another device). Read to the end: none. Otherwise the messages above
// the mark that are not mine, counted in the timeline when it holds all of
// them, or bounded by the seqs above the mark when it does not — never more
// than the row had (a read does not add unread messages), unless the row's
// count was only a lower bound (capped).
struct UnreadCount {
	int count = 0;
	bool capped = false;
};
[[nodiscard]] UnreadCount UnreadAfterRead(
	const Chat &row,
	const History *history,
	qint64 readSeq);

struct CallbackAnswer {
	bool answered = false;
	QString text;
	bool alert = false;
	QString url;
};

struct LiveShare {
	qint64 seq = 0;
	QString sender;
	qint64 until = 0;  // unix seconds
	std::optional<Location> location;
	qint64 at = 0;     // ms
};

class Messages final : public QObject {
	Q_OBJECT

public:
	explicit Messages(Session *session);

	static constexpr auto kPageSize = 50;
	static constexpr auto kCachedPageSize = 60;

	// The chat's timeline (created empty on first use). The reference stays
	// valid until the chat is removed.
	[[nodiscard]] const History &history(const QString &chat);
	[[nodiscard]] const Message *find(const QString &chat, qint64 seq) const;
	// The chat has a timeline in memory (shown now or earlier in this run).
	[[nodiscard]] bool held(const QString &chat) const;
	// …and it reaches the newest message: live changes extend it.
	[[nodiscard]] bool live(const QString &chat) const;

	// ---- loading
	// The newest messages the database remembers.
	void loadCached(const QString &chat, base::Guard guard, Fn<void(int)> done);
	// The newest page from the server (also returns a detached window to the end).
	void loadLatest(
		const QString &chat,
		base::Guard guard = base::Guard(),
		Fn<void()> done = nullptr,
		Fn<void(const Api::Error &)> fail = nullptr);
	// The page before the window: the database first, then the server.
	void loadOlder(
		const QString &chat,
		base::Guard guard,
		Fn<void(int)> done,
		Fn<void(const Api::Error &)> fail = nullptr);
	// The page after a detached window.
	void loadNewer(
		const QString &chat,
		base::Guard guard,
		Fn<void(int)> done,
		Fn<void(const Api::Error &)> fail = nullptr);
	// Replaces the window with one around `seq`.
	void loadAround(
		const QString &chat,
		qint64 seq,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail = nullptr);
	// One message that may be outside the window (a reply's target). It is
	// merged into the window only when it belongs inside it.
	void fetchOne(
		const QString &chat,
		qint64 seq,
		base::Guard guard,
		Fn<void(const std::optional<Message> &)> done);

	// ---- actions (optimistic; failures go to Data::Session::actionFailed)
	// I read the chat up to `seq`.
	void markRead(const QString &chat, qint64 seq);
	// Sets my reaction; an empty emoji removes it.
	void react(const QString &chat, qint64 seq, const QString &emoji);
	void edit(
		const QString &chat,
		qint64 seq,
		const QString &text,
		const QVector<Entity> &entities = {},
		base::Guard guard = base::Guard(),
		Fn<void()> done = nullptr,
		Fn<void(const Api::Error &)> fail = nullptr);
	// The same, also telling which link keeps its preview card: the url that
	// `POST /v1/link-preview` returned, or "" for a message without a card.
	void editWithPreview(
		const QString &chat,
		qint64 seq,
		const QString &text,
		const QVector<Entity> &entities,
		const QString &linkPreview,
		base::Guard guard = base::Guard(),
		Fn<void()> done = nullptr,
		Fn<void(const Api::Error &)> fail = nullptr);
	void deleteMessage(const QString &chat, qint64 seq, bool forEveryone);
	// scope: "both" or "me".
	void pin(const QString &chat, qint64 seq, const QString &scope = QStringLiteral("both"));
	void unpin(const QString &chat, qint64 seq, const QString &scope = QStringLiteral("both"));
	void loadPins(
		const QString &chat,
		base::Guard guard = base::Guard(),
		Fn<void(const QVector<PinnedMessage> &)> done = nullptr);
	// I played a voice / video note.
	void markPlayed(const QString &chat, qint64 seq);
	// A bot's inline button was pressed.
	void pressButton(
		const QString &chat,
		qint64 seq,
		int row,
		int column,
		base::Guard guard,
		Fn<void(const CallbackAnswer &)> done,
		Fn<void(const Api::Error &)> fail = nullptr);
	void updateLiveLocation(const QString &chat, qint64 seq, const Location &location);
	void stopLiveLocation(const QString &chat, qint64 seq);
	void loadLiveLocations(
		const QString &chat,
		base::Guard guard,
		Fn<void(const QVector<LiveShare> &)> done);

	// ---- used by the other stores (Sync, Outbox, Chats)
	// Merges server messages (any order); a server copy replaces the local
	// pending message with the same client message id.
	void merge(const QString &chat, QVector<Message> incoming, bool persist = true);
	void addLocal(const Message &message);
	// Changes a message in memory and in the cache. A server message that
	// is cached but not in memory (the chat is not open, the message is
	// older than the window) changes in the cache alone, later, on the
	// database's thread: `change` must hold values, not references.
	void patchMessage(const QString &chat, const QString &key, const Fn<void(Message &)> &change);
	void removeMessage(const QString &chat, const QString &key);
	// Brings the cached copies of these messages up to date (an old message
	// edited outside the window); one that is not cached stays out.
	void updateCached(const QString &chat, const QVector<Message> &messages);
	// Drops messages with seq < beforeSeq (all server messages when nullopt).
	void clear(const QString &chat, std::optional<qint64> beforeSeq = std::nullopt);
	// The peer read up to `seq`: my messages up to it become Read. The
	// cached ones that are not in memory become Read when they are read
	// from the cache (the mark also moves with headers, which never come
	// here).
	void markPeerRead(const QString &chat, qint64 seq);
	void setInfo(const ChatInfo &info);
	void applyPin(const QString &chat, qint64 seq, bool pinned);
	void forget(const QString &chat);
	// After a `too_long` difference: every timeline must be re-read.
	void markAllStale();
	// Something changed in the chat that no update spelled out (a history
	// cleared for me arrives in a difference as a bare "member" item): its
	// cache goes, and its timeline is read again from the end, at once when
	// it is on screen, when the chat opens otherwise.
	void markStale(const QString &chat);
	// Refetches one message of the window (a reaction changed).
	void refresh(const QString &chat, qint64 seq);

	// ---- used by Data::Groups (the changes of a group come as whole messages)
	// Messages that are new or changed: the window takes what belongs to
	// it; with `cache` the database replaces the rows it holds and adds the
	// ones newer than everything it holds.
	void applyChanges(const QString &chat, QVector<Message> changed, bool cache);
	// Messages that are gone (deleted for everyone, hidden for me).
	void dropMessages(const QString &chat, const QVector<qint64> &seqs, bool cache);
	// The new counts of one message; false when it is not in the window.
	bool applyReactions(const QString &chat, qint64 seq, const QMap<QString, qint64> &reactions);
	void setMyReaction(const QString &chat, qint64 seq, const QString &emoji);

Q_SIGNALS:
	// The chat's items / flags changed.
	void historyChanged(const QString &chat);
	void pinsChanged(const QString &chat);

private:
	struct PageOptions {
		qint64 before = 0;
		qint64 after = 0;
		qint64 around = 0;
		int limit = kPageSize;
		bool mergeIntoWindow = true;
	};
	struct Page {
		QVector<Message> messages; // oldest first
		ChatInfo info;
		std::optional<Group> group; // the header of a group's page
		bool hasMoreBefore = true;
		bool hasMoreAfter = false;
		// The oldest seq the server returned, messages that are not shown
		// (deleted ones) included: where the page begins.
		qint64 firstSeq = 0;
	};

	[[nodiscard]] History &historyRef(const QString &chat);
	[[nodiscard]] History *existing(const QString &chat);
	void loadPage(
		const QString &chat,
		PageOptions options,
		Fn<void(const Page &)> done,
		Fn<void(const Api::Error &)> fail);
	void persist(const QString &chat, const QVector<Message> &messages);
	// Changed messages of a timeline to the cache: a timeline that reaches
	// the newest message is the cached run, any other one only brings the
	// rows the cache holds up to date.
	void storeChanged(const History &history, const QVector<Message> &messages);
	void changeCached(const QString &chat, qint64 seq, Fn<void(Message &)> change);
	// The peer's read mark as far as it is known here.
	[[nodiscard]] qint64 peerReadSeq(const QString &chat) const;
	// A cached message, my own read when it is at or below the mark.
	[[nodiscard]] static Message FromCache(const QJsonObject &row, qint64 peerReadSeq);
	// A window that reaches the newest message begins at `firstSeq`, and
	// with `complete` nothing older exists. The older messages held in
	// memory and in the database go unless they are one run with it:
	// messages written while the chat was not open are never cached, and
	// the older pages are read from the cache first, so a hole between the
	// two would never be filled.
	void dropDetached(const QString &chat, qint64 firstSeq, bool complete);
	void simple(
		const QString &method,
		const QJsonObject &body,
		const QString &action,
		const QString &chat,
		Fn<void()> onFail = nullptr);

	Session *_session = nullptr;
	std::map<QString, History> _histories; // node-based: references stay valid

};

} // namespace Data
