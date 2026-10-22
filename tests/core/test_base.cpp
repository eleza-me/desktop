// lib/base helpers.
#include "base/backoff.h"
#include "base/bytes.h"
#include "base/expected.h"
#include "base/platform_file.h"
#include "base/random.h"
#include "base/text_direction.h"
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
	void safeIntegers();
	void fileNamesFromStrangers();
	void programsAreNotOpened();
	void privateTemp();
	void firstStrongDirection();

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
#ifndef Q_OS_WIN
	// A link to a directory: the link goes, the directory and its files stay.
	const auto target = dir.path() + u"/target"_s;
	const auto inside = target + u"/keep.bin"_s;
	QVERIFY(base::platform::WriteFileAtomic(inside, QByteArray("keep"), true));
	const auto link = dir.path() + u"/link"_s;
	QVERIFY(QFile::link(target, link));
	QVERIFY(base::platform::RemoveRecursively(link));
	QVERIFY(!QFileInfo(link).isSymLink());
	QCOMPARE(base::platform::ReadFile(inside).value(), QByteArray("keep"));
	// A dangling link is removed too.
	QVERIFY(QFile::link(dir.path() + u"/nowhere"_s, link));
	QVERIFY(base::platform::RemoveRecursively(link));
	QVERIFY(!QFileInfo(link).isSymLink());
#endif // !Q_OS_WIN
	QVERIFY(!base::platform::SystemDescription().isEmpty());
	QVERIFY(base::platform::DeviceName().size() <= 64);
}

void TestBase::safeIntegers() {
	// A JSON number that does not fit: saturated, never undefined behaviour.
	QCOMPARE(base::SafeInt64(1e30), std::numeric_limits<qint64>::max());
	QCOMPARE(base::SafeInt64(-1e30), std::numeric_limits<qint64>::min());
	QCOMPARE(base::SafeInt64(9223372036854775808.), std::numeric_limits<qint64>::max());
	QCOMPARE(base::SafeInt64(std::numeric_limits<double>::quiet_NaN()), qint64(0));
	QCOMPARE(base::SafeInt64(std::numeric_limits<double>::infinity()), std::numeric_limits<qint64>::max());
	QCOMPARE(base::SafeInt64(-12.9), qint64(-12));
	QCOMPARE(base::SafeInt(4294967296.), std::numeric_limits<int>::max());
	QCOMPARE(base::SafeInt(-4294967296.), std::numeric_limits<int>::min());
	QCOMPARE(base::SafeInt(qint64(1) << 40), std::numeric_limits<int>::max());
	QCOMPARE(base::SafeInt(-(qint64(1) << 40)), std::numeric_limits<int>::min());
	QCOMPARE(base::SafeInt(640.), 640);
}

