#include "data/data_groups.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "base/unixtime.h"
#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_json.h"
#include "data/data_messages.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"

#include <QtCore/QUrl>

#include <algorithm>

namespace Data {
namespace {

using GroupRules::FrameAction;
using GroupRules::SendAction;
using GroupRules::SyncAction;

constexpr auto kUnknownAskEveryMs = qint64(60) * 1000;
constexpr auto kColdStartEveryMs = qint64(10) * 60 * 1000;
constexpr auto kMaxUnknownAsked = 500;

[[nodiscard]] qint64 NowMs() {
	return base::unixtime::nowMs();
}

// 2 s, doubling to about half a minute.
[[nodiscard]] qint64 BackoffMs(int fails) {
	return (qint64(2000) << std::clamp(fails, 1, 5)) / 2;
}

[[nodiscard]] QString Encoded(const QString &segment) {
	return QString::fromLatin1(QUrl::toPercentEncoding(segment));
}

[[nodiscard]] QStringList MentionedUsers(const Message &message) {
	auto result = QStringList();
	for (const auto &entity : message.entities) {
		if (entity.type == u"mention" && !entity.user.isEmpty()) {
			result.push_back(entity.user);
		}
	}
	return result;
}

// What a header says about the viewer, whatever its pts.
void TakePersonal(Group &to, const Group &from) {
	to.status = from.status;
	to.role = from.role;
	to.rights = from.rights;
	to.permissions = from.permissions;
	to.canSend = from.canSend;
	to.sendBlocked = from.sendBlocked;
	to.restrictedUntil = from.restrictedUntil;
	to.slowModeS = from.slowModeS;
	to.slowModeWaitS = from.slowModeWaitS;
	to.muted = from.muted;
	to.mutedUntil = from.mutedUntil;
	to.notify = from.notify;
	to.archived = from.archived;
	to.pinned = from.pinned;
	to.clearedBeforeSeq = std::max(to.clearedBeforeSeq, from.clearedBeforeSeq);
	to.joinRequests = from.joinRequests;
	to.lastReadSeq = std::max(to.lastReadSeq, from.lastReadSeq);
}

// What a header says about the group as of its pts (not the counters).
void TakeProfile(Group &to, const Group &from) {
	to.title = from.title;
	to.about = from.about;
	to.photo = from.photo;
	to.memberCount = from.memberCount;
	to.state = from.state;
	to.defaultPermissions = from.defaultPermissions;
	to.historyVisible = from.historyVisible;
	to.pins = from.pins;
	to.createdAt = from.createdAt;
}

// The fields of a row that repeat its header.
void SyncRowWithHeader(Chat &row) {
	if (!row.group) {
		return;
	}
	const auto &group = *row.group;
	row.isGroup = true;
	row.section = u"message"_s;
	row.title = group.title;
	row.muted = group.muted;
	row.mutedUntil = group.mutedUntil;
	row.pinned = group.pinned;
	row.archived = group.archived;
	row.canSend = group.canSend;
	row.state = group.state;
	row.lastSeq = group.lastSeq;
	row.lastReadSeq = group.lastReadSeq;
	row.unread = group.unread;
	row.unreadCapped = group.unreadCapped;
	row.unreadMentions = group.unreadMentions;
	row.info = Groups::InfoFor(group);
}

} // namespace

Groups::Groups(Session *session)
: QObject(session)
, _session(session) {
	qRegisterMetaType<Data::Message>();
	if (const auto config = _session->clientConfig()) {
		_wasEnabled = config->groupsEnabled();
		connect(config, &Api::ClientConfig::changed, this, [this] {
			const auto now = enabled();
			if (_wasEnabled != now) {
				_wasEnabled = now;
				Q_EMIT enabledChanged();
				if (now) {
					requestSync();
				}
			}
		});
	}
}

// ---- gates and limits

bool Groups::enabled() const {
	const auto config = _session->clientConfig();
	return config && config->groupsEnabled();
}

bool Groups::administration() const {
	const auto config = _session->clientConfig();
	return config && config->groupsEnabled() && config->groupsAdministration();
}

int Groups::maxMembers() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.max_members"_s, 5000)) : 5000;
}

int Groups::maxInitialMembers() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.create.max_initial_members"_s, 200)) : 200;
}

int Groups::addBatchMax() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.members.add_batch_max"_s, 100)) : 100;
}

int Groups::titleMaxChars() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.title_max_chars"_s, 128)) : 128;
}

int Groups::aboutMaxChars() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.about_max_chars"_s, 255)) : 255;
}

int Groups::membersPageSize() const {
	const auto config = _session->clientConfig();
	const auto size = config ? int(config->integer(u"groups.members.page_size"_s, 100)) : 100;
	return std::clamp(size, 1, 100);
}

int Groups::maxAdmins() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.max_admins"_s, 50)) : 50;
}

int Groups::adminTitleMaxChars() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.admin_title_max_chars"_s, 16)) : 16;
}

int Groups::maxInvites() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.invites.max_active_per_group"_s, 100)) : 100;
}

int Groups::inviteNameMaxChars() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.invites.name_max_chars"_s, 32)) : 32;
}

qint64 Groups::inviteMaxExpiry() const {
	const auto fallback = qint64(365) * 24 * 3600;
	const auto config = _session->clientConfig();
	return config ? config->seconds(u"groups.invites.max_expiry"_s, fallback) : fallback;
}

int Groups::maxMentionsPerMessage() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.mentions.max_per_message"_s, 50)) : 50;
}

int Groups::bulkDeleteMax() const {
	const auto config = _session->clientConfig();
	return config ? int(config->integer(u"groups.bulk_delete.max_messages"_s, 100)) : 100;
}

qint64 Groups::deleteOwnWindow() const {
	const auto config = _session->clientConfig();
	return config ? std::max(config->seconds(u"groups.delete_own_window"_s, 0), qint64(0)) : 0;
}

QString Groups::inviteLinkBase() const {
	const auto config = _session->clientConfig();
	const auto base = config ? config->string(u"groups.invites.link_base"_s) : QString();
	return base.isEmpty() ? GroupInviteLinks::DefaultBase() : base;
}

// ---- rows

const Group *Groups::group(const QString &id) const {
	const auto row = _session->chats().chat(id);
	return (row && row->isGroup && row->group) ? &*row->group : nullptr;
}

qint64 Groups::pts(const QString &id) const {
	const auto row = _session->chats().chat(id);
	return (row && row->isGroup) ? row->groupPts : -1;
}

bool Groups::held(const QString &group) const {
	return pts(group) >= 0;
}

bool Groups::attached(const Chat &row) const {
	return row.groupCachePts >= 0 && row.groupCachePts == row.groupPts;
}

Groups::State &Groups::state(const QString &group) {
	return _states[group];
}

void Groups::forgetState(const QString &group) {
	_states.remove(group);
	_slowModeUntil.remove(group);
	_headerRequests.remove(group);
}

qint64 Groups::slowModeUntil(const QString &group) const {
	return _slowModeUntil.value(group);
}

ChatInfo Groups::InfoFor(const Group &group) {
	auto result = ChatInfo();
	result.chat = group.id;
	result.kind = u"group"_s;
	result.state = group.state;
	result.lastSeq = group.lastSeq;
	result.lastReadSeq = group.lastReadSeq;
	result.muted = group.muted;
	result.mutedUntil = group.mutedUntil;
	result.archived = group.archived;
	result.canSend = group.canSend;
	result.pins = group.pins;
	std::sort(result.pins.begin(), result.pins.end());
	return result;
}

Chat Groups::rowFor(const Group &header, const Chat *previous) const {
	auto result = previous ? *previous : Chat();
	result.id = header.id;
	result.group = header;
	SyncRowWithHeader(result);
	if (!previous) {
		result.lastActivityAt = header.createdAt;
	}
	// The group on screen is read as it is shown: a header that was read
	// before the mark reached the server would bring the count back.
	if (previous
		&& _session->chats().visibleChat() == header.id
		&& previous->lastReadSeq >= header.lastSeq) {
		result.unread = 0;
		result.unreadCapped = false;
		result.unreadMentions = 0;
	}
	return result;
}

// ---- lifecycle

void Groups::start() {
	_started = true;
}

void Groups::stop() {
	_started = false;
	_states.clear();
	_unknownAsked.clear();
	_headerRequests.clear();
	_syncing = false;
	_syncAgain = false;
}

// ---- live frames

