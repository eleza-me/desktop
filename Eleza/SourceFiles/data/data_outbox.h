// The outbox: a message is queued locally with a `client_message_id`, shown
// at once as «sending», and sent over the connection (`chat.send`, or
// `groups.send` when the destination is a group) or HTTP with the same id -
// the server dedups, so a lost answer is never a duplicate. Media is uploaded first (resumable). The queue survives a
// restart (the database's outbox table).
//
// Ordering: messages of one chat leave strictly in the order they were
// queued; a message waits (Queued) until the ones before it are sent. A
// transient failure is retried with a backoff (2 s .. 30 s, six attempts),
// then the message becomes Failed and the ones behind it go on; a Failed
// message stays in the timeline until retry() or discard().
//
//   outbox.sendText(chat, u"hello"_s);
//   outbox.sendFile(chat, path, { .caption = u"look"_s });
//   outbox.sendSticker(chat, stickerId);
//   outbox.forward(toChat, message);
#pragma once

#include "api/api_error.h"
#include "base/timer.h"
#include "base/weak_guard.h"
#include "data/data_types.h"
#include "data/data_upload.h"

#include <QtCore/QHash>

namespace Data {

class Session;

struct SendTextOptions {
	qint64 replyToSeq = 0;
	QVector<Entity> entities;
	std::optional<Quote> quote;  // an excerpt of the replied-to message
	QString linkPreview;         // the url returned by POST /v1/link-preview
	bool silent = false;         // «send without sound»: nobody is alerted
};
struct SendFileOptions {
	QString caption;
	QVector<Entity> entities;
	qint64 replyToSeq = 0;
	bool asVoice = false;               // a recorded voice message
	std::optional<UploadKind> kind;     // override the detected kind (video note, file)
	qint64 durationMs = 0;
	QString fileName;
	bool spoiler = false;
	std::optional<AudioMeta> audio;     // title / performer of a music file
	bool silent = false;                // «send without sound»
};

class Outbox final : public QObject {
	Q_OBJECT

public:
	using TextOptions = SendTextOptions;
	using FileOptions = SendFileOptions;

	explicit Outbox(Session *session);
	~Outbox();

	// Loads the stored queue, shows its bubbles and starts sending.
	void start(base::Guard guard = base::Guard(), Fn<void()> ready = nullptr);
	void stop();

	// Every send returns the client message id of the queued message
	// (Message::KeyForLocal(id) is its key in the timeline); empty when the
	// message cannot be queued (a file that does not exist).
	QString sendText(const QString &chat, const QString &text, const TextOptions &options = {});
	QString sendSticker(const QString &chat, const QString &stickerId, qint64 replyToSeq = 0);
	QString sendFile(const QString &chat, const QString &filePath, const FileOptions &options = {});
	QString sendLocation(const QString &chat, const Location &location, qint64 replyToSeq = 0);
	// `periodSeconds` is one of client-config chat.live_location.periods.
	QString sendLiveLocation(const QString &chat, const Location &location, qint64 periodSeconds);
	// Shares a feed post into a chat.
	QString sendPost(const QString &chat, const QString &postId);
	// Forwards a message I can see: text and media keep their kind (media is
	// not uploaded again); stickers, locations and posts are sent anew.
	QString forward(const QString &toChat, const Message &source);
	// A result of a bot's inline query.
	QString sendInlineResult(
		const QString &chat,
		const QString &bot,
		const QString &queryId,
		const QString &resultId);

	// Tries a failed message again.
	void retry(const QString &clientMessageId);
	// Removes a queued / failed message (and stops its upload).
	void discard(const QString &clientMessageId);
	// Removes everything queued for a chat that is gone.
	void discardChat(const QString &chat);
	// Tries everything that is not Failed (called when the connection comes up).
	void retryAll();

	[[nodiscard]] bool contains(const QString &clientMessageId) const {
		return _items.contains(clientMessageId);
	}
	[[nodiscard]] int pendingCount(const QString &chat = QString()) const;
	// Something for this chat is in the queue (also a failed message).
	[[nodiscard]] bool holds(const QString &chat) const;
	// The queue in sending order.
	[[nodiscard]] QVector<OutboxItem> items() const;

	static constexpr auto kMaxAttempts = 6;

Q_SIGNALS:
	void sent(const QString &chat, const QString &clientMessageId, qint64 seq);
	void failed(const QString &chat, const QString &clientMessageId, const Api::Error &error);

private:
	QString enqueue(OutboxItem item);
	void show(const OutboxItem &item);
	void persist(const OutboxItem &item);
	void pumpChat(const QString &chat);
	void pump(const QString &clientMessageId);
	void uploadFile(const QString &clientMessageId);
	void send(const QString &clientMessageId);
	void succeeded(const QString &clientMessageId, const QJsonObject &result);
	void errored(const QString &clientMessageId, const Api::Error &error);
	void setStatus(const OutboxItem &item, MessageStatus status);
	[[nodiscard]] QVector<QString> chatQueue(const QString &chat) const;

	Session *_session = nullptr;
	QHash<QString, OutboxItem> _items;
	QVector<QString> _order;                     // client message ids in queue order
	QHash<QString, QString> _running;            // chat -> the id being sent
	QHash<QString, Upload::Id> _uploads;
	QHash<QString, base::Timer*> _retryTimers;
	bool _started = false;

};

} // namespace Data
