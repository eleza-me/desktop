#include "data/data_messages.h"

#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "storage/storage_database.h"

#include <algorithm>

namespace Data {
namespace {

// A story reply / reaction / share that lost its story reference in the cache.
[[nodiscard]] bool StoredWithoutStory(const Message &message) {
	return (message.seq > 0)
		&& !message.story
		&& (message.kind == MessageKind::StoryReply
			|| message.kind == MessageKind::StoryReaction
			|| message.kind == MessageKind::StoryShare);
}

// The request field that names the chat: a group has routes of its own.
[[nodiscard]] QString ChatKey(const QString &chat) {
	return IsGroupId(chat) ? u"group"_s : u"chat"_s;
}

// The seq that the key of a server message names (Message::KeyForSeq), or 0.
[[nodiscard]] qint64 SeqOfKey(const QString &key) {
	auto ok = false;
	const auto seq = key.mid(key.lastIndexOf(u':') + 1).toLongLong(&ok);
	return (ok && seq > 0 && Message::KeyForSeq(seq) == key) ? seq : 0;
}

} // namespace

qint64 History::firstSeq() const {
	for (const auto &item : items) {
		if (item.seq > 0) {
			return item.seq;
		}
	}
	return 0;
}

qint64 History::lastSeq() const {
	for (auto i = items.crbegin(); i != items.crend(); ++i) {
		if (i->seq > 0) {
			return i->seq;
		}
	}
	return 0;
}

UnreadCount UnreadAfterRead(
		const Chat &row,
		const History *history,
		qint64 readSeq) {
	const auto lastSeq = std::max(row.lastSeq, history ? history->lastSeq() : 0);
	if (readSeq >= lastSeq || (row.unread <= 0 && !row.unreadCapped)) {
		return {};
	}
	if (history && history->loaded && !history->hasMoreAfter) {
		const auto first = history->firstSeq();
		const auto covers = (first > 0)
			&& (first <= readSeq + 1 || !history->hasMoreBefore);
		if (covers) {
			auto count = 0;
			for (const auto &message : history->items) {
				if (message.seq > readSeq && !message.mine && message.visible()) {
					++count;
				}
			}
			return { row.unreadCapped ? count : std::min(count, row.unread), false };
		}
	}
	const auto bound = int(std::min<qint64>(lastSeq - readSeq, 1 << 30));
	if (row.unreadCapped && bound > row.unread) {
		return { row.unread, true };
	}
	return { std::min(row.unread, bound), false };
}

int History::indexOfKey(const QString &key) const {
	for (auto i = 0; i != items.size(); ++i) {
		if (items[i].key() == key) {
			return i;
		}
	}
	return -1;
}

int History::indexOfSeq(qint64 seq) const {
	if (seq <= 0) {
		return -1;
	}
	for (auto i = 0; i != items.size(); ++i) {
		if (items[i].seq == seq) {
			return i;
		}
	}
	return -1;
}

const Message *History::find(qint64 seq) const {
	const auto index = indexOfSeq(seq);
	return (index >= 0) ? &items[index] : nullptr;
}

Messages::Messages(Session *session)
: QObject(session)
, _session(session) {
}

History &Messages::historyRef(const QString &chat) {
	const auto [i, added] = _histories.try_emplace(chat);
	if (added) {
		i->second.chat = chat;
	}
	return i->second;
}

History *Messages::existing(const QString &chat) {
	const auto i = _histories.find(chat);
	return (i != _histories.end()) ? &i->second : nullptr;
}

const History &Messages::history(const QString &chat) {
	return historyRef(chat);
}

const Message *Messages::find(const QString &chat, qint64 seq) const {
	const auto i = _histories.find(chat);
	return (i != _histories.end()) ? i->second.find(seq) : nullptr;
}

bool Messages::held(const QString &chat) const {
	const auto i = _histories.find(chat);
	return (i != _histories.end())
		&& (i->second.loaded || !i->second.items.isEmpty());
}

bool Messages::live(const QString &chat) const {
	const auto i = _histories.find(chat);
	return (i != _histories.end())
		&& i->second.loaded
		&& !i->second.hasMoreAfter;
}

void Messages::forget(const QString &chat) {
	if (_histories.erase(chat)) {
		Q_EMIT historyChanged(chat);
	}
}

void Messages::markAllStale() {
	for (auto &[chat, history] : _histories) {
		history.loaded = false;
	}
}

void Messages::markStale(const QString &chat) {
	if (const auto db = _session->local()) {
		db->clearMessages(chat);
	}
	const auto history = existing(chat);
	if (!history) {
		return;
	}
	const auto shown = live(chat) && (_session->chats().visibleChat() == chat);
	history->stale = true;
	history->loaded = false;
	if (shown) {
		loadLatest(chat);
	}
}

void Messages::setInfo(const ChatInfo &info) {
	if (!info.valid()) {
		return;
	}
	auto &history = historyRef(info.chat);
	const auto pinsChangedNow = !history.info || history.info->pins != info.pins;
	history.info = info;
	if (pinsChangedNow) {
		history.pinsLoaded = false;
		Q_EMIT pinsChanged(info.chat);
	}
}

void Messages::persist(const QString &chat, const QVector<Message> &messages) {
	const auto db = _session->local();
	if (!db) {
		return;
	}
	auto rows = QVector<Storage::Database::MessageRow>();
	for (const auto &message : messages) {
		if (message.seq > 0) {
			rows.push_back({ chat, message.seq, message.toStored() });
		}
	}
	db->putMessages(rows);
}

void Messages::dropDetached(const QString &chat, qint64 firstSeq, bool complete) {
	if (firstSeq <= 1) {
		return;
	}
	auto &items = historyRef(chat).items;
	auto newestOlder = qint64(0);
	for (const auto &message : std::as_const(items)) {
		if (message.seq > 0 && message.seq < firstSeq) {
			newestOlder = std::max(newestOlder, message.seq);
		}
	}
	if (newestOlder > 0 && (complete || newestOlder < firstSeq - 1)) {
		items.erase(std::remove_if(items.begin(), items.end(), [&](const Message &message) {
			return message.seq > 0 && message.seq < firstSeq;
		}), items.end());
	}
	if (const auto db = _session->local()) {
		if (complete) {
			db->clearMessages(chat, firstSeq);
		} else {
			db->dropDetachedMessages(chat, firstSeq);
		}
	}
}

void Messages::merge(const QString &chat, QVector<Message> incoming, bool persist) {
	if (incoming.isEmpty()) {
		return;
	}
	auto &history = historyRef(chat);
	auto &items = history.items;
	auto bySeq = QHash<qint64, int>();
	auto localByClientId = QHash<QString, int>();
	for (auto i = 0; i != items.size(); ++i) {
		if (items[i].seq > 0) {
			bySeq.insert(items[i].seq, i);
		} else if (!items[i].clientMessageId.isEmpty()) {
			localByClientId.insert(items[i].clientMessageId, i);
		}
	}
	auto toRemove = QVector<int>();
	for (auto &message : incoming) {
		if (message.seq <= 0) {
			continue;
		}
		auto index = bySeq.value(message.seq, -1);
		if (index >= 0) {
			const auto &previous = items[index];
			if (message.mine
				&& message.status == MessageStatus::Sent
				&& previous.status == MessageStatus::Read) {
				message.status = MessageStatus::Read;
			}
			if (message.localPath.isEmpty()) {
				message.localPath = previous.localPath;
			}
			items[index] = message;
		} else {
			items.push_back(message);
			index = int(items.size()) - 1;
			bySeq.insert(message.seq, index);
		}
		// My own echo replaces the local pending copy (same client message id).
		if (!message.clientMessageId.isEmpty()) {
			const auto local = localByClientId.value(message.clientMessageId, -1);
			if (local >= 0) {
				if (items[index].localPath.isEmpty()) {
					items[index].localPath = items[local].localPath;
				}
				toRemove.push_back(local);
				localByClientId.remove(message.clientMessageId);
			}
		}
	}
	if (!toRemove.isEmpty()) {
		std::sort(toRemove.begin(), toRemove.end(), std::greater<int>());
		for (const auto index : toRemove) {
			items.removeAt(index);
		}
	}
	std::stable_sort(items.begin(), items.end(), MessageLess);
	if (persist) {
		this->persist(chat, incoming);
	}
	Q_EMIT historyChanged(chat);
}

void Messages::addLocal(const Message &message) {
	auto &history = historyRef(message.chat);
	const auto index = history.indexOfKey(message.key());
	if (index >= 0) {
		history.items[index] = message;
	} else {
		history.items.push_back(message);
	}
	std::stable_sort(history.items.begin(), history.items.end(), MessageLess);
	Q_EMIT historyChanged(message.chat);
}

void Messages::patchMessage(
		const QString &chat,
		const QString &key,
		const Fn<void(Message &)> &change) {
	const auto i = existing(chat);
	const auto index = i ? i->indexOfKey(key) : -1;
	if (index < 0) {
		// Not in memory (the chat is not open, or the message is older
		// than the window): the cached copy changes all the same, or the
		// chat would show it unchanged when it is opened.
		changeCached(chat, SeqOfKey(key), change);
		return;
	}
	auto &message = i->items[index];
	change(message);
	if (message.seq > 0) {
		storeChanged(*i, { message });
	}
	Q_EMIT historyChanged(chat);
}

void Messages::storeChanged(const History &history, const QVector<Message> &messages) {
	if (history.loaded) {
		persist(history.chat, messages);
	} else {
		updateCached(history.chat, messages);
	}
}

void Messages::changeCached(const QString &chat, qint64 seq, Fn<void(Message &)> change) {
	const auto db = _session->local();
	if (!db || seq <= 0) {
		return;
	}
	db->changeMessage(chat, seq, [change = std::move(change)](QJsonObject &data) {
		auto message = Message::FromStored(data);
		change(message);
		data = message.toStored();
		return true;
	});
}

qint64 Messages::peerReadSeq(const QString &chat) const {
	auto result = qint64(0);
	if (const auto row = _session->chats().chat(chat)) {
		result = std::max(row->peerReadSeq, row->info.peerReadSeq);
	}
	if (const auto i = _histories.find(chat); i != _histories.end() && i->second.info) {
		result = std::max(result, i->second.info->peerReadSeq);
	}
	return result;
}

Message Messages::FromCache(const QJsonObject &row, qint64 peerReadSeq) {
	auto result = Message::FromStored(row);
	if (result.mine
		&& result.seq > 0
		&& result.seq <= peerReadSeq
		&& result.status == MessageStatus::Sent) {
		result.status = MessageStatus::Read;
	}
	return result;
}

void Messages::removeMessage(const QString &chat, const QString &key) {
	// Out of the cache as well, also when it is not in the window: an old
	// message deleted while it was not shown must not come back from it.
	if (const auto seq = SeqOfKey(key)) {
		if (const auto db = _session->local()) {
			db->deleteMessage(chat, seq);
		}
	}
	const auto i = existing(chat);
	if (!i) {
		return;
	}
	const auto index = i->indexOfKey(key);
	if (index < 0) {
		return;
	}
	i->items.removeAt(index);
	Q_EMIT historyChanged(chat);
}

void Messages::updateCached(const QString &chat, const QVector<Message> &messages) {
	const auto db = _session->local();
	if (!db) {
		return;
	}
	auto rows = QVector<Storage::Database::MessageRow>();
	for (const auto &message : messages) {
		if (message.seq > 0) {
			rows.push_back({ chat, message.seq, message.toStored() });
		}
	}
	db->replaceMessages(rows);
}

void Messages::clear(const QString &chat, std::optional<qint64> beforeSeq) {
	if (const auto i = existing(chat)) {
		auto &items = i->items;
		items.erase(std::remove_if(items.begin(), items.end(), [&](const Message &message) {
			return message.seq > 0 && (!beforeSeq || message.seq < *beforeSeq);
		}), items.end());
		if (!beforeSeq) {
			i->hasMoreBefore = false;
		}
		Q_EMIT historyChanged(chat);
	}
	if (const auto db = _session->local()) {
		db->clearMessages(chat, beforeSeq);
	}
}

void Messages::markPeerRead(const QString &chat, qint64 seq) {
	const auto row = _session->chats().chat(chat);
	if (row && row->peerReadSeq < seq) {
		_session->chats().patch(chat, [&](Chat &c) {
			c.peerReadSeq = seq;
			c.info.peerReadSeq = seq;
			if (c.lastMessage && c.lastMessage->mine && c.lastMessage->seq > 0
				&& c.lastMessage->seq <= seq
				&& c.lastMessage->status == MessageStatus::Sent) {
				c.lastMessage->status = MessageStatus::Read;
			}
		});
	}
	const auto i = existing(chat);
	if (!i) {
		return;
	}
	if (i->info && i->info->peerReadSeq < seq) {
		i->info->peerReadSeq = seq;
	}
	auto changed = QVector<Message>();
	for (auto &message : i->items) {
		if (message.mine
			&& message.seq > 0
			&& message.seq <= seq
			&& message.status == MessageStatus::Sent) {
			message.status = MessageStatus::Read;
			changed.push_back(message);
		}
	}
	if (!changed.isEmpty()) {
		storeChanged(*i, changed);
		Q_EMIT historyChanged(chat);
	}
}

// ---- changes of a group (Data::Groups)

void Messages::applyChanges(const QString &chat, QVector<Message> changed, bool cache) {
	if (changed.isEmpty()) {
		return;
	}
	if (const auto history = existing(chat)) {
		// The window takes what lies inside it, what extends it at the end
		// (when it reaches the end) and my own message on its way out.
		const auto first = history->firstSeq();
		const auto last = history->lastSeq();
		const auto atEnd = history->loaded && !history->hasMoreAfter;
		auto take = QVector<Message>();
		for (const auto &message : std::as_const(changed)) {
			const auto inside = (first > 0) && (message.seq >= first) && (message.seq <= last);
			const auto extends = atEnd && (message.seq > last);
			const auto echo = !message.clientMessageId.isEmpty()
				&& history->indexOfKey(Message::KeyForLocal(message.clientMessageId)) >= 0;
			if (inside || extends || echo) {
				take.push_back(message);
			}
		}
		if (!take.isEmpty()) {
			merge(chat, std::move(take), false);
		}
	}
	if (!cache) {
		return;
	}
	if (const auto db = _session->local()) {
		// Rows that are cached are replaced, rows newer than everything
		// cached are added; a change of an old message that is not cached
		// must not land in the cache as an island.
		std::sort(changed.begin(), changed.end(), MessageLess);
		auto rows = QVector<Storage::Database::MessageRow>();
		for (const auto &message : std::as_const(changed)) {
			if (message.seq > 0) {
				rows.push_back({ chat, message.seq, message.toStored() });
			}
		}
		db->mergeMessages(rows);
	}
}

void Messages::dropMessages(const QString &chat, const QVector<qint64> &seqs, bool cache) {
	if (seqs.isEmpty()) {
		return;
	}
	if (const auto history = existing(chat)) {
		auto &items = history->items;
		const auto before = items.size();
		items.erase(std::remove_if(items.begin(), items.end(), [&](const Message &message) {
			return message.seq > 0 && seqs.contains(message.seq);
		}), items.end());
		if (items.size() != before) {
			Q_EMIT historyChanged(chat);
		}
	}
	if (!cache) {
		return;
	}
	if (const auto db = _session->local()) {
		for (const auto seq : seqs) {
			db->deleteMessage(chat, seq);
		}
	}
}

bool Messages::applyReactions(
		const QString &chat,
		qint64 seq,
		const QMap<QString, qint64> &reactions) {
	if (!find(chat, seq)) {
		return false;
	}
	patchMessage(chat, Message::KeyForSeq(seq), [reactions](Message &message) {
		message.reactions = reactions;
		for (auto i = message.reactions.begin(); i != message.reactions.end();) {
			i = (i.value() <= 0) ? message.reactions.erase(i) : std::next(i);
		}
		// The counts say nothing of whose they are: mine stays while the
		// emoji is still there.
		if (!message.myReaction.isEmpty() && !message.reactions.contains(message.myReaction)) {
			message.myReaction = QString();
		}
	});
	return true;
}

void Messages::setMyReaction(const QString &chat, qint64 seq, const QString &emoji) {
	patchMessage(chat, Message::KeyForSeq(seq), [emoji](Message &message) {
		message.myReaction = emoji;
	});
}

// ---- loading

void Messages::loadCached(const QString &chat, base::Guard guard, Fn<void(int)> done) {
	const auto finish = [=](int count) {
		if (done && guard.alive()) {
			done(count);
		}
	};
	const auto db = _session->local();
	if (!db) {
		finish(0);
		return;
	}
	db->messages(chat, kCachedPageSize, 0, this, [=, this](QVector<QJsonObject> rows) {
		auto &history = historyRef(chat);
		// The server's newest page arrived first: the cache has nothing to add.
		if (rows.isEmpty() || history.loaded || history.hasMoreAfter) {
			finish(0);
			return;
		}
		auto list = QVector<Message>();
		list.reserve(rows.size());
		auto outdated = QVector<qint64>();
		const auto read = peerReadSeq(chat);
		for (const auto &row : rows) {
			auto message = FromCache(row, read);
			if (StoredWithoutStory(message)) {
				// Cached by a build that did not keep the story reference.
				outdated.push_back(message.seq);
			}
			if (message.visible()) {
				list.push_back(std::move(message));
			}
		}
		const auto count = int(list.size());
		merge(chat, std::move(list), false);
		for (const auto seq : outdated) {
			refresh(chat, seq);
		}
		finish(count);
	});
}

void Messages::loadPage(
		const QString &chat,
		PageOptions options,
		Fn<void(const Page &)> done,
		Fn<void(const Api::Error &)> fail) {
	const auto group = IsGroupId(chat);
	const auto newest = !options.before && !options.after && !options.around;
	auto body = QJsonObject{
		{ ChatKey(chat), chat },
		{ u"limit"_s, QString::number(options.limit) },
	};
	if (options.before > 0) {
		body.insert(u"before"_s, QString::number(options.before));
	}
	if (options.after > 0) {
		body.insert(u"after"_s, QString::number(options.after));
	}
	if (options.around > 0) {
		body.insert(u"around"_s, QString::number(options.around));
	}
	_session->transport().rpc(group ? u"groups.history"_s : u"chat.history"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto page = Page();
			if (group) {
				const auto header = Group::FromJson(result.value("group"_L1).toObject());
				if (header.valid()) {
					page.group = header;
					page.info = Groups::InfoFor(header);
					// Read receipts never go back (frames may have crossed the page).
					if (const auto row = _session->chats().chat(chat)) {
						page.info.peerReadSeq = std::max(page.info.peerReadSeq, row->peerReadSeq);
					}
				}
			} else {
				page.info = ChatInfo::FromJson(result.value("chat"_L1).toObject());
			}
			if (!page.info.valid()) {
				// Tolerate a server that leaves the header out of a page.
				if (const auto &known = historyRef(chat).info) {
					page.info = *known;
				} else {
					page.info.chat = chat;
				}
			}
			// Absent means unknown: assume there is more.
			page.hasMoreBefore = Json::Bool(result, "has_more_before"_L1, true);
			page.hasMoreAfter = Json::Bool(result, "has_more_after"_L1, false);
			const auto me = _session->userId();
			for (const auto &item : result.value("messages"_L1).toArray()) {
				auto message = Message::FromJson(item.toObject());
				if (message.seq > 0 && (!page.firstSeq || message.seq < page.firstSeq)) {
					page.firstSeq = message.seq;
				}
				if (message.chat.isEmpty()) {
					message.chat = chat;
				}
				if (!message.visible()) {
					continue;
				}
				message.resolveLocal(&page.info, me);
				page.messages.push_back(std::move(message));
			}
			std::sort(page.messages.begin(), page.messages.end(), MessageLess);
			if (page.group) {
				// The header moves the row; the newest page is what the
				// cached messages of the group are current against.
				_session->groups().historyPage(*page.group, newest);
				auto people = QStringList();
				for (const auto &message : std::as_const(page.messages)) {
					if (!people.contains(message.sender)) {
						people.push_back(message.sender);
					}
					const auto named = GroupRules::NoteUser(message.system);
					if (!named.isEmpty() && !people.contains(named)) {
						people.push_back(named);
					}
				}
				_session->users().request(people);
			}
			done(page);
		})
		.fail([=, this](const Api::Error &error) {
			if (group) {
				_session->groups().handleError(chat, error);
			}
			if (fail) {
				fail(error);
			}
		})
		.send();
}

