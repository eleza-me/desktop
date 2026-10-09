#include "data/data_messages.h"

#include "data/data_chats.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "storage/storage_database.h"

#include <algorithm>

namespace Data {

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
	if (!i) {
		return;
	}
	const auto index = i->indexOfKey(key);
	if (index < 0) {
		return;
	}
	auto &message = i->items[index];
	change(message);
	if (message.seq > 0 && i->loaded) {
		persist(chat, { message });
	}
	Q_EMIT historyChanged(chat);
}

void Messages::removeMessage(const QString &chat, const QString &key) {
	const auto i = existing(chat);
	if (!i) {
		return;
	}
	const auto index = i->indexOfKey(key);
	if (index < 0) {
		return;
	}
	const auto seq = i->items[index].seq;
	i->items.removeAt(index);
	if (seq > 0) {
		if (const auto db = _session->local()) {
			db->deleteMessage(chat, seq);
		}
	}
	Q_EMIT historyChanged(chat);
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
		if (i->loaded) {
			persist(chat, changed);
		}
		Q_EMIT historyChanged(chat);
	}
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
		for (const auto &row : rows) {
			auto message = Message::FromStored(row);
			if (message.visible()) {
				list.push_back(std::move(message));
			}
		}
		const auto count = int(list.size());
		merge(chat, std::move(list), false);
		finish(count);
	});
}

void Messages::loadPage(
		const QString &chat,
		PageOptions options,
		Fn<void(const Page &)> done,
		Fn<void(const Api::Error &)> fail) {
	auto body = QJsonObject{
		{ u"chat"_s, chat },
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
	_session->transport().rpc(u"chat.history"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto page = Page();
			page.info = ChatInfo::FromJson(result.value("chat"_L1).toObject());
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
			done(page);
		})
		.fail(std::move(fail))
		.send();
}

void Messages::loadLatest(
		const QString &chat,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	loadPage(chat, PageOptions(), [=, this](const Page &page) {
		auto &history = historyRef(chat);
		if (history.hasMoreAfter) {
			// Coming back from a detached window: start over at the end.
			auto &items = history.items;
			items.erase(std::remove_if(items.begin(), items.end(), [](const Message &message) {
				return message.seq > 0;
			}), items.end());
			history.hasMoreAfter = false;
		}
		setInfo(page.info);
		history.loaded = true;
		history.hasMoreBefore = page.hasMoreBefore;
		if (page.messages.isEmpty()) {
			Q_EMIT historyChanged(chat);
		} else {
			merge(chat, page.messages, true);
		}
		// The header refreshes the row (read marks, pins, theme).
		_session->users().ensure(page.info.peopleIds(), this, [=, this] {
			_session->chats().applyInfo(page.info);
		});
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
		for (const auto &row : rows) {
			auto message = Message::FromStored(row);
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
		if (!history.hasMoreAfter) {
			// The window reached the end again: live updates extend it.
			history.loaded = true;
		}
		const auto count = int(page.messages.size());
		if (count) {
			merge(chat, page.messages, false);
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
}

void Messages::loadAround(
		const QString &chat,
		qint64 seq,
		base::Guard guard,
		Fn<void()> done,
		Fn<void(const Api::Error &)> fail) {
	if (historyRef(chat).find(seq)) {
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
		history.hasMoreBefore = page.hasMoreBefore;
		history.hasMoreAfter = page.hasMoreAfter;
		history.loaded = !page.hasMoreAfter;
		if (page.messages.isEmpty()) {
			Q_EMIT historyChanged(chat);
		} else {
			merge(chat, page.messages, false);
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
				merge(chat, { message }, historyRef(chat).loaded);
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
	_session->chats().patch(chat, [&](Chat &c) {
		c.unread = 0;
		c.unreadCapped = false;
		c.lastReadSeq = seq;
		c.info.lastReadSeq = seq;
	});
	// Best effort: the next read mark covers a lost one.
	_session->transport().rpc(u"chat.read"_s)
		.body(QJsonObject{ { u"chat"_s, chat }, { u"seq"_s, double(seq) } })
		.guard(this)
		.send();
}

void Messages::react(const QString &chat, qint64 seq, const QString &emoji) {
	const auto key = Message::KeyForSeq(seq);
	patchMessage(chat, key, [&](Message &message) {
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
	auto body = QJsonObject{ { u"chat"_s, chat }, { u"seq"_s, double(seq) } };
	if (!emoji.isEmpty()) {
		body.insert(u"emoji"_s, emoji);
	}
	simple(
		emoji.isEmpty() ? u"chat.unreact"_s : u"chat.react"_s,
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
		{ u"chat"_s, chat },
		{ u"seq"_s, double(seq) },
		{ u"text"_s, text },
	};
	Json::Put(body, "entities"_L1, Json::Array(entities));
	_session->transport().rpc(u"chat.edit"_s)
		.body(body)
		.guard(this)
		.done([=, this] {
			// The `edited` update brings the server's copy; show ours meanwhile.
			patchMessage(chat, Message::KeyForSeq(seq), [&](Message &message) {
				message.text = text;
				message.entities = entities;
				message.edited = true;
			});
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=, this](const Api::Error &error) {
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
	simple(u"chat.delete"_s, QJsonObject{
		{ u"chat"_s, chat },
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
	simple(u"chat.message.pin"_s, QJsonObject{
		{ u"chat"_s, chat },
		{ u"seq"_s, double(seq) },
		{ u"scope"_s, scope },
	}, u"message.pin"_s, chat, [=, this] { applyPin(chat, seq, false); });
}

void Messages::unpin(const QString &chat, qint64 seq, const QString &scope) {
	applyPin(chat, seq, false);
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
	_session->transport().rpc(u"chat.pins"_s)
		.body(QJsonObject{ { u"chat"_s, chat } })
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
	if (!found || found->played) {
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
