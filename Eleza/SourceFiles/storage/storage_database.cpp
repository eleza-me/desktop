#include "storage/storage_database.h"

#include "base/platform_file.h"

#include <utility>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSemaphore>
#include <QtCore/QUuid>
#include <QtCore/QVariant>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

namespace Storage {
namespace {

// Schema migrations, applied in order inside one transaction each; the index
// + 1 is the resulting PRAGMA user_version. Never edit a shipped entry:
// append a new one.
const char *const kMigrations[] = {
	// 1: the initial layout.
	"CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value BLOB NOT NULL) WITHOUT ROWID;"
	"CREATE TABLE kv (key TEXT PRIMARY KEY NOT NULL, value BLOB NOT NULL) WITHOUT ROWID;"
	"CREATE TABLE chats (id TEXT PRIMARY KEY NOT NULL, data BLOB NOT NULL) WITHOUT ROWID;"
	"CREATE TABLE messages (chat TEXT NOT NULL, seq INTEGER NOT NULL, data BLOB NOT NULL,"
	" PRIMARY KEY (chat, seq)) WITHOUT ROWID;"
	"CREATE TABLE users (id TEXT PRIMARY KEY NOT NULL, data BLOB NOT NULL) WITHOUT ROWID;"
	"CREATE TABLE outbox (position INTEGER PRIMARY KEY AUTOINCREMENT, id TEXT NOT NULL UNIQUE,"
	" chat TEXT NOT NULL, created INTEGER NOT NULL, data BLOB NOT NULL);"
	"CREATE INDEX outbox_order ON outbox (created, position);"
	"CREATE TABLE drafts (chat TEXT PRIMARY KEY NOT NULL, data BLOB NOT NULL) WITHOUT ROWID;",
};
constexpr auto kVersion = int(sizeof(kMigrations) / sizeof(kMigrations[0]));

const char *const kTables[] = { "meta", "kv", "chats", "messages", "users", "outbox", "drafts" };

[[nodiscard]] QByteArray Serialize(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QJsonObject Parse(const QVariant &value) {
	return QJsonDocument::fromJson(value.toByteArray()).object();
}

// Any JSON value (also a bare number or string) wrapped so it is a document.
[[nodiscard]] QByteArray SerializeValue(const QJsonValue &value) {
	return Serialize(QJsonObject{ { u"v"_s, value } });
}

[[nodiscard]] QJsonValue ParseValue(const QVariant &value) {
	return Parse(value).value(u"v"_s);
}

} // namespace

namespace details {

// Lives on the worker thread; owns the connection.
class DatabaseWorker final : public QObject {
public:
	DatabaseWorker(Database *owner, const QString &path)
	: _owner(owner)
	, _path(path)
	, _connection(u"eleza-db-"_s + QUuid::createUuid().toString(QUuid::Id128)) {
	}

	~DatabaseWorker() {
		close();
	}

	void close() {
		if (_opened) {
			{
				auto db = QSqlDatabase::database(_connection, false);
				if (db.isOpen()) {
					exec(u"PRAGMA optimize"_s);
					db.close();
				}
			}
			QSqlDatabase::removeDatabase(_connection);
			_opened = false;
		}
		_ready = false;
	}

	// False when the database is unusable: operations become no-ops.
	[[nodiscard]] bool ready() {
		if (!_opened) {
			open();
		}
		return _ready;
	}

	bool exec(const QString &sql, const QVariantList &values = {}, QSqlQuery *out = nullptr) {
		auto query = QSqlQuery(QSqlDatabase::database(_connection, false));
		query.setForwardOnly(true);
		if (!query.prepare(sql)) {
			fail(query.lastError().text() + u" in "_s + sql);
			return false;
		}
		for (const auto &value : values) {
			query.addBindValue(value);
		}
		if (!query.exec()) {
			fail(query.lastError().text() + u" in "_s + sql);
			return false;
		}
		if (out) {
			*out = std::move(query);
		}
		return true;
	}