void Messages::loadLatest(
		const QString &chat,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	loadPage(chat, PageOptions(), [=, this](const Page &page) {
		auto &history = historyRef(chat);
		if (history.hasMoreAfter || history.stale) {
			// Coming back from a detached window, or what is held may be
			// gone: start over at the end.
			auto &items = history.items;
			items.erase(std::remove_if(items.begin(), items.end(), [](const Message &message) {
				return message.seq > 0;
			}), items.end());
			history.hasMoreAfter = false;
			history.stale = false;
		}
		// What is held older than the newest page must be one run with it
		// (an empty page without more before it: the chat has nothing up to
		// the header's last seq).
		const auto from = page.firstSeq
			? page.firstSeq
			: page.hasMoreBefore
			? qint64(0)
			: (page.info.lastSeq + 1);
		dropDetached(chat, from, !page.hasMoreBefore);
		setInfo(page.info);
		history.loaded = true;
		history.hasMoreBefore = page.hasMoreBefore;
		if (page.messages.isEmpty()) {
			Q_EMIT historyChanged(chat);
		} else {
			merge(chat, page.messages, true);
		}
		if (page.group) {
			// The page is the group as it is now: its newest message is the
			// row's preview (a group's header carries none).
			if (!page.messages.isEmpty()) {
				const auto &last = page.messages.back();
				_session->chats().patch(chat, [&](Chat &c) {
					const auto pending = c.lastMessage && c.lastMessage->local();
					if (!pending && (!c.lastMessage || last.seq >= c.lastMessage->seq)) {
						c.lastMessage = last;
						c.lastMine = last.mine;
						c.lastActivityAt = std::max(c.lastActivityAt, last.createdAt);
					}
				});
			}
		} else {
			// The header refreshes the row (read marks, pins, theme).
			_session->users().ensure(page.info.peopleIds(), this, [=, this] {
				_session->chats().applyInfo(page.info);
			});
		}
		if (done && guard.alive()) {
			done();
		}
	}, [=](const Api::Error &error) {
		if (fail && guard.alive()) {
			fail(error);
		}
	});
}

