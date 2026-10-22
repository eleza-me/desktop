// SecureBlob, KeyValue and the SQLite cache (ordering, paging, persistence, wipe).
#include "storage/storage_database.h"
#include "storage/storage_key_value.h"
#include "storage/storage_secure_blob.h"

#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtTest/QtTest>

using Storage::Database;

namespace {

[[nodiscard]] QJsonObject Text(const QString &text) {
	return QJsonObject{ { u"text"_s, text } };
}

// Reads a list back synchronously (the callback arrives on this thread).
template <typename Result, typename Start>
[[nodiscard]] Result Read(Start &&start) {
	auto result = Result();
	auto done = false;
	auto guard = QObject();
	start(&guard, [&](Result value) {
		result = std::move(value);
		done = true;
	});
	const auto ok = QTest::qWaitFor([&] { return done; }, 5000);
	Q_ASSERT(ok);
	Q_UNUSED(ok);
	return result;
}

[[nodiscard]] QStringList Texts(const QVector<QJsonObject> &rows) {
	auto result = QStringList();
	for (const auto &row : rows) {
		result.push_back(row.value(u"text"_s).toString());
	}
	return result;
}

} // namespace

class TestStorage final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void secureBlob();
	void keyValue();
	void metaAndValues();
	void chats();
	void messagesPaging();
	void detachedMessagesGo();
	void users();
	void outboxKeepsOrder();
	void drafts();
	void callbacksRespectGuardAndThread();
	void persistsAcrossReopenAndWipes();
	void schemaVersionAndNewerFile();

private:
	QTemporaryDir _dir;

};

void TestStorage::secureBlob() {
	const auto blob = Storage::SecureBlob(_dir.path() + u"/secrets/device.key"_s);
	QVERIFY(!blob.exists());
	QVERIFY(!blob.read().has_value());
	const auto secret = QByteArray("\x00\x01secret\xff", 9);
	QVERIFY(blob.write(secret));
	QVERIFY(blob.exists());
	QCOMPARE(blob.read().value(), secret);
	QVERIFY(blob.write(QByteArray()));
	QVERIFY(blob.read().has_value());
	QVERIFY(blob.read()->isEmpty());
#ifndef Q_OS_WIN
	const auto others = QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther;
	QCOMPARE(QFile::permissions(blob.path()) & others, QFileDevice::Permissions());
	QCOMPARE(QFile::permissions(_dir.path() + u"/secrets"_s) & others, QFileDevice::Permissions());
#endif // !Q_OS_WIN
	// A file that is not ours reads as "nothing stored".
	{
		auto file = QFile(blob.path());
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("garbage");
	}
	QVERIFY(!blob.read().has_value());
	blob.remove();
	QVERIFY(!blob.exists());
}

void TestStorage::keyValue() {
	const auto path = _dir.path() + u"/kv/settings.json"_s;
	{
		auto kv = Storage::KeyValue(path);
		QVERIFY(!kv.contains(u"device.id"_s));
		QCOMPARE(kv.string(u"device.id"_s, u"none"_s), u"none"_s);
		kv.set(u"device.id"_s, u"abc"_s);
		kv.set(u"count"_s, 5);
		kv.set(u"flag"_s, true);
		kv.set(u"rt.keys"_s, QJsonObject{ { u"preferred"_s, 1 } });
		kv.set(u"rt.other"_s, 1);
		QCOMPARE(kv.string(u"device.id"_s), u"abc"_s);
		QCOMPARE(kv.integer(u"count"_s), 5);
		QVERIFY(kv.boolean(u"flag"_s));
		QCOMPARE(kv.object(u"rt.keys"_s).value(u"preferred"_s).toInt(), 1);
		kv.set(u"count"_s, QJsonValue()); // null removes
		QVERIFY(!kv.contains(u"count"_s));
		kv.removePrefix(u"rt."_s);
		QVERIFY(!kv.contains(u"rt.keys"_s) && !kv.contains(u"rt.other"_s));
		// Not yet on disk: the write is coalesced...
		QVERIFY(!QFileInfo::exists(path));
		QTRY_VERIFY(QFileInfo::exists(path)); // ...and lands on the next loop turn.
		kv.set(u"late"_s, u"x"_s);
	} // the destructor flushes
	auto reopened = Storage::KeyValue(path);
	QCOMPARE(reopened.string(u"device.id"_s), u"abc"_s);
	QCOMPARE(reopened.string(u"late"_s), u"x"_s);
	QVERIFY(reopened.boolean(u"flag"_s));
}

