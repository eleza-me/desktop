// The chat list: rows (Data::Chat) by section, unread counts, mute / pin /
// archive, requests, drafts, typing indicators and presence.
//
//   const auto rows = chats.list(Data::Chats::Section::Messages);  // sorted
//   connect(&chats, &Data::Chats::listChanged, this, [=] { ... });
//   connect(&chats, &Data::Chats::chatChanged, this, [=](const QString &id) { ... });
//
// The list is filled from the database at start (instant), then from the
// server once (`loadInbox`), and after that kept current by Data::Sync.
// Actions apply optimistically; a failure is reverted where that is
// meaningful and reported through Data::Session::actionFailed.
#pragma once

#include "api/api_error.h"
#include "base/timer.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <QtCore/QHash>

namespace Data {

class Session;

class Chats final : public QObject {
	Q_OBJECT

public:
	enum class Section {
		Messages,      // direct chats that are neither archived nor requests
		Conversations, // anonymous talker / listener conversations (not archived)
		Requests,      // message requests
		Archived,
		All,
	};

	explicit Chats(Session *session);

	// ---- rows
	// Null when the chat has no row. The pointer is valid until the list changes.
	[[nodiscard]] const Chat *chat(const QString &id) const;
	// Pinned first, then newest activity first.
	[[nodiscard]] QVector<Chat> list(Section section = Section::Messages) const;
	[[nodiscard]] int count(Section section) const;
	// The chat a conversation was projected into ("" when not listed yet).
	[[nodiscard]] QString chatOfConversation(const QString &conversationId) const;
	// «Saved messages», when it is in the list.
	[[nodiscard]] const Chat *savedChat() const;

	// Chats with unread messages that are not muted, archived or requests (the badge).
	[[nodiscard]] int unreadChats() const {
		return _unreadChats;
	}
	// Muted chats with unread messages (not part of unreadChats()).
	[[nodiscard]] int unreadMutedChats() const {
		return _unreadMutedChats;
	}
	[[nodiscard]] int unreadRequests() const;

	// ---- loading
	[[nodiscard]] bool inboxLoading() const {
		return _inboxLoading;
	}
	// The last inbox load failed (for the list's empty state); cleared by a success.
	[[nodiscard]] const std::optional<Api::Error> &inboxError() const {
		return _inboxError;
	}
	[[nodiscard]] bool inboxLoadedOnce() const {
		return _inboxLoaded;
	}
	// Rows from the database (startup); `done(true)` when there were any.
	void loadCached(base::Guard guard, Fn<void(bool)> done);
	// The whole inbox (message + conversation + request sections) from the server.
	void loadInbox(
		base::Guard guard = base::Guard(),
		Fn<void()> done = nullptr,
		Fn<void(const Api::Error &)> fail = nullptr);
	// One chat's header (`chat.header`); updates its row.
	void loadHeader(
		const QString &chat,
		base::Guard guard,
		Fn<void(const ChatInfo &)> done,
		Fn<void(const Api::Error &)> fail = nullptr);
	// The direct chat with a user, created when none exists (`chat.thread`).
	void openDirect(
		const QString &userId,
		base::Guard guard,
		Fn<void(const Chat &)> done,
		Fn<void(const Api::Error &)> fail = nullptr);
	void openSaved(
		base::Guard guard,
		Fn<void(const Chat &)> done,
		Fn<void(const Api::Error &)> fail = nullptr);

	// The chat on screen: incoming messages there do not count as unread.
	void setVisibleChat(const QString &id);
	[[nodiscard]] QString visibleChat() const {
		return _visibleChat;
	}

	// ---- actions
	void setMuted(const QString &chat, bool muted);
	// "1h" / "8h" / "2d" / "forever" / "off" (client-config chat.mute.durations).
	void muteFor(const QString &chat, const QString &duration);
	void setArchived(const QString &chat, bool archived);
	void setPinned(const QString &chat, bool pinned);
	// Replaces the order of the pinned chats (`chat.pinned_chats`).
	void setPinnedOrder(const QStringList &chats);
	void setTheme(const QString &chat, const QString &theme);
	// Clears the history for me, or deletes the chat for both sides.
	void deleteChat(const QString &chat, bool forEveryone);
	void acceptRequest(const QString &chat);
	void declineRequest(const QString &chat);