void TestBase::fileNamesFromStrangers() {
	using base::platform::SafeFileName;
	const auto rlo = QChar(0x202E);
	QCOMPARE(SafeFileName(u"report.pdf"_s), u"report.pdf"_s);
	QCOMPARE(SafeFileName(u"گزارش نهایی.pdf"_s), u"گزارش نهایی.pdf"_s);
	// No way out of the directory.
	QCOMPARE(SafeFileName(u"..\\..\\Windows\\evil.dll"_s), u"_.._Windows_evil.dll"_s);
	QCOMPARE(SafeFileName(u"../../etc/cron.d/evil"_s), u"_.._etc_cron.d_evil"_s);
	QCOMPARE(SafeFileName(u"/etc/passwd"_s), u"_etc_passwd"_s);
	QCOMPARE(SafeFileName(u".."_s), u"file"_s);
	QCOMPARE(SafeFileName(u"...."_s, u"photo.jpg"_s), u"photo.jpg"_s);
	QCOMPARE(SafeFileName(QString()), u"file"_s);
	QCOMPARE(SafeFileName(u" . . "_s), u"file"_s);
	// Hidden files and what Windows silently drops from the end.
	QCOMPARE(SafeFileName(u".bashrc"_s), u"bashrc"_s);
	QCOMPARE(SafeFileName(u"evil.exe."_s), u"evil.exe"_s);
	QCOMPARE(SafeFileName(u"evil.exe . . "_s), u"evil.exe"_s);
	QCOMPARE(SafeFileName(u"evil.exe::$DATA"_s), u"evil.exe__$DATA"_s);
	// The right-to-left override that shows "photo<RLO>gnp.exe" as "photoexe.png".
	QCOMPARE(SafeFileName(u"photo"_s + rlo + u"gnp.exe"_s), u"photognp.exe"_s);
	QCOMPARE(SafeFileName(QString(QChar(0x2066)) + u"a"_s + QChar(0x200B) + u"b"_s + QChar(0xFEFF) + QChar(0x2069)), u"ab"_s);
	// Control characters and line ends.
	QCOMPARE(SafeFileName(u"a\nb\tc"_s + QChar(0) + u"d"_s + QChar(0x7F) + u".txt"_s), u"a_b_c_d_.txt"_s);
	// Device names, with or without an extension.
	QCOMPARE(SafeFileName(u"CON"_s), u"_CON"_s);
	QCOMPARE(SafeFileName(u"nul.txt"_s), u"_nul.txt"_s);
	QCOMPARE(SafeFileName(u"Com1.tar.gz"_s), u"_Com1.tar.gz"_s);
	QCOMPARE(SafeFileName(u"LPT9 .x"_s), u"_LPT9 .x"_s);
	QCOMPARE(SafeFileName(u"console.log"_s), u"console.log"_s);
	// Long names are cut; the result never needs another pass.
	const auto longName = SafeFileName(QString(5000, u'n') + u".txt"_s);
	QCOMPARE(longName.size(), 180);
	for (const auto &name : { u"CON."_s + QString(300, u'x'), u" . a . "_s, QString(200, u'.') + u"x"_s, QString(179, u'a') + u". b"_s }) {
		const auto once = SafeFileName(name);
		QCOMPARE(SafeFileName(once), once);
		QVERIFY(once.size() <= 181);
	}
}

