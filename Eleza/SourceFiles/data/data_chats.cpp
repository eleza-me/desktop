#include "data/data_chats.h"

#include "base/unixtime.h"
#include "data/data_json.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "storage/storage_database.h"

#include <algorithm>

namespace Data {
namespace {

constexpr auto kMaxInboxPages = 20;
constexpr auto kInboxPageSize = 50;
constexpr auto kTypingThrottleMs = 3000;
const auto kClearedKey = u"clearedChats"_s;

[[nodiscard]] bool InSection(const Chat &chat, Chats::Section section) {
	switch (section) {
	case Chats::Section::Messages:
		return !chat.archived && !chat.isRequest() && !chat.isConversation;
	case Chats::Section::Conversations:
		return !chat.archived && !chat.isRequest() && chat.isConversation;
	case Chats::Section::Requests:
		return chat.isRequest();
	case Chats::Section::Archived:
		return chat.archived && !chat.isRequest();
	case Chats::Section::All:
		return true;
	}
	return false;
}

} // namespace

Chats::Chats(Session *session)
: QObject(session)
, _session(session)
, _activityTimer([this] { activityTick(); }) {
}

const Chat *Chats::chat(const QString &id) const {
	const auto i = _chats.constFind(id);
	return (i != _chats.cend()) ? &*i : nullptr;
}

QVector<Chat> Chats::list(Section section) const {
	auto result = QVector<Chat>();
	for (const auto &chat : _chats) {
		if (InSection(chat, section)) {
			result.push_back(chat);
		}
	}
	std::sort(result.begin(), result.end(), ChatLess);
	return result;
}

int Chats::count(Section section) const {
	return int(std::count_if(_chats.cbegin(), _chats.cend(), [&](const Chat &chat) {
		return InSection(chat, section);
	}));
}

QVector<Chat> Chats::all() const {
	return QVector<Chat>(_chats.cbegin(), _chats.cend());
}

QString Chats::chatOfConversation(const QString &conversationId) const {
	for (const auto &chat : _chats) {
		if (chat.conversation && chat.conversation->id == conversationId) {
			return chat.id;
		}
	}
	return QString();
}

const Chat *Chats::savedChat() const {
	for (const auto &chat : _chats) {
		if (chat.saved) {
			return &chat;
		}
	}
	return nullptr;
}

int Chats::unreadRequests() const {
	return int(std::count_if(_chats.cbegin(), _chats.cend(), [](const Chat &chat) {
		return chat.isRequest() && chat.unread > 0;
	}));
}

void Chats::recount() {
	auto unread = 0;
	for (const auto &chat : _chats) {
		if (!chat.archived && !chat.muted && !chat.isRequest() && chat.unread > 0) {
			++unread;
		}
	}
	if (_unreadChats != unread) {
		_unreadChats = unread;
		Q_EMIT unreadChanged();
	}
}

void Chats::store(const Chat &chat) {
	if (const auto db = _session->local()) {
		db->putChats({ { chat.id, chat.toStored() } });
	}
}

void Chats::changed(const QString &id, bool reordered) {
	recount();
	Q_EMIT chatChanged(id);
	if (reordered) {
		Q_EMIT listChanged();
	}
}

// ---- hidden after «delete for me»
//
// The server keeps listing a cleared chat (an empty row would come back on
// the next inbox read): the clear is remembered so the row stays hidden until
// something newer than the recorded seq arrives.

bool Chats::isCleared(const Chat &chat) {
	const auto i = _cleared.find(chat.id);
	if (i == _cleared.end()) {
		return false;
	}
	if (chat.lastSeq > *i || (chat.draft && !chat.draft->empty())) {
		_cleared.erase(i);
		persistCleared();
		return false;
	}
	return true;
}

void Chats::markCleared(const QString &id, qint64 lastSeq) {
	_cleared.insert(id, lastSeq);
	persistCleared();
}

void Chats::persistCleared() {
	if (const auto db = _session->local()) {
		auto object = QJsonObject();
		for (auto i = _cleared.cbegin(); i != _cleared.cend(); ++i) {
			object.insert(i.key(), double(i.value()));
		}
		db->setMeta(kClearedKey, object);
	}
}

// ---- mapping

std::optional<Chat> Chats::rowFromEntry(const InboxEntry &entry) const {
	if (!entry.chatInfo) {
		return std::nullopt;
	}
	const auto &info = *entry.chatInfo;
	const auto me = _session->userId();
	const auto peer = info.peer();
	auto result = Chat();
	result.id = info.chat;
	result.isConversation = info.conversation.has_value();
	result.section = entry.section.isEmpty()
		? (!info.request.isEmpty()
			? u"request"_s
			: info.conversation
			? u"conversation"_s
			: u"message"_s)
		: entry.section;
	result.saved = info.saved;
	result.peerUserId = peer ? peer->userId : QString();
	result.peerRef = peer ? peer->ref : QString();
	const auto card = result.peerUserId.isEmpty()
		? User()
		: _session->users().peek(result.peerUserId);
	result.anonymous = info.conversation
		&& ((peer && peer->masked) || result.peerUserId.isEmpty());
	if (result.saved) {
		result.title = QString();
	} else if (info.conversation) {
		result.title = info.conversation->topic;
	} else if (peer && peer->masked && !peer->maskName.isEmpty()) {
		result.title = peer->maskName;
	} else {
		result.title = card.displayName;
	}
	result.isBot = card.isBot || (peer && peer->bot);
	result.verified = card.verified;
	result.peerDeleted = card.deleted
		|| (peer && peer->deleted)
		|| info.peerDeleted
		|| entry.peerDeleted;
	if (entry.lastMessage && entry.lastMessage->visible()) {
		auto last = *entry.lastMessage;
		last.resolveLocal(&info, me);
		result.lastMine = last.mine;
		result.lastMessage = std::move(last);
	}
	result.lastActivityAt = entry.lastActivityAt
		? entry.lastActivityAt
		: entry.lastMessage
		? entry.lastMessage->createdAt
		: 0;
	result.unread = entry.unread;
	result.unreadCapped = entry.unreadCapped;
	result.muted = entry.muted.value_or(info.muted);
	result.mutedUntil = entry.mutedUntil ? entry.mutedUntil : info.mutedUntil;
	result.pinned = entry.pinned.value_or(false);
	result.archived = entry.archived.value_or(info.archived);
	result.request = !info.request.isEmpty()
		? info.request
		: (entry.section == u"request")
		? u"incoming"_s
		: QString();
	result.canSend = info.canSend;
	result.state = info.state;
	if (entry.draft && !entry.draft->empty()) {
		result.draft = entry.draft;
	} else if (info.draft && !info.draft->empty()) {
		result.draft = info.draft;
	}
	result.lastSeq = std::max(entry.lastSeq, info.lastSeq);
	result.peerReadSeq = info.peerReadSeq;
	result.lastReadSeq = info.lastReadSeq;
	result.conversation = info.conversation;
	result.theme = info.theme;
	result.timeRingExpiresAt = entry.timeRingExpiresAt;
	result.info = info;
	return result;
}

bool Chats::applyInfo(const ChatInfo &info) {
	if (!info.valid()) {
		return false;
	}
	const auto previous = chat(info.chat);
	auto entry = InboxEntry();
	entry.chat = info.chat;
	entry.section = previous
		? previous->section
		: !info.request.isEmpty()
		? u"request"_s
		: info.conversation
		? u"conversation"_s
		: u"message"_s;
	entry.state = info.state;
	entry.lastSeq = info.lastSeq;
	entry.unread = previous ? previous->unread : 0;
	entry.unreadCapped = previous ? previous->unreadCapped : false;
	entry.muted = info.muted;
	entry.mutedUntil = info.mutedUntil;
	if (previous) {
		entry.pinned = previous->pinned;
		entry.lastActivityAt = previous->lastActivityAt;
		entry.timeRingExpiresAt = previous->timeRingExpiresAt;
	}
	entry.archived = info.archived;
	entry.chatInfo = info;
	entry.draft = info.draft;
	auto row = rowFromEntry(entry);
	if (!row) {
		return false;
	}
	if (previous) {
		// The header has no last message: the preview stays.
		row->lastMessage = previous->lastMessage;
		row->lastMine = previous->lastMine;
		row->lastSeq = std::max(row->lastSeq, previous->lastSeq);
		// A request that was accepted moves to its section.
		if (previous->isRequest() && info.request.isEmpty()) {
			row->section = info.conversation ? u"conversation"_s : u"message"_s;
		}
	}
	if (const auto local = _localDrafts.constFind(row->id); local != _localDrafts.cend()) {
		row->draft = local->empty() ? std::nullopt : std::make_optional(*local);
	}
	if (isCleared(*row)) {
		return false;
	}
	upsert(*row);
	return true;
}

// ---- store operations

void Chats::replaceAll(const QVector<Chat> &chats) {
	const auto before = _chats.keys();
	_chats.clear();
	auto rows = QVector<Storage::Database::Row>();
	for (const auto &chat : chats) {
		if (isCleared(chat)) {
			continue;
		}
		_chats.insert(chat.id, chat);
		rows.push_back({ chat.id, chat.toStored() });
	}
	if (const auto db = _session->local()) {
		db->putChats(rows, true);
	}
	recount();
	for (const auto &id : before) {
		if (!_chats.contains(id)) {
			Q_EMIT chatRemoved(id);
		}
	}
	for (const auto &chat : _chats) {
		Q_EMIT chatChanged(chat.id);
	}
	Q_EMIT listChanged();
}

void Chats::upsert(const Chat &chat) {
	if (isCleared(chat)) {
		return;
	}
	_chats.insert(chat.id, chat);
	store(chat);
	changed(chat.id, true);
}

void Chats::patch(const QString &id, const Fn<void(Chat &)> &change) {
	const auto i = _chats.find(id);
	if (i == _chats.end()) {
		return;
	}
	const auto pinned = i->pinned;
	const auto activity = i->lastActivityAt;
	const auto archived = i->archived;
	const auto section = i->section;
	change(*i);
	i->id = id;
	store(*i);
	changed(id, pinned != i->pinned
		|| activity != i->lastActivityAt
		|| archived != i->archived
		|| section != i->section);
}

void Chats::remove(const QString &id) {
	if (!_chats.remove(id)) {
		return;
	}
	_session->messages().forget(id);
	if (const auto db = _session->local()) {
		db->deleteChat(id);
	}
	_localDrafts.remove(id);
	recount();
	Q_EMIT chatRemoved(id);
	Q_EMIT listChanged();
}

void Chats::applyCard(const User &card) {
	if (!card.valid()) {
		return;
	}
	for (auto i = _chats.begin(); i != _chats.end(); ++i) {
		if (i->peerUserId != card.id || i->saved || i->conversation) {
			continue;
		}
		const auto title = card.displayName.isEmpty() ? i->title : card.displayName;
		const auto deleted = card.deleted || i->info.peerDeleted;
		if (i->title == title
			&& i->verified == card.verified
			&& i->isBot == (card.isBot || i->isBot)
			&& i->peerDeleted == deleted) {
			continue;
		}
		i->title = title;
		i->verified = card.verified;
		i->isBot = card.isBot || i->isBot;
		i->peerDeleted = deleted;
		store(*i);
		Q_EMIT chatChanged(i->id);
	}
}

void Chats::setVisibleChat(const QString &id) {
	_visibleChat = id;
}

// ---- loading

void Chats::loadCached(base::Guard guard, Fn<void(bool)> done) {
	const auto finish = [=](bool any) {
		if (done && guard.alive()) {
			done(any);
		}
	};
	const auto db = _session->local();
	if (!db) {
		finish(false);
		return;
	}
	db->meta(kClearedKey, this, [=, this](QJsonValue value) {
		const auto object = value.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			_cleared.insert(i.key(), Json::Int64(i.value()));
		}
	});
	db->drafts(this, [=, this](QHash<QString, QJsonObject> drafts) {
		for (auto i = drafts.cbegin(); i != drafts.cend(); ++i) {
			_localDrafts.insert(i.key(), Draft::FromJson(i.value()));
		}
	});
	db->chats(this, [=, this](QVector<QJsonObject> rows) {
		if (rows.isEmpty() || _inboxLoaded) {
			finish(!rows.isEmpty());
			return;
		}
		auto list = QVector<Chat>();
		list.reserve(rows.size());
		for (const auto &row : rows) {
			auto chat = Chat::FromStored(row);
			if (chat.id.isEmpty()) {
				continue;
			}
			if (const auto local = _localDrafts.constFind(chat.id); local != _localDrafts.cend()) {
				chat.draft = local->empty() ? std::nullopt : std::make_optional(*local);
			}
			list.push_back(std::move(chat));
		}
		// Rows written by replaceAll are back unchanged: fill the map without
		// rewriting the table.
		_chats.clear();
		for (const auto &chat : list) {
			if (!isCleared(chat)) {
				_chats.insert(chat.id, chat);
			}
		}
		recount();
		Q_EMIT listChanged();
		finish(true);
	});
}

