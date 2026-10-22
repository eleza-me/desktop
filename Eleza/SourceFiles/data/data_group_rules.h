// The decisions of groups as plain functions (no stores, no JSON, no
// widgets), so that they are tested one by one (tests/core/test_group_rules.cpp)
// and the stores and screens only do what these answer:
//
//   GroupRules        the sync rules (a live frame, the answer of a send, an
//                     entry of the catch-up call, a difference), service
//                     notes, the list row and the chat screen
//   GroupRights       the rights table the server evaluates, for what a screen
//                     has to decide before it asks (which actions a member row
//                     offers, which rights a granter may tick)
//   GroupInviteLinks  https://eleza.me/g/<token>
//   MentionRules      «@…» while typing, entity offsets, "does it address me"
//   GroupAdminLog     the sentence of an entry of the admin log, its filter
//                     by kind of action, the steps of slow mode
//   MemberTags        the tag next to a sender's name (Telegram's member
//                     tags): which text, which look, who may set it, the tag
//                     of a row of the member list
//
// A group has two counters. `seq` numbers its messages; `pts` numbers its
// changes (a new message, an edit, a delete for everyone, a reaction, a
// change of the profile, the owner or the pins). The cache of a group is up
// to date as far as the pts applied to it.
#pragma once

#include "base/basic_types.h"
#include "data/data_group_types.h"

#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace Data {

// A group is kept in the chat list under its public id, and every request
// about a chat is routed by the prefix of that id: "j…" is a group
// (`/v1/groups/{id}/…`, `groups.*` on the connection), anything else a chat
// or a conversation.
[[nodiscard]] bool IsGroupId(QStringView id);

