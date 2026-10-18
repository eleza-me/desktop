#include "data/data_group_rules.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

#include <algorithm>
#include <array>

namespace Data {
namespace {

[[nodiscard]] bool IsAsciiLetterOrDigit(QChar ch) {
	const auto code = ch.unicode();
	return (code >= 'A' && code <= 'Z')
		|| (code >= 'a' && code <= 'z')
		|| (code >= '0' && code <= '9');
}

[[nodiscard]] int CodePoints(const QString &text) {
	auto result = 0;
	for (auto i = 0; i < text.size(); ++i) {
		if (text[i].isHighSurrogate()
			&& i + 1 < text.size()
			&& text[i + 1].isLowSurrogate()) {
			++i;
		}
		++result;
	}
	return result;
}

// The first `count` code points of the text (never half of a pair).
[[nodiscard]] QString LeftCodePoints(const QString &text, int count) {
	auto units = 0;
	for (auto taken = 0; units < text.size() && taken < count; ++taken) {
		if (text[units].isHighSurrogate()
			&& units + 1 < text.size()
			&& text[units + 1].isLowSurrogate()) {
			++units;
		}
		++units;
	}
	return text.left(units);
}

[[nodiscard]] QString NoteCode(const QString &system) {
	const auto cut = system.indexOf(u':');
	return ((cut < 0) ? system : system.left(cut)).trimmed().toLower();
}

[[nodiscard]] QString NoteArgument(const QString &system) {
	const auto cut = system.indexOf(u':');
	return (cut < 0) ? QString() : system.mid(cut + 1).trimmed();
}

} // namespace

bool IsGroupId(QStringView id) {
	return id.size() > 1 && id[0] == u'j';
}

namespace GroupRules {

FrameAction FrameActionFor(qint64 storedPts, qint64 framePts, bool hasData) {
	if (framePts <= storedPts) {
		return FrameAction::Drop;
	}
	return (framePts == storedPts + 1 && hasData)
		? FrameAction::Apply
		: FrameAction::CatchUp;
}

SendAction SendActionFor(
		qint64 storedPts,
		qint64 answerPts,
		bool duplicate,
		bool cached) {
	if (duplicate) {
		return cached ? SendAction::Keep : SendAction::CatchUp;
	} else if (answerPts == storedPts + 1) {
		return SendAction::Advance;
	}
	return (answerPts <= storedPts) ? SendAction::Keep : SendAction::CatchUp;
}

SyncAction SyncActionFor(
		qint64 sentPts,
		qint64 heldPts,
		qint64 entryPts,
		bool hasCache) {
	if (heldPts < 0) {
		return SyncAction::Take;
	} else if (heldPts != sentPts && entryPts < heldPts) {
		// Raced by live frames.
		return SyncAction::Skip;
	} else if (entryPts < heldPts) {
		return hasCache ? SyncAction::Rewind : SyncAction::Take;
	} else if (!hasCache) {
		return SyncAction::Take;
	}
	return SyncAction::Behind;
}

int UnreadFromDifference(
		const QVector<Change> &changes,
		qint64 lastSeqBefore,
		qint64 lastReadSeq) {
	auto result = 0;
	for (const auto &change : changes) {
		if (change.isNew
			&& !change.mine
			&& !change.deleted
			&& change.seq > lastSeqBefore
			&& change.seq > lastReadSeq) {
			++result;
		}
	}
	return result;
}

qint64 ReadTarget(qint64 seenSeq, qint64 lastReadSeq, qint64 lastSeq) {
	const auto seq = std::min(seenSeq, lastSeq);
	return (seq > lastReadSeq) ? seq : 0;
}

Note NoteOf(const QString &system) {
	const auto code = NoteCode(system);
	if (code == u"group_created") {
		return Note::Created;
	} else if (code == u"member_added") {
		return Note::Added;
	} else if (code == u"member_left") {
		return Note::Left;
	} else if (code == u"member_removed") {
		return Note::Removed;
	} else if (code == u"title_changed") {
		return Note::TitleChanged;
	} else if (code == u"photo_changed") {
		return Note::PhotoChanged;
	} else if (code == u"member_joined") {
		return Note::Joined;
	} else if (code == u"pinned") {
		return (NotePinnedSeq(system) > 0) ? Note::Pinned : Note::None;
	}
	return Note::None;
}

QString NoteUser(const QString &system) {
	const auto code = NoteCode(system);
	return (code == u"member_added" || code == u"member_removed")
		? NoteArgument(system)
		: QString();
}

qint64 NotePinnedSeq(const QString &system) {
	if (NoteCode(system) != u"pinned") {
		return 0;
	}
	auto ok = false;
	const auto seq = NoteArgument(system).toLongLong(&ok);
	return (ok && seq > 0) ? seq : 0;
}

int MemberDelta(const QString &system) {
	switch (NoteOf(system)) {
	case Note::Added:
	case Note::Joined: return 1;
	case Note::Left:
	case Note::Removed: return -1;
	default: break;
	}
	return 0;
}

bool HeaderChanged(const QString &system) {
	const auto note = NoteOf(system);
	return note == Note::TitleChanged || note == Note::PhotoChanged;
}

QString FirstName(const QString &displayName) {
	auto name = displayName.trimmed();
	for (auto i = 0; i < name.size(); ++i) {
		if (name[i].isSpace()) {
			name = name.left(i);
			break;
		}
	}
	return (CodePoints(name) > 24) ? LeftCodePoints(name, 24) : name;
}

int NameColorIndex(QStringView userId, int size) {
	if (size <= 1 || userId.isEmpty()) {
		return 0;
	}
	auto hash = quint32(2166136261U);
	for (const auto ch : userId) {
		hash ^= ch.unicode();
		hash *= 16777619U;
	}
	return int(hash % quint32(size));
}

bool CanDeleteForEveryone(
		bool mine,
		bool serviceNote,
		qint64 seq,
		qint64 sentAtMs,
		qint64 nowMs,
		qint64 ownWindowMs,
		bool deleteRight) {
	if (serviceNote || seq <= 0) {
		return false;
	} else if (deleteRight) {
		return true;
	} else if (!mine) {
		return false;
	}
	return ownWindowMs <= 0 || sentAtMs <= 0 || (nowMs - sentAtMs) < ownWindowMs;
}

Composer ComposerFor(
		bool canSend,
		const QString &sendBlocked,
		qint64 waitUntilMs,
		qint64 nowMs) {
	if (canSend) {
		return Composer::Compose;
	} else if (sendBlocked == u"slow_mode") {
		return (waitUntilMs > nowMs) ? Composer::Wait : Composer::Compose;
	}
	return Composer::Restricted;
}

qint64 WaitSeconds(qint64 waitUntilMs, qint64 nowMs) {
	const auto left = waitUntilMs - nowMs;
	return (left <= 0) ? 0 : ((left + 999) / 1000);
}

std::optional<TimeLeft> RestrictionLeft(qint64 untilS, qint64 nowMs) {
	if (untilS <= 0) {
		return std::nullopt;
	}
	const auto left = untilS * 1000 - nowMs;
	if (left <= 0) {
		return std::nullopt;
	}
	const auto minutes = (left + 59'999) / 60'000;
	if (minutes < 60) {
		return TimeLeft{ minutes, TimeUnit::Minutes };
	}
	const auto hours = (minutes + 59) / 60;
	if (hours < 48) {
		return TimeLeft{ hours, TimeUnit::Hours };
	}
	return TimeLeft{ (hours + 23) / 24, TimeUnit::Days };
}

bool CanAdd(bool addPermission, int memberCount, int maxMembers) {
	return addPermission && memberCount < maxMembers;
}

int AddRoom(int memberCount, int maxMembers, int batchMax) {
	return std::max(0, std::min(batchMax, maxMembers - memberCount));
}

bool MatchesMember(
		const QString &name,
		const QString &handle,
		const QString &query) {
	auto needle = query.trimmed().toLower();
	if (needle.startsWith(u'@')) {
		needle = needle.mid(1);
	}
	if (needle.isEmpty()) {
		return true;
	}
	return name.toLower().contains(needle) || handle.toLower().contains(needle);
}

QString CleanTitle(const QString &typed) {
	return typed.simplified();
}

bool TitleOk(const QString &typed, int max) {
	const auto count = CodePoints(CleanTitle(typed));
	return count >= 1 && count <= max;
}

} // namespace GroupRules

namespace GroupRights {
namespace {

[[nodiscard]] int MaskOf(const QStringList &names, const QStringList &table) {
	auto result = 0;
	for (const auto &name : names) {
		const auto index = table.indexOf(name);
		if (index >= 0) {
			result |= (1 << index);
		}
	}
	return result;
}

[[nodiscard]] QStringList ListOf(int mask, const QStringList &table) {
	auto result = QStringList();
	for (auto i = 0; i != table.size(); ++i) {
		if (mask & (1 << i)) {
			result.push_back(table[i]);
		}
	}
	return result;
}

} // namespace

const QStringList &RightNames() {
	static const auto kResult = QStringList{
		u"change_info"_s,
		u"delete_messages"_s,
		u"ban_users"_s,
		u"invite_users"_s,
		u"pin_messages"_s,
		u"add_admins"_s,
	};
	return kResult;
}

const QStringList &PermissionNames() {
	static const auto kResult = QStringList{
		u"send_messages"_s,
		u"send_media"_s,
		u"send_links"_s,
		u"add_members"_s,
		u"pin_messages"_s,
		u"change_info"_s,
	};
	return kResult;
}

int RightsMask(const QStringList &names) {
	return MaskOf(names, RightNames());
}

int PermissionsMask(const QStringList &names) {
	return MaskOf(names, PermissionNames());
}

QStringList RightsList(int mask) {
	return ListOf(mask, RightNames());
}

QStringList PermissionsList(int mask) {
	return ListOf(mask, PermissionNames());
}

Member Member::Of(const QString &user, const QString &role, int granted) {
	auto result = Member();
	result.user = user;
	result.status = u"member"_s;
	result.role = role;
	result.granted = granted;
	return result;
}

int Rights(const Member &member) {
	if (!member.in()) {
		return 0;
	} else if (member.role == u"owner") {
		return kRightsAll;
	} else if (member.role == u"admin") {
		return member.granted & kRightsAll;
	}
	return 0;
}

int Permissions(const Member &member, int defaults) {
	if (!member.in()) {
		return 0;
	} else if (member.role == u"owner") {
		return kPermissionsAll;
	} else if (member.role == u"admin") {
		auto result = defaults | kPermissionsSend;
		const auto rights = Rights(member);
		if (rights & kChangeInfo) {
			result |= kChangeInfoPermission;
		}
		if (rights & kPinMessagesRight) {
			result |= kPinMessages;
		}
		if (rights & kInviteUsers) {
			result |= kAddMembers;
		}
		return result & kPermissionsAll;
	}
	auto result = defaults;
	if (member.restricted) {
		result &= ~member.denied;
	}
	return result & kPermissionsAll;
}

Action ActSend(int permissions) {
	return Action{ permissions & kPermissionsSend, 0, false };
}

int Needs(const QString &kind, bool linkPreview, bool textLink) {
	auto result = int(kSendMessages);
	if (kind != u"text" && kind != u"forwarded_post") {
		result |= kSendMedia;
	}
	if (linkPreview || textLink) {
		result |= kSendLinks;
	}
	return result;
}

bool Can(int defaults, const Member &member, const Action &action) {
	if (!member.in()) {
		return false;
	} else if (member.role == u"owner") {
		return true;
	} else if (action.owner) {
		return false;
	}
	return ((Permissions(member, defaults) & action.permission) == action.permission)
		&& ((Rights(member) & action.right) == action.right);
}

bool CanActOn(
		int defaults,
		const Member &actor,
		const Member &target,
		const Action &action) {
	if (!Can(defaults, actor, action)) {
		return false;
	} else if (!actor.user.isEmpty() && actor.user == target.user) {
		return false;
	} else if (!target.in()) {
		return true;
	} else if (target.role == u"owner") {
		return false;
	} else if (target.role == u"admin") {
		return actor.role == u"owner";
	}
	return true;
}

int Grantable(const Member &actor) {
	return Rights(actor);
}

bool CanPromote(const Member &me, const Member &target, int admins, int maxAdmins) {
	return target.in()
		&& target.role == u"member"
		&& admins < maxAdmins
		&& CanActOn(0, me, target, kActAddAdmins);
}

bool CanEditAdmin(const Member &me, const Member &target) {
	return target.in()
		&& target.role == u"admin"
		&& CanActOn(0, me, target, kActAddAdmins);
}

bool CanRestrict(const Member &me, const Member &target) {
	return target.in()
		&& target.role == u"member"
		&& CanActOn(0, me, target, kActBan);
}

bool CanBan(const Member &me, const Member &target) {
	return CanActOn(0, me, target, kActBan);
}

} // namespace GroupRights

namespace GroupInviteLinks {
namespace {

constexpr auto kSecretLength = 22;

// The server's prefix as compared here: lower case, no scheme, no "www.",
// ending in a slash; empty for none.
[[nodiscard]] QString NormalBase(const QString &base) {
	auto result = base.trimmed().toLower();
	if (result.startsWith(u"https://")) {
		result = result.mid(8);
	} else if (result.startsWith(u"http://")) {
		result = result.mid(7);
	}
	if (result.startsWith(u"www.")) {
		result = result.mid(4);
	}
	if (result.isEmpty()) {
		return QString();
	}
	return result.endsWith(u'/') ? result : (result + u'/');
}

} // namespace

QString DefaultBase() {
	return u"eleza.me/g/"_s;
}

bool IsToken(const QString &text) {
	const auto dot = text.lastIndexOf(u'.');
	if (dot <= 0 || text.size() - dot - 1 != kSecretLength) {
		return false;
	}
	const auto group = QStringView(text).left(dot);
	if (!IsGroupId(group)) {
		return false;
	}
	for (auto i = 1; i < group.size(); ++i) {
		if (!IsAsciiLetterOrDigit(group[i])) {
			return false;
		}
	}
	for (auto i = dot + 1; i < text.size(); ++i) {
		const auto ch = text[i];
		if (!IsAsciiLetterOrDigit(ch) && ch != u'-' && ch != u'_') {
			return false;
		}
	}
	return true;
}

QString GroupOf(const QString &token) {
	return IsToken(token) ? token.left(token.lastIndexOf(u'.')) : QString();
}

QString Token(const QString &link, const QString &base) {
	const auto text = link.trimmed();
	if (text.isEmpty()) {
		return QString();
	} else if (IsToken(text)) {
		return text;
	}
	const auto lower = text.toLower();
	auto rest = QString();
	auto found = false;
	const auto own = u"eleza://g/"_s;
	if (lower.startsWith(own)) {
		rest = text.mid(own.size());
		found = true;
	} else {
		auto skip = lower.startsWith(u"https://")
			? 8
			: lower.startsWith(u"http://")
			? 7
			: 0;
		if (QStringView(lower).mid(skip).startsWith(u"www.")) {
			skip += 4;
		}
		for (const auto &prefix : { NormalBase(base), DefaultBase() }) {
			if (!prefix.isEmpty() && QStringView(lower).mid(skip).startsWith(prefix)) {
				rest = text.mid(skip + prefix.size());
				found = true;
				break;
			}
		}
	}
	if (!found) {
		return QString();
	}
	for (auto i = 0; i < rest.size(); ++i) {
		const auto ch = rest[i];
		if (ch == u'?' || ch == u'#' || ch == u'/' || ch.isSpace()) {
			rest = rest.left(i);
			break;
		}
	}
	// A link pasted at the end of a sentence: the token itself never ends in these.
	static const auto kTrailing = u".,;:!)»]،؛"_s;
	while (!rest.isEmpty() && !IsToken(rest) && kTrailing.contains(rest.back())) {
		rest.chop(1);
	}
	return IsToken(rest) ? rest : QString();
}

QString Url(const QString &token, const QString &base) {
	const auto prefix = NormalBase(base);
	return u"https://"_s + (prefix.isEmpty() ? DefaultBase() : prefix) + token;
}

} // namespace GroupInviteLinks

namespace MentionRules {
namespace {

// What may stand right before the «@» of a mention.
[[nodiscard]] bool StartsWord(QChar before) {
	if (before.isSpace()) {
		return true;
	} else if (before.isLetterOrNumber()) {
		return false;
	}
	return before != u'@'
		&& before != u'_'
		&& before != u'.'
		&& before != u'/'
		&& before != u':';
}

} // namespace

std::optional<Query> QueryAt(const QString &text, int cursor) {
	if (cursor <= 0 || cursor > text.size()) {
		return std::nullopt;
	}
	const auto from = std::max(0, cursor - kQueryMax - 1);
	for (auto i = cursor - 1; i >= from; --i) {
		const auto ch = text[i];
		if (ch == u'@') {
			if (i > 0 && !StartsWord(text[i - 1])) {
				return std::nullopt;
			}
			return Query{ i, text.mid(i + 1, cursor - i - 1) };
		} else if (ch.isSpace()) {
			return std::nullopt;
		}
	}
	return std::nullopt;
}

QString Label(
		const QString &displayName,
		const QString &handle,
		const QString &fallback) {
	auto result = displayName.simplified();
	if (result.isEmpty()) {
		result = handle.trimmed();
	}
	if (result.isEmpty()) {
		result = fallback.trimmed();
	}
	if (CodePoints(result) > kLabelMax) {
		result = LeftCodePoints(result, kLabelMax).trimmed();
	}
	return result;
}

bool Room(const QStringList &users, const QString &next, int max) {
	if (max <= 0) {
		return false;
	}
	const auto seen = QSet<QString>(users.cbegin(), users.cend());
	return seen.contains(next) || seen.size() < max;
}

std::optional<Range> ToWire(int from, int till, const QString &typed) {
	const auto size = int(typed.size());
	if (from < 0 || till <= from || till > size) {
		return std::nullopt;
	}
	auto lead = 0;
	while (lead < size && typed[lead].isSpace()) {
		++lead;
	}
	const auto sent = typed.trimmed();
	const auto length = int(sent.size());
	auto start = std::max(0, from - lead);
	auto end = std::min(length, till - lead);
	if (start > 0
		&& start < length
		&& sent[start].isLowSurrogate()
		&& sent[start - 1].isHighSurrogate()) {
		--start;
	}
	if (end > 0
		&& end < length
		&& sent[end].isLowSurrogate()
		&& sent[end - 1].isHighSurrogate()) {
		++end;
	}
	return (end > start)
		? std::make_optional(Range{ start, end - start })
		: std::nullopt;
}

std::optional<Range> Clip(int offset, int length, int textLength) {
	if (offset < 0 || length <= 0 || offset >= textLength) {
		return std::nullopt;
	}
	const auto end = std::min(qint64(offset) + length, qint64(textLength));
	return Range{ offset, int(end - offset) };
}

bool AddressesMe(
		const QString &me,
		const QString &sender,
		const QStringList &mentioned,
		const QString &replyToSender) {
	if (me.isEmpty() || me == sender) {
		return false;
	} else if (me == replyToSender) {
		return true;
	}
	return mentioned.contains(me);
}

qint64 NextMention(const QVector<qint64> &seqs, qint64 afterSeq) {
	for (const auto seq : seqs) {
		if (seq > afterSeq) {
			return seq;
		}
	}
	return 0;
}

} // namespace MentionRules

namespace GroupAdminLog {
namespace {

[[nodiscard]] const std::array<QStringList, kCategoryCount> &Names() {
	static const auto result = std::array<QStringList, kCategoryCount>{
		QStringList{
			u"restrict"_s, u"unrestrict"_s, u"remove"_s, u"ban"_s,
			u"unban"_s, u"join_approve"_s, u"join_decline"_s },
		QStringList{
			u"promote"_s, u"admin_rights"_s, u"demote"_s, u"transfer"_s },
		QStringList{
			u"permissions"_s, u"slow_mode"_s, u"hide_members"_s,
			u"title"_s, u"photo"_s, u"about"_s },
		QStringList{
			u"pin"_s, u"unpin"_s, u"delete_message"_s,
			u"delete_messages"_s, u"delete_all_messages"_s },
		QStringList{ u"invite_create"_s, u"invite_revoke"_s },
	};
	return result;
}

// Seconds of one value of the list: "0", "30", "10s", "1m", "1h30m".
[[nodiscard]] int StepSeconds(const QString &value) {
	static const auto kPart = QRegularExpression(u"(\\d+)(s|m|h|d)"_s);
	const auto text = value.trimmed();
	if (text.isEmpty()) {
		return -1;
	}
	auto ok = false;
	const auto plain = text.toLongLong(&ok);
	if (ok) {
		return (plain >= 0 && plain <= 7 * 86400) ? int(plain) : -1;
	}
	auto total = qint64(0);
	auto consumed = qsizetype(0);
	auto i = kPart.globalMatch(text);
	while (i.hasNext()) {
		const auto match = i.next();
		if (match.capturedStart() != consumed) {
			return -1;
		}
		consumed = match.capturedEnd();
		const auto number = match.captured(1).toLongLong();
		const auto unit = match.captured(2);
		total += number * ((unit == u"d")
			? 86400
			: (unit == u"h")
			? 3600
			: (unit == u"m")
			? 60
			: 1);
		if (total > 7 * 86400) {
			return -1;
		}
	}
	return (consumed == text.size()) ? int(total) : -1;
}

} // namespace

Form FormOf(const GroupAdminLogEntry &entry) {
	const auto &action = entry.action;
	const auto named = !entry.linkName.trimmed().isEmpty();
	if (action == u"promote") {
		return Form::Promote;
	} else if (action == u"admin_rights") {
		return Form::AdminRights;
	} else if (action == u"demote") {
		return Form::Demote;
	} else if (action == u"restrict") {
		return (entry.until > 0) ? Form::RestrictUntil : Form::Restrict;
	} else if (action == u"unrestrict") {
		return Form::Unrestrict;
	} else if (action == u"remove") {
		return Form::Remove;
	} else if (action == u"ban") {
		return entry.deleteMessages ? Form::BanDelete : Form::Ban;
	} else if (action == u"unban") {
		return Form::Unban;
	} else if (action == u"permissions") {
		return Form::Permissions;
	} else if (action == u"slow_mode") {
		return (entry.slowModeS > 0) ? Form::SlowModeOn : Form::SlowModeOff;
	} else if (action == u"hide_members") {
		return entry.hideMembers ? Form::HideMembersOn : Form::HideMembersOff;
	} else if (action == u"title") {
		return Form::Title;
	} else if (action == u"photo") {
		return Form::Photo;
	} else if (action == u"about") {
		return Form::About;
	} else if (action == u"pin") {
		return Form::Pin;
	} else if (action == u"unpin") {
		return Form::Unpin;
	} else if (action == u"invite_create") {
		return named ? Form::InviteCreateNamed : Form::InviteCreate;
	} else if (action == u"invite_revoke") {
		return named ? Form::InviteRevokeNamed : Form::InviteRevoke;
	} else if (action == u"join_approve") {
		return Form::JoinApprove;
	} else if (action == u"join_decline") {
		return Form::JoinDecline;
	} else if (action == u"delete_message") {
		return Form::DeleteMessage;
	} else if (action == u"delete_messages") {
		// The sender is named only when every message was that one person's.
		return entry.target.isEmpty()
			? Form::DeleteMessages
			: Form::DeleteMessagesOf;
	} else if (action == u"delete_all_messages") {
		return Form::DeleteAll;
	} else if (action == u"transfer") {
		return Form::Transfer;
	}
	return Form::Generic;
}

bool NamesTarget(Form form) {
	switch (form) {
	case Form::Promote:
	case Form::AdminRights:
	case Form::Demote:
	case Form::Restrict:
	case Form::RestrictUntil:
	case Form::Unrestrict:
	case Form::Remove:
	case Form::Ban:
	case Form::BanDelete:
	case Form::Unban:
	case Form::JoinApprove:
	case Form::JoinDecline:
	case Form::DeleteMessage:
	case Form::DeleteMessagesOf:
	case Form::DeleteAll:
	case Form::Transfer:
		return true;
	default:
		return false;
	}
}

QStringList Actions(int category) {
	return (category >= 0 && category < kCategoryCount)
		? Names()[category]
		: QStringList();
}

QString ActionsParam(int mask) {
	const auto picked = PickedCount(mask);
	if (!picked) {
		return QString();
	}
	auto result = QStringList();
	for (auto i = 0; i != kCategoryCount; ++i) {
		if (mask & (1 << i)) {
			result += Names()[i];
		}
	}
	return result.join(u',');
}

int PickedCount(int mask) {
	auto result = 0;
	for (auto i = 0; i != kCategoryCount; ++i) {
		if (mask & (1 << i)) {
			++result;
		}
	}
	return (result == kCategoryCount) ? 0 : result;
}

QVector<int> SlowModeSteps(const QStringList &values) {
	auto result = QVector<int>{ 0 };
	for (const auto &value : values) {
		const auto seconds = StepSeconds(value);
		if (seconds > 0 && !result.contains(seconds)) {
			result.push_back(seconds);
		}
	}
	if (result.size() == 1) {
		result = { 0, 10, 30, 60, 300, 900, 3600 };
	}
	std::sort(result.begin(), result.end());
	return result;
}

} // namespace GroupAdminLog

} // namespace Data