void Chats::loadSection(
		const QString &section,
		const QString &cursor,
		int page,
		std::shared_ptr<QVector<InboxEntry>> into,
		Fn<void(bool)> done) {
	auto body = QJsonObject{
		{ u"section"_s, section },
		{ u"limit"_s, QString::number(kInboxPageSize) },
	};
	if (!cursor.isEmpty()) {
		body.insert(u"cursor"_s, cursor);
	}
	_session->transport().rpc(u"chat.inbox"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			for (const auto &item : result.value("chats"_L1).toArray()) {
				auto entry = InboxEntry::FromJson(item.toObject());
				if (entry.section.isEmpty()) {
					entry.section = section;
				}
				into->push_back(std::move(entry));
			}
			const auto next = result.value("next_cursor"_L1).toString();
			if (next.isEmpty() || next == cursor || page + 1 >= kMaxInboxPages) {
				done(true);
			} else {
				loadSection(section, next, page + 1, into, done);
			}
		})
		.fail([=, this](const Api::Error &error) {
			// The requests section is optional; the other two decide.
			if (section != u"request" && !_inboxError) {
				_inboxError = error;
			}
			done(section == u"request");
		})
		.send();
}

void Chats::loadInbox(
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	if (done || fail) {
		_inboxWaiters.push_back({ guard, { std::move(done), std::move(fail) } });
	}
	if (_inboxLoading) {
		return;
	}
	_inboxLoading = true;
	_inboxError = std::nullopt;
	Q_EMIT inboxLoadingChanged();

	struct State {
		std::shared_ptr<QVector<InboxEntry>> messages = std::make_shared<QVector<InboxEntry>>();
		std::shared_ptr<QVector<InboxEntry>> conversations = std::make_shared<QVector<InboxEntry>>();
		std::shared_ptr<QVector<InboxEntry>> requests = std::make_shared<QVector<InboxEntry>>();
		int left = 3;
		bool ok = true;
	};
	const auto state = std::make_shared<State>();
	const auto sectionDone = [=, this](bool ok) {
		state->ok = state->ok && ok;
		if (--state->left > 0) {
			return;
		}
		auto waiters = std::exchange(_inboxWaiters, {});
		if (!state->ok) {
			_inboxLoading = false;
			if (!_inboxError) {
				_inboxError = Api::Error::Offline();
			}
			Q_EMIT inboxLoadingChanged();
			const auto error = *_inboxError;
			for (const auto &[alive, callbacks] : waiters) {
				if (callbacks.second && alive.alive()) {
					callbacks.second(error);
				}
			}
			return;
		}
		auto entries = *state->messages + *state->conversations + *state->requests;
		auto people = QStringList();
		for (const auto &entry : entries) {
			if (entry.chatInfo) {
				people += entry.chatInfo->peopleIds();
			}
		}
		// Cards first, so the rows appear with their names.
		_session->users().ensure(people, this, [=, this] {
			finishInbox(entries);
			_inboxLoading = false;
			_inboxLoaded = true;
			_inboxError = std::nullopt;
			Q_EMIT inboxLoadingChanged();
			for (const auto &[alive, callbacks] : waiters) {
				if (callbacks.first && alive.alive()) {
					callbacks.first();
				}
			}
		});
	};
	loadSection(u"message"_s, QString(), 0, state->messages, sectionDone);
	loadSection(u"conversation"_s, QString(), 0, state->conversations, sectionDone);
	loadSection(u"request"_s, QString(), 0, state->requests, sectionDone);
}