namespace GroupRules {

// ---- a live frame

enum class FrameAction {
	Apply,   // the very next change: apply it and store its pts
	Drop,    // already applied (a repeat, or the answer of my own send came first)
	CatchUp, // frames were lost, or this one only says «the group moved»
};
// A frame without data can never be applied.
[[nodiscard]] FrameAction FrameActionFor(qint64 storedPts, qint64 framePts, bool hasData);

// ---- the answer of a send

enum class SendAction {
	Advance, // the message is the only change since the stored pts
	Keep,    // its pts is stored already (the live frame came first)
	CatchUp, // something else happened in between: the message, then a difference
};
// `duplicate`: the server says this was a retry of a message it already had,
// so its pts says nothing about order; a difference is needed only when the
// message is not in the cache (`cached`).
[[nodiscard]] SendAction SendActionFor(
	qint64 storedPts,
	qint64 answerPts,
	bool duplicate,
	bool cached);

// ---- an entry of the catch-up call (`groups.sync`) or of the list

enum class SyncAction {
	Skip,   // live changes moved this group past what the answer knows
	Take,   // nothing cached beyond the row: the header and its pts replace it
	Behind, // a message cache is held: the header, the applied pts stays, a difference is owed
	Rewind, // the server is behind the pts we sent: the header, the cache is checked again from 0
};
// sentPts: the applied pts sent as «known» (-1: not sent — new to us);
// heldPts: the applied pts now (-1: not held); entryPts: the header's pts;
// hasCache: more than the list row's message is cached.
[[nodiscard]] SyncAction SyncActionFor(
	qint64 sentPts,
	qint64 heldPts,
	qint64 entryPts,
	bool hasCache);

// ---- unread

struct Change {
	qint64 seq = 0;
	bool isNew = false;   // was not in the cache before
	bool mine = false;
	bool deleted = false;
};
// How many of a difference's messages add to the unread count: the ones
// above the last seq the row knew of (everything up to it is counted
// already), that are new here, not mine, not deleted and above my read mark.
[[nodiscard]] int UnreadFromDifference(
	const QVector<Change> &changes,
	qint64 lastSeqBefore,
	qint64 lastReadSeq);
// The seq to send as my read mark for what is on screen up to `seenSeq`:
// never back, never past the last message; 0 when there is nothing to send.
[[nodiscard]] qint64 ReadTarget(qint64 seenSeq, qint64 lastReadSeq, qint64 lastSeq);

// ---- service notes (`kind: "system"`, field `system`)

enum class Note {
	None,         // a code this build has no words for: the message is skipped
	Created,      // group_created
	Added,        // member_added:<user>
	Left,         // member_left
	Removed,      // member_removed:<user> (also a ban)
	TitleChanged, // title_changed
	PhotoChanged, // photo_changed
	Joined,       // member_joined (by a link, or a request was approved)
	Pinned,       // pinned:<seq>
};
[[nodiscard]] Note NoteOf(const QString &system);
// The person a member_added / member_removed note names ("" for any other).
[[nodiscard]] QString NoteUser(const QString &system);
// The message a pinned:<seq> note names; 0 for any other note or a broken one.
[[nodiscard]] qint64 NotePinnedSeq(const QString &system);
// +1 joined or added, -1 left or removed, 0 anything else.
[[nodiscard]] int MemberDelta(const QString &system);
// The title or the photo changed: the header is read again.
[[nodiscard]] bool HeaderChanged(const QString &system);

// ---- the list row

// The first word of a name (what the preview has room for), at most 24 characters.
[[nodiscard]] QString FirstName(const QString &displayName);

// ---- the chat screen

// Which colour of a palette of `size` a member's name takes: the same for
// the same user id on every screen, every start and every device (FNV-1a
// over the id's UTF-16 units). No id takes the first one.
[[nodiscard]] int NameColorIndex(QStringView userId, int size);

// May this message be deleted for everyone now? My own: always, or only for
// `ownWindowMs` after it was sent when the server sets a window (0 = none;
// an unknown send time counts as inside it). Someone else's: only with the
// right to delete messages. Service notes and unsent messages never.
[[nodiscard]] bool CanDeleteForEveryone(
	bool mine,
	bool serviceNote,
	qint64 seq,
	qint64 sentAtMs,
	qint64 nowMs,
	qint64 ownWindowMs,
	bool deleteRight);

enum class Composer {
	Compose,    // the field is there
	Restricted, // a bar instead: I may not send here
	Wait,       // a bar with a countdown: slow mode, I still wait
};
// `waitUntilMs`: until when slow mode makes me wait (0 = not waiting). A
// wait that ran out gives the field back at once.
[[nodiscard]] Composer ComposerFor(
	bool canSend,
	const QString &sendBlocked,
	qint64 waitUntilMs,
	qint64 nowMs);
// Whole seconds still to wait (rounded up, never negative).
[[nodiscard]] qint64 WaitSeconds(qint64 waitUntilMs, qint64 nowMs);

enum class TimeUnit {
	Minutes,
	Hours,
	Days,
};
struct TimeLeft {
	qint64 amount = 0;
	TimeUnit unit = TimeUnit::Minutes;
};
// When a restriction with an end stops applying, rounded up to whole
// minutes / hours / days (never «0»); nullopt when it has ended or has no end.
[[nodiscard]] std::optional<TimeLeft> RestrictionLeft(qint64 untilS, qint64 nowMs);

// ---- members

// «Add people»: with the permission, while the group has room.
[[nodiscard]] bool CanAdd(bool addPermission, int memberCount, int maxMembers);
// How many people one «add» may still take.
[[nodiscard]] int AddRoom(int memberCount, int maxMembers, int batchMax);
// Does a name or a handle contain what was typed (case-insensitive, a
// leading «@» ignored, empty = everyone)?
[[nodiscard]] bool MatchesMember(
	const QString &name,
	const QString &handle,
	const QString &query);
// A title as typed, made fit to send: one line, trimmed, runs of blanks as one space.
[[nodiscard]] QString CleanTitle(const QString &typed);
// 1..max characters (code points) after cleaning.
[[nodiscard]] bool TitleOk(const QString &typed, int max);

} // namespace GroupRules