	template <typename Callback>
	void transaction(Callback &&callback) {
		if (!ready()) {
			return;
		}
		auto db = QSqlDatabase::database(_connection, false);
		if (!db.transaction()) {
			fail(db.lastError().text());
			return;
		}
		if (callback()) {
			if (!db.commit()) {
				fail(db.lastError().text());
				db.rollback();
			}
		} else {
			db.rollback();
		}
	}

	[[nodiscard]] QVector<QJsonObject> rows(const QString &sql, const QVariantList &values = {}) {
		auto result = QVector<QJsonObject>();
		auto query = QSqlQuery();
		if (ready() && exec(sql, values, &query)) {
			while (query.next()) {
				result.push_back(Parse(query.value(0)));
			}
		}
		return result;
	}

	[[nodiscard]] QJsonValue single(const QString &table, const QString &key) {
		auto query = QSqlQuery();
		if (ready()
			&& exec(u"SELECT value FROM %1 WHERE key = ?"_s.arg(table), { key }, &query)
			&& query.next()) {
			return ParseValue(query.value(0));
		}
		return QJsonValue(QJsonValue::Undefined);
	}

	[[nodiscard]] Database *owner() const {
		return _owner;
	}

private:
	void open() {
		_opened = true;
		base::platform::EnsureDirectory(QFileInfo(_path).absolutePath(), true);
		for (auto attempt = 0; attempt != 2; ++attempt) {
			if (tryOpen()) {
				_ready = true;
				return;
			}
			// A damaged file or one written by a newer build: the cache is
			// disposable, start over once.
			{
				auto db = QSqlDatabase::database(_connection, false);
				db.close();
			}
			QSqlDatabase::removeDatabase(_connection);
			Database::Wipe(_path);
		}
		// Not the path: it has the name of the user's home folder in it.
		fail(u"could not open the database file"_s);
	}

	[[nodiscard]] bool tryOpen() {
		if (!QSqlDatabase::isDriverAvailable(u"QSQLITE"_s)) {
			fail(u"the QSQLITE driver is not available"_s);
			return false;
		}
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, _connection);
		db.setDatabaseName(_path);
		db.setConnectOptions(u"QSQLITE_BUSY_TIMEOUT=5000"_s);
		if (!db.open()) {
			fail(db.lastError().text());
			return false;
		}
#ifndef Q_OS_WIN
		QFile::setPermissions(_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif // !Q_OS_WIN
		auto query = QSqlQuery();
		if (!exec(u"PRAGMA journal_mode = WAL"_s)
			|| !exec(u"PRAGMA synchronous = NORMAL"_s)
			|| !exec(u"PRAGMA user_version"_s, {}, &query)
			|| !query.next()) {
			return false;
		}
		const auto version = query.value(0).toInt();
		query = QSqlQuery();
		if (version > kVersion || version < 0) {
			return false;
		}
		for (auto i = version; i != kVersion; ++i) {
			if (!db.transaction()) {
				return false;
			}
			auto ok = true;
			const auto statements = QString::fromLatin1(kMigrations[i]).split(u';', Qt::SkipEmptyParts);
			for (const auto &statement : statements) {
				if (!exec(statement.trimmed())) {
					ok = false;
					break;
				}
			}
			ok = ok && exec(u"PRAGMA user_version = %1"_s.arg(i + 1));
			if (!ok || !db.commit()) {
				db.rollback();
				return false;
			}
		}
		return true;
	}

	void fail(const QString &error) {
		qWarning("Storage: %s", qPrintable(error));
		const auto owner = _owner;
		QMetaObject::invokeMethod(owner, [owner, error] {
			Q_EMIT owner->failed(error);
		}, Qt::QueuedConnection);
	}