void Chats::finishInbox(const QVector<InboxEntry> &entries) {
	auto list = QVector<Chat>();
	auto peers = QStringList();
	for (const auto &entry : entries) {
		auto row = rowFromEntry(entry);
		if (!row) {
			continue;
		}
		// Keep a draft typed here that the server does not know yet.
		if (const auto local = _localDrafts.constFind(row->id); local != _localDrafts.cend()) {
			row->draft = local->empty() ? std::nullopt : std::make_optional(*local);
		}
		// A message queued in the outbox stays the row's preview.
		if (const auto previous = chat(row->id)) {
			if (previous->lastMessage && previous->lastMessage->local()) {
				row->lastMessage = previous->lastMessage;
				row->lastMine = true;
				row->lastActivityAt = std::max(row->lastActivityAt, previous->lastActivityAt);
			}
		}
		if (!row->peerUserId.isEmpty()) {
			peers.push_back(row->peerUserId);
		}
		list.push_back(std::move(*row));
	}
	replaceAll(list);
	_session->transport().followProfiles(peers);
}

void Chats::loadHeader(
		const QString &chat,
		base::Guard guard,
		Fn<void(const ChatInfo &)> done,
		Fn<void(const Api::Error &)> fail) {
	_session->transport().rpc(u"chat.header"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto info = ChatInfo::FromJson(result);
			_session->messages().setInfo(info);
			_session->users().ensure(info.peopleIds(), this, [=, this] {
				applyInfo(info);
				if (done && guard.alive()) {
					done(info);
				}
			});
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Chats::openDirect(
		const QString &userId,
		base::Guard guard,
		Fn<void(const Chat &)> done,
		Fn<void(const Api::Error &)> fail) {
	_session->transport().rpc(u"chat.thread"_s)
		.body(QJsonObject{ { u"user_id"_s, userId } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto info = ChatInfo::FromJson(result);
			_session->messages().setInfo(info);
			_session->users().ensure(info.peopleIds(), this, [=, this] {
				// A chat the user opens on purpose is not hidden any more.
				if (_cleared.remove(info.chat)) {
					persistCleared();
				}
				const auto ok = applyInfo(info);
				const auto row = chat(info.chat);
				if (!ok || !row) {
					if (fail && guard.alive()) {
						fail(Api::Error::Local(500, u"bad_thread"_s, u"bad thread"_s));
					}
					return;
				}
				if (done && guard.alive()) {
					done(*row);
				}
			});
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Chats::openSaved(
		base::Guard guard,
		Fn<void(const Chat &)> done,
		Fn<void(const Api::Error &)> fail) {
	if (const auto existing = savedChat()) {
		if (done && guard.alive()) {
			done(*existing);
		}
		return;
	}
	const auto me = _session->userId();
	if (me.isEmpty()) {
		if (fail && guard.alive()) {
			fail(Api::Error::Local(401, u"unauthorized"_s, u"no session"_s));
		}
		return;
	}
	openDirect(me, guard, std::move(done), std::move(fail));
}

// ---- actions

void Chats::member(
		const QString &chat,
		const QJsonObject &body,
		const QString &action,
		Fn<void()> revert) {
	auto request = body;
	request.insert(u"chat"_s, chat);
	_session->transport().rpc(u"chat.member"_s)
		.body(request)
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			if (revert) {
				revert();
			}
			_session->reportFailure(action, chat, error);
		})
		.send();
}

void Chats::setMuted(const QString &chat, bool muted) {
	muteFor(chat, muted ? u"forever"_s : u"off"_s);
}

void Chats::muteFor(const QString &chat, const QString &duration) {
	const auto row = this->chat(chat);
	if (!row) {
		return;
	}
	const auto was = row->muted;
	const auto wasUntil = row->mutedUntil;
	const auto muted = (duration != u"off");
	patch(chat, [&](Chat &c) {
		c.muted = muted;
		c.mutedUntil = 0;
	});
	member(chat, QJsonObject{ { u"mute_for"_s, duration } }, u"chat.mute"_s, [=, this] {
		patch(chat, [&](Chat &c) {
			c.muted = was;
			c.mutedUntil = wasUntil;
		});
	});
}

void Chats::setArchived(const QString &chat, bool archived) {
	const auto row = this->chat(chat);
	if (!row) {
		return;
	}
	const auto was = row->archived;
	patch(chat, [&](Chat &c) { c.archived = archived; });
	member(chat, QJsonObject{ { u"archived"_s, archived } }, u"chat.archive"_s, [=, this] {
		patch(chat, [&](Chat &c) { c.archived = was; });
	});
}

void Chats::setPinned(const QString &chat, bool pinned) {
	const auto row = this->chat(chat);
	if (!row) {
		return;
	}
	const auto was = row->pinned;
	patch(chat, [&](Chat &c) { c.pinned = pinned; });
	_session->transport().rpc(pinned ? u"chat.pin"_s : u"chat.unpin"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			patch(chat, [&](Chat &c) { c.pinned = was; });
			_session->reportFailure(u"chat.pin"_s, chat, error);
		})
		.send();
}

void Chats::setPinnedOrder(const QStringList &chats) {
	// The list orders pinned rows by activity; the server keeps the explicit
	// order for the clients that show one.
	for (const auto &chat : _chats) {
		const auto pinned = chats.contains(chat.id);
		if (chat.pinned != pinned && !chat.isConversation && !chat.isRequest()) {
			patch(chat.id, [&](Chat &c) { c.pinned = pinned; });
		}
	}
	auto ids = QJsonArray();
	for (const auto &id : chats) {
		ids.push_back(id);
	}
	_session->transport().rpc(u"chat.pinned_chats"_s)
		.body(QJsonObject{ { u"chats"_s, ids } })
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			_session->reportFailure(u"chat.pin"_s, QString(), error);
			loadInbox();
		})
		.send();
}

