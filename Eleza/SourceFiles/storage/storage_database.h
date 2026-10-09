// The account's local cache (the role IndexedDB plays in the web client): the
// chat list, messages by (chat, seq), people cards, the outbox, drafts, pts
// and other sync state, and a free-form key-value table. One SQLite file per
// signed-in user, so switching accounts never mixes caches.
//
// Threading: every statement runs on one worker thread owned by the
// Database. Writes return at once; reads take a guard and a callback that is
// invoked later on the thread that owns the Database (the main thread),
// unless the guard died meanwhile. Operations run in the order they were
// issued, so a read sees every write made before it.
//
// Rows are stored as JSON documents produced by the Data:: models
// (`toJson()` / `FromJson()`); this layer knows nothing about their fields.
// The cache is disposable: a file written by a newer build is dropped and
// recreated, and logout wipes it (Database::Wipe).
#pragma once

#include "base/weak_guard.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QThread>

namespace Storage {

namespace details {
class DatabaseWorker;
} // namespace details

class Database final : public QObject {
	Q_OBJECT

public:
	struct Row {
		QString id;       // chat id / user id / client message id
		QJsonObject data;
	};
	struct MessageRow {
		QString chat;
		qint64 seq = 0;
		QJsonObject data;
	};
	struct OutboxRow {
		QString id;       // client_message_id
		QString chat;
		qint64 createdAt = 0;
		QJsonObject data;
	};

	// The schema version this build writes.
	[[nodiscard]] static int CurrentVersion();

	// Starts the worker; the file (and its directory) is created on first use.
	explicit Database(const QString &path, QObject *parent = nullptr);
	// Runs everything already queued, then closes the file.
	~Database();

	[[nodiscard]] QString path() const {
		return _path;
	}

	// ---- meta: sync state ("pts", "clearedChats", ...)
	void setMeta(const QString &key, const QJsonValue &value);
	void removeMeta(const QString &key);
	// `done` receives Undefined when the key is absent.
	void meta(const QString &key, base::Guard guard, Fn<void(QJsonValue)> done);

	// ---- kv: anything else a store wants to remember (sticker packs, ...)
	void setValue(const QString &key, const QJsonValue &value);
	void removeValue(const QString &key);
	void value(const QString &key, base::Guard guard, Fn<void(QJsonValue)> done);

	// ---- chats
	void putChats(const QVector<Row> &chats, bool replaceAll = false);
	// Removes the row and the chat's messages and draft.
	void deleteChat(const QString &id);
	void chats(base::Guard guard, Fn<void(QVector<QJsonObject>)> done);

	// ---- messages, keyed by (chat, seq)
	void putMessages(const QVector<MessageRow> &messages);
	void deleteMessage(const QString &chat, qint64 seq);
	// Removes messages with seq < beforeSeq, or all of the chat's when nullopt.
	void clearMessages(const QString &chat, std::optional<qint64> beforeSeq = std::nullopt);
	// Up to `limit` newest messages with seq < beforeSeq (0: no bound), oldest first.
	void messages(
		const QString &chat,
		int limit,
		qint64 beforeSeq,
		base::Guard guard,
		Fn<void(QVector<QJsonObject>)> done);
	// Up to `limit` oldest messages with seq > afterSeq, oldest first.
	void messagesAfter(
		const QString &chat,
		int limit,
		qint64 afterSeq,
		base::Guard guard,
		Fn<void(QVector<QJsonObject>)> done);

	// ---- users
	void putUsers(const QVector<Row> &users);
	void users(
		const QStringList &ids,
		base::Guard guard,
		Fn<void(QHash<QString, QJsonObject>)> done);

	// ---- outbox, in sending order (createdAt, then insertion)
	void putOutbox(const OutboxRow &item);
	void deleteOutbox(const QString &id);
	void outbox(base::Guard guard, Fn<void(QVector<QJsonObject>)> done);

	// ---- drafts not yet confirmed by the server
	void putDraft(const QString &chat, const QJsonObject &draft);
	void deleteDraft(const QString &chat);
	void drafts(base::Guard guard, Fn<void(QHash<QString, QJsonObject>)> done);

	// Empties every table (the file stays).
	void clearAll();

	// Invokes `done` after everything issued so far has been executed.
	void flush(base::Guard guard, Fn<void()> done);
	// Blocks until everything issued so far has been executed (tests, shutdown).
	void flushSync();

	// Deletes the database files of `path` (the Database must not be open).
	static bool Wipe(const QString &path);

Q_SIGNALS:
	// The file could not be opened or a statement failed; the cache keeps
	// working as "empty" and the app goes on with the network.
	void failed(const QString &error);

private:
	template <typename Method>
	void post(Method &&method);
	template <typename Result>
	void reply(base::Guard guard, Fn<void(Result)> done, Result result);

	QString _path;
	QThread _thread;
	details::DatabaseWorker *_worker = nullptr;

};

} // namespace Storage
