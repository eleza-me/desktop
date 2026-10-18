#include "data/data_outbox.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "storage/storage_database.h"

#include <algorithm>
#include <cmath>

namespace Data {

Outbox::Outbox(Session *session)
: QObject(session)
, _session(session) {
	qRegisterMetaType<Api::Error>();
}

Outbox::~Outbox() {
	qDeleteAll(_retryTimers);
}

void Outbox::start(base::Guard guard, Fn<void()> ready) {
	const auto begin = [=, this] {
		_started = true;
		auto chats = QStringList();
		for (const auto &id : _order) {
			const auto &item = _items[id];
			show(item);
			if (!chats.contains(item.chat)) {
				chats.push_back(item.chat);
			}
		}
		for (const auto &chat : chats) {
			pumpChat(chat);
		}
		if (ready && guard.alive()) {
			ready();
		}
	};
	const auto db = _session->local();
	if (!db || _started) {
		begin();
		return;
	}
	db->outbox(this, [=, this](QVector<QJsonObject> rows) {
		for (const auto &row : rows) {
			auto item = OutboxItem::FromJson(row);
			if (item.clientMessageId.isEmpty() || _items.contains(item.clientMessageId)) {
				continue;
			}
			_order.push_back(item.clientMessageId);
			_items.insert(item.clientMessageId, std::move(item));
		}
		// Stored messages go before anything queued while the table was read.
		std::stable_sort(_order.begin(), _order.end(), [&](const QString &a, const QString &b) {
			return _items.value(a).createdAt < _items.value(b).createdAt;
		});
		begin();
	});
}

void Outbox::stop() {
	_started = false;
	for (const auto id : std::as_const(_uploads)) {
		_session->upload().cancel(id);
	}
	_uploads.clear();
	_running.clear();
	qDeleteAll(_retryTimers);
	_retryTimers.clear();
}

int Outbox::pendingCount(const QString &chat) const {
	auto result = 0;
	for (const auto &item : _items) {
		if (!item.failed && (chat.isEmpty() || item.chat == chat)) {
			++result;
		}
	}
	return result;
}

QVector<OutboxItem> Outbox::items() const {
	auto result = QVector<OutboxItem>();
	for (const auto &id : _order) {
		result.push_back(_items.value(id));
	}
	return result;
}

QVector<QString> Outbox::chatQueue(const QString &chat) const {
	auto result = QVector<QString>();
	for (const auto &id : _order) {
		const auto i = _items.constFind(id);
		if (i != _items.cend() && i->chat == chat) {
			result.push_back(id);
		}
	}
	return result;
}

// ---- queueing

QString Outbox::sendText(const QString &chat, const QString &text, const TextOptions &options) {
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = u"text"_s;
	item.body = QJsonObject{ { u"kind"_s, u"text"_s }, { u"text"_s, text } };
	Json::Put(item.body, "reply_to_seq"_L1, options.replyToSeq);
	Json::Put(item.body, "entities"_L1, Json::Array(options.entities));
	if (options.quote && options.replyToSeq) {
		item.body.insert("quote"_L1, options.quote->toJson());
	}
	Json::Put(item.body, "link_preview"_L1, options.linkPreview);
	Json::Put(item.body, "silent"_L1, options.silent);
	return enqueue(std::move(item));
}

QString Outbox::sendSticker(const QString &chat, const QString &stickerId, qint64 replyToSeq) {
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = u"sticker"_s;
	item.body = QJsonObject{ { u"kind"_s, u"sticker"_s }, { u"sticker"_s, stickerId } };
	Json::Put(item.body, "reply_to_seq"_L1, replyToSeq);
	return enqueue(std::move(item));
}

QString Outbox::sendFile(const QString &chat, const QString &filePath, const FileOptions &options) {
	const auto probe = Upload::Probe(filePath, options.asVoice);
	if (!probe.exists) {
		return QString();
	}
	const auto kind = options.kind.value_or(probe.kind);
	const auto messageKind = Upload::MessageKindFor(kind);
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = messageKind;
	item.filePath = filePath;
	item.fileName = options.fileName.isEmpty() ? probe.fileName : options.fileName;
	item.mime = probe.mime;
	item.fileSize = probe.size;
	item.durationMs = options.durationMs;
	item.width = probe.width;
	item.height = probe.height;
	item.body = QJsonObject{ { u"kind"_s, messageKind } };
	Json::Put(item.body, "text"_L1, options.caption);
	Json::Put(item.body, "entities"_L1, Json::Array(options.entities));
	Json::Put(item.body, "reply_to_seq"_L1, options.replyToSeq);
	Json::Put(item.body, "media_spoiler"_L1, options.spoiler);
	if (messageKind == u"file" && options.caption.isEmpty()) {
		// A file shows its name.
		item.body.insert("text"_L1, item.fileName);
	}
	if (messageKind == u"audio" && options.audio) {
		Json::Put(item.body, "audio"_L1, options.audio->toJson());
	}
	Json::Put(item.body, "silent"_L1, options.silent);
	return enqueue(std::move(item));
}

QString Outbox::sendLocation(const QString &chat, const Location &location, qint64 replyToSeq) {
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = u"location"_s;
	item.body = QJsonObject{
		{ u"kind"_s, u"location"_s },
		{ u"location"_s, location.toJson() },
	};
	Json::Put(item.body, "reply_to_seq"_L1, replyToSeq);
	return enqueue(std::move(item));
}

QString Outbox::sendLiveLocation(const QString &chat, const Location &location, qint64 periodSeconds) {
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = u"live_location"_s;
	item.body = QJsonObject{
		{ u"kind"_s, u"live_location"_s },
		{ u"location"_s, location.toJson() },
		{ u"live_period"_s, double(periodSeconds) },
	};
	return enqueue(std::move(item));
}

QString Outbox::sendPost(const QString &chat, const QString &postId) {
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = u"forwarded_post"_s;
	item.body = QJsonObject{
		{ u"kind"_s, u"forwarded_post"_s },
		{ u"forwarded_post"_s, postId },
	};
	return enqueue(std::move(item));
}

QString Outbox::forward(const QString &toChat, const Message &source) {
	switch (source.kind) {
	case MessageKind::Sticker:
		return source.sticker ? sendSticker(toChat, source.sticker->id) : QString();
	case MessageKind::Location:
		return source.location ? sendLocation(toChat, *source.location) : QString();
	case MessageKind::ForwardedPost:
		return source.forwardedPost.isEmpty() ? QString() : sendPost(toChat, source.forwardedPost);
	case MessageKind::Text:
	case MessageKind::Photo:
	case MessageKind::Video:
	case MessageKind::Gif:
	case MessageKind::Voice:
	case MessageKind::VideoNote:
	case MessageKind::Audio:
	case MessageKind::File:
		break;
	default:
		return QString(); // system messages, calls, live locations
	}
	if (source.seq <= 0) {
		return QString();
	}
	auto item = OutboxItem();
	item.chat = toChat;
	item.kind = source.kindName.isEmpty() ? MessageKindToString(source.kind) : source.kindName;
	item.body = QJsonObject{
		{ u"kind"_s, item.kind },
		{ u"forward_from"_s, QJsonObject{
			{ u"chat"_s, source.chat },
			{ u"seq"_s, double(source.seq) },
		} },
	};
	// A text forward carries the text itself; the source only proves I could see it.
	Json::Put(item.body, "text"_L1, source.text);
	Json::Put(item.body, "entities"_L1, Json::Array(source.entities));
	Json::Put(item.body, "media_spoiler"_L1, source.mediaSpoiler);
	if (source.audio) {
		Json::Put(item.body, "audio"_L1, source.audio->toJson());
	}
	if (source.media) {
		item.mime = source.media->mime;
		item.fileSize = source.media->size;
		item.durationMs = source.media->durationMs;
		item.width = source.media->width;
		item.height = source.media->height;
	}
	return enqueue(std::move(item));
}

QString Outbox::sendInlineResult(
		const QString &chat,
		const QString &bot,
		const QString &queryId,
		const QString &resultId,
		const QString &previewText,
		qint64 replyToSeq) {
	auto item = OutboxItem();
	item.chat = chat;
	item.kind = u"text"_s;
	item.previewText = previewText;
	// The content comes from the bot's answer: kind and text must be absent.
	item.body = QJsonObject{
		{ u"inline_result"_s, QJsonObject{
			{ u"bot"_s, bot },
			{ u"query_id"_s, queryId },
			{ u"result_id"_s, resultId },
		} },
	};
	Json::Put(item.body, "reply_to_seq"_L1, replyToSeq);
	return enqueue(std::move(item));
}

QString Outbox::enqueue(OutboxItem item) {
	item.clientMessageId = base::RandomUuid();
	item.createdAt = base::unixtime::nowMs();
	// Strictly increasing within the queue, whatever the clock does.
	if (!_order.isEmpty()) {
		item.createdAt = std::max(item.createdAt, _items.value(_order.back()).createdAt + 1);
	}
	const auto id = item.clientMessageId;
	const auto chat = item.chat;
	_order.push_back(id);
	_items.insert(id, item);
	persist(item);
	show(item);
	pumpChat(chat);
	return id;
}

void Outbox::persist(const OutboxItem &item) {
	if (const auto db = _session->local()) {
		db->putOutbox({ item.clientMessageId, item.chat, item.createdAt, item.toJson() });
	}
}

// The local bubble.
void Outbox::show(const OutboxItem &item) {
	auto message = Message();
	message.chat = item.chat;
	message.seq = 0;
	message.id = u"local:"_s + item.clientMessageId;
	message.sender = _session->userId();
	message.clientMessageId = item.clientMessageId;
	message.kindName = item.kind;
	message.kind = MessageKindFromString(item.kind);
	message.text = Json::String(item.body, "text"_L1);
	if (message.text.isEmpty()) {
		message.text = item.previewText;
	}
	message.replyToSeq = Json::Int64(item.body, "reply_to_seq"_L1);
	message.createdAt = item.createdAt;
	message.mediaSpoiler = Json::Bool(item.body, "media_spoiler"_L1);
	message.entities = Json::List<Entity>(item.body.value("entities"_L1));
	message.quote = Json::Optional<Quote>(item.body, "quote"_L1);
	message.location = Json::Optional<Location>(item.body, "location"_L1);
	message.audio = Json::Optional<AudioMeta>(item.body, "audio"_L1);
	message.forwardedPost = Json::String(item.body, "forwarded_post"_L1);
	message.forwarded = item.body.contains("forward_from"_L1);
	if (const auto period = Json::Int64(item.body, "live_period"_L1)) {
		auto live = LiveInfo();
		live.period = period;
		live.until = item.createdAt / 1000 + period;
		message.live = live;
	}
	const auto hasMedia = !item.filePath.isEmpty()
		|| item.fileSize > 0
		|| item.width > 0
		|| item.durationMs > 0
		|| !item.mime.isEmpty();
	if (hasMedia) {
		auto media = MediaMeta();
		media.size = item.fileSize;
		media.mime = item.mime;
		media.durationMs = item.durationMs;
		media.width = item.width;
		media.height = item.height;
		message.media = media;
	}
	if (item.kind == u"sticker") {
		auto sticker = Sticker();
		sticker.id = Json::String(item.body, "sticker"_L1);
		message.sticker = sticker;
	}
	message.attachment = item.mediaId;
	message.mine = true;
	message.localPath = item.filePath;
	message.errorCode = item.errorCode;
	message.errorText = item.errorText;
	const auto queue = chatQueue(item.chat);
	const auto head = std::find_if(queue.cbegin(), queue.cend(), [&](const QString &id) {
		return !_items.value(id).failed;
	});
	message.status = item.failed
		? MessageStatus::Failed
		: (head != queue.cend() && *head == item.clientMessageId)
		? MessageStatus::Sending
		: MessageStatus::Queued;
	_session->messages().addLocal(message);
	// The row's preview and order follow the queued message.
	_session->chats().patch(item.chat, [&](Chat &chat) {
		chat.lastMessage = message;
		chat.lastMine = true;
		chat.lastActivityAt = std::max(chat.lastActivityAt, item.createdAt);
		chat.draft = std::nullopt;
	});
}

void Outbox::setStatus(const OutboxItem &item, MessageStatus status) {
	_session->messages().patchMessage(
		item.chat,
		Message::KeyForLocal(item.clientMessageId),
		[&](Message &message) {
			message.status = status;
			message.errorCode = item.errorCode;
			message.errorText = item.errorText;
			if (status != MessageStatus::Sending) {
				message.uploadProgress = -1.;
			}
		});
}

// ---- sending

void Outbox::retry(const QString &clientMessageId) {
	const auto i = _items.find(clientMessageId);
	if (i == _items.end()) {
		return;
	}
	i->failed = false;
	i->errorCode = QString();
	i->errorText = QString();
	i->attempts = 0;
	persist(*i);
	setStatus(*i, MessageStatus::Queued);
	pumpChat(i->chat);
}

void Outbox::discard(const QString &clientMessageId) {
	const auto i = _items.find(clientMessageId);
	if (i == _items.end()) {
		return;
	}
	const auto item = *i;
	if (const auto upload = _uploads.take(clientMessageId)) {
		_session->upload().cancel(upload);
	}
	if (!item.uploadId.isEmpty() && item.mediaId.isEmpty()) {
		_session->upload().abandon(item.uploadId);
	}
	delete _retryTimers.take(clientMessageId);
	_items.erase(i);
	_order.removeAll(clientMessageId);
	if (_running.value(item.chat) == clientMessageId) {
		_running.remove(item.chat);
	}
	if (const auto db = _session->local()) {
		db->deleteOutbox(clientMessageId);
	}
	_session->messages().removeMessage(item.chat, Message::KeyForLocal(clientMessageId));
	pumpChat(item.chat);
}

// The chat is gone (a group I am no longer in): what waited for it never goes.
void Outbox::discardChat(const QString &chat) {
	const auto queue = chatQueue(chat);
	for (const auto &id : queue) {
		const auto i = _items.find(id);
		if (i == _items.end()) {
			continue;
		}
		if (const auto upload = _uploads.take(id)) {
			_session->upload().cancel(upload);
		}
		if (!i->uploadId.isEmpty() && i->mediaId.isEmpty()) {
			_session->upload().abandon(i->uploadId);
		}
		delete _retryTimers.take(id);
		_items.erase(i);
		_order.removeAll(id);
		if (const auto db = _session->local()) {
			db->deleteOutbox(id);
		}
	}
	_running.remove(chat);
}

void Outbox::retryAll() {
	if (!_started) {
		return;
	}
	auto chats = QStringList();
	for (const auto &id : _order) {
		const auto &item = _items[id];
		if (!item.failed && !chats.contains(item.chat)) {
			chats.push_back(item.chat);
		}
	}
	for (const auto &chat : chats) {
		// A message waiting out its backoff goes now.
		const auto queue = chatQueue(chat);
		for (const auto &id : queue) {
			if (!_items.value(id).failed) {
				if (const auto timer = _retryTimers.take(id)) {
					delete timer;
				}
				break;
			}
		}
		pumpChat(chat);
	}
}

// Starts the first message of the chat that is not Failed, unless one is on its way.
void Outbox::pumpChat(const QString &chat) {
	if (!_started || _running.contains(chat)) {
		return;
	}
	for (const auto &id : chatQueue(chat)) {
		const auto &item = _items[id];
		if (item.failed) {
			continue;
		}
		if (_retryTimers.contains(id)) {
			return; // waiting out its backoff; later messages wait behind it
		}
		pump(id);
		return;
	}
}

void Outbox::pump(const QString &clientMessageId) {
	const auto i = _items.find(clientMessageId);
	if (!_started || i == _items.end() || i->failed) {
		return;
	}
	_running.insert(i->chat, clientMessageId);
	setStatus(*i, MessageStatus::Sending);
	if (!i->filePath.isEmpty() && i->mediaId.isEmpty()) {
		uploadFile(clientMessageId);
	} else {
		send(clientMessageId);
	}
}

void Outbox::uploadFile(const QString &clientMessageId) {
	const auto &item = _items[clientMessageId];
	const auto chat = item.chat;
	const auto key = Message::KeyForLocal(clientMessageId);
	auto request = UploadRequest();
	request.filePath = item.filePath;
	request.kind = Upload::KindForMessageKind(item.kind);
	const auto row = _session->chats().chat(chat);
	request.purpose = (row && row->isConversation)
		? UploadPurpose::Conversation
		: UploadPurpose::Chat;
	request.fileName = item.fileName;
	request.mime = item.mime;
	request.resumeId = item.uploadId;
	auto callbacks = UploadCallbacks();
	callbacks.onUploadId = [=, this](const QString &uploadId) {
		const auto i = _items.find(clientMessageId);
		if (i != _items.end() && i->uploadId != uploadId) {
			i->uploadId = uploadId;
			persist(*i);
		}
	};
	callbacks.progress = [=, this](qint64 sent, qint64 total) {
		_session->messages().patchMessage(chat, key, [&](Message &message) {
			message.uploadProgress = total ? (double(sent) / double(total)) : 0.;
		});
	};
	callbacks.done = [=, this](const UploadResult &result) {
		_uploads.remove(clientMessageId);
		const auto i = _items.find(clientMessageId);
		if (i == _items.end()) {
			return;
		}
		i->mediaId = result.mediaId;
		i->body.insert("attachment"_L1, result.mediaId);
		persist(*i);
		send(clientMessageId);
	};
	callbacks.fail = [=, this](const Api::Error &error) {
		_uploads.remove(clientMessageId);
		errored(clientMessageId, error);
	};
	_uploads.insert(clientMessageId, _session->upload().start(request, this, std::move(callbacks)));
}

void Outbox::send(const QString &clientMessageId) {
	const auto i = _items.find(clientMessageId);
	if (i == _items.end()) {
		return;
	}
	auto body = i->body;
	// The route is picked by the id of the destination: a group has its own.
	const auto group = IsGroupId(i->chat);
	body.insert(group ? "group"_L1 : "chat"_L1, i->chat);
	body.insert("client_message_id"_L1, clientMessageId);
	// Over the connection or HTTP: the same id either way, so the server
	// answers a repeat with `duplicate` instead of a second message.
	_session->transport().rpc(group ? u"groups.send"_s : u"chat.send"_s)
		.body(body)
		.idempotencyKey(clientMessageId)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			succeeded(clientMessageId, result);
		})
		.fail([=, this](const Api::Error &error) {
			errored(clientMessageId, error);
		})
		.send();
}