void TestStorage::metaAndValues() {
	auto db = Database(_dir.path() + u"/meta/cache.db"_s);
	const auto meta = [&](const QString &key) {
		return Read<QJsonValue>([&](QObject *guard, Fn<void(QJsonValue)> done) {
			db.meta(key, guard, std::move(done));
		});
	};
	const auto value = [&](const QString &key) {
		return Read<QJsonValue>([&](QObject *guard, Fn<void(QJsonValue)> done) {
			db.value(key, guard, std::move(done));
		});
	};
	QVERIFY(meta(u"pts"_s).isUndefined());
	db.setMeta(u"pts"_s, 12345678901.);
	db.setMeta(u"me"_s, QJsonObject{ { u"id"_s, u"u_me"_s } });
	db.setValue(u"pts"_s, u"another table"_s);
	QCOMPARE(qint64(meta(u"pts"_s).toDouble()), qint64(12345678901));
	QCOMPARE(meta(u"me"_s).toObject().value(u"id"_s).toString(), u"u_me"_s);
	QCOMPARE(value(u"pts"_s).toString(), u"another table"_s);
	db.setMeta(u"pts"_s, 7);
	QCOMPARE(meta(u"pts"_s).toInt(), 7);
	db.removeMeta(u"pts"_s);
	db.removeValue(u"pts"_s);
	QVERIFY(meta(u"pts"_s).isUndefined());
	QVERIFY(value(u"pts"_s).isUndefined());
}

void TestStorage::chats() {
	auto db = Database(_dir.path() + u"/chats/cache.db"_s);
	const auto all = [&] {
		auto rows = Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.chats(guard, std::move(done));
		});
		auto texts = Texts(rows);
		texts.sort();
		return texts;
	};
	db.putChats({ { u"c1"_s, Text(u"one"_s) }, { u"c2"_s, Text(u"two"_s) } });
	db.putChats({ { u"c2"_s, Text(u"two again"_s) } });
	QCOMPARE(all(), (QStringList{ u"one"_s, u"two again"_s }));
	db.putMessages({ { u"c1"_s, 1, Text(u"m1"_s) }, { u"c2"_s, 1, Text(u"m2"_s) } });
	db.putDraft(u"c1"_s, Text(u"draft"_s));
	db.deleteChat(u"c1"_s);
	QCOMPARE(all(), (QStringList{ u"two again"_s }));
	// Its messages and draft went with it; the other chat's stayed.
	QVERIFY(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
		db.messages(u"c1"_s, 10, 0, guard, std::move(done));
	}).isEmpty());
	QCOMPARE(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
		db.messages(u"c2"_s, 10, 0, guard, std::move(done));
	}).size(), 1);
	QVERIFY((Read<QHash<QString, QJsonObject>>([&](QObject *guard, Fn<void(QHash<QString, QJsonObject>)> done) {
		db.drafts(guard, std::move(done));
	}).isEmpty()));
	db.putChats({ { u"c9"_s, Text(u"only"_s) } }, true);
	QCOMPARE(all(), (QStringList{ u"only"_s }));
	db.putChats({}, true);
	QVERIFY(all().isEmpty());
}