void Groups::handleFrame(const QJsonObject &frame) {
	if (!_started || !enabled()) {
		return;
	}
	const auto group = Json::String(frame, "group"_L1);
	if (!IsGroupId(group)) {
		return;
	}
	if (const auto i = _states.find(group); i != _states.end() && i->busy) {
		// A difference of this group is on its way: afterwards (most of these
		// will be old by then; they only say what the next difference will).
		if (i->pending.size() >= kMaxPendingFrames) {
			i->pending.clear();
			i->again = true;
		}
		i->pending.push_back(frame);
		return;
	}
	applyRule(group, frame);
}

void Groups::applyRule(const QString &group, const QJsonObject &frame) {
	const auto row = _session->chats().chat(group);
	if (!row || !row->isGroup) {
		// A membership this device has not heard of yet: the catch-up brings
		// the group with its row. Once a minute per group at most — every
		// frame of a busy group would ask again.
		const auto now = NowMs();
		if (const auto asked = _unknownAsked.value(group); asked && now - asked < kUnknownAskEveryMs) {
			return;
		}
		if (_unknownAsked.size() > kMaxUnknownAsked) {
			_unknownAsked.clear();
		}
		_unknownAsked.insert(group, now);
		syncForFrame();
		return;
	}
	const auto data = frame.value("data"_L1);
	const auto pts = Json::Int64(frame, "pts"_L1);
	switch (GroupRules::FrameActionFor(row->groupPts, pts, data.isObject())) {
	case FrameAction::Drop:
		return;
	case FrameAction::Apply:
		if (applyFrame(group, pts, Json::String(frame, "kind"_L1), data.toObject())) {
			return;
		}
		break;
	case FrameAction::CatchUp:
		break;
	}
	catchUp(group);
}

// Frames were lost (or one could not be applied): ask what changed.
void Groups::catchUp(const QString &group) {
	const auto row = _session->chats().chat(group);
	if (!row) {
		return;
	}
	// A difference repairs cached messages. Without a timeline in memory
	// there is nothing to repair now: the catch-up call returns the header
	// with the last message, and the cache follows when the group is opened.
	if (row->groupCachePts >= 0 && _session->messages().held(group)) {
		startDifference(group);
	} else {
		syncForFrame();
	}
}

bool Groups::addressesMe(const QString &group, const Message &message) const {
	if (!administration() || message.deleted) {
		return false;
	}
	auto replyTo = QString();
	if (message.replyToSeq > 0) {
		if (const auto target = _session->messages().find(group, message.replyToSeq)) {
			replyTo = target->sender;
		}
	}
	return MentionRules::AddressesMe(
		_session->userId(),
		message.sender,
		MentionedUsers(message),
		replyTo);
}

bool Groups::applyFrame(
		const QString &group,
		qint64 pts,
		const QString &kind,
		const QJsonObject &data) {
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	const auto row = chats.chat(group);
	if (!row) {
		return false;
	}
	const auto cache = attached(*row);
	auto cacheApplied = true;
	auto headerDirty = false;
	auto people = false;
	if (kind == u"message") {
		applyMessageFrame(group, data, cache);
	} else if (kind == u"edited" || kind == u"deleted") {
		applyChangedMessage(group, data, cache);
	} else if (kind == u"deleted_many") {
		applyTombstones(group, Json::Int64s(data.value("seqs"_L1)), cache);
	} else if (kind == u"unpinned") {
		const auto seq = Json::Int64(data, "seq"_L1);
		messages.applyPin(group, seq, false);
		chats.patch(group, [&](Chat &c) {
			if (c.group) {
				c.group->pins.removeAll(seq);
			}
		});
	} else if (kind == u"reaction") {
		const auto seq = Json::Int64(data, "seq"_L1);
		const auto applied = messages.applyReactions(
			group,
			seq,
			Json::CountMap(data.value("reactions"_L1)));
		// The counts alone cannot be written into a cached row that is not
		// in memory: the cache stays behind and one difference brings it.
		cacheApplied = applied || !cache;
	} else if (kind == u"info") {
		const auto me = _session->userId();
		const auto owner = Json::String(data, "owner"_L1);
		const auto member = data.value("member"_L1).toObject();
		const auto defaults = data.contains("default_permissions"_L1);
		chats.patch(group, [&](Chat &c) {
			if (!c.group) {
				return;
			}
			if (data.contains("member_count"_L1)) {
				c.group->memberCount = Json::Int(data, "member_count"_L1);
			}
			if (data.contains("about"_L1)) {
				c.group->about = Json::String(data, "about"_L1);
			}
			if (data.contains("history_visible"_L1)) {
				c.group->historyVisible = Json::Bool(data, "history_visible"_L1);
			}
			if (defaults) {
				c.group->defaultPermissions = Json::Strings(data.value("default_permissions"_L1));
			}
		});
		// The ownership moved, my own role changed, or the defaults did:
		// what they leave me is in the header.
		headerDirty = !owner.isEmpty()
			|| (Json::String(member, "user"_L1) == me && !me.isEmpty())
			|| defaults;
		// A list on screen reads itself again for a new owner, a new or
		// former admin and — when the frame names nothing — a join / leave
		// in a group too large for notes.
		people = !owner.isEmpty()
			|| !member.isEmpty()
			|| (!data.contains("about"_L1) && !data.contains("history_visible"_L1) && !defaults);
	} else if (kind == u"purged") {
		// The group was deleted: its last frame. Nothing of it stays.
		drop(group, u"deleted"_s);
		return true;
	}
	// A kind newer than this build: nothing to show, but the change counts.
	if (!chats.chat(group)) {
		return true;
	}
	chats.patch(group, [&](Chat &c) {
		c.groupPts = pts;
		if (cache && cacheApplied) {
			c.groupCachePts = pts;
		}
	});
	if (headerDirty) {
		refreshHeader(group);
	}
	if (people) {
		Q_EMIT membersChanged(group);
	}
	if (cache && !cacheApplied && messages.held(group)) {
		startDifference(group);
	}
	return true;
}

void Groups::applyMessageFrame(const QString &group, const QJsonObject &data, bool cache) {
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	const auto row = chats.chat(group);
	if (!row) {
		return;
	}
	auto message = Message::FromJson(data);
	if (message.chat.isEmpty()) {
		message.chat = group;
	}
	message.resolveLocal(&row->info, _session->userId());
	const auto mine = message.mine;
	const auto isNew = message.seq > row->lastSeq;
	const auto visible = message.visible();
	// On screen with its newest message in view: read as it comes.
	const auto onScreen = chats.readingLive(group);
	const auto service = (message.kind == MessageKind::System);
	const auto delta = service ? GroupRules::MemberDelta(message.system) : 0;
	const auto pinned = service ? GroupRules::NotePinnedSeq(message.system) : 0;
	const auto addresses = isNew && !mine && !service && addressesMe(group, message);

	if (visible) {
		const auto &history = messages.history(group);
		const auto echo = !message.clientMessageId.isEmpty()
			&& history.indexOfKey(Message::KeyForLocal(message.clientMessageId)) >= 0;
		if (cache) {
			messages.applyChanges(group, { message }, true);
		} else if (echo || history.find(message.seq)) {
			// My own message on its way out is this message: the bubble
			// stops «sending» also while the cache waits for its difference.
			messages.merge(group, { message }, false);
		}
	}
	if (pinned > 0) {
		messages.applyPin(group, pinned, true);
	}

	auto people = QStringList{ message.sender };
	if (const auto named = GroupRules::NoteUser(message.system); !named.isEmpty()) {
		people.push_back(named);
	}
	fetchPeople(people);

	chats.patch(group, [&](Chat &c) {
		if (isNew) {
			c.lastSeq = message.seq;
			c.info.lastSeq = message.seq;
			c.lastActivityAt = std::max(c.lastActivityAt, message.createdAt);
			if (c.group) {
				c.group->lastSeq = message.seq;
			}
			if (mine) {
				// Sending reads the group.
				c.lastReadSeq = std::max(c.lastReadSeq, message.seq);
				c.info.lastReadSeq = c.lastReadSeq;
				c.draft = std::nullopt;
				c.unread = 0;
				c.unreadCapped = false;
				c.unreadMentions = 0;
			} else if (!onScreen) {
				++c.unread;
				if (addresses) {
					++c.unreadMentions;
				}
			}
		}
		// A message waiting in the outbox stays the row's preview.
		const auto pending = c.lastMessage && c.lastMessage->local()
			&& c.lastMessage->clientMessageId != message.clientMessageId;
		if (visible && !pending && (!c.lastMessage || message.seq >= c.lastMessage->seq)) {
			c.lastMessage = message;
			c.lastMine = mine;
		}
		if (c.group) {
			if (delta && isNew) {
				c.group->memberCount = std::max(c.group->memberCount + delta, 0);
			}
			if (pinned > 0 && !c.group->pins.contains(pinned)) {
				// «X pinned a message»: the newest pin comes first.
				c.group->pins.push_front(pinned);
			}
			c.group->unread = c.unread;
			c.group->unreadMentions = c.unreadMentions;
			c.group->lastReadSeq = c.lastReadSeq;
		}
	});

	if (service && isNew) {
		if (GroupRules::HeaderChanged(message.system)) {
			// A new title / photo: the header says which.
			refreshHeader(group);
		}
		if (delta) {
			Q_EMIT membersChanged(group);
		}
	}
	if (isNew && !mine && visible && !service) {
		// Who is alerted: everything at the level "all" unless muted; what
		// addresses me also through a mute and at the level "mentions".
		const auto current = chats.chat(group);
		const auto level = (current && current->group && !current->group->notify.isEmpty())
			? current->group->notify
			: u"all"_s;
		const auto muted = current && current->muted;
		const auto alert = (level == u"all" && !muted) || (addresses && level != u"none");
		if (alert) {
			Q_EMIT messageReceived(message, addresses);
		}
	}
}