	// ---- drafts
	// Saves (or, with an empty text, clears) the chat's draft: at once in the
	// row and the database, then on the server.
	void saveDraft(const QString &chat, const Draft &draft);
	void clearDraft(const QString &chat);

	// ---- typing
	// Tells the peer what I am doing ("typing", "recording_voice", ...,
	// "cancel"); throttled, needs the connection.
	void sendTyping(const QString &chat, const QString &kind = QStringLiteral("typing"));
	// What the peer is doing right now, if anything.
	[[nodiscard]] std::optional<Activity> activity(const QString &chat) const;

	// ---- presence
	// Reads and subscribes to the presence of these users (presenceChanged follows).
	void watchPresence(const QStringList &users);
	void unwatchPresence(const QStringList &users);
	[[nodiscard]] std::optional<Presence> presence(const QString &user) const;

	// ---- used by the other stores (Sync, Outbox, Messages)
	[[nodiscard]] QVector<Chat> all() const;
	void upsert(const Chat &chat);
	// Applies `change` to the row (if any) and stores it.
	void patch(const QString &id, const Fn<void(Chat &)> &change);
	void remove(const QString &id);
	void replaceAll(const QVector<Chat> &chats);
	// Refreshes (or creates) a row from a newer header; false when it stays hidden.
	bool applyInfo(const ChatInfo &info);
	// Maps an inbox entry to a row; nullopt when it has no header.
	[[nodiscard]] std::optional<Chat> rowFromEntry(const InboxEntry &entry) const;
	void setActivity(const QString &chat, const QString &kind, int ttlSeconds);
	void setPresence(const Presence &presence);
	// A card changed: rows with that peer take the new name / badge.
	void applyCard(const User &card);
	// Re-sends drafts the server has not confirmed (after a reconnect).
	void flushDrafts();
	void markCleared(const QString &id, qint64 lastSeq);

Q_SIGNALS:
	// Rows were added, removed or reordered (also emitted with chatChanged).
	void listChanged();
	void chatChanged(const QString &id);
	void chatRemoved(const QString &id);
	void unreadChanged();
	void inboxLoadingChanged();
	void activityChanged(const QString &chat);
	void presenceChanged(const QString &user);

private:
	struct Capability {
		QString cap;
		qint64 expiresAt = 0; // ms
	};

	void loadSection(
		const QString &section,
		const QString &cursor,
		int page,
		std::shared_ptr<QVector<InboxEntry>> into,
		Fn<void(bool)> done);
	void finishInbox(const QVector<InboxEntry> &entries);
	[[nodiscard]] bool isCleared(const Chat &chat);
	void persistCleared();
	void changed(const QString &id, bool reordered);
	void recount();
	void store(const Chat &chat);
	void member(const QString &chat, const QJsonObject &body, const QString &action, Fn<void()> revert);
	void pushDraft(const QString &chat, const Draft &draft);
	void capability(const QString &chat, Fn<void(const QString &)> done);
	void activityTick();

	Session *_session = nullptr;
	QHash<QString, Chat> _chats;
	QHash<QString, qint64> _cleared;
	QHash<QString, Draft> _localDrafts;
	QHash<QString, Presence> _presence;
	QHash<QString, Activity> _activity;
	QHash<QString, Capability> _capabilities;
	QHash<QString, qint64> _typingSentAt;
	base::Timer _activityTimer;
	QString _visibleChat;
	std::optional<Api::Error> _inboxError;
	QVector<std::pair<base::Guard, std::pair<Fn<void()>, Fn<void(const Api::Error &)>>>> _inboxWaiters;
	int _unreadChats = 0;
	int _unreadMutedChats = 0;
	bool _inboxLoading = false;
	bool _inboxLoaded = false;

};

} // namespace Data