void Messages::loadOlder(
		const QString &chat,
		base::Guard guard,
		Fn<void(int)> done,
		Fn<void(const Api::Error &)> fail) {
	const auto before = historyRef(chat).firstSeq();
	if (before <= 1) {
		if (before == 1) {
			historyRef(chat).hasMoreBefore = false;
		}
		if (before == 0) {
			// Nothing to anchor on: this is a first load.
			loadLatest(chat, guard, [=, this] {
				if (done) {
					done(int(historyRef(chat).items.size()));
				}
			}, fail);
			return;
		}
		if (done && guard.alive()) {
			done(0);
		}
		return;
	}
	const auto fromServer = [=, this] {
		auto options = PageOptions();
		options.before = before;
		loadPage(chat, options, [=, this](const Page &page) {
			auto &history = historyRef(chat);
			setInfo(page.info);
			history.hasMoreBefore = page.hasMoreBefore && !page.messages.isEmpty();
			const auto count = int(page.messages.size());
			if (count) {
				// Only a window that reaches the newest message is one
				// unbroken run worth caching.
				merge(chat, page.messages, !history.hasMoreAfter);
			} else {
				Q_EMIT historyChanged(chat);
			}
			if (done && guard.alive()) {
				done(count);
			}
		}, [=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		});
	};
	const auto db = _session->local();
	if (!db || historyRef(chat).hasMoreAfter) {
		fromServer();
		return;
	}
	db->messages(chat, kCachedPageSize, before, this, [=, this](QVector<QJsonObject> rows) {
		auto list = QVector<Message>();
		const auto read = peerReadSeq(chat);
		for (const auto &row : rows) {
			auto message = FromCache(row, read);
			if (message.visible()) {
				list.push_back(std::move(message));
			}
		}
		if (list.isEmpty()) {
			fromServer();
			return;
		}
		const auto count = int(list.size());
		merge(chat, std::move(list), false);
		if (done && guard.alive()) {
			done(count);
		}
	});
}

