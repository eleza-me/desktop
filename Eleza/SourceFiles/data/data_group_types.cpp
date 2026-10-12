#include "data/data_group_types.h"

#include "data/data_json.h"

namespace Data {
namespace {

using namespace Json;

} // namespace

bool Group::can(QStringView permission) const {
	for (const auto &name : permissions) {
		if (name == permission) {
			return true;
		}
	}
	return false;
}

bool Group::hasRight(QStringView right) const {
	for (const auto &name : rights) {
		if (name == right) {
			return true;
		}
	}
	return false;
}

Group Group::FromJson(const QJsonObject &json) {
	auto result = Group();
	result.id = String(json, "group"_L1);
	result.title = Name(json, "title"_L1);
	result.about = String(json, "about"_L1);
	result.photo = String(json, "photo"_L1);
	result.memberCount = Int(json, "member_count"_L1);
	result.state = String(json, "state"_L1);
	result.status = String(json, "status"_L1);
	result.role = String(json, "role"_L1);
	result.rights = Strings(json.value("rights"_L1));
	result.permissions = Strings(json.value("permissions"_L1));
	result.defaultPermissions = Strings(json.value("default_permissions"_L1));
	result.canSend = Bool(json, "can_send"_L1);
	result.sendBlocked = String(json, "send_blocked"_L1);
	result.restrictedUntil = Int64(json, "restricted_until"_L1);
	result.slowModeS = Int(json, "slow_mode_s"_L1);
	result.slowModeWaitS = Int(json, "slow_mode_wait_s"_L1);
	result.historyVisible = Bool(json, "history_visible"_L1);
	result.lastSeq = Int64(json, "last_seq"_L1);
	result.pts = Int64(json, "pts"_L1);
	result.lastReadSeq = Int64(json, "last_read_seq"_L1);
	result.unread = Int(json, "unread"_L1);
	result.unreadCapped = Bool(json, "unread_capped"_L1);
	result.unreadMentions = Int(json, "unread_mentions"_L1);
	result.muted = Bool(json, "muted"_L1);
	result.mutedUntil = Int64(json, "muted_until"_L1);
	result.notify = String(json, "notify"_L1);
	result.archived = Bool(json, "archived"_L1);
	result.pinned = Bool(json, "pinned"_L1);
	result.pins = Int64s(json.value("pins"_L1));
	result.clearedBeforeSeq = Int64(json, "cleared_before_seq"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.joinRequests = Int(json, "join_requests"_L1);
	return result;
}

QJsonObject Group::toJson() const {
	auto result = QJsonObject();
	result.insert("group"_L1, id);
	result.insert("title"_L1, title);
	Put(result, "about"_L1, about);
	Put(result, "photo"_L1, photo);
	result.insert("member_count"_L1, memberCount);
	Put(result, "state"_L1, state);
	Put(result, "status"_L1, status);
	Put(result, "role"_L1, role);
	result.insert("rights"_L1, Array(rights));
	result.insert("permissions"_L1, Array(permissions));
	result.insert("default_permissions"_L1, Array(defaultPermissions));
	result.insert("can_send"_L1, canSend);
	Put(result, "send_blocked"_L1, sendBlocked);
	Put(result, "restricted_until"_L1, restrictedUntil);
	Put(result, "slow_mode_s"_L1, slowModeS);
	Put(result, "slow_mode_wait_s"_L1, slowModeWaitS);
	result.insert("history_visible"_L1, historyVisible);
	Put(result, "last_seq"_L1, lastSeq);
	Put(result, "pts"_L1, pts);
	Put(result, "last_read_seq"_L1, lastReadSeq);
	Put(result, "unread"_L1, unread);
	Put(result, "unread_capped"_L1, unreadCapped);
	Put(result, "unread_mentions"_L1, unreadMentions);
	Put(result, "muted"_L1, muted);
	Put(result, "muted_until"_L1, mutedUntil);
	Put(result, "notify"_L1, notify);
	Put(result, "archived"_L1, archived);
	Put(result, "pinned"_L1, pinned);
	Put(result, "pins"_L1, Array(pins));
	Put(result, "cleared_before_seq"_L1, clearedBeforeSeq);
	Put(result, "created_at"_L1, createdAt);
	Put(result, "join_requests"_L1, joinRequests);
	return result;
}

GroupMember GroupMember::FromJson(const QJsonObject &json) {
	auto result = GroupMember();
	result.user = String(json, "user"_L1);
	result.role = String(json, "role"_L1);
	result.rights = Strings(json.value("rights"_L1));
	result.adminTitle = Name(json, "admin_title"_L1);
	result.status = String(json, "status"_L1);
	result.denied = Strings(json.value("denied"_L1));
	result.restrictedUntil = Int64(json, "restricted_until"_L1);
	result.joinedAt = Int64(json, "joined_at"_L1);
	result.invitedBy = String(json, "invited_by"_L1);
	return result;
}

QJsonObject GroupMember::toJson() const {
	auto result = QJsonObject();
	result.insert("user"_L1, user);
	result.insert("role"_L1, role);
	result.insert("rights"_L1, Array(rights));
	Put(result, "admin_title"_L1, adminTitle);
	Put(result, "status"_L1, status);
	Put(result, "denied"_L1, Array(denied));
	Put(result, "restricted_until"_L1, restrictedUntil);
	Put(result, "joined_at"_L1, joinedAt);
	Put(result, "invited_by"_L1, invitedBy);
	return result;
}

GroupNotAdded GroupNotAdded::FromJson(const QJsonObject &json) {
	auto result = GroupNotAdded();
	result.user = String(json, "user"_L1);
	result.reason = String(json, "reason"_L1);
	return result;
}

GroupInvite GroupInvite::FromJson(const QJsonObject &json) {
	auto result = GroupInvite();
	result.link = String(json, "link"_L1);
	result.url = String(json, "url"_L1);
	result.name = Name(json, "name"_L1);
	result.uses = Int(json, "uses"_L1);
	result.maxUses = Int(json, "max_uses"_L1);
	result.expiresAt = Int64(json, "expires_at"_L1);
	result.requiresApproval = Bool(json, "requires_approval"_L1);
	result.revoked = Bool(json, "revoked"_L1);
	result.expired = Bool(json, "expired"_L1);
	result.createdBy = String(json, "created_by"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	return result;
}

GroupInvitePreview GroupInvitePreview::FromJson(const QJsonObject &json) {
	auto result = GroupInvitePreview();
	result.group = String(json, "group"_L1);
	result.title = Name(json, "title"_L1);
	result.about = String(json, "about"_L1);
	result.photo = String(json, "photo"_L1);
	result.memberCount = Int(json, "member_count"_L1);
	result.requiresApproval = Bool(json, "requires_approval"_L1);
	result.status = String(json, "status"_L1);
	return result;
}

GroupJoinRequest GroupJoinRequest::FromJson(const QJsonObject &json) {
	auto result = GroupJoinRequest();
	result.user = String(json, "user"_L1);
	result.createdAt = Int64(json, "created_at"_L1);
	result.linkName = Name(json, "link_name"_L1);
	return result;
}

} // namespace Data