void Outbox::succeeded(const QString &clientMessageId, const QJsonObject &result) {
	const auto i = _items.find(clientMessageId);
	if (i == _items.end()) {
		return;
	}
	const auto item = *i;
	const auto seq = Json::Int64(result, "seq"_L1);
	// Sent: the echo update (same client message id) replaces the local
	// bubble; until it lands, show the local one as sent with its seq.
	const auto &history = _session->messages().history(item.chat);
	const auto index = history.indexOfKey(Message::KeyForLocal(clientMessageId));
	// The live frame of a group's message may have come before this answer.
	const auto cached = (seq > 0) && (history.find(seq) != nullptr);
	if (index >= 0 && seq > 0) {
		auto message = history.items[index];
		message.seq = seq;
		message.id = Json::String(result, "message_id"_L1);
		if (const auto created = Json::Int64(result, "created_at"_L1)) {
			message.createdAt = created;
		}
		if (const auto until = Json::Int64(result, "live_until"_L1); until && message.live) {
			message.live->until = until;
		}
		message.attachment = item.mediaId;
		message.status = MessageStatus::Sent;
		message.uploadProgress = -1.;
		// merge() swaps the pending copy for this one (same client message id).
		_session->messages().merge(item.chat, { message }, history.loaded && !history.hasMoreAfter);
		_session->chats().patch(item.chat, [&](Chat &chat) {
			if (seq >= chat.lastSeq) {
				chat.lastSeq = seq;
				chat.lastMessage = message;
				chat.lastMine = true;
				chat.lastActivityAt = std::max(chat.lastActivityAt, message.createdAt);
			}
		});
	} else if (index >= 0) {
		_session->messages().removeMessage(item.chat, Message::KeyForLocal(clientMessageId));
	}
	if (IsGroupId(item.chat) && seq > 0) {
		// The answer carries the group's pts: the message is the next
		// change, or something else happened in between.
		const auto found = _session->messages().find(item.chat, seq);
		_session->groups().sent(item.chat, found ? *found : Message(), result, cached);
	}
	_items.erase(i);
	_order.removeAll(clientMessageId);
	_running.remove(item.chat);
	if (const auto db = _session->local()) {
		db->deleteOutbox(clientMessageId);
	}
	Q_EMIT sent(item.chat, clientMessageId, seq);
	// The next one of this chat becomes «sending».
	pumpChat(item.chat);
}

