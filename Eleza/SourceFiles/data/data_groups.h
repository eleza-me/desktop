// Groups: the second sync track and everything a member does to a group.
//
// A group is not a chat on the server — it never appears in the inbox call or
// in the user-level difference, and nothing is prepared per member — but it
// lives in the same list: this store keeps the group rows of Data::Chats
// (Chat::isGroup, Chat::group = the header) and the message timelines of
// Data::Messages current.
//
//   catch-up      after every Hello of the connection, and wherever the
//                 user-level difference runs without one: `groups.sync` with
//                 every group held and the pts applied to its row; the answer
//                 replaces the rows that changed and names the groups I am no
//                 longer in. No group held at all: the list is paged instead.
//   live frames   the pts rule (Data::GroupRules::FrameActionFor): only the
//                 very next change is applied as it comes, an older one is
//                 dropped, anything else is a gap — closed by the group's
//                 difference when its messages are cached, by the catch-up
//                 otherwise.
//   difference    `groups.difference`: the messages whose last change is
//                 after a pts, as they are now. Applied by seq, so it has no
//                 gaps and applying it twice changes nothing.
//   my own place  added / removed / role, read elsewhere, settings, my
//                 reaction and my draft arrive on the user's own update log
//                 (kind `group`), through Data::Sync.
//
// Two counters are kept per row. Chat::groupPts is the last change applied to
// the row (title, preview, unread, pins…): frames are compared with it.
// Chat::groupCachePts is the pts the cached messages of the group are current
// at (-1: nothing cached): a difference starts from it. While the timeline of
// a group is attached (both equal) frames move both; a group that is not
// open only moves the first, and its cache is brought up to date by one
// difference when it is opened. Per group a difference in flight holds back
// that group's frames; they go through the pts rule afterwards.
//
// While `feature.groups` is off nothing here makes a request or adds a row.
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

namespace Data {

class Session;

struct GroupMembersPage {
	QVector<GroupMember> members;
	QString nextCursor;
};

struct GroupJoinRequestsPage {
	QVector<GroupJoinRequest> requests;
	QString nextCursor;
};

struct GroupAddResult {
	QStringList added;
	QVector<GroupNotAdded> notAdded;
};

struct GroupCreateRequest {
	QString title;
	QString about;
	QString photo;       // media id of an uploaded picture
	QStringList members;
};

struct GroupInviteRequest {
	QString name;
	bool requiresApproval = false;
	qint64 expiresIn = 0; // seconds, 0 = never
	int maxUses = 0;      // 0 = no limit
};

// The answer of joining by a link: the group, or a request that waits.
struct GroupJoinResult {
	std::optional<Group> group;
	bool pending = false;
};

class Groups final : public QObject {
	Q_OBJECT

public:
	using Fail = Fn<void(const Api::Error &)>;

	explicit Groups(Session *session);

	// ---- the gates (client-config)
	// `feature.groups`: absent or false means no group UI and no group calls.
	[[nodiscard]] bool enabled() const;
	// The second release of groups (admins, invite links, pins, mentions,
	// drafts): a server that has it lists `groups.max_admins`.
	[[nodiscard]] bool administration() const;
	[[nodiscard]] int maxMembers() const;
	[[nodiscard]] int maxInitialMembers() const;
	[[nodiscard]] int addBatchMax() const;
	[[nodiscard]] int titleMaxChars() const;
	[[nodiscard]] int aboutMaxChars() const;
	[[nodiscard]] int membersPageSize() const;
	[[nodiscard]] int maxAdmins() const;
	[[nodiscard]] int adminTitleMaxChars() const;
	[[nodiscard]] int maxInvites() const;
	[[nodiscard]] int inviteNameMaxChars() const;
	[[nodiscard]] qint64 inviteMaxExpiry() const;     // seconds
	[[nodiscard]] int maxMentionsPerMessage() const;
	[[nodiscard]] int bulkDeleteMax() const;
	[[nodiscard]] qint64 deleteOwnWindow() const;     // seconds, 0 = no limit
	[[nodiscard]] QString inviteLinkBase() const;

	// ---- rows
	// The header of a group that is in the list (null otherwise). Valid
	// until the list changes.
	[[nodiscard]] const Group *group(const QString &id) const;
	// The pts applied to the row; -1 when the group is not held.
	[[nodiscard]] qint64 pts(const QString &id) const;