void Chats::setTheme(const QString &chat, const QString &theme) {
	const auto row = this->chat(chat);
	const auto was = row ? row->theme : QString();
	patch(chat, [&](Chat &c) {
		c.theme = theme;
		c.info.theme = theme;
	});
	_session->transport().rpc(u"chat.theme"_s)
		.body(QJsonObject{ { u"chat"_s, chat }, { u"theme"_s, theme } })
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			patch(chat, [&](Chat &c) {
				c.theme = was;
				c.info.theme = was;
			});
			_session->reportFailure(u"chat.theme"_s, chat, error);
		})
		.send();
}

void Chats::deleteChat(const QString &chat, bool forEveryone) {
	const auto row = this->chat(chat);
	if (!forEveryone) {
		markCleared(chat, row ? row->lastSeq : 0);
	}
	remove(chat);
	if (forEveryone) {
		_session->transport().rpc(u"chat.delete_for_both"_s)
			.body(QJsonObject{ { u"chat"_s, chat } })
			.guard(this)
			.fail([=, this](const Api::Error &error) {
				_session->reportFailure(u"chat.delete"_s, chat, error);
				loadInbox();
			})
			.send();
	} else {
		member(chat, QJsonObject{ { u"clear_history"_s, true } }, u"chat.delete"_s, [=, this] {
			if (_cleared.remove(chat)) {
				persistCleared();
			}
			loadInbox();
		});
	}
}