void TestBase::programsAreNotOpened() {
	using base::platform::OpensAsProgram;
	for (const auto name : {
		"a.exe", "a.EXE", "a.bat", "a.cmd", "a.com", "a.scr", "a.msi", "a.ps1", "a.vbs", "a.js",
		"a.jar", "a.lnk", "a.url", "a.reg", "a.hta", "a.cpl", "a.pif", "a.msc", "a.wsf", "a.chm",
		"a.dll", "a.iso", "a.docm", "a.xlsm", "a.sh", "a.run", "a.desktop", "a.AppImage", "a.deb",
		"a.rpm", "a.py", "a.pl", "a.command", "a.app", "a.dmg", "a.pkg", "a.html", "a.svg",
		// The name as the file system reads it, not as it is spelled.
		"report.pdf.exe", "a.exe.", "a.exe ", "a.exe. . .", "a.exe::$DATA", "a.txt:evil.exe",
		"dir.with.dots/a.sh",
	}) {
		QVERIFY2(OpensAsProgram(QString::fromLatin1(name)), name);
	}
	// The stream name decides nothing: "a.txt:evil.exe" is a stream of a.txt,
	// but nothing good comes from opening such a path either.
	for (const auto name : {
		"a.pdf", "a.txt", "a.jpg", "a.png", "a.mp4", "a.mp3", "a.ogg", "a.zip", "a.docx", "a.exe.txt",
		"a", "exe", "dir.exe/a.txt",
	}) {
		QVERIFY2(!OpensAsProgram(QString::fromLatin1(name)), name);
	}
#ifndef Q_OS_WIN
	// No extension: a program when the file may be run.
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto tool = dir.filePath(u"tool"_s);
	{
		auto file = QFile(tool);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("#!/bin/sh\n");
	}
	QVERIFY(!OpensAsProgram(tool));
	QVERIFY(QFile::setPermissions(tool, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
	QVERIFY(OpensAsProgram(tool));
#endif // !Q_OS_WIN
}

void TestBase::privateTemp() {
	auto dir = QTemporaryDir();
	QVERIFY(dir.isValid());
	const auto root = dir.filePath(u"data/temp"_s);
	base::platform::SetPrivateTempRoot(root);
	const auto path = base::platform::PrivateTempDirectory();
	QCOMPARE(path, QDir(root).absolutePath());
	QVERIFY(QFileInfo(path).isDir());
#ifndef Q_OS_WIN
	// Ours alone, also when it existed with an open mode.
	const auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
	const auto everyone = QFileDevice::Permissions(0x7777);
	QCOMPARE(QFileInfo(path).permissions() & everyone, QFileDevice::Permissions(ownerOnly | QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser));
	const auto open = dir.filePath(u"open"_s);
	QVERIFY(QDir().mkpath(open));
	QVERIFY(QFile::setPermissions(open, QFileDevice::Permissions(0x7777)));
	base::platform::SetPrivateTempRoot(open);
	QCOMPARE(base::platform::PrivateTempDirectory(), open);
	QCOMPARE(QFileInfo(open).permissions() & QFileDevice::Permissions(0x0077), QFileDevice::Permissions());
	// A link somebody put where our directory should be is not followed.
	const auto target = dir.filePath(u"elsewhere"_s);
	const auto link = dir.filePath(u"link"_s);
	QVERIFY(QDir().mkpath(target));
	QVERIFY(QFile::link(target, link));
	base::platform::SetPrivateTempRoot(link);
	const auto fallback = base::platform::PrivateTempDirectory();
	QVERIFY(!fallback.isEmpty());
	QVERIFY(fallback != link);
	QVERIFY(QFileInfo(fallback).isDir() && !QFileInfo(fallback).isSymLink());
	QCOMPARE(QFileInfo(fallback).permissions() & QFileDevice::Permissions(0x0077), QFileDevice::Permissions());
	base::platform::SetPrivateTempRoot(root);
#endif // !Q_OS_WIN
	// Leftovers of an earlier run go, the directory stays.
	{
		auto file = QFile(QDir(path).filePath(u"voice.ogg"_s));
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("x");
	}
	QVERIFY(QDir(path).mkpath(u"eleza-paste/inner"_s));
	base::platform::ClearPrivateTemp();
	QVERIFY(QFileInfo(path).isDir());
	QVERIFY(QDir(path).isEmpty());
	base::platform::SetPrivateTempRoot(QString());
}

void TestBase::firstStrongDirection() {
	using base::FirstStrongIsRtl;
	QCOMPARE(FirstStrongIsRtl(u"abc"), std::optional<bool>(false));
	QCOMPARE(FirstStrongIsRtl(u"123 \u0633"), std::optional<bool>(true));
	QCOMPARE(FirstStrongIsRtl(u"123 ..."), std::optional<bool>());
	QCOMPARE(FirstStrongIsRtl(u""), std::optional<bool>());
	// LRI / RLI / FSI ... PDI are skipped, nested ones included.
	QCOMPARE(FirstStrongIsRtl(u"\u2066abc\u2069 \u0633"), std::optional<bool>(true));
	QCOMPARE(FirstStrongIsRtl(u"\u2067\u0633\u2069 abc"), std::optional<bool>(false));
	QCOMPARE(
		FirstStrongIsRtl(u"\u2068a\u2066b\u2069c\u2069\u0633"),
		std::optional<bool>(true));
	QCOMPARE(FirstStrongIsRtl(u"\u2066abc"), std::optional<bool>());
	QCOMPARE(FirstStrongIsRtl(u"\u2069abc"), std::optional<bool>(false));
}

QTEST_GUILESS_MAIN(TestBase)

#include "test_base.moc"