void Messages::loadNewer(
		const QString &chat,
		base::Guard guard,
		Fn<void(int)> done,
		Fn<void(const Api::Error &)> fail) {
	const auto &history = historyRef(chat);
	if (!history.hasMoreAfter) {
		if (done && guard.alive()) {
			done(0);
		}
		return;
	}
	auto options = PageOptions();
	options.after = history.lastSeq();
	loadPage(chat, options, [=, this](const Page &page) {
		auto &history = historyRef(chat);
		setInfo(page.info);
		history.hasMoreAfter = page.hasMoreAfter && !page.messages.isEmpty();
		const auto count = int(page.messages.size());
		if (count) {
			merge(chat, page.messages, false);
		} else {
			Q_EMIT historyChanged(chat);
		}
		if (!history.hasMoreAfter) {
			// The window reached the end again: live updates extend it, and
			// what they cache must continue the cached run, so the window
			// becomes that run.
			history.loaded = true;
			dropDetached(chat, history.firstSeq(), !history.hasMoreBefore);
			persist(chat, history.items);
		}
		if (done && guard.alive()) {
			done(count);
		}
	}, [=](const Api::Error &error) {
		if (fail && guard.alive()) {
			fail(error);
		}
	});
}

void Messages::loadAround(
		const QString &chat,
		qint64 seq,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	if (const auto &history = historyRef(chat); !history.stale && history.find(seq)) {
		if (done && guard.alive()) {
			done();
		}
		return;
	}
	auto options = PageOptions();
	options.around = seq;
	loadPage(chat, options, [=, this](const Page &page) {
		auto &history = historyRef(chat);
		setInfo(page.info);
		// A new window: the server messages held so far are not adjacent to it.
		auto &items = history.items;
		items.erase(std::remove_if(items.begin(), items.end(), [](const Message &message) {
			return message.seq > 0;
		}), items.end());
		history.stale = false;
		history.hasMoreBefore = page.hasMoreBefore;
		history.hasMoreAfter = page.hasMoreAfter;
		history.loaded = !page.hasMoreAfter;
		if (history.loaded) {
			// The window reaches the newest message: as the newest page, it
			// is what live updates continue in the cache.
			dropDetached(chat, page.firstSeq, !page.hasMoreBefore);
		}
		if (page.messages.isEmpty()) {
			Q_EMIT historyChanged(chat);
		} else {
			merge(chat, page.messages, history.loaded);
		}
		if (done && guard.alive()) {
			done();
		}
	}, [=](const Api::Error &error) {
		if (fail && guard.alive()) {
			fail(error);
		}
	});
}