void Chats::acceptRequest(const QString &chat) {
	patch(chat, [&](Chat &c) {
		c.request = QString();
		c.info.request = QString();
		c.section = c.isConversation ? u"conversation"_s : u"message"_s;
		c.canSend = true;
	});
	_session->transport().rpc(u"chat.accept"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			_session->reportFailure(u"chat.accept"_s, chat, error);
			loadInbox();
		})
		.send();
}

void Chats::declineRequest(const QString &chat) {
	remove(chat);
	_session->transport().rpc(u"chat.decline"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			_session->reportFailure(u"chat.decline"_s, chat, error);
			loadInbox();
		})
		.send();
}

// ---- drafts

void Chats::saveDraft(const QString &chat, const Draft &draft) {
	const auto row = this->chat(chat);
	const auto current = (row && row->draft) ? *row->draft : Draft();
	if (current.text == draft.text
		&& current.replyToSeq == draft.replyToSeq
		&& current.entities.size() == draft.entities.size()) {
		return;
	}
	auto local = draft;
	local.updatedAt = base::unixtime::now();
	_localDrafts.insert(chat, local);
	if (const auto db = _session->local()) {
		db->putDraft(chat, local.toJson());
	}
	patch(chat, [&](Chat &c) {
		c.draft = local.empty() ? std::nullopt : std::make_optional(local);
	});
	pushDraft(chat, local);
}