void TestStorage::messagesPaging() {
	auto db = Database(_dir.path() + u"/messages/cache.db"_s);
	auto rows = QVector<Database::MessageRow>();
	for (auto seq = 1; seq <= 10; ++seq) {
		rows.push_back({ u"c1"_s, seq, Text(QString::number(seq)) });
	}
	rows.push_back({ u"c1"_s, 0, Text(u"local, never stored"_s) });
	rows.push_back({ u"c2"_s, 3, Text(u"other chat"_s) });
	db.putMessages(rows);
	const auto page = [&](int limit, qint64 before) {
		return Texts(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.messages(u"c1"_s, limit, before, guard, std::move(done));
		}));
	};
	const auto after = [&](int limit, qint64 seq) {
		return Texts(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.messagesAfter(u"c1"_s, limit, seq, guard, std::move(done));
		}));
	};
	// The newest `limit`, oldest first.
	QCOMPARE(page(3, 0), (QStringList{ u"8"_s, u"9"_s, u"10"_s }));
	QCOMPARE(page(3, 8), (QStringList{ u"5"_s, u"6"_s, u"7"_s }));
	QCOMPARE(page(50, 3), (QStringList{ u"1"_s, u"2"_s }));
	QCOMPARE(page(50, 0).size(), 10);
	QCOMPARE(after(2, 7), (QStringList{ u"8"_s, u"9"_s }));
	QVERIFY(after(5, 10).isEmpty());
	// Upsert by (chat, seq).
	db.putMessages({ { u"c1"_s, 10, Text(u"ten, edited"_s) } });
	QCOMPARE(page(1, 0), (QStringList{ u"ten, edited"_s }));
	db.deleteMessage(u"c1"_s, 9);
	QCOMPARE(page(2, 0), (QStringList{ u"8"_s, u"ten, edited"_s }));
	db.clearMessages(u"c1"_s, 8); // seq < 8
	QCOMPARE(page(50, 0), (QStringList{ u"8"_s, u"ten, edited"_s }));
	db.clearMessages(u"c1"_s);
	QVERIFY(page(50, 0).isEmpty());
	QCOMPARE(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
		db.messages(u"c2"_s, 10, 0, guard, std::move(done));
	}).size(), 1);
}

// The run of cached messages is dropped when it cannot reach messages that
// begin at a given seq; a run that touches or overlaps them stays.
void TestStorage::detachedMessagesGo() {
	auto db = Database(_dir.path() + u"/detached/cache.db"_s);
	auto rows = QVector<Database::MessageRow>();
	for (auto seq = 1; seq <= 10; ++seq) {
		rows.push_back({ u"c1"_s, seq, Text(QString::number(seq)) });
	}
	rows.push_back({ u"c2"_s, 3, Text(u"other chat"_s) });
	db.putMessages(rows);
	const auto all = [&](const QString &chat) {
		return Texts(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.messages(chat, 100, 0, guard, std::move(done));
		}));
	};
	db.dropDetachedMessages(u"c1"_s, 11); // right after the newest: one run
	QCOMPARE(all(u"c1"_s).size(), 10);
	db.dropDetachedMessages(u"c1"_s, 5); // overlapping
	QCOMPARE(all(u"c1"_s).size(), 10);
	db.dropDetachedMessages(u"c1"_s, 12); // seq 11 is missing: a hole
	QVERIFY(all(u"c1"_s).isEmpty());
	QCOMPARE(all(u"c2"_s), (QStringList{ u"other chat"_s }));
	// Nothing cached: nothing to do.
	db.dropDetachedMessages(u"c3"_s, 100);
	QVERIFY(all(u"c3"_s).isEmpty());
}

void TestStorage::users() {
	auto db = Database(_dir.path() + u"/users/cache.db"_s);
	db.putUsers({ { u"u1"_s, Text(u"sara"_s) }, { u"u2"_s, Text(u"ali"_s) } });
	const auto found = Read<QHash<QString, QJsonObject>>([&](QObject *guard, Fn<void(QHash<QString, QJsonObject>)> done) {
		db.users({ u"u2"_s, u"u3"_s, u"u1"_s }, guard, std::move(done));
	});
	QCOMPARE(found.size(), 2);
	QCOMPARE(found.value(u"u1"_s).value(u"text"_s).toString(), u"sara"_s);
	QVERIFY(!found.contains(u"u3"_s));
}