void Groups::applyChangedMessage(const QString &group, const QJsonObject &data, bool cache) {
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	auto message = Message::FromJson(data);
	if (message.chat.isEmpty()) {
		message.chat = group;
	}
	if (message.seq <= 0) {
		return;
	}
	if (!message.visible()) {
		applyTombstones(group, { message.seq }, cache);
		return;
	}
	const auto row = chats.chat(group);
	message.resolveLocal(row ? &row->info : nullptr, _session->userId());
	// Frames are the same for every member, so they never carry my reaction.
	if (const auto existing = messages.find(group, message.seq)) {
		message.myReaction = existing->myReaction;
	}
	if (cache) {
		messages.applyChanges(group, { message }, true);
	} else if (messages.find(group, message.seq)) {
		messages.merge(group, { message }, false);
	}
	if (row && row->lastMessage && row->lastMessage->seq == message.seq) {
		chats.patch(group, [&](Chat &c) {
			c.lastMessage = message;
			c.lastMine = message.mine;
		});
	}
}

void Groups::applyTombstones(const QString &group, const QVector<qint64> &seqs, bool cache) {
	if (seqs.isEmpty()) {
		return;
	}
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	messages.dropMessages(group, seqs, cache);
	const auto row = chats.chat(group);
	if (!row) {
		return;
	}
	// A message deleted for everyone stops being pinned and takes its
	// mention with it (only the header knows how many are left).
	const auto hadMentions = row->unreadMentions > 0;
	const auto lastGone = row->lastMessage && seqs.contains(row->lastMessage->seq);
	auto pinsChanged = false;
	for (const auto seq : seqs) {
		if (row->group && row->group->pins.contains(seq)) {
			pinsChanged = true;
			messages.applyPin(group, seq, false);
		}
	}
	if (lastGone || pinsChanged) {
		auto last = std::optional<Message>();
		if (lastGone) {
			const auto &items = messages.history(group).items;
			for (auto i = items.crbegin(); i != items.crend(); ++i) {
				if (i->seq > 0) {
					last = *i;
					break;
				}
			}
		}
		chats.patch(group, [&](Chat &c) {
			if (lastGone) {
				if (last) {
					c.lastMessage = last;
					c.lastMine = last->mine;
				} else if (c.lastMessage) {
					// Nothing older is at hand: the row says «deleted message».
					c.lastMessage->deleted = true;
					c.lastMessage->text = QString();
					c.lastMessage->entities.clear();
				}
			}
			if (c.group) {
				for (const auto seq : seqs) {
					c.group->pins.removeAll(seq);
				}
			}
		});
	}
	if (hadMentions) {
		refreshHeader(group);
	}
}

// ---- per-group difference

void Groups::startDifference(const QString &group) {
	if (!_started || !enabled()) {
		return;
	}
	auto &st = state(group);
	if (st.busy) {
		st.again = true;
		return;
	}
	// After a failure the next try waits: every frame of the group finds
	// the same gap, and each would otherwise ask again at once.
	const auto wait = st.notBefore - NowMs();
	if (wait > 0) {
		if (!st.scheduled) {
			st.scheduled = true;
			base::call_delayed(int(wait), this, [=, this] {
				const auto i = _states.find(group);
				if (i == _states.end()) {
					return;
				}
				i->scheduled = false;
				startDifference(group);
			});
		}
		return;
	}
	st.busy = true;
	differenceStep(group, 0);
}

void Groups::differenceStep(const QString &group, int page) {
	const auto row = _session->chats().chat(group);
	if (!_started || !row || row->groupCachePts < 0 || page >= kMaxDifferencePages) {
		differenceDone(group, row == nullptr || row->groupCachePts < 0);
		return;
	}
	const auto from = row->groupCachePts;
	_session->transport().rpc(u"groups.difference"_s)
		.body(QJsonObject{
			{ u"group"_s, group },
			{ u"pts"_s, QString::number(from) },
		})
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			applyDifference(group, from, result, page);
		})
		.fail([=, this](const Api::Error &error) {
			qInfo("Data: groups.difference failed: %s", qPrintable(error.forLog()));
			if (!handleError(group, error)) {
				differenceDone(group, false);
			}
		})
		.send();
}

