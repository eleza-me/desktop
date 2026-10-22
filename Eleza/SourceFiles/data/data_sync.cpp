#include "data/data_sync.h"

#include "api/api_client_config.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_graph.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_messages.h"
#include "data/data_notifications.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_upload.h"
#include "data/data_users.h"
#include "rt/rt_proto.h"
#include "storage/storage_database.h"

namespace Data {
namespace {

const auto kPtsKey = u"pts"_s;

// The payload of an update: over the connection `data` is the UpdateData
// oneof ({ <field of kind>: payload }), over HTTP it is the payload itself.
[[nodiscard]] QJsonObject Payload(const QString &kind, const QJsonObject &data) {
	const auto field = Rt::Proto::DataFieldOfKind(kind);
	if (!field.isEmpty() && data.size() == 1 && data.value(field).isObject()) {
		return data.value(field).toObject();
	}
	return data;
}

[[nodiscard]] bool IsMessageKind(const QString &kind) {
	return kind == u"message"
		|| kind == u"edited"
		|| kind == u"deleted"
		|| kind == u"reaction"
		|| kind == u"live_stopped";
}

} // namespace

Sync::Sync(Session *session)
: QObject(session)
, _session(session)
, _fallbackTimer([this] {
	if (!_started) {
		return;
	} else if (!_session->transport().connected()) {
		catchUp();
		// Groups have a sync track of their own; without the connection
		// nothing else tells what changed in them.
		_session->groups().requestSync();
		scheduleFallback();
	}
})
, _inboxReload([this] {
	if (_started) {
		Q_EMIT inboxReloadRequested();
		_session->chats().loadInbox();
	}
}) {
	qRegisterMetaType<Data::Message>();
}

void Sync::start(base::Guard guard, Fn<void()> ready) {
	const auto begin = [=, this] {
		if (_started) {
			return;
		}
		_started = true;
		_session->transport().setPtsProvider([this] {
			return _pts.value_or(0);
		});
		connect(
			&_session->transport(),
			&Transport::frameReceived,
			this,
			&Sync::handle,
			Qt::UniqueConnection);
		scheduleFallback();
		if (!_pts && !_session->transport().connected()) {
			// The connection is not up (yet, or at all): without a pts the
			// first fallback tick would only learn where the log is, and
			// everything that happened between the first inbox and that tick
			// would be lost. Read the state at once; a Hello that comes
			// first wins, one that comes later reads the difference from it.
			catchUp();
		}
		if (ready && guard.alive()) {
			ready();
		}
	};
	const auto db = _session->local();
	if (!db) {
		begin();
		return;
	}
	db->meta(kPtsKey, this, [=, this](QJsonValue value) {
		if (!_pts && !value.isUndefined() && !value.isNull()) {
			_pts = Json::Int64(value);
		}
		begin();
	});
}

void Sync::stop() {
	if (!_started) {
		return;
	}
	_started = false;
	disconnect(&_session->transport(), &Transport::frameReceived, this, &Sync::handle);
	_session->transport().setPtsProvider(nullptr);
	_fallbackTimer.cancel();
	_inboxReload.cancel();
	_replaying = false;
	_syncAgain = false;
	updateUpdating();
}

void Sync::setPts(qint64 pts) {
	_pts = pts;
	if (const auto db = _session->local()) {
		db->setMeta(kPtsKey, double(pts));
	}
	Q_EMIT ptsChanged(pts);
}

void Sync::updateUpdating() {
	const auto now = updating();
	if (_wasUpdating != now) {
		_wasUpdating = now;
		_session->transport().setUpdating(now);
		Q_EMIT updatingChanged(now);
	}
}

void Sync::handle(const Rt::Frame &frame) {
	if (!_started) {
		return;
	}
	using Type = Rt::Frame::Type;
	switch (frame.type) {
	case Type::Hello:
		_fallbackTimer.cancel();
		if (!_pts) {
			setPts(frame.pts);
		} else if (frame.resumed) {
			_replaying = true;
			updateUpdating();
		} else if (frame.pts > *_pts) {
			_replaying = false;
			catchUp();
		}
		if (!frame.resumed) {
			_replaying = false;
			updateUpdating();
		}
		// What waited for the connection goes out now.
		_session->outbox().retryAll();
		_session->chats().flushDrafts();
		_session->chats().flushActionBarDismissals();
		// Connected (first connect, reconnect, resume — every time): what
		// the frames of the gap would have said about my groups.
		_session->groups().requestSync();
		break;
	case Type::Resumed:
		_replaying = false;
		updateUpdating();
		if (_syncAgain) {
			_syncAgain = false;
			catchUp();
		}
		flushAck(true);
		break;
	case Type::Resync:
		_replaying = false;
		updateUpdating();
		catchUp();
		break;
	case Type::Disconnected:
		_replaying = false;
		_syncAgain = false;
		updateUpdating();
		scheduleFallback();
		break;
	case Type::Replayed: {
		const auto local = _pts.value_or(-1);
		if (frame.pts <= local) {
			return;
		} else if (frame.pts != local + 1) {
			// A hole in the replay: the difference after it fills it.
			_syncAgain = true;
			return;
		}
		applyItem(frame.data, false);
		setPts(frame.pts);
		flushAck(false);
	} break;
	case Type::Update: {
		if (_pts && frame.pts <= *_pts) {
			return;
		} else if (_pts && frame.pts == *_pts + 1) {
			setPts(frame.pts);
			if (!frame.data.isEmpty()) {
				applyLive(frame.kind, frame.data, frame.pts, true);
			}
			flushAck(false);
		} else {
			catchUp();
		}
	} break;
	case Type::Typing:
		_session->chats().setActivity(
			frame.chat,
			(frame.kind == u"cancel") ? QString() : frame.kind,
			frame.ttl);
		break;
	case Type::Presence:
		if (!frame.reset) {
			auto presence = Presence();
			presence.user = frame.user;
			presence.status = frame.status;
			presence.lastSeenAt = frame.lastSeenAt;
			_session->chats().setPresence(presence);
		}
		break;
	case Type::Profile:
		// The card changed: Users::changed() updates the rows that show it.
		_session->users().refresh(frame.user);
		break;
	case Type::Topics:
		_session->conversations().notifyOpenListChanged(frame.category);
		break;
	case Type::Group:
		// A change of a group: its own counter, its own rule.
		_session->groups().handleFrame(frame.data);
		break;
	case Type::GroupTyping:
		// Mine from another device is not shown.
		if (frame.user != _session->userId()) {
			_session->chats().setActivity(
				frame.chat,
				(frame.kind == u"cancel") ? QString() : frame.kind,
				frame.ttl,
				frame.user);
		}
		break;
	case Type::GroupAdmin:
		_session->groups().applyAdminSignal(frame.chat, frame.kind, frame.count, frame.user);
		break;
	case Type::Config:
		// Right after every Hello and on every change: read the document
		// only when it is not the one held.
		if (const auto config = _session->clientConfig()) {
			if (frame.version <= 0 || frame.version != config->version()) {
				config->load();
			}
		}
		break;
	case Type::Settings:
		_session->graph().settingsVersionChanged(frame.version);
		break;
	case Type::Media:
		_session->upload().mediaProcessed(
			Json::String(frame.data, "media"_L1),
			Json::String(frame.data, "state"_L1),
			Json::String(frame.data, "reason"_L1));
		break;
	case Type::FeedNew:
	case Type::LiveLocation:
	case Type::Call:
	case Type::Signal:
		// Screens that care listen to the transport's frames themselves.
		break;
	}
}

void Sync::flushAck(bool force) {
	++_unacked;
	if (!force && _unacked < kAckEvery) {
		return;
	}
	_unacked = 0;
	if (_pts) {
		_session->transport().ack(*_pts);
	}
}

// A live update whose typed payload came with the frame.
void Sync::applyLive(
		const QString &kind,
		const QJsonObject &d,
		qint64 framePts,
		bool live) {
	const auto chat = Json::String(d, "chat"_L1);
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	if (kind == u"message" || kind == u"edited" || kind == u"deleted" || kind == u"live_stopped") {
		applyMessage(Message::FromJson(d), live && kind == u"message");
	} else if (kind == u"reaction") {
		// The frame carries only the delta: read the message once.
		messages.refresh(chat, Json::Int64(d, "seq"_L1));
	} else if (kind == u"read") {
		if (chat.isEmpty()) {
			return;
		}
		const auto seq = Json::Int64(d, "seq"_L1);
		const auto reader = Json::String(d, "reader"_L1);
		const auto row = chats.chat(chat);
		const auto &known = messages.history(chat).info;
		const auto info = known ? &*known : row ? &row->info : nullptr;
		const auto mine = info
			? info->myRefs(_session->userId()).contains(reader)
			: (reader == _session->userId());
		if (!reader.isEmpty() && !mine) {
			messages.markPeerRead(chat, seq);
		} else if (row && row->lastReadSeq < seq) {
			// I read it on another device, maybe not to the end.
			const auto left = UnreadAfterRead(
				*row,
				messages.held(chat) ? &messages.history(chat) : nullptr,
				seq);
			chats.patch(chat, [&](Chat &c) {
				c.lastReadSeq = seq;
				c.info.lastReadSeq = seq;
				c.unread = left.count;
				c.unreadCapped = left.capped;
			});
		}
	} else if (kind == u"played") {
		const auto seq = Json::Int64(d, "seq"_L1);
		if (!chat.isEmpty() && seq > 0) {
			messages.patchMessage(chat, Message::KeyForSeq(seq), [](Message &message) {
				message.played = true;
			});
		}
	} else if (kind == u"draft") {
		if (!chat.isEmpty()) {
			const auto draft = Json::Optional<Draft>(d, "draft"_L1);
			chats.patch(chat, [&](Chat &c) {
				c.draft = (draft && !draft->empty()) ? draft : std::nullopt;
			});
		}
	} else if (kind == u"cleared") {
		if (!chat.isEmpty()) {
			applyCleared(chat, Json::Int64(d, "cleared_before_seq"_L1), true);
		}
	} else if (kind == u"group") {
		// My own place in a group: membership, read mark, settings, my
		// reaction, my draft.
		_session->groups().applyUser(d);
	} else if (kind == u"member") {
		if (chat.isEmpty()) {
			return;
		}
		chats.patch(chat, [&](Chat &c) {
			if (Json::Has(d, "muted"_L1)) {
				c.muted = Json::Bool(d, "muted"_L1);
				c.info.muted = c.muted;
				c.mutedUntil = Json::Int64(d, "muted_until"_L1);
			}
			if (Json::Has(d, "archived"_L1)) {
				c.archived = Json::Bool(d, "archived"_L1);
				c.info.archived = c.archived;
			}
		});
		if (const auto cleared = Json::Int64(d, "cleared_before_seq"_L1)) {
			applyCleared(chat, cleared, false);
		}
		if (const auto hidden = Json::Int64(d, "hidden_seq"_L1)) {
			// I deleted one message for myself on another device.
			messages.removeMessage(chat, Message::KeyForSeq(hidden));
		}
		if (Json::Bool(d, "action_bar_dismissed"_L1)) {
			// The report / block bar went on another device (×, a report).
			chats.forgetActionBar(chat);
		}
	} else if (kind == u"message_pinned" || kind == u"message_unpinned") {
		const auto seq = Json::Int64(d, "seq"_L1);
		if (!chat.isEmpty() && seq > 0) {
			const auto pinned = Json::Has(d, "pinned"_L1)
				? Json::Bool(d, "pinned"_L1)
				: (kind == u"message_pinned");
			messages.applyPin(chat, seq, pinned && kind == u"message_pinned");
		}
	} else if (kind == u"state") {
		if (chat.isEmpty()) {
			return;
		}
		const auto state = Json::String(d, "state"_L1);
		auto conversation = std::optional<Conversation>();
		if (const auto object = d.value("conversation"_L1).toObject();
			!Json::String(object, "id"_L1).isEmpty()) {
			conversation = Conversation::FromJson(object);
		}
		_session->conversations().notifyState(chat, state, conversation, Json::String(d, "event"_L1));
		if (state == u"purged") {
			messages.clear(chat);
		}
		const auto known = (chats.chat(chat) != nullptr);
		if (known) {
			chats.patch(chat, [&](Chat &c) {
				if (!state.isEmpty()) {
					c.state = state;
					c.info.state = state;
				}
				if (conversation) {
					c.conversation = conversation;
					c.info.conversation = conversation;
				}
			});
		}
		static const auto kFinal = QStringList{
			u"readonly"_s, u"ended"_s, u"expired"_s, u"closed"_s, u"purged"_s, u"continued"_s,
		};
		if (conversation && !kFinal.contains(conversation->state)) {
			// A join / a request accepted: the full header comes with one difference item.
			catchUp(framePts - 1);
		} else if (!known) {
			reloadInboxSoon();
		}
	} else if (kind == u"notif") {
		_session->notifications().applyUnread(
			Json::Int64(d, "news"_L1),
			Json::CountMap(d.value("kinds"_L1)));
	}
}

// I cleared the history up to `clearedTo` (here or on another device): the
// messages at or below it are gone for me. A chat whose whole history is
// cleared leaves the list until something new is written in it, unless it
// is pinned or has a draft — the same rule the server's inbox applies.
void Sync::applyCleared(const QString &chat, qint64 clearedTo, bool all) {
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	if (clearedTo > 0) {
		messages.clear(chat, clearedTo + 1);
	} else if (all) {
		messages.clear(chat);
	} else {
		return;
	}
	const auto row = chats.chat(chat);
	if (!row) {
		return;
	}
	const auto upTo = (clearedTo > 0) ? clearedTo : row->lastSeq;
	const auto whole = (upTo >= row->lastSeq);
	const auto kept = row->pinned || (row->draft && !row->draft->empty());
	if (whole && !kept && !row->isGroup) {
		chats.markCleared(chat, std::max(upTo, row->lastSeq));
		chats.remove(chat);
	} else if (row->lastMessage
		&& row->lastMessage->seq > 0
		&& row->lastMessage->seq <= upTo) {
		chats.patch(chat, [&](Chat &c) {
			c.lastMessage = std::nullopt;
			c.lastMine = false;
			c.unread = 0;
			c.unreadCapped = false;
		});
	}
}

void Sync::applyMessage(Message message, bool live) {
	if (message.chat.isEmpty()) {
		return;
	}
	auto &chats = _session->chats();
	auto &messages = _session->messages();
	const auto chatId = message.chat;
	const auto row = chats.chat(chatId);
	const auto &history = messages.history(chatId);
	const auto info = history.info ? &*history.info : row ? &row->info : nullptr;
	if (!row) {
		// A chat we have no row for (a new thread, a request): the inbox has it.
		reloadInboxSoon();
	}
	if (!message.visible()) {
		messages.removeMessage(chatId, Message::KeyForSeq(message.seq));
		if (row && row->lastSeq == message.seq) {
			auto last = std::optional<Message>();
			for (auto i = history.items.crbegin(); i != history.items.crend(); ++i) {
				if (i->seq > 0) {
					last = *i;
					break;
				}
			}
			chats.patch(chatId, [&](Chat &c) {
				c.lastMessage = last;
				c.lastMine = last && last->mine;
			});
		}
		return;
	}
	message.resolveLocal(info, _session->userId());
	const auto mine = message.mine;
	// A timeline takes what lies inside its window and, when it reaches the
	// newest message, what extends it (anything else would show a hole
	// between its run and this message); my own echo always replaces its
	// pending copy. The cache follows a timeline that reaches the newest
	// message; anywhere else only a copy it holds is brought up to date (an
	// old message edited), so it never gets an island either.
	const auto echo = !message.clientMessageId.isEmpty()
		&& history.indexOfKey(Message::KeyForLocal(message.clientMessageId)) >= 0;
	const auto attached = history.loaded && !history.hasMoreAfter;
	const auto first = history.firstSeq();
	const auto last = history.lastSeq();
	const auto inside = (first > 0) && (message.seq >= first) && (message.seq <= last);
	const auto extends = attached && (first == 0 || message.seq > last);
	const auto cached = attached && (inside || extends || echo);
	if (inside || extends || echo) {
		messages.merge(chatId, { message }, cached);
	}
	if (!cached) {
		messages.updateCached(chatId, { message });
	}
	if (const auto current = chats.chat(chatId)) {
		const auto isNew = message.seq > current->lastSeq;
		const auto isLast = isNew || message.seq == current->lastSeq;
		const auto countsUnread = isNew && !mine && !chats.readingLive(chatId);
		chats.patch(chatId, [&](Chat &c) {
			// A message waiting in the outbox stays the row's preview.
			const auto pending = c.lastMessage && c.lastMessage->local()
				&& c.lastMessage->clientMessageId != message.clientMessageId;
			if (isLast && !pending) {
				c.lastMessage = message;
				c.lastMine = mine;
			}
			if (isNew) {
				c.lastActivityAt = std::max(c.lastActivityAt, message.createdAt);
				c.lastSeq = message.seq;
				c.info.lastSeq = std::max(c.info.lastSeq, message.seq);
				if (mine) {
					c.draft = std::nullopt;
				}
			}
			if (countsUnread) {
				++c.unread;
			}
		});
		if (live && isNew && !mine) {
			Q_EMIT messageReceived(message);
		}
	}
}

void Sync::applyItem(const QJsonObject &item, bool live) {
	if (const auto info = item.value("chat_info"_L1).toObject();
		!Json::String(info, "chat"_L1).isEmpty()) {
		rememberInfo(ChatInfo::FromJson(info));
	}
	const auto kind = Json::String(item, "kind"_L1);
	const auto data = item.value("data"_L1).toObject();
	if (IsMessageKind(kind)) {
		const auto wrapped = data.value("message"_L1).toObject();
		const auto view = !Json::String(wrapped, "chat"_L1).isEmpty()
			? wrapped
			: item.value("message"_L1).toObject();
		const auto chat = Json::String(item, "chat"_L1);
		const auto seq = Json::Int64(item, "seq"_L1);
		if (!Json::String(view, "chat"_L1).isEmpty()) {
			applyMessage(Message::FromJson(view), live && kind == u"message");
		} else if (Json::Bool(item, "removed"_L1) && !chat.isEmpty() && seq > 0) {
			_session->messages().removeMessage(chat, Message::KeyForSeq(seq));
		}
		return;
	}
	if (kind == u"read" && item.value("chat_info"_L1).isObject()) {
		const auto info = ChatInfo::FromJson(item.value("chat_info"_L1).toObject());
		if (info.valid()) {
			if (info.peerReadSeq) {
				_session->messages().markPeerRead(info.chat, info.peerReadSeq);
			}
			const auto row = _session->chats().chat(info.chat);
			if (info.lastReadSeq && row && row->lastReadSeq < info.lastReadSeq) {
				auto &messages = _session->messages();
				const auto left = UnreadAfterRead(
					*row,
					messages.held(info.chat) ? &messages.history(info.chat) : nullptr,
					info.lastReadSeq);
				_session->chats().patch(info.chat, [&](Chat &c) {
					c.lastReadSeq = info.lastReadSeq;
					c.info.lastReadSeq = info.lastReadSeq;
					c.unread = left.count;
					c.unreadCapped = left.capped;
				});
			}
			return;
		}
	}
	if (!data.isEmpty()) {
		applyLive(kind, Payload(kind, data), Json::Int64(item, "pts"_L1), live);
	}
}

void Sync::rememberInfo(const ChatInfo &info) {
	const auto known = (_session->chats().chat(info.chat) != nullptr);
	// Names fill in when the cards arrive (Users::changed updates the rows).
	_session->users().request(info.peopleIds());
	if (_session->chats().applyInfo(info) && !known) {
		// The row is new: the inbox entry has its unread count and last message.
		reloadInboxSoon();
	}
	_session->messages().setInfo(info);
}

void Sync::reloadInboxSoon() {
	if (_started) {
		_inboxReload.callOnce(kInboxReloadDelayMs);
	}
}

// ---- fallback difference while the connection is down

void Sync::scheduleFallback() {
	if (!_started || _session->transport().connected()) {
		_fallbackTimer.cancel();
		return;
	}
	const auto config = _session->clientConfig();
	const auto seconds = config ? config->fallbackSyncInterval() : 30;
	_fallbackTimer.callOnce(int(std::clamp<qint64>(seconds, 5, 600)) * 1000);
}

// ---- difference

void Sync::catchUp(std::optional<qint64> from) {
	if (!_started) {
		return;
	} else if (_syncing || _replaying) {
		_syncAgain = true;
		return;
	}
	_syncing = true;
	updateUpdating();
	syncLoop(from);
}

void Sync::syncLoop(std::optional<qint64> from) {
	_syncAgain = false;
	syncOnce(from, [this] {
		if (_syncAgain && _started) {
			syncLoop(std::nullopt);
			return;
		}
		_syncing = false;
		updateUpdating();
	});
}

void Sync::readState(Fn<void(std::optional<qint64>)> done) {
	_session->transport().rpc(u"sync.state"_s)
		.guard(this)
		.done([=](const QJsonObject &result) {
			done(Json::Int64(result, "pts"_L1));
		})
		.fail([=](const Api::Error &error) {
			qInfo("Data: sync.state failed: %s", qPrintable(error.forLog()));
			done(std::nullopt);
		})
		.send();
}

void Sync::syncOnce(std::optional<qint64> from, Fn<void()> done) {
	if (!_pts) {
		readState([=, this](std::optional<qint64> pts) {
			if (pts && !_pts) {
				setPts(*pts);
			}
			done();
		});
		return;
	}
	differenceStep(from.value_or(*_pts), 0, std::move(done));
}

void Sync::differenceStep(qint64 pts, int page, Fn<void()> done) {
	if (!_started) {
		done();
		return;
	} else if (page >= kMaxDifferencePages) {
		// Enough pages for one round, but the log goes on: another round
		// starts from where this one stopped (every page moved the pts),
		// instead of leaving the rest for a gap that may never come.
		_syncAgain = true;
		done();
		return;
	}
	_session->transport().rpc(u"sync.difference"_s)
		.body(QJsonObject{ { u"pts"_s, QString::number(pts) } })
		.guard(this)
		.done([=, this](const QJsonObject &diff) {
			const auto serverPts = Json::Int64(diff, "pts"_L1);
			if (Json::Bool(diff, "too_long"_L1)) {
				// The log no longer reaches back to our pts: take the current
				// state and read everything that is cached again.
				readState([=, this](std::optional<qint64> state) {
					if (!state) {
						done();
						return;
					}
					const auto resumed = std::max(*state, serverPts);
					setPts(resumed);
					_session->chats().loadInbox();
					_session->messages().markAllStale();
					if (resumed <= pts) {
						done();
					} else {
						differenceStep(resumed, page + 1, done);
					}
				});
				return;
			}
			const auto updates = diff.value("updates"_L1).toArray();
			// The highest pts applied: an item without one (or out of
			// order) must not move the point back.
			auto last = updates.isEmpty() ? std::max(pts, serverPts) : pts;
			for (const auto &item : updates) {
				const auto update = item.toObject();
				applyItem(update, false);
				last = std::max(last, Json::Int64(update, "pts"_L1));
			}
			if (last > _pts.value_or(0)) {
				setPts(last);
			}
			if (updates.isEmpty() || last >= serverPts || last <= pts) {
				// Done, or a page that did not move: the next gap asks again.
				done();
			} else {
				differenceStep(last, page + 1, done);
			}
		})
		.fail([=](const Api::Error &error) {
			qInfo("Data: sync.difference failed: %s", qPrintable(error.forLog()));
			done();
		})
		.send();
}

} // namespace Data