namespace GroupRights {

// Admin rights and member permissions. On the wire both are lists of names;
// the masks only make the table short. Bit i = name i of the wire order
// (the server's bits: a later name only ever comes last).
enum Right : int {
	kChangeInfo = 1 << 0,
	kDeleteMessages = 1 << 1,
	kBanUsers = 1 << 2,
	kInviteUsers = 1 << 3,
	kPinMessagesRight = 1 << 4,
	kAddAdmins = 1 << 5,
	kManageTags = 1 << 6,     // set other people's tags
	kRightsAll = (1 << 7) - 1,
};
enum Permission : int {
	kSendMessages = 1 << 0,
	kSendMedia = 1 << 1,
	kSendLinks = 1 << 2,
	kAddMembers = 1 << 3,
	kPinMessages = 1 << 4,
	kChangeInfoPermission = 1 << 5,
	kEditTag = 1 << 6,        // set my own tag
	kPermissionsAll = (1 << 7) - 1,
	kPermissionsSend = kSendMessages | kSendMedia | kSendLinks,
	// What an ordinary member of a new group may do.
	kDefaultPermissions = kSendMessages
		| kSendMedia
		| kSendLinks
		| kAddMembers
		| kEditTag,
};

[[nodiscard]] const QStringList &RightNames();
[[nodiscard]] const QStringList &PermissionNames();
// The order the screens list them in (Telegram's): «edit member tags»
// before «add admins», «edit own tag» after «pin» and before «change info».
[[nodiscard]] const QStringList &ShownRights();
[[nodiscard]] const QStringList &ShownPermissions();
// The bit of a name (0 for a name this build does not know).
[[nodiscard]] int RightBit(const QString &name);
[[nodiscard]] int PermissionBit(const QString &name);
// A name this build does not know is left out; unknown bits are never listed.
[[nodiscard]] int RightsMask(const QStringList &names);
[[nodiscard]] int PermissionsMask(const QStringList &names);
[[nodiscard]] QStringList RightsList(int mask);
[[nodiscard]] QStringList PermissionsList(int mask);

// A row of the group, as far as the rights model cares.
struct Member {
	QString user;
	QString status;          // member | left | kicked | banned; empty: never was a member
	QString role;            // owner | admin | member
	int granted = 0;         // the rights granted to an admin
	int denied = 0;          // what a restriction takes away…
	bool restricted = false; // …and whether it applies now