void Groups::applyDifference(
		const QString &group,
		qint64 from,
		const QJsonObject &result,
		int page) {
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	const auto row = chats.chat(group);
	if (!row || !_started) {
		differenceDone(group, true);
		return;
	} else if (row->groupCachePts != from) {
		// The cache moved while this was on its way: again from where it is.
		differenceStep(group, page + 1);
		return;
	}
	const auto newPts = Json::Int64(result, "pts"_L1, from);
	const auto headerJson = result.value("group"_L1).toObject();
	const auto header = Json::String(headerJson, "group"_L1).isEmpty()
		? std::optional<Group>()
		: std::make_optional(Group::FromJson(headerJson));
	if (Json::Bool(result, "too_long"_L1)) {
		// More changed than one difference covers: the cached messages go
		// and the newest page is read again (it attaches the cache anew).
		const auto shown = messages.held(group);
		messages.clear(group);
		chats.patch(group, [&](Chat &c) {
			c.groupCachePts = -1;
		});
		if (header) {
			applyHeader(*header);
		}
		differenceDone(group, true);
		if (shown) {
			messages.loadLatest(group);
		} else {
			syncForFrame();
		}
		return;
	}
	const auto me = _session->userId();
	const auto lastSeqBefore = row->lastSeq;
	const auto lastReadBefore = row->lastReadSeq;
	const auto rowBehind = (newPts > row->groupPts);
	// On screen with its newest message in view: read as it comes.
	const auto onScreen = chats.readingLive(group);
	auto changes = QVector<GroupRules::Change>();
	auto changed = QVector<Message>();
	auto gone = QVector<qint64>();
	auto fresh = QVector<Message>();
	auto people = QStringList();
	auto ownSeq = qint64(0);
	auto newest = std::optional<Message>();
	for (const auto &item : result.value("messages"_L1).toArray()) {
		auto message = Message::FromJson(item.toObject());
		if (message.seq <= 0) {
			continue;
		}
		if (message.chat.isEmpty()) {
			message.chat = group;
		}
		message.resolveLocal(&row->info, me);
		const auto isNew = message.seq > lastSeqBefore;
		const auto deleted = !message.visible();
		changes.push_back({ message.seq, isNew, message.mine, deleted });
		if (isNew && message.mine && !deleted) {
			ownSeq = std::max(ownSeq, message.seq);
		}
		if (deleted) {
			gone.push_back(message.seq);
			continue;
		}
		if (isNew && !message.mine && message.seq > lastReadBefore) {
			fresh.push_back(message);
		}
		if (!newest || message.seq > newest->seq) {
			newest = message;
		}
		people.push_back(message.sender);
		if (const auto named = GroupRules::NoteUser(message.system); !named.isEmpty()) {
			people.push_back(named);
		}
		changed.push_back(std::move(message));
	}
	// The answer is ordered by time of change: each row simply replaces by seq.
	std::sort(changed.begin(), changed.end(), MessageLess);
	messages.dropMessages(group, gone, true);
	messages.applyChanges(group, changed, true);
	for (const auto seq : std::as_const(gone)) {
		messages.applyPin(group, seq, false);
	}
	// After every row is written: a reply may answer a message of the same answer.
	auto mentions = 0;
	for (const auto &message : std::as_const(fresh)) {
		if (message.kind != MessageKind::System && addressesMe(group, message)) {
			++mentions;
		}
	}
	const auto unread = GroupRules::UnreadFromDifference(changes, lastSeqBefore, lastReadBefore);
	chats.patch(group, [&](Chat &c) {
		c.groupCachePts = newPts;
		if (!rowBehind) {
			// The row followed the frames already; only what it shows of a
			// changed message is refreshed.
			if (c.lastMessage) {
				for (const auto &message : std::as_const(changed)) {
					if (message.seq == c.lastMessage->seq) {
						c.lastMessage = message;
						c.lastMine = message.mine;
					}
				}
			}
			return;
		}
		c.groupPts = newPts;
		if (header) {
			// Present when the profile, the owner, the member count or the
			// pins changed: its unread, mentions and pins are the server's.
			c.group = *header;
			SyncRowWithHeader(c);
			if (onScreen) {
				c.unread = 0;
				c.unreadCapped = false;
				c.unreadMentions = 0;
			}
		} else {
			if (!onScreen) {
				c.unread += unread;
				c.unreadMentions += mentions;
			}
			if (c.group) {
				for (const auto seq : std::as_const(gone)) {
					c.group->pins.removeAll(seq);
				}
			}
		}
		if (newest && newest->seq > c.lastSeq) {
			c.lastSeq = newest->seq;
			c.info.lastSeq = newest->seq;
		}
		if (newest && (!c.lastMessage || newest->seq >= c.lastMessage->seq)) {
			const auto pending = c.lastMessage && c.lastMessage->local();
			if (!pending) {
				c.lastMessage = newest;
				c.lastMine = newest->mine;
			}
			c.lastActivityAt = std::max(c.lastActivityAt, newest->createdAt);
		} else if (c.lastMessage && gone.contains(c.lastMessage->seq)) {
			c.lastMessage->deleted = true;
			c.lastMessage->text = QString();
			c.lastMessage->entities.clear();
		}
		if (ownSeq > c.lastReadSeq) {
			// Sent from another device: sending reads the group.
			c.lastReadSeq = ownSeq;
			c.info.lastReadSeq = ownSeq;
		}
		if (c.group) {
			c.group->lastSeq = std::max(c.group->lastSeq, c.lastSeq);
			c.group->lastReadSeq = c.lastReadSeq;
			c.group->unread = c.unread;
			c.group->unreadMentions = c.unreadMentions;
		}
	});
	if (header) {
		_session->messages().setInfo(InfoFor(*header));
	}
	fetchPeople(people);
	if (Json::Bool(result, "more"_L1)) {
		differenceStep(group, page + 1);
	} else {
		differenceDone(group, true);
	}
}

// The difference of a group ended (whatever became of it): the frames that
// waited go through the rule.
void Groups::differenceDone(const QString &group, bool ok) {
	const auto i = _states.find(group);
	if (i == _states.end()) {
		return;
	}
	i->busy = false;
	if (ok) {
		i->fails = 0;
		i->notBefore = 0;
	} else {
		i->fails = std::min(i->fails + 1, 5);
		i->notBefore = NowMs() + BackoffMs(i->fails);
	}
	const auto again = std::exchange(i->again, false);
	const auto waited = std::exchange(i->pending, {});
	for (const auto &frame : waited) {
		// One of them found a gap: the rest wait for that difference.
		const auto now = _states.find(group);
		if (now != _states.end() && now->busy) {
			now->pending.push_back(frame);
		} else {
			applyRule(group, frame);
		}
	}
	const auto now = _states.find(group);
	if (now == _states.end()) {
		return;
	}
	if (again && !now->busy && ok) {
		startDifference(group);
		return;
	}
	if (!now->busy && !now->scheduled && !now->fails && now->pending.isEmpty()) {
		_states.erase(now);
	}
}

// ---- catch-up (`groups.sync`) and the cold start

void Groups::requestSync() {
	if (!_started || !enabled()) {
		return;
	} else if (_syncing) {
		_syncAgain = true;
		return;
	}
	runSync();
}

void Groups::ensureSynced() {
	if (!_syncedOnce) {
		requestSync();
	}
}

// The catch-up as a frame asks for it: not while one has just failed —
// frames keep coming and each would ask again at once.
void Groups::syncForFrame() {
	if (NowMs() >= _syncNotBefore) {
		requestSync();
	}
}

void Groups::runSync() {
	_syncing = true;
	_syncAgain = false;
	auto known = QHash<QString, qint64>();
	auto list = QJsonArray();
	for (const auto &chat : _session->chats().all()) {
		if (!chat.isGroup || chat.groupPts < 0 || known.size() >= kMaxKnown) {
			continue;
		}
		known.insert(chat.id, chat.groupPts);
		list.push_back(QJsonObject{
			{ u"group"_s, chat.id },
			{ u"pts"_s, double(chat.groupPts) },
		});
	}
	if (known.isEmpty()) {
		// No group held (first run, a new device): page through my groups.
		coldStart(QString(), 0, false);
		return;
	}
	_session->transport().rpc(u"groups.sync"_s)
		.body(QJsonObject{ { u"known"_s, list } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (!_started) {
				return;
			}
			applySync(known, result);
			auto have = 0;
			for (const auto &chat : _session->chats().all()) {
				if (chat.isGroup) {
					++have;
				}
			}
			// Fewer held than I am in: the cache lost groups (or never heard
			// of them) — the list once.
			if (have < Json::Int64(result, "count"_L1)
				&& NowMs() - _coldStartAt > kColdStartEveryMs) {
				coldStart(QString(), 0, false);
			} else {
				syncSucceeded();
			}
		})
		.fail([=, this](const Api::Error &error) {
			syncFailed(error);
		})
		.send();
}

void Groups::syncSucceeded() {
	_syncing = false;
	_syncedOnce = true;
	_syncFails = 0;
	_syncNotBefore = 0;
	if (_syncAgain && _started) {
		runSync();
	}
}

void Groups::syncFailed(const Api::Error &error) {
	qInfo("Data: groups.sync failed: %s", qPrintable(error.forLog()));
	_syncing = false;
	_syncAgain = false;
	_syncFails = std::min(_syncFails + 1, 5);
	_syncNotBefore = NowMs() + BackoffMs(_syncFails);
	handleError(QString(), error);
}

void Groups::applySync(const QHash<QString, qint64> &known, const QJsonObject &result) {
	for (const auto &item : result.value("removed"_L1).toArray()) {
		const auto id = item.toString();
		// Only what was sent: a group joined while the call was on its way
		// is not «removed».
		if (IsGroupId(id) && known.contains(id) && held(id)) {
			drop(id, u"removed"_s);
		}
	}
	auto people = QStringList();
	for (const auto &item : result.value("groups"_L1).toArray()) {
		const auto entry = item.toObject();
		const auto id = Json::String(entry.value("group"_L1).toObject(), "group"_L1);
		const auto owed = applyEntry(entry, &known, false);
		if (const auto row = _session->chats().chat(id); row && row->lastMessage) {
			people.push_back(row->lastMessage->sender);
		}
		// A cached group that is behind: its difference at once when its
		// timeline is in memory, otherwise when it is opened.
		if (owed && _session->messages().held(id)) {
			startDifference(id);
		}
	}
	fetchPeople(people);
}

