// lib/base helpers.
#include "base/backoff.h"
#include "base/bytes.h"
#include "base/expected.h"
#include "base/platform_file.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_guard.h"

#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

class TestBase final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void bytesHexAndBase64();
	void wipeAndCompare();
	void guardsDropCallbacks();
	void timerOnceEachCancel();
	void callDelayedRespectsGuard();
	void backoffGrowsAndResets();
	void randomAndUuid();
	void unixtimeShift();
	void expectedHoldsEither();
	void atomicFiles();

};

void TestBase::bytesHexAndBase64() {
	using namespace base::bytes;
	QCOMPARE(toHex(QByteArray::fromHex("00ff10")), u"00ff10"_s);
	QCOMPARE(fromHex(u"00FF10"_s), QByteArray::fromHex("00ff10"));
	QVERIFY(fromHex(u"abc"_s).isNull());  // odd length
	QVERIFY(fromHex(u"zz"_s).isNull());   // not hex
	QVERIFY(!fromHex(QString()).isNull());
	QCOMPARE(toBase64(QByteArray("\x09\x08", 2)), u"CQg="_s);
	QCOMPARE(toBase64Url(QByteArray::fromHex("fbff")), u"-_8"_s);
	QCOMPARE(fromBase64(u"-_8"_s), QByteArray::fromHex("fbff"));     // url-safe, unpadded
	QCOMPARE(fromBase64(u"+/8="_s), QByteArray::fromHex("fbff"));    // standard, padded
	QCOMPARE(fromBase64(u" C Q\ng= "_s), QByteArray("\x09\x08", 2)); // whitespace ignored
	QCOMPARE(concat({ QByteArray("ab"), QByteArray(), QByteArray("c") }), QByteArray("abc"));
}

void TestBase::wipeAndCompare() {
	auto secret = QByteArray("secret");
	base::bytes::wipe(secret);
	QVERIFY(secret.isEmpty());
	QVERIFY(base::bytes::constantTimeEquals(QByteArray("abc"), QByteArray("abc")));
	QVERIFY(!base::bytes::constantTimeEquals(QByteArray("abc"), QByteArray("abd")));
	QVERIFY(!base::bytes::constantTimeEquals(QByteArray("abc"), QByteArray("ab")));
}

void TestBase::guardsDropCallbacks() {
	auto calls = 0;
	auto object = std::make_unique<QObject>();
	auto guarded = base::guard(object.get(), [&](int add) { calls += add; });
	guarded(2);
	QCOMPARE(calls, 2);
	const auto token = base::Guard(object.get());
	QVERIFY(token.alive() && token.bound());
	auto byToken = base::guard(token, [&] { ++calls; });
	byToken();
	QCOMPARE(calls, 3);
	object = nullptr;
	guarded(5);
	byToken();
	QCOMPARE(calls, 3);
	QVERIFY(!token.alive());
	QVERIFY(base::Guard().alive()); // unbound: always alive
	QVERIFY(!base::Guard().bound());
	QVERIFY(base::Guard(nullptr).alive());

	auto flag = std::shared_ptr<const bool>();
	{
		const auto alive = base::AliveFlag();
		flag = alive.make();
		QVERIFY(*flag);
	}
	QVERIFY(!*flag);
}

void TestBase::timerOnceEachCancel() {
	auto fired = 0;
	auto timer = base::Timer([&] { ++fired; });
	QVERIFY(!timer.isActive());
	QCOMPARE(timer.remainingTime(), -1);
	timer.callOnce(10);
	QVERIFY(timer.isActive());
	QTRY_COMPARE(fired, 1);
	QVERIFY(!timer.isActive());
	timer.callEach(5);
	QTRY_VERIFY(fired >= 4);
	timer.cancel();
	const auto after = fired;
	QTest::qWait(30);
	QCOMPARE(fired, after);
	// Restarting replaces the pending shot.
	timer.callOnce(1000);
	timer.callOnce(5);
	QTRY_COMPARE(fired, after + 1);
}