void TestStorage::outboxKeepsOrder() {
	auto db = Database(_dir.path() + u"/outbox/cache.db"_s);
	const auto queue = [&] {
		return Texts(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.outbox(guard, std::move(done));
		}));
	};
	db.putOutbox({ u"b"_s, u"c1"_s, 200, Text(u"second"_s) });
	db.putOutbox({ u"a"_s, u"c1"_s, 100, Text(u"first"_s) });
	db.putOutbox({ u"c"_s, u"c2"_s, 200, Text(u"third (same time, later insert)"_s) });
	QCOMPARE(queue(), (QStringList{ u"first"_s, u"second"_s, u"third (same time, later insert)"_s }));
	// Rewriting an item (attempts, upload id) keeps its place.
	db.putOutbox({ u"b"_s, u"c1"_s, 200, Text(u"second, retried"_s) });
	QCOMPARE(queue(), (QStringList{ u"first"_s, u"second, retried"_s, u"third (same time, later insert)"_s }));
	db.deleteOutbox(u"a"_s);
	QCOMPARE(queue(), (QStringList{ u"second, retried"_s, u"third (same time, later insert)"_s }));
}

void TestStorage::drafts() {
	auto db = Database(_dir.path() + u"/drafts/cache.db"_s);
	db.putDraft(u"c1"_s, Text(u"hello"_s));
	db.putDraft(u"c1"_s, Text(u"hello again"_s));
	db.putDraft(u"c2"_s, Text(u"other"_s));
	db.deleteDraft(u"c2"_s);
	const auto drafts = Read<QHash<QString, QJsonObject>>([&](QObject *guard, Fn<void(QHash<QString, QJsonObject>)> done) {
		db.drafts(guard, std::move(done));
	});
	QCOMPARE(drafts.size(), 1);
	QCOMPARE(drafts.value(u"c1"_s).value(u"text"_s).toString(), u"hello again"_s);
}

void TestStorage::callbacksRespectGuardAndThread() {
	auto db = Database(_dir.path() + u"/guard/cache.db"_s);
	db.setMeta(u"k"_s, 1);
	auto called = 0;
	auto thread = static_cast<QThread*>(nullptr);
	auto guard = std::make_unique<QObject>();
	db.meta(u"k"_s, guard.get(), [&](QJsonValue) {
		++called;
		thread = QThread::currentThread();
	});
	QTRY_COMPARE(called, 1);
	QCOMPARE(thread, QThread::currentThread()); // delivered on the owner's thread
	// A dead guard drops the callback.
	db.meta(u"k"_s, guard.get(), [&](QJsonValue) { ++called; });
	guard = nullptr;
	auto flushed = false;
	db.flush(base::Guard(), [&] { flushed = true; });
	QTRY_VERIFY(flushed);
	QTest::qWait(20);
	QCOMPARE(called, 1);
	// Reads see every write issued before them, in order.
	for (auto i = 0; i != 200; ++i) {
		db.setMeta(u"counter"_s, i);
	}
	auto last = -1;
	auto owner = QObject();
	db.meta(u"counter"_s, &owner, [&](QJsonValue value) { last = value.toInt(); });
	QTRY_COMPARE(last, 199);
	db.flushSync();

	// What a callback holds dies on the owner's thread, never on the worker
	// (even when the worker's task is the last to let go of it).
	struct Holder {
		QVector<QThread*> *threads = nullptr;
		~Holder() {
			threads->push_back(QThread::currentThread());
		}
	};
	auto threads = QVector<QThread*>();
	auto answered = 0;
	for (auto i = 0; i != 20; ++i) {
		auto holder = std::make_shared<Holder>();
		holder->threads = &threads;
		db.meta(u"k"_s, &owner, [&answered, holder](QJsonValue) { ++answered; });
		db.chats(&owner, [holder](QVector<QJsonObject>) {});
		db.flush(&owner, [holder] {});
		auto dropped = std::make_shared<Holder>();
		dropped->threads = &threads;
		db.meta(u"k"_s, base::Guard(), nullptr);
		db.users({ u"u1"_s }, &owner, [dropped](QHash<QString, QJsonObject>) {});
	}
	QTRY_COMPARE(threads.size(), 40);
	QCOMPARE(answered, 20);
	for (const auto thread : threads) {
		QCOMPARE(thread, QThread::currentThread());
	}
}