void Messages::fetchOne(
		const QString &chat,
		qint64 seq,
		base::Guard guard,
		Fn<void(const std::optional<Message> &)> done) {
	if (const auto found = find(chat, seq)) {
		if (done && guard.alive()) {
			done(*found);
		}
		return;
	}
	auto options = PageOptions();
	options.before = seq + 1;
	options.limit = 1;
	loadPage(chat, options, [=, this](const Page &page) {
		auto result = std::optional<Message>();
		for (const auto &message : page.messages) {
			if (message.seq == seq) {
				result = message;
			}
		}
		const auto &history = historyRef(chat);
		if (result
			&& history.firstSeq() > 0
			&& seq >= history.firstSeq()
			&& seq <= history.lastSeq()) {
			merge(chat, { *result }, history.loaded);
		}
		if (done && guard.alive()) {
			done(result);
		}
	}, [=](const Api::Error &) {
		if (done && guard.alive()) {
			done(std::nullopt);
		}
	});
}

void Messages::refresh(const QString &chat, qint64 seq) {
	if (chat.isEmpty() || seq <= 0 || !find(chat, seq)) {
		return;
	}
	auto options = PageOptions();
	options.before = seq + 1;
	options.limit = 1;
	loadPage(chat, options, [=, this](const Page &page) {
		for (const auto &message : page.messages) {
			if (message.seq == seq && find(chat, seq)) {
				const auto &history = historyRef(chat);
				merge(chat, { message }, history.loaded);
				if (!history.loaded) {
					updateCached(chat, { message });
				}
			}
		}
	}, [](const Api::Error &) {
	});
}