	[[nodiscard]] bool in() const {
		return status == u"member";
	}
	[[nodiscard]] static Member Of(const QString &user, const QString &role, int granted = 0);
};

// All for the owner, the granted ones for an admin, none for anyone else.
[[nodiscard]] int Rights(const Member &member);
// The owner everything; an admin the defaults, every send permission, their
// own tag and what their rights imply; a member the defaults minus what a
// restriction that applies now denies.
[[nodiscard]] int Permissions(const Member &member, int defaults);

// Something a member may want to do: the permissions and the rights it
// takes (all of them), or the owner.
struct Action {
	int permission = 0;
	int right = 0;
	bool owner = false;
};
inline constexpr auto kActMember = Action{};
inline constexpr auto kActAddMembers = Action{ kAddMembers, 0, false };
inline constexpr auto kActPin = Action{ kPinMessages, 0, false };
inline constexpr auto kActChangeInfo = Action{ kChangeInfoPermission, 0, false };
inline constexpr auto kActDeleteMessages = Action{ 0, kDeleteMessages, false };
// Removing, banning, unbanning and restricting; the banned and restricted
// lists; join requests; the default permissions.
inline constexpr auto kActBan = Action{ 0, kBanUsers, false };
inline constexpr auto kActInvites = Action{ 0, kInviteUsers, false };
inline constexpr auto kActAddAdmins = Action{ 0, kAddAdmins, false };
// Whether new members see earlier messages: the right, never the default
// permission of the same name.
inline constexpr auto kActHistory = Action{ 0, kChangeInfo, false };
inline constexpr auto kActOwner = Action{ 0, 0, true };
// My own tag; somebody else's (with the target rule of CanActOn).
inline constexpr auto kActEditOwnTag = Action{ kEditTag, 0, false };
inline constexpr auto kActManageTags = Action{ 0, kManageTags, false };
[[nodiscard]] Action ActSend(int permissions);

// The send permissions a message needs: every message send_messages;
// anything but a text or a forwarded post send_media too; a link preview or
// a text_link entity send_links. A mention needs nothing more.
[[nodiscard]] int Needs(const QString &kind, bool linkPreview, bool textLink);

[[nodiscard]] bool Can(int defaults, const Member &member, const Action &action);
// Besides the actor's own right: nobody acts on themselves, nobody on the
// owner, only the owner on an admin. Someone who is not in the group now has
// no role to protect.
[[nodiscard]] bool CanActOn(
	int defaults,
	const Member &actor,
	const Member &target,
	const Action &action);
// The rights `actor` may give to an admin: the ones they hold themselves.
[[nodiscard]] int Grantable(const Member &actor);

// ---- what a member row offers
[[nodiscard]] bool CanPromote(const Member &me, const Member &target, int admins, int maxAdmins);
// «Edit admin rights» / «Remove admin»: the owner only.
[[nodiscard]] bool CanEditAdmin(const Member &me, const Member &target);
// An ordinary member only (an admin is made a member first).
[[nodiscard]] bool CanRestrict(const Member &me, const Member &target);
// «Remove» / «Ban».
[[nodiscard]] bool CanBan(const Member &me, const Member &target);

} // namespace GroupRights

namespace GroupInviteLinks {

// The prefix every build knows, used until client-config says another
// (`groups.invites.link_base`).
[[nodiscard]] QString DefaultBase();
// A group id, a dot, 22 URL-safe characters.
[[nodiscard]] bool IsToken(const QString &text);
// The group a token names ("" for a malformed one).
[[nodiscard]] QString GroupOf(const QString &token);
// The token of an invite link, or "" when `link` is not one. Accepted: the
// address with or without https:// / http:// and www., under `base` or under
// the default prefix; eleza://g/<token>; a bare token. A query, a fragment, a
// trailing slash and what a sentence leaves behind a pasted link (a dot, a
// comma, a bracket) are dropped.
[[nodiscard]] QString Token(const QString &link, const QString &base = QString());
// What people share: https:// + the server's prefix + the token.
[[nodiscard]] QString Url(const QString &token, const QString &base = QString());

} // namespace GroupInviteLinks

namespace MentionRules {

// The longest «@word» that still asks for suggestions (units after the «@»).
inline constexpr auto kQueryMax = 32;
// The longest name written into the text for a mention (code points).
inline constexpr auto kLabelMax = 64;

struct Query {
	int at = 0;   // where the «@» is
	QString text; // the word typed so far (may be empty)
};
// The mention being typed just before the cursor. It starts at an «@» that
// begins the text or follows a blank or an opening mark — not one inside a
// word, an address or an e-mail — and runs to the cursor without a blank.
[[nodiscard]] std::optional<Query> QueryAt(const QString &text, int cursor);

// The name a picked member is written as: the display name on one line, at
// most kLabelMax code points (never half of a pair); without a name the
// handle; without either, `fallback`.
[[nodiscard]] QString Label(
	const QString &displayName,
	const QString &handle,
	const QString &fallback);

// May one more person be mentioned in a message that already names `users`?
[[nodiscard]] bool Room(const QStringList &users, const QString &next, int max);

struct Range {
	int offset = 0; // UTF-16 code units, like every entity
	int length = 0;
};
// A range [from, till) of the typed text as it lies in the text that goes
// out, which is the typed text trimmed. Nullopt when nothing of it is left.
// A boundary never falls between the two halves of a surrogate pair.
[[nodiscard]] std::optional<Range> ToWire(int from, int till, const QString &typed);
// A received entity cut to a text of `textLength` units; nullopt when it
// cannot be drawn (a negative offset, no length, a start beyond the text).
[[nodiscard]] std::optional<Range> Clip(int offset, int length, int textLength);

// Does a message address me — it mentions me, or it replies to a message I
// wrote? My own message never does. `replyToSender`: who wrote the message
// it replies to ("" = no reply, or not known).
[[nodiscard]] bool AddressesMe(
	const QString &me,
	const QString &sender,
	const QStringList &mentioned,
	const QString &replyToSender);
// The first of `seqs` (oldest first) above `afterSeq`, or 0.
[[nodiscard]] qint64 NextMention(const QVector<qint64> &seqs, qint64 afterSeq);

} // namespace MentionRules