	Database *_owner = nullptr;
	QString _path;
	QString _connection;
	bool _opened = false;
	bool _ready = false;

};

} // namespace details

int Database::CurrentVersion() {
	return kVersion;
}

Database::Database(const QString &path, QObject *parent)
: QObject(parent)
, _path(path)
, _worker(new details::DatabaseWorker(this, path)) {
	_thread.setObjectName(u"eleza-db"_s);
	_worker->moveToThread(&_thread);
	_thread.start();
}

Database::~Database() {
	// Queued after everything issued so far: pending writes are not lost.
	const auto worker = _worker;
	const auto thread = &_thread;
	QMetaObject::invokeMethod(worker, [worker, thread] {
		worker->close();
		worker->moveToThread(nullptr);
		thread->quit();
	}, Qt::QueuedConnection);
	_thread.wait();
	delete _worker;
}

template <typename Method>
void Database::post(Method &&method) {
	const auto worker = _worker;
	QMetaObject::invokeMethod(worker, [worker, method = std::forward<Method>(method)]() mutable {
		method(*worker);
	}, Qt::QueuedConnection);
}

template <typename Result>
void Database::reply(base::Guard &guard, Fn<void(Result)> &done, Result result) {
	// Called on the worker thread: hop back to the owner's thread. The
	// callback (and whatever it holds) is taken out of the worker's task, so
	// it is never destroyed on the worker thread.
	QMetaObject::invokeMethod(this, [
		guard = std::exchange(guard, base::Guard()),
		done = std::exchange(done, nullptr),
		result = std::move(result)
	]() mutable {
		if (done && guard.alive()) {
			done(std::move(result));
		}
	}, Qt::QueuedConnection);
}

void Database::setMeta(const QString &key, const QJsonValue &value) {
	post([key, bytes = SerializeValue(value)](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)"_s, { key, bytes });
		}
	});
}

void Database::removeMeta(const QString &key) {
	post([key](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"DELETE FROM meta WHERE key = ?"_s, { key });
		}
	});
}

void Database::meta(const QString &key, base::Guard guard, Fn<void(QJsonValue)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		reply<QJsonValue>(guard, done, worker.single(u"meta"_s, key));
	});
}

void Database::setValue(const QString &key, const QJsonValue &value) {
	post([key, bytes = SerializeValue(value)](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?)"_s, { key, bytes });
		}
	});
}

void Database::removeValue(const QString &key) {
	post([key](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"DELETE FROM kv WHERE key = ?"_s, { key });
		}
	});
}

void Database::value(const QString &key, base::Guard guard, Fn<void(QJsonValue)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		reply<QJsonValue>(guard, done, worker.single(u"kv"_s, key));
	});
}

void Database::putChats(const QVector<Row> &chats, bool replaceAll) {
	if (chats.isEmpty() && !replaceAll) {
		return;
	}
	post([chats, replaceAll](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			if (replaceAll && !worker.exec(u"DELETE FROM chats"_s)) {
				return false;
			}
			for (const auto &chat : chats) {
				if (!worker.exec(
						u"INSERT OR REPLACE INTO chats (id, data) VALUES (?, ?)"_s,
						{ chat.id, Serialize(chat.data) })) {
					return false;
				}
			}
			return true;
		});
	});
}

void Database::deleteChat(const QString &id) {
	post([id](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			return worker.exec(u"DELETE FROM chats WHERE id = ?"_s, { id })
				&& worker.exec(u"DELETE FROM messages WHERE chat = ?"_s, { id })
				&& worker.exec(u"DELETE FROM drafts WHERE chat = ?"_s, { id });
		});
	});
}

void Database::chats(base::Guard guard, Fn<void(QVector<QJsonObject>)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		reply<QVector<QJsonObject>>(guard, done, worker.rows(u"SELECT data FROM chats"_s));
	});
}