	// ---- sync (called by Data::Sync, Data::Messages, Data::Outbox)
	void start();
	void stop();
	// Queues the catch-up call; asks that come while one waits share it.
	void requestSync();
	// The list is on screen: one catch-up if this run has not had one yet.
	void ensureSynced();
	// A `Signal t:"group"`: { group, pts, kind, data? }.
	void handleFrame(const QJsonObject &frame);
	// The data of a user update of kind `group`.
	void applyUser(const QJsonObject &data);
	// A page of the group's history arrived (its header moves the row; the
	// newest page attaches the message cache).
	void historyPage(const Group &header, bool newest);
	// The answer of a send: { message_id, seq, pts, created_at, duplicate? };
	// `cached`: the message was in the timeline before the answer.
	void sent(const QString &group, const Message &message, const QJsonObject &result, bool cached);
	// What an error of a group call means for the cache: `group.not_found` on
	// a group I hold drops it, `group.disabled` reads client-config again,
	// `group.send_restricted` / `right_required` / `slow_mode` read the header.
	// True when the group is gone (queued sends fail for good).
	bool handleError(const QString &group, const Api::Error &error);
	// A header obtained elsewhere (the answer of patch, transfer, permissions).
	void applyHeader(const Group &header);
	// One coalesced header read (my role, rights, permissions, title, photo).
	void refreshHeader(const QString &group);
	// Until when slow mode makes me wait in a group (ms, local clock; 0: no wait).
	[[nodiscard]] qint64 slowModeUntil(const QString &group) const;

	// ---- my own actions
	void create(
		const GroupCreateRequest &request,
		base::Guard guard,
		Fn<void(const Group &, const QVector<GroupNotAdded> &)> done,
		Fail fail = nullptr);
	void leave(const QString &group, base::Guard guard, Fn<void()> done, Fail fail = nullptr);
	void deleteGroup(const QString &group, base::Guard guard, Fn<void()> done, Fail fail = nullptr);
	// Deletes the history for me, up to the newest message; the group stays.
	void clearHistory(const QString &group);
	// "all" | "mentions" | "none".
	void setNotify(const QString &group, const QString &level);
	void edit(
		const QString &group,
		const std::optional<QString> &title,
		const std::optional<QString> &about,
		const std::optional<QString> &photo, // "" removes the picture
		base::Guard guard,
		Fn<void(const Group &)> done,
		Fail fail = nullptr);
	void transfer(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void(const Group &)> done,
		Fail fail = nullptr);

	// ---- members
	// filter: "all" | "admins" | "restricted" | "banned".
	void loadMembers(
		const QString &group,
		const QString &filter,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const GroupMembersPage &)> done,
		Fail fail = nullptr);
	// The members whose name or handle matches (the server's user search,
	// limited to this group; one page).
	void searchMembers(
		const QString &group,
		const QString &query,
		base::Guard guard,
		Fn<void(const QVector<User> &)> done);
	void addMembers(
		const QString &group,
		const QStringList &users,
		base::Guard guard,
		Fn<void(const GroupAddResult &)> done,
		Fail fail = nullptr);
	void removeMember(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void()> done,
		Fail fail = nullptr);

	// ---- administration (the second release)
	// Makes a member an admin, or changes an admin's rights and title.
	// `title`: nullopt keeps the current one, "" removes it.
	void setAdmin(
		const QString &group,
		const QString &user,
		const QStringList &rights,
		const std::optional<QString> &title,
		base::Guard guard,
		Fn<void(const GroupMember &)> done,
		Fail fail = nullptr);
	void unsetAdmin(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void(const GroupMember &)> done,
		Fail fail = nullptr);
	// An empty `denied` lifts the restriction; `until` 0 = until lifted.
	void restrict(
		const QString &group,
		const QString &user,
		const QStringList &denied,
		qint64 until,
		base::Guard guard,
		Fn<void(const GroupMember &)> done,
		Fail fail = nullptr);
	void ban(
		const QString &group,
		const QString &user,
		bool deleteMessages,
		base::Guard guard,
		Fn<void()> done,
		Fail fail = nullptr);
	void unban(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void()> done,
		Fail fail = nullptr);
	// nullopt = unchanged. An empty list means «ordinary members may do
	// nothing», which only HTTP can say: this call never uses the connection.
	void setPermissions(
		const QString &group,
		const std::optional<QStringList> &defaultPermissions,
		const std::optional<bool> &historyVisible,
		base::Guard guard,
		Fn<void(const Group &)> done,
		Fail fail = nullptr);
	// Deletes several messages for everyone in one change.
	void deleteMessages(
		const QString &group,
		const QVector<qint64> &seqs,
		base::Guard guard = base::Guard(),
		Fn<void()> done = nullptr,
		Fail fail = nullptr);