// One GroupEntry of the catch-up call or of the list: the header and the
// last message replace the row. `known`: the pts that were sent with the
// call (null: none were — the list). `listed`: a page of the list, which
// cannot tell a header that is behind because live changes were applied
// meanwhile from a server that is behind — such a header is left out.
// Returns true when the cached messages of the group are behind the row.
bool Groups::applyEntry(
		const QJsonObject &entry,
		const QHash<QString, qint64> *known,
		bool listed) {
	const auto header = Group::FromJson(entry.value("group"_L1).toObject());
	if (!IsGroupId(header.id) || !header.isMember()) {
		return false;
	}
	auto &chats = _session->chats();
	const auto previous = chats.chat(header.id);
	const auto heldPts = (previous && previous->isGroup) ? previous->groupPts : -1;
	const auto cachePts = previous ? previous->groupCachePts : -1;
	const auto sent = (known && known->contains(header.id))
		? known->value(header.id)
		: heldPts;
	if (listed && heldPts >= 0 && header.pts < heldPts) {
		return false;
	}
	const auto action = GroupRules::SyncActionFor(sent, heldPts, header.pts, cachePts >= 0);
	if (action == SyncAction::Skip) {
		return false;
	}
	auto row = rowFor(header, previous);
	row.groupPts = header.pts;
	row.groupCachePts = (action == SyncAction::Rewind)
		? 0 // the server never had what we cached against: checked again from 0
		: (action == SyncAction::Take)
		? -1
		: cachePts;
	if (action == SyncAction::Take && cachePts >= 0) {
		// A leftover cache that the difference could never repair goes.
		_session->messages().clear(header.id);
	}
	const auto lastJson = entry.value("last_message"_L1).toObject();
	// A message queued in the outbox stays the row's preview.
	const auto pending = previous && previous->lastMessage && previous->lastMessage->local();
	if (!pending) {
		if (!Json::String(lastJson, "id"_L1).isEmpty() || Json::Int64(lastJson, "seq"_L1) > 0) {
			auto last = Message::FromJson(lastJson);
			if (last.chat.isEmpty()) {
				last.chat = header.id;
			}
			last.resolveLocal(&row.info, _session->userId());
			row.lastMine = last.mine;
			row.lastMessage = std::move(last);
		} else {
			row.lastMessage = std::nullopt;
			row.lastMine = false;
		}
	}
	if (const auto at = Json::Int64(entry, "last_activity_at"_L1)) {
		row.lastActivityAt = std::max(at, pending ? row.lastActivityAt : qint64(0));
	}
	// My saved draft of the group rides on its entry (absent = none). Only
	// with a server that keeps group drafts: on any other a draft never left
	// this device and stays as it is.
	if (administration() && _session->clientConfig()->draftsEnabled()) {
		const auto draft = Json::Optional<Draft>(entry, "draft"_L1);
		if (!chats.hasLocalDraft(header.id)) {
			row.draft = (draft && !draft->empty()) ? draft : std::nullopt;
		}
	}
	chats.upsert(row);
	_session->messages().setInfo(row.info);
	return row.groupCachePts >= 0 && row.groupCachePts < row.groupPts;
}

void Groups::coldStart(const QString &cursor, int page, bool restarted) {
	if (!_started) {
		_syncing = false;
		return;
	}
	if (page == 0) {
		_coldStartAt = NowMs();
	}
	auto body = QJsonObject{ { u"limit"_s, QString::number(kListPageSize) } };
	if (!cursor.isEmpty()) {
		body.insert(u"cursor"_s, cursor);
	}
	_session->transport().rpc(u"groups.list"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (!_started) {
				return;
			}
			auto people = QStringList();
			for (const auto &item : result.value("groups"_L1).toArray()) {
				// A group returned twice by two pages is simply written twice.
				const auto entry = item.toObject();
				applyEntry(entry, nullptr, true);
				const auto id = Json::String(entry.value("group"_L1).toObject(), "group"_L1);
				if (const auto row = _session->chats().chat(id); row && row->lastMessage) {
					people.push_back(row->lastMessage->sender);
				}
			}
			fetchPeople(people);
			const auto next = Json::String(result, "next_cursor"_L1);
			if (next.isEmpty() || next == cursor || page + 1 >= kMaxListPages) {
				syncSucceeded();
			} else {
				coldStart(next, page + 1, restarted);
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (error.code == u"cursor.invalid" && !restarted) {
				coldStart(QString(), 0, true);
			} else {
				syncFailed(error);
			}
		})
		.send();
}

void Groups::fetchPeople(const QStringList &ids) {
	auto &users = _session->users();
	for (const auto &id : ids) {
		if (!id.isEmpty() && id != _session->userId()) {
			users.request(id);
		}
	}
}

// ---- my own place (the user update of kind "group")

void Groups::applyUser(const QJsonObject &d) {
	if (!_started || !enabled()) {
		return;
	}
	const auto group = Json::String(d, "group"_L1);
	if (!IsGroupId(group)) {
		return;
	}
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	const auto status = Json::String(d, "status"_L1);
	if (!status.isEmpty()) {
		if (status == u"member") {
			// New to me (added, joined by a link here or elsewhere, my
			// request approved): the catch-up returns it with its last
			// message. Held: my role, my admin rights or title changed, or I
			// was restricted / the restriction was lifted — the header says how.
			if (held(group)) {
				refreshHeader(group);
			} else {
				requestSync();
			}
		} else {
			// Kicked, banned, left on another device, the group deleted, …
			drop(group, status);
		}
		return;
	}
	const auto row = chats.chat(group);
	if (!row || !row->isGroup) {
		return;
	}
	if (const auto read = Json::Int64(d, "read_seq"_L1); read > 0) {
		const auto toEnd = (read >= row->lastSeq);
		if (read > row->lastReadSeq) {
			chats.patch(group, [&](Chat &c) {
				c.lastReadSeq = read;
				c.info.lastReadSeq = read;
				if (toEnd) {
					c.unread = 0;
					c.unreadCapped = false;
					c.unreadMentions = 0;
				}
				if (c.group) {
					c.group->lastReadSeq = read;
					c.group->unread = c.unread;
					c.group->unreadMentions = c.unreadMentions;
				}
			});
			if (!toEnd) {
				// Read elsewhere, but not to the end: how much is left is
				// the header's to say.
				refreshHeader(group);
			}
		}
	}
	if (const auto draft = d.value("draft"_L1); draft.isObject()) {
		// Saved or cleared on another device, or used up by a message I sent.
		const auto parsed = Draft::FromJson(draft.toObject());
		if (!chats.hasLocalDraft(group)) {
			chats.patch(group, [&](Chat &c) {
				c.draft = parsed.text.trimmed().isEmpty()
					? std::nullopt
					: std::make_optional(parsed);
			});
		}
	}
	if (const auto settings = d.value("settings"_L1); settings.isObject()) {
		const auto s = settings.toObject();
		const auto cleared = Json::Int64(s, "cleared_before_seq"_L1);
		const auto grew = cleared > (row->group ? row->group->clearedBeforeSeq : 0);
		chats.patch(group, [&](Chat &c) {
			c.muted = Json::Bool(s, "muted"_L1);
			c.mutedUntil = Json::Int64(s, "muted_until"_L1);
			c.archived = Json::Bool(s, "archived"_L1);
			c.pinned = Json::Bool(s, "pinned"_L1);
			c.info.muted = c.muted;
			c.info.mutedUntil = c.mutedUntil;
			c.info.archived = c.archived;
			if (c.group) {
				c.group->muted = c.muted;
				c.group->mutedUntil = c.mutedUntil;
				c.group->archived = c.archived;
				c.group->pinned = c.pinned;
				c.group->notify = Json::String(s, "notify"_L1);
				c.group->clearedBeforeSeq = std::max(c.group->clearedBeforeSeq, cleared);
			}
			if (grew && c.lastMessage && c.lastMessage->seq > 0 && c.lastMessage->seq <= cleared) {
				c.lastMessage = std::nullopt;
				c.lastMine = false;
			}
		});
		if (grew) {
			// The messages at or below the mark are gone for me.
			messages.clear(group, cleared + 1);
		}
	}
	if (const auto hidden = Json::Int64(d, "hidden_seq"_L1); hidden > 0) {
		messages.dropMessages(group, { hidden }, true);
	}
	if (const auto reaction = d.value("reaction"_L1); reaction.isObject()) {
		const auto r = reaction.toObject();
		if (const auto seq = Json::Int64(r, "seq"_L1); seq > 0) {
			messages.setMyReaction(group, seq, Json::String(r, "emoji"_L1));
		}
	}
}

// ---- headers