void Database::putMessages(const QVector<MessageRow> &messages) {
	if (messages.isEmpty()) {
		return;
	}
	post([messages](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			for (const auto &message : messages) {
				if (message.seq <= 0) {
					continue;
				}
				if (!worker.exec(
						u"INSERT OR REPLACE INTO messages (chat, seq, data) VALUES (?, ?, ?)"_s,
						{ message.chat, message.seq, Serialize(message.data) })) {
					return false;
				}
			}
			return true;
		});
	});
}

void Database::mergeMessages(const QVector<MessageRow> &messages) {
	if (messages.isEmpty()) {
		return;
	}
	post([messages](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			for (const auto &message : messages) {
				if (message.seq <= 0) {
					continue;
				}
				if (!worker.exec(
						u"INSERT OR REPLACE INTO messages (chat, seq, data) "
						"SELECT ?, ?, ? WHERE "
						"EXISTS (SELECT 1 FROM messages WHERE chat = ? AND seq = ?) "
						"OR ? > COALESCE((SELECT MAX(seq) FROM messages WHERE chat = ?), 0)"_s,
						{
							message.chat,
							message.seq,
							Serialize(message.data),
							message.chat,
							message.seq,
							message.seq,
							message.chat,
						})) {
					return false;
				}
			}
			return true;
		});
	});
}

void Database::dropDetachedMessages(const QString &chat, qint64 firstSeq) {
	post([chat, firstSeq](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(
				u"DELETE FROM messages WHERE chat = ? AND seq < ? "
				"AND (SELECT MAX(seq) FROM messages WHERE chat = ?) < ?"_s,
				{ chat, firstSeq, chat, firstSeq - 1 });
		}
	});
}

void Database::deleteMessage(const QString &chat, qint64 seq) {
	post([chat, seq](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"DELETE FROM messages WHERE chat = ? AND seq = ?"_s, { chat, seq });
		}
	});
}

void Database::clearMessages(const QString &chat, std::optional<qint64> beforeSeq) {
	post([chat, beforeSeq](details::DatabaseWorker &worker) {
		if (!worker.ready()) {
			return;
		} else if (beforeSeq) {
			worker.exec(u"DELETE FROM messages WHERE chat = ? AND seq < ?"_s, { chat, *beforeSeq });
		} else {
			worker.exec(u"DELETE FROM messages WHERE chat = ?"_s, { chat });
		}
	});
}

void Database::messages(
		const QString &chat,
		int limit,
		qint64 beforeSeq,
		base::Guard guard,
		Fn<void(QVector<QJsonObject>)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		auto list = (beforeSeq > 0)
			? worker.rows(
				u"SELECT data FROM messages WHERE chat = ? AND seq < ? ORDER BY seq DESC LIMIT ?"_s,
				{ chat, beforeSeq, limit })
			: worker.rows(
				u"SELECT data FROM messages WHERE chat = ? ORDER BY seq DESC LIMIT ?"_s,
				{ chat, limit });
		std::reverse(list.begin(), list.end());
		reply<QVector<QJsonObject>>(guard, done, std::move(list));
	});
}

void Database::messagesAfter(
		const QString &chat,
		int limit,
		qint64 afterSeq,
		base::Guard guard,
		Fn<void(QVector<QJsonObject>)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		reply<QVector<QJsonObject>>(guard, done, worker.rows(
			u"SELECT data FROM messages WHERE chat = ? AND seq > ? ORDER BY seq ASC LIMIT ?"_s,
			{ chat, afterSeq, limit }));
	});
}

void Database::putUsers(const QVector<Row> &users) {
	if (users.isEmpty()) {
		return;
	}
	post([users](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			for (const auto &user : users) {
				if (!worker.exec(
						u"INSERT OR REPLACE INTO users (id, data) VALUES (?, ?)"_s,
						{ user.id, Serialize(user.data) })) {
					return false;
				}
			}
			return true;
		});
	});
}