void Chats::pushDraft(const QString &chat, const Draft &draft) {
	const auto confirmed = [=, this] {
		// Unless a newer draft was typed meanwhile, the server has it now.
		const auto i = _localDrafts.find(chat);
		if (i != _localDrafts.end()
			&& i->text == draft.text
			&& i->replyToSeq == draft.replyToSeq) {
			_localDrafts.erase(i);
			if (const auto db = _session->local()) {
				db->deleteDraft(chat);
			}
		}
	};
	const auto failed = [=, this](const Api::Error &error) {
		// Best effort: after a transport failure the draft stays local and is
		// sent again when the connection is back (flushDrafts).
		if (!error.transient()) {
			confirmed();
			_session->reportFailure(u"draft.save"_s, chat, error);
		}
	};
	if (!draft.empty()) {
		auto body = QJsonObject{
			{ u"chat"_s, chat },
			{ u"text"_s, draft.text },
		};
		Json::Put(body, "reply_to_seq"_L1, draft.replyToSeq);
		Json::Put(body, "entities"_L1, Json::Array(draft.entities));
		_session->transport().rpc(u"chat.draft.save"_s)
			.body(body)
			.guard(this)
			.done(confirmed)
			.fail(failed)
			.send();
	} else {
		_session->transport().rpc(u"chat.draft.clear"_s)
			.body(QJsonObject{ { u"chat"_s, chat } })
			.guard(this)
			.done(confirmed)
			.fail(failed)
			.send();
	}
}