// ---- actions

void Messages::simple(
		const QString &method,
		const QJsonObject &body,
		const QString &action,
		const QString &chat,
		Fn<void()> onFail) {
	_session->transport().rpc(method)
		.body(body)
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			if (IsGroupId(chat) && _session->groups().handleError(chat, error)) {
				// The group is gone, and its timeline with it.
				_session->reportFailure(action, chat, error);
				return;
			}
			if (onFail) {
				onFail();
			}
			_session->reportFailure(action, chat, error);
		})
		.send();
}

void Messages::markRead(const QString &chat, qint64 seq) {
	const auto row = _session->chats().chat(chat);
	if (seq <= 0 || (row && row->lastReadSeq >= seq)) {
		return;
	}
	const auto group = IsGroupId(chat);
	const auto mentionsLeft = group && row && row->unreadMentions > 0 && seq < row->lastSeq;
	// Read to the middle (scrolled up): the messages below stay unread.
	const auto left = row ? UnreadAfterRead(*row, existing(chat), seq) : UnreadCount();
	_session->chats().patch(chat, [&](Chat &c) {
		c.unread = left.count;
		c.unreadCapped = left.capped;
		c.lastReadSeq = seq;
		c.info.lastReadSeq = seq;
		if (!mentionsLeft) {
			c.unreadMentions = 0;
		}
		if (c.group) {
			c.group->lastReadSeq = seq;
			c.group->unread = left.count;
			c.group->unreadCapped = left.capped;
			c.group->unreadMentions = c.unreadMentions;
		}
	});
	// Best effort: the next read mark covers a lost one.
	_session->transport().rpc(group ? u"groups.read"_s : u"chat.read"_s)
		.body(QJsonObject{ { ChatKey(chat), chat }, { u"seq"_s, double(seq) } })
		.guard(this)
		.done([=, this] {
			if (mentionsLeft) {
				// Read, but not to the end: how many mentions are left is
				// the header's to say.
				_session->groups().refreshHeader(chat);
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (group) {
				_session->groups().handleError(chat, error);
			}
		})
		.send();
}

void Messages::react(const QString &chat, qint64 seq, const QString &emoji) {
	const auto key = Message::KeyForSeq(seq);
	patchMessage(chat, key, [emoji](Message &message) {
		if (!message.myReaction.isEmpty()) {
			const auto left = message.reactions.value(message.myReaction) - 1;
			if (left > 0) {
				message.reactions.insert(message.myReaction, left);
			} else {
				message.reactions.remove(message.myReaction);
			}
		}
		message.myReaction = emoji;
		if (!emoji.isEmpty()) {
			message.reactions.insert(emoji, message.reactions.value(emoji) + 1);
		}
	});
	const auto group = IsGroupId(chat);
	auto body = QJsonObject{ { ChatKey(chat), chat }, { u"seq"_s, double(seq) } };
	if (!emoji.isEmpty()) {
		body.insert(u"emoji"_s, emoji);
	}
	simple(
		emoji.isEmpty()
			? (group ? u"groups.unreact"_s : u"chat.unreact"_s)
			: (group ? u"groups.react"_s : u"chat.react"_s),
		body,
		u"message.react"_s,
		chat,
		[=, this] { refresh(chat, seq); });
}

void Messages::edit(
		const QString &chat,
		qint64 seq,
		const QString &text,
		const QVector<Entity> &entities,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	auto body = QJsonObject{
		{ ChatKey(chat), chat },
		{ u"seq"_s, double(seq) },
		{ u"text"_s, text },
	};
	Json::Put(body, "entities"_L1, Json::Array(entities));
	_session->transport().rpc(IsGroupId(chat) ? u"groups.edit"_s : u"chat.edit"_s)
		.body(body)
		.guard(this)
		.done([=, this] {
			// The `edited` update brings the server's copy; show ours meanwhile.
			patchMessage(chat, Message::KeyForSeq(seq), [=](Message &message) {
				message.text = text;
				message.entities = entities;
				message.edited = true;
			});
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (IsGroupId(chat)) {
				_session->groups().handleError(chat, error);
			}
			if (fail && guard.alive()) {
				fail(error);
			} else {
				_session->reportFailure(u"message.edit"_s, chat, error);
			}
		})
		.send();
}

void Messages::editWithPreview(
		const QString &chat,
		qint64 seq,
		const QString &text,
		const QVector<Entity> &entities,
		const QString &linkPreview,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	auto body = QJsonObject{
		{ ChatKey(chat), chat },
		{ u"seq"_s, double(seq) },
		{ u"text"_s, text },
	};
	Json::Put(body, "entities"_L1, Json::Array(entities));
	Json::Put(body, "link_preview"_L1, linkPreview);
	_session->transport().rpc(IsGroupId(chat) ? u"groups.edit"_s : u"chat.edit"_s)
		.body(body)
		.guard(this)
		.done([=, this] {
			// The `edited` update brings the server's copy; show ours meanwhile.
			patchMessage(chat, Message::KeyForSeq(seq), [=](Message &message) {
				message.text = text;
				message.entities = entities;
				message.edited = true;
				if (linkPreview.isEmpty()
					|| (message.linkPreview && message.linkPreview->url != linkPreview)) {
					message.linkPreview = std::nullopt;
				}
			});
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (IsGroupId(chat)) {
				_session->groups().handleError(chat, error);
			}
			if (fail && guard.alive()) {
				fail(error);
			} else {
				_session->reportFailure(u"message.edit"_s, chat, error);
			}
		})
		.send();
}

void Messages::deleteMessage(const QString &chat, qint64 seq, bool forEveryone) {
	removeMessage(chat, Message::KeyForSeq(seq));
	// The row's preview follows when the newest message went away.
	if (const auto row = _session->chats().chat(chat); row && row->lastMessage && row->lastMessage->seq == seq) {
		const auto &items = historyRef(chat).items;
		auto last = std::optional<Message>();
		for (auto i = items.crbegin(); i != items.crend(); ++i) {
			if (i->seq > 0) {
				last = *i;
				break;
			}
		}
		_session->chats().patch(chat, [&](Chat &c) {
			c.lastMessage = last;
			c.lastMine = last && last->mine;
		});
	}
	simple(IsGroupId(chat) ? u"groups.delete"_s : u"chat.delete"_s, QJsonObject{
		{ ChatKey(chat), chat },
		{ u"seq"_s, double(seq) },
		{ u"scope"_s, forEveryone ? u"everyone"_s : u"me"_s },
	}, u"message.delete"_s, chat, [=, this] {
		// Bring it back.
		if (historyRef(chat).loaded) {
			loadLatest(chat);
		}
	});
}

void Messages::applyPin(const QString &chat, qint64 seq, bool pinned) {
	auto &history = historyRef(chat);
	const auto apply = [&](QVector<qint64> &pins) {
		const auto has = pins.contains(seq);
		if (pinned && !has) {
			pins.push_back(seq);
			std::sort(pins.begin(), pins.end());
			return true;
		} else if (!pinned && has) {
			pins.removeAll(seq);
			return true;
		}
		return false;
	};
	auto changed = false;
	if (history.info) {
		changed = apply(history.info->pins);
	}
	if (_session->chats().chat(chat)) {
		_session->chats().patch(chat, [&](Chat &c) {
			changed = apply(c.info.pins) || changed;
			if (c.group) {
				// The header lists the newest pin first.
				c.group->pins.removeAll(seq);
				if (pinned) {
					c.group->pins.push_front(seq);
				}
			}
		});
	}
	if (!pinned) {
		const auto before = history.pins.size();
		history.pins.erase(std::remove_if(history.pins.begin(), history.pins.end(), [&](const PinnedMessage &pin) {
			return pin.message.seq == seq;
		}), history.pins.end());
		changed = changed || (history.pins.size() != before);
	} else if (changed) {
		history.pinsLoaded = false;
	}
	if (changed) {
		Q_EMIT pinsChanged(chat);
	}
}

void Messages::pin(const QString &chat, qint64 seq, const QString &scope) {
	applyPin(chat, seq, true);
	if (IsGroupId(chat)) {
		// A group's pin is for everyone: there is no scope.
		simple(u"groups.message.pin"_s, QJsonObject{
			{ u"group"_s, chat },
			{ u"seq"_s, double(seq) },
		}, u"message.pin"_s, chat, [=, this] { applyPin(chat, seq, false); });
		return;
	}
	simple(u"chat.message.pin"_s, QJsonObject{
		{ u"chat"_s, chat },
		{ u"seq"_s, double(seq) },
		{ u"scope"_s, scope },
	}, u"message.pin"_s, chat, [=, this] { applyPin(chat, seq, false); });
}

void Messages::unpin(const QString &chat, qint64 seq, const QString &scope) {
	applyPin(chat, seq, false);
	if (IsGroupId(chat)) {
		simple(u"groups.message.unpin"_s, QJsonObject{
			{ u"group"_s, chat },
			{ u"seq"_s, double(seq) },
		}, u"message.unpin"_s, chat, [=, this] { applyPin(chat, seq, true); });
		return;
	}
	simple(u"chat.message.unpin"_s, QJsonObject{
		{ u"chat"_s, chat },
		{ u"seq"_s, double(seq) },
		{ u"scope"_s, scope },
	}, u"message.unpin"_s, chat, [=, this] { applyPin(chat, seq, true); });
}

void Messages::loadPins(
		const QString &chat,
		base::Guard guard,
		Fn<void(const QVector<PinnedMessage> &)> done) {
	_session->transport().rpc(IsGroupId(chat) ? u"groups.pins"_s : u"chat.pins"_s)
		.body(QJsonObject{ { ChatKey(chat), chat } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto &history = historyRef(chat);
			auto pins = QVector<PinnedMessage>();
			const auto me = _session->userId();
			for (const auto &item : result.value("pins"_L1).toArray()) {
				auto pin = PinnedMessage::FromJson(item.toObject());
				if (pin.message.chat.isEmpty()) {
					pin.message.chat = chat;
				}
				pin.message.resolveLocal(history.info ? &*history.info : nullptr, me);
				pins.push_back(std::move(pin));
			}
			history.pins = pins;
			history.pinsLoaded = true;
			Q_EMIT pinsChanged(chat);
			if (done && guard.alive()) {
				done(pins);
			}
		})
		.fail([=, this](const Api::Error &) {
			if (done && guard.alive()) {
				done(historyRef(chat).pins);
			}
		})
		.send();
}

void Messages::markPlayed(const QString &chat, qint64 seq) {
	const auto found = find(chat, seq);
	if (!found || found->played || IsGroupId(chat)) {
		// Nobody learns what a member of a group has played.
		return;
	}
	patchMessage(chat, Message::KeyForSeq(seq), [](Message &message) {
		message.played = true;
	});
	_session->transport().rpc(u"chat.played"_s)
		.body(QJsonObject{ { u"chat"_s, chat }, { u"seq"_s, double(seq) } })
		.guard(this)
		.send();
}

void Messages::pressButton(
		const QString &chat,
		qint64 seq,
		int row,
		int column,
		base::Guard guard,
		Fn<void(const CallbackAnswer &)> done,
		Fn<void(const Api::Error &)> fail) {
	_session->transport().rpc(u"chat.callback"_s)
		.body(QJsonObject{
			{ u"chat"_s, chat },
			{ u"seq"_s, double(seq) },
			{ u"row"_s, row },
			{ u"col"_s, column },
		})
		.guard(guard)
		.done([=](const QJsonObject &result) {
			auto answer = CallbackAnswer();
			answer.answered = Json::Bool(result, "answered"_L1);
			answer.text = Json::String(result, "text"_L1);
			answer.alert = Json::Bool(result, "alert"_L1);
			answer.url = Json::String(result, "url"_L1);
			if (done) {
				done(answer);
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail) {
				fail(error);
			}
		})
		.send();
}

void Messages::updateLiveLocation(const QString &chat, qint64 seq, const Location &location) {
	_session->transport().rpc(u"chat.live_location"_s)
		.body(QJsonObject{
			{ u"chat"_s, chat },
			{ u"seq"_s, double(seq) },
			{ u"location"_s, location.toJson() },
		})
		.guard(this)
		.send();
}

void Messages::stopLiveLocation(const QString &chat, qint64 seq) {
	simple(u"chat.live_location.stop"_s, QJsonObject{
		{ u"chat"_s, chat },
		{ u"seq"_s, double(seq) },
	}, u"message.live_stop"_s, chat);
}

void Messages::loadLiveLocations(
		const QString &chat,
		base::Guard guard,
		Fn<void(const QVector<LiveShare> &)> done) {
	_session->transport().rpc(u"chat.live_locations"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
		.guard(guard)
		.done([=](const QJsonObject &result) {
			auto list = QVector<LiveShare>();
			for (const auto &item : result.value("items"_L1).toArray()) {
				const auto object = item.toObject();
				auto share = LiveShare();
				share.seq = Json::Int64(object, "seq"_L1);
				share.sender = Json::String(object, "sender"_L1);
				share.until = Json::Int64(object, "until"_L1);
				share.location = Json::Optional<Location>(object, "location"_L1);
				share.at = Json::Int64(object, "at"_L1);
				list.push_back(share);
			}
			if (done) {
				done(list);
			}
		})
		.fail([=](const Api::Error &) {
			if (done) {
				done({});
			}
		})
		.send();
}

} // namespace Data