void Groups::applyHeader(const Group &header) {
	if (!header.valid() || !IsGroupId(header.id)) {
		return;
	}
	auto &chats = _session->chats();
	const auto previous = chats.chat(header.id);
	if (!previous || !previous->isGroup) {
		if (!header.isMember()) {
			return;
		}
		// New here (I created it, joined it, or opened it before the list
		// knew it): the row appears at once; its last message comes with
		// the group's first frame or with the catch-up — so the applied pts
		// starts before everything.
		auto row = rowFor(header, nullptr);
		row.groupPts = 0;
		row.groupCachePts = -1;
		chats.upsert(row);
		_session->messages().setInfo(row.info);
		requestSync();
		return;
	}
	if (!header.isMember()) {
		drop(header.id, header.status);
		return;
	}
	const auto rowPts = previous->groupPts;
	auto gap = false;
	chats.patch(header.id, [&](Chat &c) {
		if (!c.group) {
			c.group = header;
		}
		auto &group = *c.group;
		if (header.pts == rowPts) {
			// The same moment: everything, the derived counts included.
			group = header;
			SyncRowWithHeader(c);
			if (chats.visibleChat() == header.id && c.lastReadSeq >= header.lastSeq) {
				c.unread = 0;
				c.unreadCapped = false;
				c.unreadMentions = 0;
			}
			return;
		}
		TakePersonal(group, header);
		if (header.pts > rowPts) {
			// Changes this row has not applied yet: the profile now, the
			// counters with the changes themselves (frames that follow are
			// compared with the row's own pts).
			TakeProfile(group, header);
			gap = true;
		}
		const auto lastSeq = c.lastSeq;
		const auto unread = c.unread;
		const auto capped = c.unreadCapped;
		const auto mentions = c.unreadMentions;
		group.lastSeq = lastSeq;
		group.unread = unread;
		group.unreadCapped = capped;
		group.unreadMentions = mentions;
		SyncRowWithHeader(c);
	});
	if (const auto row = chats.chat(header.id)) {
		_session->messages().setInfo(row->info);
	}
	if (gap) {
		catchUp(header.id);
	}
}

void Groups::historyPage(const Group &header, bool newest) {
	if (!_started || !enabled() || !header.valid()) {
		return;
	}
	auto &chats = _session->chats();
	const auto previous = chats.chat(header.id);
	if (!previous || !previous->isGroup) {
		applyHeader(header);
		return;
	}
	if (!newest) {
		applyHeader(header);
		return;
	}
	// The newest page is the group as it is at the header's pts: a row that
	// is behind takes the header and the page's last message as they are
	// (Data::Messages hands the message over right after this call).
	const auto rowPts = previous->groupPts;
	const auto cachePts = previous->groupCachePts;
	if (header.pts > rowPts) {
		chats.patch(header.id, [&](Chat &c) {
			c.group = header;
			SyncRowWithHeader(c);
			c.groupPts = header.pts;
			if (chats.visibleChat() == header.id) {
				c.unread = 0;
				c.unreadCapped = false;
				c.unreadMentions = 0;
			}
		});
		_session->messages().setInfo(InfoFor(header));
	} else {
		applyHeader(header);
	}
	if (cachePts < 0) {
		// Nothing was cached: the page is the cache, current at its pts.
		chats.patch(header.id, [&](Chat &c) {
			c.groupCachePts = header.pts;
		});
	}
	const auto row = chats.chat(header.id);
	if (row && row->groupCachePts >= 0 && row->groupCachePts < row->groupPts) {
		// Older cached messages (or frames applied while the page was on
		// its way) are behind: one difference from where the cache is.
		startDifference(header.id);
	}
}

void Groups::applyAdminSignal(
		const QString &group,
		const QString &kind,
		int count,
		const QString &user) {
	if (!_started || !enabled() || !held(group)) {
		return;
	} else if (kind != u"join_requests") {
		// Something newer than this client knows: the header tells.
		refreshHeader(group);
		return;
	}
	_session->chats().patch(group, [&](Chat &c) {
		if (c.group) {
			c.group->joinRequests = std::max(count, 0);
		}
	});
	Q_EMIT joinRequestsChanged(group, user);
}

void Groups::refreshHeader(const QString &group) {
	if (!_started || !enabled() || !IsGroupId(group) || _headerRequests.contains(group)) {
		return;
	}
	_headerRequests.insert(group);
	_session->transport().rpc(u"groups.get"_s)
		.body(QJsonObject{ { u"group"_s, group } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			_headerRequests.remove(group);
			applyHeader(Group::FromJson(result));
		})
		.fail([=, this](const Api::Error &error) {
			_headerRequests.remove(group);
			handleError(group, error);
		})
		.send();
}

// ---- sending

void Groups::sent(
		const QString &group,
		const Message &message,
		const QJsonObject &result,
		bool cached) {
	auto &chats = _session->chats();
	const auto row = chats.chat(group);
	const auto seq = Json::Int64(result, "seq"_L1);
	if (!row || !row->isGroup || seq <= 0) {
		return;
	}
	const auto pts = Json::Int64(result, "pts"_L1);
	const auto i = _states.constFind(group);
	const auto busy = (i != _states.cend()) && i->busy;
	const auto wasAttached = attached(*row);
	const auto action = GroupRules::SendActionFor(
		row->groupPts,
		pts,
		Json::Bool(result, "duplicate"_L1),
		cached);
	const auto advance = (action == SendAction::Advance) && !busy;
	if (advance && wasAttached && message.seq > 0) {
		// The message is the only change since the stored pts: the cache
		// takes it too (the timeline in memory has it from the outbox).
		_session->messages().applyChanges(group, { message }, true);
	}
	chats.patch(group, [&](Chat &c) {
		// Sending reads the group.
		c.lastReadSeq = std::max(c.lastReadSeq, seq);
		c.info.lastReadSeq = c.lastReadSeq;
		c.unread = 0;
		c.unreadCapped = false;
		c.unreadMentions = 0;
		if (advance) {
			c.groupPts = pts;
			if (wasAttached) {
				c.groupCachePts = pts;
			}
		}
		if (c.group) {
			c.group->lastSeq = std::max(c.group->lastSeq, seq);
			c.group->lastReadSeq = c.lastReadSeq;
			c.group->unread = 0;
			c.group->unreadMentions = 0;
		}
	});
	_slowModeUntil.remove(group);
	if (advance || action == SendAction::Keep) {
		return;
	}
	if (busy) {
		// The difference in flight may have been answered before this message.
		_states[group].again = true;
	} else {
		catchUp(group);
	}
}

// ---- errors that change state

bool Groups::handleError(const QString &group, const Api::Error &error) {
	if (error.code == u"group.disabled") {
		// The feature was switched off: client-config says so (and the list
		// hides its groups until it says otherwise; the cache stays).
		if (const auto config = _session->clientConfig()) {
			config->load(this, nullptr);
		}
		return false;
	} else if (!IsGroupId(group)) {
		return false;
	} else if (error.code == u"group.not_found") {
		// I am not a member any more (removed, left elsewhere), or the group
		// was closed. No need to wait for the update.
		drop(group, u"removed"_s);
		return true;
	} else if (error.code == u"group.send_restricted"
		|| error.code == u"group.right_required") {
		// What I may do changed since the header was read.
		refreshHeader(group);
	} else if (error.code == u"group.slow_mode") {
		const auto wait = error.retryAfter.value_or(0.);
		if (wait > 0.) {
			_slowModeUntil.insert(group, NowMs() + qint64(wait * 1000.));
		}
		refreshHeader(group);
	}
	return false;
}

void Groups::drop(const QString &group, const QString &status) {
	const auto was = held(group);
	forgetState(group);
	// Its queued sends go with it: they would fail for good.
	_session->outbox().discardChat(group);
	if (!was) {
		return;
	}
	_session->chats().remove(group);
	Q_EMIT groupRemoved(group, status);
}

// ---- requests

void Groups::call(
		const QString &method,
		const QString &group,
		const QJsonObject &body,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		Fail fail) {
	_session->transport().rpc(method)
		.body(body)
		.guard(this)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(result);
			}
		})
		.fail([=, this](const Api::Error &error) {
			handleError(group, error);
			if (!guard.alive()) {
				return;
			} else if (fail) {
				fail(error);
			} else {
				_session->reportFailure(method, group, error);
			}
		})
		.send();
}

void Groups::member(
		const QString &group,
		const QJsonObject &body,
		const QString &action,
		Fn<void()> revert) {
	auto request = body;
	request.insert(u"group"_s, group);
	_session->transport().rpc(u"groups.member"_s)
		.body(request)
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			if (!handleError(group, error) && revert) {
				revert();
			}
			_session->reportFailure(action, group, error);
		})
		.send();
}

// ---- my own actions