void Chats::clearDraft(const QString &chat) {
	saveDraft(chat, Draft());
}

void Chats::flushDrafts() {
	const auto drafts = _localDrafts;
	for (auto i = drafts.cbegin(); i != drafts.cend(); ++i) {
		pushDraft(i.key(), i.value());
	}
}

// ---- typing

void Chats::capability(const QString &chat, Fn<void(const QString &)> done) {
	const auto now = base::unixtime::nowMs();
	if (const auto i = _capabilities.constFind(chat);
		i != _capabilities.cend() && i->expiresAt > now + 5000) {
		done(i->cap);
		return;
	}
	_session->transport().rpc(u"chat.capability"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto capability = Capability();
			capability.cap = result.value("capability"_L1).toString();
			capability.expiresAt = Json::ToMs(Json::Int64(result, "expires_at"_L1));
			if (capability.cap.isEmpty()) {
				return;
			}
			_capabilities.insert(chat, capability);
			done(capability.cap);
		})
		.fail([](const Api::Error &) {
			// Typing is best effort.
		})
		.send();
}

void Chats::sendTyping(const QString &chat, const QString &kind) {
	if (!_session->transport().connected()) {
		return;
	}
	const auto now = base::unixtime::nowMs();
	const auto key = chat + u'|' + kind;
	if (kind != u"cancel") {
		if (now - _typingSentAt.value(key) < kTypingThrottleMs) {
			return;
		}
		_typingSentAt.insert(key, now);
	} else {
		// A cancel re-arms every kind of this chat.
		for (auto i = _typingSentAt.begin(); i != _typingSentAt.end();) {
			i = i.key().startsWith(chat + u'|') ? _typingSentAt.erase(i) : std::next(i);
		}
	}
	capability(chat, [=, this](const QString &cap) {
		_session->transport().sendTyping(cap, kind);
	});
}