void TestBase::callDelayedRespectsGuard() {
	auto fired = 0;
	auto object = std::make_unique<QObject>();
	base::call_delayed(5, object.get(), [&] { ++fired; });
	QTRY_COMPARE(fired, 1);
	base::call_delayed(5, object.get(), [&] { ++fired; });
	object = nullptr;
	QTest::qWait(30);
	QCOMPARE(fired, 1);
}

void TestBase::backoffGrowsAndResets() {
	auto backoff = base::ExponentialBackoff();
	const auto expected = { 0.5, 1., 2., 4., 8., 16., 30., 30., 30., 30. };
	for (const auto value : expected) {
		QCOMPARE(backoff.nextWithoutJitter(), value);
	}
	backoff.reset();
	QCOMPARE(backoff.nextWithoutJitter(), 0.5);
	backoff.reset();
	for (auto i = 0; i != 200; ++i) {
		auto jittered = base::ExponentialBackoff();
		const auto first = jittered.next();
		QVERIFY(first >= 0.5 * 0.7 - 1e-9 && first <= 0.5 * 1.3 + 1e-9);
	}
}

void TestBase::randomAndUuid() {
	QCOMPARE(base::RandomBytes(32).size(), 32);
	QVERIFY(base::RandomBytes(32) != base::RandomBytes(32));
	const auto uuid = base::RandomUuid();
	static const auto kUuid = QRegularExpression(u"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"_s);
	QVERIFY2(kUuid.match(uuid).hasMatch(), qPrintable(uuid));
	QVERIFY(base::RandomUuid() != uuid);
	for (auto i = 0; i != 100; ++i) {
		const auto unit = base::RandomUnit();
		QVERIFY(unit >= 0. && unit < 1.);
		QVERIFY(base::RandomBounded(7) < 7);
	}
	QCOMPARE(base::RandomBounded(0), 0U);
}

void TestBase::unixtimeShift() {
	const auto before = base::unixtime::nowMs();
	base::unixtime::setTestShiftMs(3600 * 1000);
	QVERIFY(base::unixtime::nowMs() - before >= 3600 * 1000);
	QVERIFY(base::unixtime::now() - before / 1000 >= 3599);
	base::unixtime::setTestShiftMs(0);
	QVERIFY(base::unixtime::nowMs() - before < 5000);
}

void TestBase::expectedHoldsEither() {
	const auto good = base::Expected<int, QString>(5);
	QVERIFY(good);
	QCOMPARE(*good, 5);
	const auto bad = base::Expected<int, QString>(base::make_unexpected(u"nope"_s));
	QVERIFY(!bad);
	QCOMPARE(bad.error(), u"nope"_s);
}

void TestBase::atomicFiles() {
	auto dir = QTemporaryDir();
	const auto path = dir.path() + u"/nested/dir/file.bin"_s;
	QVERIFY(!base::platform::ReadFile(path).has_value());
	QVERIFY(base::platform::WriteFileAtomic(path, QByteArray("one"), true));
	QCOMPARE(base::platform::ReadFile(path).value(), QByteArray("one"));
	QVERIFY(base::platform::WriteFileAtomic(path, QByteArray(), true));
	QVERIFY(base::platform::ReadFile(path).has_value());
	QVERIFY(!base::platform::ReadFile(path)->isNull());
#ifndef Q_OS_WIN
	QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther), QFileDevice::Permissions());
#endif // !Q_OS_WIN
	QVERIFY(base::platform::RemoveRecursively(dir.path() + u"/nested"_s));
	QVERIFY(!QFileInfo::exists(path));
	QVERIFY(!base::platform::SystemDescription().isEmpty());
	QVERIFY(base::platform::DeviceName().size() <= 64);
}

QTEST_GUILESS_MAIN(TestBase)

#include "test_base.moc"