void Groups::create(
		const GroupCreateRequest &request,
		base::Guard guard,
		Fn<void(const Group &, const QVector<GroupNotAdded> &)> done,
		Fail fail) {
	auto body = QJsonObject{ { u"title"_s, GroupRules::CleanTitle(request.title) } };
	Json::Put(body, "about"_L1, request.about.trimmed());
	Json::Put(body, "photo"_L1, request.photo);
	Json::Put(body, "members"_L1, Json::Array(request.members));
	call(u"groups.create"_s, QString(), body, guard, [=, this](const QJsonObject &result) {
		const auto header = Group::FromJson(result.value("group"_L1).toObject());
		applyHeader(header);
		if (done) {
			done(header, Json::List<GroupNotAdded>(result.value("not_added"_L1)));
		}
	}, std::move(fail));
}

void Groups::leave(const QString &group, base::Guard guard, Fn<void()> done, Fail fail) {
	call(u"groups.leave"_s, group, QJsonObject{ { u"group"_s, group } }, guard, [=, this](const QJsonObject &) {
		drop(group, u"left"_s);
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Groups::deleteGroup(const QString &group, base::Guard guard, Fn<void()> done, Fail fail) {
	call(u"groups.delete_group"_s, group, QJsonObject{ { u"group"_s, group } }, guard, [=, this](const QJsonObject &) {
		// Gone from the cache at once: the last frame of the group and the
		// update that say so find nothing left to do.
		drop(group, u"deleted"_s);
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Groups::clearHistory(const QString &group) {
	const auto row = _session->chats().chat(group);
	if (!row) {
		return;
	}
	const auto upTo = std::max(row->lastSeq, _session->messages().history(group).lastSeq());
	_session->messages().clear(group);
	_session->chats().patch(group, [&](Chat &c) {
		c.lastMessage = std::nullopt;
		c.lastMine = false;
		c.unread = 0;
		c.unreadCapped = false;
		c.unreadMentions = 0;
		if (c.group) {
			c.group->clearedBeforeSeq = std::max(c.group->clearedBeforeSeq, upTo);
			c.group->unread = 0;
			c.group->unreadMentions = 0;
		}
	});
	member(group, QJsonObject{ { u"clear_history"_s, true } }, u"chat.delete"_s, [=, this] {
		// Bring it back.
		if (_session->messages().held(group)) {
			_session->messages().loadLatest(group);
		}
		syncForFrame();
	});
}

void Groups::setNotify(const QString &group, const QString &level) {
	const auto row = _session->chats().chat(group);
	if (!row || !row->group || row->group->notify == level) {
		return;
	}
	const auto was = row->group->notify;
	_session->chats().patch(group, [&](Chat &c) {
		if (c.group) {
			c.group->notify = level;
		}
	});
	member(group, QJsonObject{ { u"notify"_s, level } }, u"chat.mute"_s, [=, this] {
		_session->chats().patch(group, [&](Chat &c) {
			if (c.group) {
				c.group->notify = was;
			}
		});
	});
}

void Groups::edit(
		const QString &group,
		const std::optional<QString> &title,
		const std::optional<QString> &about,
		const std::optional<QString> &photo,
		base::Guard guard,
		Fn<void(const Group &)> done,
		Fail fail) {
	auto body = QJsonObject{ { u"group"_s, group } };
	if (title) {
		body.insert(u"title"_s, GroupRules::CleanTitle(*title));
	}
	if (about) {
		body.insert(u"about"_s, about->trimmed());
	}
	if (photo) {
		body.insert(u"photo"_s, *photo);
	}
	call(u"groups.patch"_s, group, body, guard, [=, this](const QJsonObject &result) {
		const auto header = Group::FromJson(result);
		applyHeader(header);
		if (done) {
			done(header);
		}
	}, std::move(fail));
}

void Groups::transfer(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void(const Group &)> done,
		Fail fail) {
	const auto body = QJsonObject{ { u"group"_s, group }, { u"user"_s, user } };
	call(u"groups.transfer"_s, group, body, guard, [=, this](const QJsonObject &result) {
		const auto header = Group::FromJson(result);
		applyHeader(header);
		Q_EMIT membersChanged(group);
		if (done) {
			done(header);
		}
	}, std::move(fail));
}

// ---- members

void Groups::loadMembers(
		const QString &group,
		const QString &filter,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const GroupMembersPage &)> done,
		Fail fail) {
	auto body = QJsonObject{
		{ u"group"_s, group },
		{ u"limit"_s, QString::number(membersPageSize()) },
	};
	if (!filter.isEmpty() && filter != u"all") {
		body.insert(u"filter"_s, filter);
	}
	if (!cursor.isEmpty()) {
		body.insert(u"cursor"_s, cursor);
	}
	call(u"groups.members"_s, group, body, guard, [=, this](const QJsonObject &result) {
		auto page = GroupMembersPage();
		page.members = Json::List<GroupMember>(result.value("members"_L1));
		page.nextCursor = Json::String(result, "next_cursor"_L1);
		auto ids = QStringList();
		for (const auto &member : std::as_const(page.members)) {
			ids.push_back(member.user);
		}
		// Cards first, so the rows appear with their names.
		_session->users().ensure(ids, guard, [=] {
			if (done) {
				done(page);
			}
		});
	}, std::move(fail));
}

void Groups::searchMembers(
		const QString &group,
		const QString &query,
		base::Guard guard,
		Fn<void(const QVector<User> &)> done) {
	_session->api().request(Api::Get, u"/v1/search"_s)
		.query(u"type"_s, u"users"_s)
		.query(u"q"_s, query)
		.query(u"group"_s, group)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto users = Json::List<User>(result.value("users"_L1));
			_session->users().seed(users);
			if (done && guard.alive()) {
				done(users);
			}
		})
		.fail([=](const Api::Error &) {
			if (done && guard.alive()) {
				done({});
			}
		})
		.send();
}

void Groups::addMembers(
		const QString &group,
		const QStringList &users,
		base::Guard guard,
		Fn<void(const GroupAddResult &)> done,
		Fail fail) {
	const auto body = QJsonObject{
		{ u"group"_s, group },
		{ u"users"_s, Json::Array(users) },
	};
	call(u"groups.members.add"_s, group, body, guard, [=, this](const QJsonObject &result) {
		auto answer = GroupAddResult();
		answer.added = Json::Strings(result.value("added"_L1));
		answer.notAdded = Json::List<GroupNotAdded>(result.value("not_added"_L1));
		if (!answer.added.isEmpty()) {
			Q_EMIT membersChanged(group);
		}
		if (done) {
			done(answer);
		}
	}, std::move(fail));
}

void Groups::removeMember(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void()> done,
		Fail fail) {
	const auto body = QJsonObject{ { u"group"_s, group }, { u"user"_s, user } };
	call(u"groups.members.remove"_s, group, body, guard, [=, this](const QJsonObject &) {
		Q_EMIT membersChanged(group);
		if (done) {
			done();
		}
	}, std::move(fail));
}

// ---- administration

void Groups::setAdmin(
		const QString &group,
		const QString &user,
		const QStringList &rights,
		const std::optional<QString> &title,
		base::Guard guard,
		Fn<void(const GroupMember &)> done,
		Fail fail) {
	auto body = QJsonObject{
		{ u"group"_s, group },
		{ u"user"_s, user },
		{ u"rights"_s, Json::Array(rights) },
	};
	if (title) {
		body.insert(u"title"_s, title->simplified());
	}
	call(u"groups.admin.set"_s, group, body, guard, [=, this](const QJsonObject &result) {
		Q_EMIT membersChanged(group);
		if (done) {
			done(GroupMember::FromJson(result));
		}
	}, std::move(fail));
}

void Groups::unsetAdmin(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void(const GroupMember &)> done,
		Fail fail) {
	const auto body = QJsonObject{ { u"group"_s, group }, { u"user"_s, user } };
	call(u"groups.admin.unset"_s, group, body, guard, [=, this](const QJsonObject &result) {
		Q_EMIT membersChanged(group);
		if (done) {
			done(GroupMember::FromJson(result));
		}
	}, std::move(fail));
}

void Groups::restrict(
		const QString &group,
		const QString &user,
		const QStringList &denied,
		qint64 until,
		base::Guard guard,
		Fn<void(const GroupMember &)> done,
		Fail fail) {
	auto body = QJsonObject{
		{ u"group"_s, group },
		{ u"user"_s, user },
		{ u"denied"_s, Json::Array(denied) },
	};
	if (until > 0 && !denied.isEmpty()) {
		body.insert(u"until"_s, double(until));
	}
	call(u"groups.restrict"_s, group, body, guard, [=, this](const QJsonObject &result) {
		Q_EMIT membersChanged(group);
		if (done) {
			done(GroupMember::FromJson(result));
		}
	}, std::move(fail));
}

void Groups::ban(
		const QString &group,
		const QString &user,
		bool deleteMessages,
		base::Guard guard,
		Fn<void()> done,
		Fail fail) {
	auto body = QJsonObject{ { u"group"_s, group }, { u"user"_s, user } };
	if (deleteMessages) {
		body.insert(u"delete_messages"_s, true);
	}
	call(u"groups.ban"_s, group, body, guard, [=, this](const QJsonObject &) {
		Q_EMIT membersChanged(group);
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Groups::unban(
		const QString &group,
		const QString &user,
		base::Guard guard,
		Fn<void()> done,
		Fail fail) {
	const auto body = QJsonObject{ { u"group"_s, group }, { u"user"_s, user } };
	call(u"groups.unban"_s, group, body, guard, [=, this](const QJsonObject &) {
		Q_EMIT membersChanged(group);
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Groups::setPermissions(
		const QString &group,
		const std::optional<QStringList> &defaultPermissions,
		const std::optional<bool> &historyVisible,
		base::Guard guard,
		Fn<void(const Group &)> done,
		Fail fail) {
	auto body = QJsonObject();
	if (defaultPermissions) {
		body.insert(u"default_permissions"_s, Json::Array(*defaultPermissions));
	}
	if (historyVisible) {
		body.insert(u"history_visible"_s, *historyVisible);
	}
	// A realtime request is a protobuf message, where an empty list cannot
	// be told from an absent one: over the connection «members may do
	// nothing» would read «unchanged». HTTP says what is meant.
	_session->api().request(Api::Put, u"/v1/groups/%1/permissions"_s.arg(Encoded(group)))
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto header = Group::FromJson(result);
			applyHeader(header);
			if (done && guard.alive()) {
				done(header);
			}
		})
		.fail([=, this](const Api::Error &error) {
			handleError(group, error);
			if (!guard.alive()) {
				return;
			} else if (fail) {
				fail(error);
			} else {
				_session->reportFailure(u"groups.permissions"_s, group, error);
			}
		})
		.send();
}

void Groups::deleteMessages(
		const QString &group,
		const QVector<qint64> &seqs,
		base::Guard guard,
		Fn<void()> done,
		Fail fail) {
	auto sorted = seqs;
	std::sort(sorted.begin(), sorted.end());
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	sorted.erase(
		std::remove_if(sorted.begin(), sorted.end(), [](qint64 seq) { return seq <= 0; }),
		sorted.end());
	if (sorted.isEmpty()) {
		if (done && guard.alive()) {
			done();
		}
		return;
	}
	// One bulk delete takes so many; the rest follows in further calls.
	const auto limit = std::max(bulkDeleteMax(), 1);
	const auto batch = sorted.mid(0, limit);
	const auto rest = sorted.mid(limit);
	const auto body = QJsonObject{
		{ u"group"_s, group },
		{ u"seqs"_s, Json::Array(batch) },
	};
	call(u"groups.delete_many"_s, group, body, guard, [=, this](const QJsonObject &result) {
		// The `deleted_many` frame says the same; whichever comes first.
		const auto row = _session->chats().chat(group);
		applyTombstones(
			group,
			Json::Int64s(result.value("deleted"_L1)),
			row && attached(*row));
		if (!rest.isEmpty()) {
			deleteMessages(group, rest, guard, done, fail);
		} else if (done) {
			done();
		}
	}, fail);
}

// ---- invite links and join requests

void Groups::loadInvites(
		const QString &group,
		base::Guard guard,
		Fn<void(const QVector<GroupInvite> &)> done,
		Fail fail) {
	call(u"groups.invites"_s, group, QJsonObject{ { u"group"_s, group } }, guard, [=](const QJsonObject &result) {
		if (done) {
			done(Json::List<GroupInvite>(result.value("invites"_L1)));
		}
	}, std::move(fail));
}

void Groups::createInvite(
		const QString &group,
		const GroupInviteRequest &request,
		base::Guard guard,
		Fn<void(const GroupInvite &)> done,
		Fail fail) {
	auto body = QJsonObject{ { u"group"_s, group } };
	Json::Put(body, "name"_L1, request.name.simplified());
	Json::Put(body, "requires_approval"_L1, request.requiresApproval);
	Json::Put(body, "expires_in"_L1, request.expiresIn);
	Json::Put(body, "max_uses"_L1, request.maxUses);
	call(u"groups.invites.create"_s, group, body, guard, [=](const QJsonObject &result) {
		if (done) {
			done(GroupInvite::FromJson(result));
		}
	}, std::move(fail));
}

void Groups::revokeInvite(
		const QString &group,
		const QString &link,
		base::Guard guard,
		Fn<void()> done,
		Fail fail) {
	const auto body = QJsonObject{ { u"group"_s, group }, { u"link"_s, link } };
	call(u"groups.invites.revoke"_s, group, body, guard, [=](const QJsonObject &) {
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Groups::invitePreview(
		const QString &token,
		base::Guard guard,
		Fn<void(const GroupInvitePreview &)> done,
		Fail fail) {
	// No group id: the caller is usually not a member yet.
	call(u"groups.invite.preview"_s, QString(), QJsonObject{ { u"token"_s, token } }, guard, [=](const QJsonObject &result) {
		if (done) {
			done(GroupInvitePreview::FromJson(result));
		}
	}, std::move(fail));
}

void Groups::joinByInvite(
		const QString &token,
		base::Guard guard,
		Fn<void(const GroupJoinResult &)> done,
		Fail fail) {
	call(u"groups.invite.join"_s, QString(), QJsonObject{ { u"token"_s, token } }, guard, [=, this](const QJsonObject &result) {
		auto answer = GroupJoinResult();
		const auto header = result.value("group"_L1).toObject();
		if (!Json::String(header, "group"_L1).isEmpty()) {
			// Joined (or a member already): the row appears at once; the
			// update that says the same finds the group held.
			answer.group = Group::FromJson(header);
			applyHeader(*answer.group);
		} else {
			answer.pending = true;
		}
		if (done) {
			done(answer);
		}
	}, std::move(fail));
}

void Groups::loadJoinRequests(
		const QString &group,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const GroupJoinRequestsPage &)> done,
		Fail fail) {
	auto body = QJsonObject{
		{ u"group"_s, group },
		{ u"limit"_s, QString::number(std::min(membersPageSize(), 50)) },
	};
	if (!cursor.isEmpty()) {
		body.insert(u"cursor"_s, cursor);
	}
	call(u"groups.join_requests"_s, group, body, guard, [=, this](const QJsonObject &result) {
		auto page = GroupJoinRequestsPage();
		page.requests = Json::List<GroupJoinRequest>(result.value("requests"_L1));
		page.nextCursor = Json::String(result, "next_cursor"_L1);
		auto ids = QStringList();
		for (const auto &request : std::as_const(page.requests)) {
			ids.push_back(request.user);
		}
		_session->users().ensure(ids, guard, [=] {
			if (done) {
				done(page);
			}
		});
	}, std::move(fail));
}

void Groups::decideJoinRequest(
		const QString &group,
		const QString &user,
		bool approve,
		base::Guard guard,
		Fn<void()> done,
		Fail fail) {
	const auto body = QJsonObject{ { u"group"_s, group }, { u"user"_s, user } };
	const auto method = approve
		? u"groups.join_requests.approve"_s
		: u"groups.join_requests.decline"_s;
	call(method, group, body, guard, [=, this](const QJsonObject &) {
		// The count of the waiting requests is the header's.
		refreshHeader(group);
		if (approve) {
			Q_EMIT membersChanged(group);
		}
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Groups::loadMentions(
		const QString &group,
		base::Guard guard,
		Fn<void(const QVector<qint64> &)> done) {
	_session->transport().rpc(u"groups.mentions"_s)
		.body(QJsonObject{ { u"group"_s, group } })
		.guard(this)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(Json::Int64s(result.value("seqs"_L1)));
			}
		})
		.fail([=, this](const Api::Error &error) {
			handleError(group, error);
			if (done && guard.alive()) {
				done({});
			}
		})
		.send();
}

} // namespace Data