void TestStorage::persistsAcrossReopenAndWipes() {
	const auto path = _dir.path() + u"/persist/cache.db"_s;
	{
		auto db = Database(path);
		db.setMeta(u"pts"_s, 42);
		db.putChats({ { u"c1"_s, Text(u"one"_s) } });
		db.putMessages({ { u"c1"_s, 5, Text(u"five"_s) } });
		// No flush: the destructor runs what was queued.
	}
	QVERIFY(QFileInfo::exists(path));
	{
		auto db = Database(path);
		QCOMPARE(Read<QJsonValue>([&](QObject *guard, Fn<void(QJsonValue)> done) {
			db.meta(u"pts"_s, guard, std::move(done));
		}).toInt(), 42);
		QCOMPARE(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.messages(u"c1"_s, 10, 0, guard, std::move(done));
		}).size(), 1);
		db.clearAll();
		QVERIFY(Read<QVector<QJsonObject>>([&](QObject *guard, Fn<void(QVector<QJsonObject>)> done) {
			db.chats(guard, std::move(done));
		}).isEmpty());
		QVERIFY(Read<QJsonValue>([&](QObject *guard, Fn<void(QJsonValue)> done) {
			db.meta(u"pts"_s, guard, std::move(done));
		}).isUndefined());
	}
	QVERIFY(Database::Wipe(path));
	QVERIFY(!QFileInfo::exists(path));
	QVERIFY(!QFileInfo::exists(path + u"-wal"_s));
	QVERIFY(Database::Wipe(path)); // nothing left: still fine
}

void TestStorage::schemaVersionAndNewerFile() {
	const auto path = _dir.path() + u"/schema/cache.db"_s;
	const auto version = [&] {
		auto result = -1;
		{
			auto raw = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"inspect"_s);
			raw.setDatabaseName(path);
			if (raw.open()) {
				auto query = QSqlQuery(raw);
				if (query.exec(u"PRAGMA user_version"_s) && query.next()) {
					result = query.value(0).toInt();
				}
			}
			raw.close();
		}
		QSqlDatabase::removeDatabase(u"inspect"_s);
		return result;
	};
	{
		auto db = Database(path);
		db.setMeta(u"pts"_s, 9);
		db.flushSync();
	}
	QVERIFY(Database::CurrentVersion() >= 1);
	QCOMPARE(version(), Database::CurrentVersion());
	// A file written by a newer build: the cache is dropped and recreated.
	{
		auto raw = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"bump"_s);
		raw.setDatabaseName(path);
		QVERIFY(raw.open());
		QVERIFY(QSqlQuery(raw).exec(u"PRAGMA user_version = 999"_s));
		raw.close();
	}
	QSqlDatabase::removeDatabase(u"bump"_s);
	QCOMPARE(version(), 999);
	{
		auto db = Database(path);
		auto failures = 0;
		connect(&db, &Database::failed, this, [&] { ++failures; });
		const auto pts = Read<QJsonValue>([&](QObject *guard, Fn<void(QJsonValue)> done) {
			db.meta(u"pts"_s, guard, std::move(done));
		});
		QVERIFY(pts.isUndefined());
		db.setMeta(u"pts"_s, 10);
		QCOMPARE(Read<QJsonValue>([&](QObject *guard, Fn<void(QJsonValue)> done) {
			db.meta(u"pts"_s, guard, std::move(done));
		}).toInt(), 10);
		QCOMPARE(failures, 0);
	}
	QCOMPARE(version(), Database::CurrentVersion());
}

QTEST_GUILESS_MAIN(TestStorage)

#include "test_storage.moc"