	// ---- invite links and join requests
	void loadInvites(
		const QString &group,
		base::Guard guard,
		Fn<void(const QVector<GroupInvite> &)> done,
		Fail fail = nullptr);
	void createInvite(
		const QString &group,
		const GroupInviteRequest &request,
		base::Guard guard,
		Fn<void(const GroupInvite &)> done,
		Fail fail = nullptr);
	void revokeInvite(
		const QString &group,
		const QString &link,
		base::Guard guard,
		Fn<void()> done,
		Fail fail = nullptr);
	void invitePreview(
		const QString &token,
		base::Guard guard,
		Fn<void(const GroupInvitePreview &)> done,
		Fail fail = nullptr);
	void joinByInvite(
		const QString &token,
		base::Guard guard,
		Fn<void(const GroupJoinResult &)> done,
		Fail fail = nullptr);
	void loadJoinRequests(
		const QString &group,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const GroupJoinRequestsPage &)> done,
		Fail fail = nullptr);
	void decideJoinRequest(
		const QString &group,
		const QString &user,
		bool approve,
		base::Guard guard,
		Fn<void()> done,
		Fail fail = nullptr);

	// ---- mentions
	// The messages above my read mark that mention me or reply to me, oldest first.
	void loadMentions(
		const QString &group,
		base::Guard guard,
		Fn<void(const QVector<qint64> &)> done);

	// The row of a header (used by the store itself and by the tests).
	[[nodiscard]] static ChatInfo InfoFor(const Group &group);

	static constexpr auto kListPageSize = 50;
	static constexpr auto kMaxListPages = 40;
	static constexpr auto kMaxDifferencePages = 50;
	static constexpr auto kMaxKnown = 1000;
	static constexpr auto kMaxPendingFrames = 256;

Q_SIGNALS:
	// I am no longer a member (removed, banned, left — here or on another
	// device — or the group was deleted): its row and messages are gone.
	// status: kicked | left | banned | deleted | removed (the server only
	// said «not found»).
	void groupRemoved(const QString &group, const QString &status);
	// Someone joined, left, was removed or changed role: a member list of
	// this group that is on screen reads itself again.
	void membersChanged(const QString &group);
	// A live message of somebody else that may alert (the notify level and
	// the mute of the group were applied): for notifications / sounds.
	void messageReceived(const Data::Message &message, bool addressesMe);
	// The feature was switched on or off.
	void enabledChanged();

private:
	// What is going on with one group.
	struct State {
		bool busy = false;      // a difference is in flight: frames wait in `pending`
		bool again = false;     // asked for again while one was running
		int fails = 0;          // differences that failed in a row
		qint64 notBefore = 0;   // ms: no new try before this
		bool scheduled = false; // a try is waiting for notBefore
		QVector<QJsonObject> pending;
	};

	[[nodiscard]] bool held(const QString &group) const;
	[[nodiscard]] bool attached(const Chat &row) const;
	[[nodiscard]] State &state(const QString &group);
	void forgetState(const QString &group);

	// ---- frames
	void applyRule(const QString &group, const QJsonObject &frame);
	bool applyFrame(const QString &group, qint64 pts, const QString &kind, const QJsonObject &data);
	void applyMessageFrame(const QString &group, const QJsonObject &data, bool cache);
	void applyChangedMessage(const QString &group, const QJsonObject &data, bool cache);
	void applyTombstones(const QString &group, const QVector<qint64> &seqs, bool cache);
	[[nodiscard]] bool addressesMe(const QString &group, const Message &message) const;
	void catchUp(const QString &group);

	// ---- difference
	void startDifference(const QString &group);
	void differenceStep(const QString &group, int page);
	void applyDifference(const QString &group, qint64 from, const QJsonObject &result, int page);
	void differenceDone(const QString &group, bool ok);

	// ---- catch-up and cold start
	void runSync();
	void syncFailed(const Api::Error &error);
	void syncSucceeded();
	void applySync(const QHash<QString, qint64> &known, const QJsonObject &result);
	bool applyEntry(const QJsonObject &entry, const QHash<QString, qint64> *known, bool listed);
	void coldStart(const QString &cursor, int page, bool restarted);
	void syncForFrame();
	void fetchPeople(const QStringList &ids);

	// ---- rows
	[[nodiscard]] Chat rowFor(const Group &header, const Chat *previous) const;
	void drop(const QString &group, const QString &status);

	// ---- requests
	void call(
		const QString &method,
		const QString &group,
		const QJsonObject &body,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		Fail fail);
	void member(const QString &group, const QJsonObject &body, const QString &action, Fn<void()> revert);

	Session *_session = nullptr;
	QHash<QString, State> _states;
	QHash<QString, qint64> _unknownAsked; // groups not held whose frames asked for a catch-up
	QSet<QString> _headerRequests;
	QHash<QString, qint64> _slowModeUntil;
	bool _started = false;
	bool _syncing = false;
	bool _syncAgain = false;
	bool _syncedOnce = false;
	bool _wasEnabled = false;
	int _syncFails = 0;
	qint64 _syncNotBefore = 0;
	qint64 _coldStartAt = 0;

};

} // namespace Data