namespace GroupAdminLog {

// The sentence an entry reads as. Generic: an action this build does not know.
enum class Form {
	Generic,
	Promote,
	AdminRights,
	Demote,
	Restrict,
	RestrictUntil,
	Unrestrict,
	Remove,
	Ban,
	BanDelete,
	Unban,
	Permissions,
	SlowModeOn,
	SlowModeOff,
	HideMembersOn,
	HideMembersOff,
	Title,
	Photo,
	About,
	Pin,
	Unpin,
	InviteCreate,
	InviteCreateNamed,
	InviteRevoke,
	InviteRevokeNamed,
	JoinApprove,
	JoinDecline,
	DeleteMessage,
	DeleteMessages,   // a bulk delete of several people's messages
	DeleteMessagesOf, // a bulk delete of one person's messages (target set)
	DeleteAll,
	Transfer,
	// edit_tag: somebody's tag (detail tag / prev_tag), or the actor's own.
	TagChanged,
	TagSet,
	TagRemoved,
	OwnTagChanged,
	OwnTagSet,
	OwnTagRemoved,
};
[[nodiscard]] Form FormOf(const GroupAdminLogEntry &entry);
// The sentence names the person the action was done to.
[[nodiscard]] bool NamesTarget(Form form);

// The kinds of action the filter offers, in its order.
enum Category : int {
	Members,
	Admins,
	Settings,
	Messages,
	Links,
	Tags,      // «تغییر تگ‌ها»: edit_tag
	kCategoryCount,
};
[[nodiscard]] QStringList Actions(int category);
// The `actions` parameter for a mask of categories (1 << Category): the
// names as a comma list, or "" when nothing narrows (none or every kind
// picked: an action of a later release is then still listed).
[[nodiscard]] QString ActionsParam(int mask);
// The categories that narrow the list (0: the filter does not narrow).
[[nodiscard]] int PickedCount(int mask);

// The steps of slow mode from client-config `groups.slow_mode.values`
// ("0", "10s", "1m"…, or plain seconds): seconds, ascending, without
// repeats, always starting with 0 (off); the server's defaults when the
// list says nothing usable.
[[nodiscard]] QVector<int> SlowModeSteps(const QStringList &values);

} // namespace GroupAdminLog

