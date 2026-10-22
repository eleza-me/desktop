// Typed models of groups (`/v1/groups…`, the `groups.*` methods of the
// realtime connection). Like the models of data_types.h each is parsed once
// from wire JSON; a key the server leaves out reads as false / 0 / "" / [].
//
// A group is not a chat on the server (its own id type "j…", its own routes,
// its own sync track), but it lives in the same chat list: Data::Chat::group
// holds the header of a row whose id is a group id.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonObject>

namespace Data {

// The header of a group as one member sees it (GroupView).
struct Group {
	QString id;
	QString title;
	QString about;
	QString photo;              // media id; the bytes come from MediaRef::GroupPhoto
	int memberCount = 0;
	QString state;              // active
	QString status;             // member (anything else: not in the group any more)
	QString role;               // owner | admin | member
	QString myTag;              // my own tag in the group ("" = none)
	QStringList rights;         // admin rights I hold
	QStringList permissions;    // what I may do now: the UI is enabled from these
	QStringList defaultPermissions; // what an ordinary member may do
	bool canSend = true;
	QString sendBlocked;        // restricted | slow_mode (when !canSend)
	qint64 restrictedUntil = 0; // unix seconds; 0: not restricted, or without an end
	int slowModeS = 0;
	int slowModeWaitS = 0;
	bool historyVisible = true;
	qint64 lastSeq = 0;
	qint64 pts = 0;             // the header's pts (not the pts applied to the cache)
	qint64 lastReadSeq = 0;
	int unread = 0;
	bool unreadCapped = false;
	int unreadMentions = 0;
	bool muted = false;
	qint64 mutedUntil = 0;      // unix seconds
	QString notify;             // all | mentions | none
	bool archived = false;
	bool pinned = false;
	QVector<qint64> pins;       // pinned message seqs, newest pin first
	qint64 clearedBeforeSeq = 0;
	qint64 createdAt = 0;       // ms
	int joinRequests = 0;       // undecided requests, for those who may decide
	int adminCount = 0;         // the owner and the admins together
	bool hideMembers = false;   // the group's setting: ordinary members see only the admins
	bool membersHidden = false; // the member list is limited for me (the setting, and I am a member)

	[[nodiscard]] bool valid() const {
		return !id.isEmpty();
	}
	[[nodiscard]] bool isMember() const {
		return status.isEmpty() || status == u"member";
	}
	[[nodiscard]] bool isOwner() const {
		return role == u"owner";
	}
	// "send_messages", "add_members", … (`permissions`).
	[[nodiscard]] bool can(QStringView permission) const;
	// "ban_users", "invite_users", … (`rights`).
	[[nodiscard]] bool hasRight(QStringView right) const;

	[[nodiscard]] static Group FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct GroupMember {
	QString user;
	QString role;               // owner | admin | member
	QStringList rights;
	QString adminTitle;         // only for the owner and admins (= their tag)
	QString tag;                // everybody's tag ("" = none)
	QString status;             // member | banned (the banned list)
	QStringList denied;         // what a restriction that applies now takes away
	qint64 restrictedUntil = 0; // unix seconds
	qint64 joinedAt = 0;        // ms
	QString invitedBy;

	[[nodiscard]] bool restricted() const {
		return !denied.isEmpty();
	}
	// The tag; for an admin of a server without tags, the admin title.
	[[nodiscard]] QString currentTag() const;
	[[nodiscard]] static GroupMember FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct GroupNotAdded {
	QString user;
	QString reason; // privacy | left | banned | limit | unavailable

	[[nodiscard]] static GroupNotAdded FromJson(const QJsonObject &json);
};

struct GroupInvite {
	QString link;               // the token: "<group id>.<22 characters>"
	QString url;                // what people share
	QString name;
	int uses = 0;
	int maxUses = 0;            // 0: no limit
	qint64 expiresAt = 0;       // unix seconds; 0: never
	bool requiresApproval = false;
	bool revoked = false;
	bool expired = false;       // past its time or used up (still counts until revoked)
	QString createdBy;
	qint64 createdAt = 0;       // ms

	[[nodiscard]] static GroupInvite FromJson(const QJsonObject &json);
};

// What an invite link leads to, for someone who holds it.
struct GroupInvitePreview {
	QString group;              // only when status is "member"
	QString title;
	QString about;
	QString photo;              // the bytes come from MediaRef::InvitePhoto
	int memberCount = 0;
	bool requiresApproval = false;
	QString status;             // joinable | member | pending | banned

	[[nodiscard]] static GroupInvitePreview FromJson(const QJsonObject &json);
};

struct GroupJoinRequest {
	QString user;
	qint64 createdAt = 0;       // ms
	QString linkName;

	[[nodiscard]] static GroupJoinRequest FromJson(const QJsonObject &json);
};

// One entry of the admin log («اقدام‌های اخیر»). Every value of the detail
// is the value after the action; an absent one is the zero one.
struct GroupAdminLogEntry {
	QString id;
	qint64 at = 0;              // ms
	QString actor;
	QString action;             // promote | restrict | slow_mode | … (unknown: generic)
	QString target;             // empty: the action is about nobody
	qint64 seq = 0;             // pin, unpin, delete_message
	QVector<qint64> seqs;       // delete_messages
	QStringList rights;         // promote, admin_rights
	QString adminTitle;
	QStringList denied;         // restrict
	qint64 until = 0;           // restrict: unix seconds, 0 = until lifted
	QStringList defaultPermissions; // permissions
	bool historyVisible = false;
	int slowModeS = 0;          // slow_mode: 0 = off
	bool hideMembers = false;   // hide_members
	QString linkName;           // invite_create, invite_revoke
	bool requiresApproval = false;
	bool deleteMessages = false; // ban
	QString tag;                // edit_tag: the tag now ("" = removed)…
	QString prevTag;            // …and before ("" = there was none)

	[[nodiscard]] static GroupAdminLogEntry FromJson(const QJsonObject &json);
};

// Who reacted to a message, and with what.
struct GroupReaction {
	QString user;
	QString emoji;
	qint64 at = 0;              // ms

	[[nodiscard]] static GroupReaction FromJson(const QJsonObject &json);
};

// A request of a bulk decision that was not decided.
struct GroupDecideFailure {
	QString user;
	QString reason;             // not_found | full | limit | banned

	[[nodiscard]] static GroupDecideFailure FromJson(const QJsonObject &json);
};

// Somebody who joined through an invite link and is a member now.
struct GroupInviteMember {
	QString user;
	qint64 joinedAt = 0;        // ms

	[[nodiscard]] static GroupInviteMember FromJson(const QJsonObject &json);
};

} // namespace Data