void Outbox::errored(const QString &clientMessageId, const Api::Error &error) {
	auto i = _items.find(clientMessageId);
	if (i == _items.end()) {
		return;
	}
	const auto chat = i->chat;
	auto slowMode = false;
	if (IsGroupId(chat)) {
		// «Not a member any more» drops the group, and its queued sends
		// with it: none of them is tried again.
		if (_session->groups().handleError(chat, error)) {
			return;
		}
		i = _items.find(clientMessageId);
		if (i == _items.end()) {
			return;
		}
		slowMode = (error.code == u"group.slow_mode");
	}
	if (_running.value(chat) == clientMessageId) {
		_running.remove(chat);
	}
	if (!slowMode) {
		++i->attempts;
	}
	if (slowMode || (error.transient() && i->attempts < kMaxAttempts)) {
		persist(*i);
		// Slow mode names the wait: the message stays queued and goes with
		// the same id when it is over.
		const auto delay = slowMode
			? int(std::clamp(error.retryAfter.value_or(5.), 1., 3600.) * 1000) + 250
			: int(std::min(30., std::pow(2., i->attempts)) * 1000);
		const auto timer = new base::Timer([=, this] {
			if (const auto own = _retryTimers.take(clientMessageId)) {
				own->deleteLater();
			}
			pumpChat(chat);
		});
		delete _retryTimers.take(clientMessageId);
		_retryTimers.insert(clientMessageId, timer);
		timer->callOnce(delay);
		// The message stays first in line: order within the chat is kept.
		return;
	}
	i->failed = true;
	i->errorCode = error.code;
	i->errorText = error.message;
	persist(*i);
	setStatus(*i, MessageStatus::Failed);
	Q_EMIT failed(chat, clientMessageId, error);
	// The ones behind it are not held up by a message that will not go.
	pumpChat(chat);
}

} // namespace Data