void Database::users(
		const QStringList &ids,
		base::Guard guard,
		Fn<void(QHash<QString, QJsonObject>)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		auto result = QHash<QString, QJsonObject>();
		if (worker.ready()) {
			for (const auto &id : ids) {
				auto query = QSqlQuery();
				if (worker.exec(u"SELECT data FROM users WHERE id = ?"_s, { id }, &query)
					&& query.next()) {
					result.insert(id, Parse(query.value(0)));
				}
			}
		}
		reply<QHash<QString, QJsonObject>>(guard, done, std::move(result));
	});
}

void Database::putOutbox(const OutboxRow &item) {
	post([item](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			// UPDATE first so a rewritten item keeps its place in the queue.
			auto query = QSqlQuery();
			if (!worker.exec(
					u"UPDATE outbox SET chat = ?, created = ?, data = ? WHERE id = ?"_s,
					{ item.chat, item.createdAt, Serialize(item.data), item.id },
					&query)) {
				return false;
			}
			if (query.numRowsAffected() > 0) {
				return true;
			}
			return worker.exec(
				u"INSERT INTO outbox (id, chat, created, data) VALUES (?, ?, ?, ?)"_s,
				{ item.id, item.chat, item.createdAt, Serialize(item.data) });
		});
	});
}

void Database::deleteOutbox(const QString &id) {
	post([id](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"DELETE FROM outbox WHERE id = ?"_s, { id });
		}
	});
}

void Database::outbox(base::Guard guard, Fn<void(QVector<QJsonObject>)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		reply<QVector<QJsonObject>>(guard, done, worker.rows(
			u"SELECT data FROM outbox ORDER BY created ASC, position ASC"_s));
	});
}

void Database::putDraft(const QString &chat, const QJsonObject &draft) {
	post([chat, bytes = Serialize(draft)](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"INSERT OR REPLACE INTO drafts (chat, data) VALUES (?, ?)"_s, { chat, bytes });
		}
	});
}

void Database::deleteDraft(const QString &chat) {
	post([chat](details::DatabaseWorker &worker) {
		if (worker.ready()) {
			worker.exec(u"DELETE FROM drafts WHERE chat = ?"_s, { chat });
		}
	});
}

void Database::drafts(base::Guard guard, Fn<void(QHash<QString, QJsonObject>)> done) {
	post([=, this](details::DatabaseWorker &worker) mutable {
		auto result = QHash<QString, QJsonObject>();
		auto query = QSqlQuery();
		if (worker.ready() && worker.exec(u"SELECT chat, data FROM drafts"_s, {}, &query)) {
			while (query.next()) {
				result.insert(query.value(0).toString(), Parse(query.value(1)));
			}
		}
		reply<QHash<QString, QJsonObject>>(guard, done, std::move(result));
	});
}

void Database::clearAll() {
	post([](details::DatabaseWorker &worker) {
		worker.transaction([&] {
			for (const auto table : kTables) {
				if (!worker.exec(u"DELETE FROM %1"_s.arg(QLatin1StringView(table)))) {
					return false;
				}
			}
			return true;
		});
	});
}

void Database::flush(base::Guard guard, Fn<void()> done) {
	post([=, this](details::DatabaseWorker &) mutable {
		QMetaObject::invokeMethod(this, [
			guard = std::exchange(guard, base::Guard()),
			done = std::exchange(done, nullptr)
		] {
			if (done && guard.alive()) {
				done();
			}
		}, Qt::QueuedConnection);
	});
}

void Database::flushSync() {
	auto semaphore = QSemaphore();
	const auto pointer = &semaphore;
	post([pointer](details::DatabaseWorker &) {
		pointer->release();
	});
	semaphore.acquire();
}

bool Database::Wipe(const QString &path) {
	auto result = true;
	for (const auto suffix : { "", "-wal", "-shm", "-journal" }) {
		const auto file = path + QLatin1StringView(suffix);
		if (QFileInfo::exists(file) && !QFile::remove(file)) {
			result = false;
		}
	}
	return result;
}

} // namespace Storage