// The tag of a member of a group (Telegram's member tags). Every member may
// carry one: one line, at most `groups.admin_title_max_chars` characters. An
// admin's title is their tag. Next to a sender's name it is a badge:
//
//   owner   the tag, or «مالک»: purple text on a purple pill
//   admin   the tag, or «ادمین»: green text on a green pill
//   member  the tag in grey, no pill; no tag: nothing
//
// A message of a group carries its sender's current tag and role
// (`sender_tag`, `sender_role`); the chat screen keeps one map of them per
// group (Data::Groups::senderTag) so that a change shows on old messages too.
namespace MemberTags {

enum class Role {
	Member,
	Admin,
	Owner,
};
// "owner" / "admin"; anything else (a member, a former one, "") is Member.
[[nodiscard]] Role RoleOf(QStringView wire);
// "owner" / "admin" / "member".
[[nodiscard]] QString WireRole(Role role);

// A tag as a badge shows it: one line, emoji removed, blanks collapsed
// (Telegram: RemoveEmoji(SingleLine(text))).
[[nodiscard]] QString Shown(const QString &tag);
// A tag as typed, made fit to send: the same cleaning, trimmed ("" removes it).
[[nodiscard]] QString Typed(const QString &typed);
// At most `max` characters (code points) after cleaning; empty is fine.
[[nodiscard]] bool TypedOk(const QString &typed, int max);
// The emoji of a text taken out (pictographs, flags, keycaps, the joiners
// and modifiers of a sequence).
[[nodiscard]] QString RemoveEmoji(const QString &text);

struct Badge {
	QString text;              // empty: no badge
	Role role = Role::Member;

	[[nodiscard]] bool empty() const {
		return text.isEmpty();
	}
	// Owner and admin badges are pills; a member's is plain grey text.
	[[nodiscard]] bool pill() const {
		return role != Role::Member;
	}
};
// The badge of a sender: the tag (Shown) or the role's own word.
[[nodiscard]] Badge BadgeFor(
	Role role,
	const QString &tag,
	const QString &ownerDefault,
	const QString &adminDefault);
// Only an incoming message of a group that is not a service note has one.
[[nodiscard]] bool HasBadge(bool group, bool mine, bool service);

// What one person is known as in one group.
struct Sender {
	QString tag;
	Role role = Role::Member;

	friend bool operator==(const Sender &a, const Sender &b) {
		return (a.tag == b.tag) && (a.role == b.role);
	}
	friend bool operator!=(const Sender &a, const Sender &b) {
		return !(a == b);
	}
};
// What a message of the server says of its sender. Both fields are left out
// for an ordinary member without a tag and for a former member — which only a
// server that has tags means (`tagAware`); an older one says nothing.
[[nodiscard]] std::optional<Sender> FromMessage(
	const QString &senderRole,
	const QString &senderTag,
	bool tagAware);
// What a member row says (`adminTitle`: the tag of an admin as an older
// server sends it). Nobody who is not in the group has a role or a tag.
[[nodiscard]] Sender FromMember(
	const QString &status,
	const QString &role,
	const QString &tag,
	const QString &adminTitle);

// ---- who may set a tag (the server has the last word)
// My own: the owner and the admins always, a member with `edit_tag` in the
// permissions of my header; nobody who is not in the group.
[[nodiscard]] bool CanEditOwn(
	const QString &myRole,
	bool editTagPermission,
	bool member);
// Somebody else's: the right `manage_tags`; an ordinary member's tag, an
// admin's only for the owner, the owner's for nobody. The target must be in
// the group.
[[nodiscard]] bool CanEditOther(
	const GroupRights::Member &me,
	const GroupRights::Member &target);

// ---- the tag of a row of the member list (Telegram's MemberListRow)
enum class RowMode {
	None,
	Pill,  // an owner or an admin: their tag or the role's word, coloured
	Text,  // a member's tag, grey
	Add,   // my own row without a tag: «افزودن تگ» (light blue)
};
struct RowTag {
	RowMode mode = RowMode::None;
	QString text;
	Role role = Role::Member;
	bool interactive = false; // a click opens the edit box

	friend bool operator==(const RowTag &a, const RowTag &b) {
		return (a.mode == b.mode)
			&& (a.text == b.text)
			&& (a.role == b.role)
			&& (a.interactive == b.interactive);
	}
};
// `canEdit`: I may set this row's tag (CanEditOwn for my row, CanEditOther
// for another one).
[[nodiscard]] RowTag RowTagFor(
	Role role,
	const QString &tag,
	bool self,
	bool canEdit,
	const QString &ownerDefault,
	const QString &adminDefault,
	const QString &addText);

} // namespace MemberTags

} // namespace Data