std::optional<Activity> Chats::activity(const QString &chat) const {
	const auto i = _activity.constFind(chat);
	if (i == _activity.cend() || i->until < base::unixtime::nowMs()) {
		return std::nullopt;
	}
	return *i;
}

void Chats::setActivity(const QString &chat, const QString &kind, int ttlSeconds) {
	if (kind.isEmpty() || kind == u"cancel") {
		if (!_activity.remove(chat)) {
			return;
		}
	} else {
		const auto ttl = std::clamp(ttlSeconds, 1, 30);
		_activity.insert(chat, { kind, base::unixtime::nowMs() + qint64(ttl) * 1000 });
	}
	Q_EMIT activityChanged(chat);
	activityTick();
}

void Chats::activityTick() {
	const auto now = base::unixtime::nowMs();
	auto nearest = qint64(0);
	for (auto i = _activity.begin(); i != _activity.end();) {
		if (i->until <= now) {
			const auto chat = i.key();
			i = _activity.erase(i);
			Q_EMIT activityChanged(chat);
		} else {
			nearest = nearest ? std::min(nearest, i->until) : i->until;
			++i;
		}
	}
	if (nearest) {
		_activityTimer.callOnce(int(nearest - now) + 50);
	} else {
		_activityTimer.cancel();
	}
}

// ---- presence

void Chats::watchPresence(const QStringList &users) {
	if (users.isEmpty()) {
		return;
	}
	const auto apply = [=, this](const QJsonArray &list) {
		for (const auto &item : list) {
			setPresence(Presence::FromJson(item.toObject()));
		}
	};
	_session->transport().subscribePresence(users, this, [=, this](const QJsonArray &list) {
		if (!list.isEmpty()) {
			apply(list);
			return;
		}
		// Not connected (or nothing came back): one plain read.
		auto ids = QJsonArray();
		for (const auto &user : users) {
			ids.push_back(user);
		}
		_session->transport().rpc(u"presence"_s)
			.body(QJsonObject{ { u"users"_s, ids } })
			.guard(this)
			.done([=](const QJsonObject &result) {
				apply(result.value("presence"_L1).toArray());
			})
			.send();
	});
}

void Chats::unwatchPresence(const QStringList &users) {
	_session->transport().unsubscribePresence(users);
}

std::optional<Presence> Chats::presence(const QString &user) const {
	const auto i = _presence.constFind(user);
	return (i != _presence.cend()) ? std::make_optional(*i) : std::nullopt;
}

void Chats::setPresence(const Presence &presence) {
	if (presence.user.isEmpty()) {
		return;
	}
	auto stored = presence;
	stored.receivedAt = base::unixtime::nowMs();
	_presence.insert(presence.user, stored);
	Q_EMIT presenceChanged(presence.user);
}

} // namespace Data
