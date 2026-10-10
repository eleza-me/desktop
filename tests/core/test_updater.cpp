// In-app updates: the signed manifest, version rules, the package unpacker
// against hostile archives, and the checker (check -> download -> verify ->
// unpack) against a local HTTP server: source and mirror fallback, resuming,
// replayed and tampered manifests, packages that are not what was signed.
#include "crypto/crypto_primitives.h"
#include "test_fakes.h"
#include "updater/updater_archive.h"
#include "updater/updater_checker.h"
#include "updater/updater_install.h"
#include "updater/updater_keys.h"
#include "updater/updater_manifest.h"

#include <QtCore/QDirIterator>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

#include <miniz.h>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif // !Q_OS_WIN

using namespace Updater;
using Request = Test::HttpServer::Request;
using Response = Test::HttpServer::Response;

namespace {

// ---- a zip writer that writes whatever it is told, lies included

struct ZipEntry {
	QByteArray name = QByteArray();
	QByteArray data = QByteArray();
	quint32 mode = 0100644; // Unix mode, in the upper half of the attributes
	bool deflate = false;
	quint16 flags = 0;
	int host = 3; // 3: Unix, 0: DOS
	quint32 dosAttributes = 0;
	qint64 declaredSize = -1; // >= 0: what the headers claim instead of the truth
};

void Put16(QByteArray &to, quint16 value) {
	to.append(char(value & 0xFF));
	to.append(char(value >> 8));
}

void Put32(QByteArray &to, quint32 value) {
	Put16(to, quint16(value & 0xFFFF));
	Put16(to, quint16(value >> 16));
}

[[nodiscard]] QByteArray Deflate(const QByteArray &data) {
	auto size = size_t(0);
	const auto memory = tdefl_compress_mem_to_heap(
		data.constData(),
		size_t(data.size()),
		&size,
		TDEFL_DEFAULT_MAX_PROBES);
	auto result = QByteArray(static_cast<const char*>(memory), qsizetype(size));
	mz_free(memory);
	return result;
}

[[nodiscard]] QByteArray MakeZip(const QVector<ZipEntry> &entries) {
	auto result = QByteArray();
	auto central = QByteArray();
	for (const auto &entry : entries) {
		const auto stored = entry.deflate ? Deflate(entry.data) : entry.data;
		const auto crc = quint32(mz_crc32(
			MZ_CRC32_INIT,
			reinterpret_cast<const unsigned char*>(entry.data.constData()),
			size_t(entry.data.size())));
		const auto size = (entry.declaredSize >= 0)
			? quint32(entry.declaredSize)
			: quint32(entry.data.size());
		const auto method = quint16(entry.deflate ? 8 : 0);
		const auto offset = quint32(result.size());

		Put32(result, 0x04034b50);
		Put16(result, 20);
		Put16(result, entry.flags);
		Put16(result, method);
		Put16(result, 0);
		Put16(result, 0x21);
		Put32(result, crc);
		Put32(result, quint32(stored.size()));
		Put32(result, size);
		Put16(result, quint16(entry.name.size()));
		Put16(result, 0);
		result.append(entry.name);
		result.append(stored);

		Put32(central, 0x02014b50);
		Put16(central, quint16((entry.host << 8) | 20));
		Put16(central, 20);
		Put16(central, entry.flags);
		Put16(central, method);
		Put16(central, 0);
		Put16(central, 0x21);
		Put32(central, crc);
		Put32(central, quint32(stored.size()));
		Put32(central, size);
		Put16(central, quint16(entry.name.size()));
		Put16(central, 0);
		Put16(central, 0);
		Put16(central, 0);
		Put16(central, 0);
		Put32(central, (entry.mode << 16) | entry.dosAttributes);
		Put32(central, offset);
		central.append(entry.name);
	}
	const auto centralOffset = quint32(result.size());
	result.append(central);
	Put32(result, 0x06054b50);
	Put16(result, 0);
	Put16(result, 0);
	Put16(result, quint16(entries.size()));
	Put16(result, quint16(entries.size()));
	Put32(result, quint32(central.size()));
	Put32(result, centralOffset);
	Put16(result, 0);
	return result;
}

[[nodiscard]] QVector<ZipEntry> GoodPackage(const QByteArray &marker = "new") {
	return {
		{ .name = "Eleza", .data = "#!/bin/sh\necho " + marker + "\n", .mode = 0100755 },
		{ .name = "eleza-files.list", .data = "Eleza\neleza-files.list\nlib/data.bin\n" },
		{ .name = "lib/", .data = "", .mode = 0040755 },
		{ .name = "lib/data.bin", .data = QByteArray(40000, 'x') + marker, .deflate = true },
	};
}

void WriteBytes(const QString &path, const QByteArray &bytes) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	QVERIFY(file.open(QIODevice::WriteOnly));
	file.write(bytes);
}

[[nodiscard]] QByteArray ReadBytes(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Every file and directory under a directory, relative.
[[nodiscard]] QStringList Listing(const QString &root) {
	auto result = QStringList();
	auto i = QDirIterator(
		root,
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		QDirIterator::Subdirectories);
	while (i.hasNext()) {
		result.push_back(QDir(root).relativeFilePath(i.next()));
	}
	result.sort();
	return result;
}

struct Keys {
	QByteArray privateKey = Crypto::Ed25519GeneratePrivate();
	QByteArray publicKey = Crypto::Ed25519PublicFromPrivate(privateKey);

	[[nodiscard]] QByteArray sign(const QByteArray &bytes) const {
		return Crypto::Ed25519Sign(privateKey, bytes).toBase64() + '\n';
	}
};

struct ManifestArgs {
	QString version = u"0.2.0"_s;
	QString published = u"2026-10-11T10:00:00Z"_s;
	QString minSupported = QString();
	QByteArray package = QByteArray();
	QStringList urls = QStringList();
	QString platform = u"linux-x86_64"_s;
};

[[nodiscard]] QByteArray ManifestJson(const ManifestArgs &args) {
	auto root = QJsonObject{
		{ u"format"_s, 1 },
		{ u"channel"_s, u"stable"_s },
		{ u"version"_s, args.version },
		{ u"published_at"_s, args.published },
		{ u"notes"_s, QJsonObject{
			{ u"fa"_s, u"تازه‌ها"_s },
			{ u"en"_s, u"What is new"_s },
		} },
	};
	if (!args.minSupported.isEmpty()) {
		root.insert(u"min_supported"_s, args.minSupported);
	}
	if (!args.urls.isEmpty()) {
		root.insert(u"packages"_s, QJsonObject{
			{ args.platform, QJsonObject{
				{ u"size"_s, double(args.package.size()) },
				{ u"sha256"_s, QString::fromLatin1(
					Crypto::Sha256(args.package).toHex()) },
				{ u"urls"_s, QJsonArray::fromStringList(args.urls) },
			} },
		});
	}
	return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// A server with routes, keys and a checker configuration for one test.
struct Fixture {
	QTemporaryDir temp;
	Test::HttpServer server;
	Keys keys;
	QHash<QByteArray, Fn<Response(const Request &)>> routes;
	bool autoDownload = true;

	Fixture() {
		server.handler = [this](const Request &request) {
			const auto i = routes.constFind(request.path);
			return (i != routes.cend())
				? (*i)(request)
				: Response::Problem(404, "not_found");
		};
	}

	[[nodiscard]] QString url(const char *path) const {
		return server.base() + QLatin1StringView(path);
	}
	[[nodiscard]] QString updatesDir() const {
		return temp.filePath(u"work/updates"_s);
	}

	void serve(const QByteArray &path, const QByteArray &bytes) {
		routes.insert(path, [bytes](const Request &) {
			return Response::Bytes(bytes);
		});
	}
	// Honours "Range: bytes=N-".
	void serveRanged(const QByteArray &path, const QByteArray &bytes) {
		routes.insert(path, [bytes](const Request &request) {
			const auto range = request.header("range");
			if (!range.startsWith("bytes=")) {
				return Response::Bytes(bytes);
			}
			const auto from = range.mid(6, range.indexOf('-') - 6).toLongLong();
			if (from >= bytes.size()) {
				auto response = Response();
				response.status = 416;
				return response;
			}
			auto response = Response::Bytes(bytes.mid(from));
			response.status = 206;
			response.headers.push_back({
				"Content-Range",
				"bytes " + QByteArray::number(from) + '-'
					+ QByteArray::number(bytes.size() - 1) + '/'
					+ QByteArray::number(bytes.size()),
			});
			return response;
		});
	}
	// The manifest and its signature at <prefix>/update.json(.sig).
	void publish(const QByteArray &manifest, const QByteArray &prefix = "") {
		serve(prefix + "/update.json", manifest);
		serve(prefix + "/update.json.sig", keys.sign(manifest));
	}

	[[nodiscard]] Config config() {
		auto result = Config();
		result.currentVersion = u"0.1.0"_s;
		result.updatesDir = updatesDir();
		result.platform = u"linux-x86_64"_s;
		result.sources = QStringList{ url("/update.json") };
		result.publicKeys = { keys.publicKey };
		result.allowInsecureLoopback = true;
		result.selfUpdate = true;
		result.executable = u"Eleza"_s;
		result.autoDownload = [this] { return autoDownload; };
		// The tests ask for every check themselves.
		result.firstCheckDelayMs = 3600 * 1000;
		result.requestTimeoutMs = 5000;
		return result;
	}
};

[[nodiscard]] bool WaitForState(const Checker &checker, State state, int timeout = 10000) {
	return Test::WaitFor([&] { return checker.state() == state; }, timeout);
}

// Checking / Downloading / Preparing are over.
[[nodiscard]] bool WaitSettled(const Checker &checker, int timeout = 10000) {
	return Test::WaitFor([&] {
		const auto state = checker.state();
		return state != State::Idle
			&& state != State::Checking
			&& state != State::Downloading
			&& state != State::Preparing;
	}, timeout);
}

} // namespace

class TestUpdater final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	// ---- manifest
	void bakedKeys();
	void signatureGood();
	void signatureTamperedByte();
	void signatureWrongKey();
	void signatureTruncatedOrGarbage();
	void signatureSecondKey();
	void versionParse_data();
	void versionParse();
	void versionCompare();
	void manifestParses();
	void manifestRejects_data();
	void manifestRejects();
	void manifestUrlRules();
	void evaluateRules();

	// ---- archive
	void unpacks();
	void unpackRejects_data();
	void unpackRejects();
	void unpackBomb();
	void unpackLyingSize();
	void unpackLimits();
	void unpackNotAZip();

	// ---- checker
	void upToDate();
	void downloadsAndUnpacks();
	void readySurvivesRestart();
	void sourceFallbackWhenFirstIsDown();
	void sourceFallbackWhenFirstIsTampered();
	void tamperedEverywhere();
	void manifestTooLarge();
	void neverInstallsOlderOrSame();
	void replayedManifestRefused();
	void mirrorFallback();
	void mirrorWithWrongBytes();
	void resumesOnNextMirror();
	void resumeFromWrongBeginning();
	void resumesPartFromDisk();
	void mirrorSendsMoreThanDeclared();
	void wrongPackageEverywhere();
	void unsafePackageRefused();
	void automaticOff();
	void announcesOnlyWithoutSelfUpdate();
	void requiredDownloadsAnyway();
	void failedInstallNotRetried();
	void redirects();

	// ---- install
	void detectInstall();

};

// ---- manifest

void TestUpdater::bakedKeys() {
	const auto keys = BakedPublicKeys();
	QCOMPARE(keys.size(), qsizetype(std::size(kPublicKeys)));
	QVERIFY(!keys.isEmpty());
	for (const auto &key : keys) {
		QCOMPARE(key.size(), 32);
	}
	QCOMPARE(
		keys[0].toBase64(),
		QByteArray("1vxoEOaHgMooKRlqvM3Nd5ZyGG+QaTjZAj5FTXQ0WIc="));
	QVERIFY(!DecodePublicKey(u"not base64!"_s));
	QVERIFY(!DecodePublicKey(u"AAAA"_s));
	// The shipped sources are https and nothing else.
	for (const auto source : kManifestSources) {
		QVERIFY(IsAllowedUrl(QUrl(QString::fromLatin1(source)), false));
	}
}

void TestUpdater::signatureGood() {
	const auto keys = Keys();
	const auto manifest = ManifestJson({});
	QVERIFY(VerifySignature(manifest, keys.sign(manifest), { keys.publicKey }));
	// Without the line end, with spaces around.
	const auto bare = Crypto::Ed25519Sign(keys.privateKey, manifest).toBase64();
	QVERIFY(VerifySignature(manifest, bare, { keys.publicKey }));
	QVERIFY(VerifySignature(manifest, "  " + bare + "\r\n", { keys.publicKey }));
}

void TestUpdater::signatureTamperedByte() {
	const auto keys = Keys();
	const auto manifest = ManifestJson({});
	const auto signature = keys.sign(manifest);
	for (const auto position : { 0, int(manifest.size() / 2), int(manifest.size() - 1) }) {
		auto tampered = manifest;
		tampered[position] = char(tampered[position] ^ 1);
		QVERIFY(!VerifySignature(tampered, signature, { keys.publicKey }));
	}
	// One more byte, one byte less.
	QVERIFY(!VerifySignature(manifest + ' ', signature, { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest.chopped(1), signature, { keys.publicKey }));
	// A bit of the signature itself.
	auto raw = QByteArray::fromBase64(signature.trimmed());
	raw[10] = char(raw[10] ^ 0x40);
	QVERIFY(!VerifySignature(manifest, raw.toBase64(), { keys.publicKey }));
}

void TestUpdater::signatureWrongKey() {
	const auto keys = Keys();
	const auto other = Keys();
	const auto manifest = ManifestJson({});
	QVERIFY(!VerifySignature(manifest, other.sign(manifest), { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest, keys.sign(manifest), {}));
	QVERIFY(!VerifySignature(manifest, keys.sign(manifest), { QByteArray(32, '\0') }));
	QVERIFY(!VerifySignature(manifest, keys.sign(manifest), { QByteArray("short") }));
}

void TestUpdater::signatureTruncatedOrGarbage() {
	const auto keys = Keys();
	const auto manifest = ManifestJson({});
	const auto raw = Crypto::Ed25519Sign(keys.privateKey, manifest);
	QVERIFY(!VerifySignature(manifest, raw.left(63).toBase64(), { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest, raw.left(32).toBase64(), { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest, (raw + 'x').toBase64(), { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest, QByteArray(), { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest, "!!! not base64 !!!", { keys.publicKey }));
	// The raw bytes instead of their base64 text.
	QVERIFY(!VerifySignature(manifest, raw, { keys.publicKey }));
	QVERIFY(!VerifySignature(manifest, QByteArray(4096, 'A'), { keys.publicKey }));
	QVERIFY(!VerifySignature(QByteArray(), keys.sign(QByteArray()), { keys.publicKey }));
}

void TestUpdater::signatureSecondKey() {
	// Rotation: a build that trusts the old and the new key accepts both.
	const auto old = Keys();
	const auto next = Keys();
	const auto manifest = ManifestJson({});
	const auto trusted = QVector<QByteArray>{ old.publicKey, next.publicKey };
	QVERIFY(VerifySignature(manifest, old.sign(manifest), trusted));
	QVERIFY(VerifySignature(manifest, next.sign(manifest), trusted));
	QVERIFY(!VerifySignature(manifest, Keys().sign(manifest), trusted));
}

void TestUpdater::versionParse_data() {
	QTest::addColumn<QString>("text");
	QTest::addColumn<bool>("valid");

	QTest::newRow("plain") << u"0.2.0"_s << true;
	QTest::newRow("big") << u"12.345.6789"_s << true;
	QTest::newRow("two parts") << u"1.2"_s << false;
	QTest::newRow("four parts") << u"1.2.3.4"_s << false;
	QTest::newRow("empty") << u""_s << false;
	QTest::newRow("empty part") << u"1..3"_s << false;
	QTest::newRow("letters") << u"1.2.x"_s << false;
	QTest::newRow("suffix") << u"1.2.3-beta"_s << false;
	QTest::newRow("prefix") << u"v1.2.3"_s << false;
	QTest::newRow("sign") << u"1.+2.3"_s << false;
	QTest::newRow("negative") << u"1.-2.3"_s << false;
	QTest::newRow("space") << u" 1.2.3"_s << false;
	QTest::newRow("persian digits") << u"۱.۲.۳"_s << false;
	QTest::newRow("too long") << u"1.2.1234567"_s << false;
	QTest::newRow("path") << u"../1.2"_s << false;
}

void TestUpdater::versionParse() {
	QFETCH(QString, text);
	QFETCH(bool, valid);
	const auto parsed = Version::Parse(text);
	QCOMPARE(parsed.has_value(), valid);
	if (parsed) {
		QCOMPARE(parsed->toString(), text);
	}
}

void TestUpdater::versionCompare() {
	const auto v = [](const char *text) {
		return *Version::Parse(QString::fromLatin1(text));
	};
	QVERIFY(v("0.2.0") > v("0.1.9"));
	QVERIFY(v("0.10.0") > v("0.9.0")); // numbers, not text
	QVERIFY(v("1.0.0") > v("0.99.99"));
	QVERIFY(v("0.2.10") > v("0.2.9"));
	QVERIFY(v("0.2.0") == v("0.2.0"));
	QVERIFY(!(v("0.2.0") < v("0.2.0")));
	QVERIFY(v("0.2.0") < v("0.2.1"));
}

void TestUpdater::manifestParses() {
	const auto package = QByteArray("package bytes");
	const auto bytes = ManifestJson({
		.minSupported = u"0.1.5"_s,
		.package = package,
		.urls = { u"https://eleza.me/desktop/updates/a.zip"_s, u"https://github.com/eleza-me/desktop/releases/download/v0.2.0/a.zip"_s },
	});
	const auto parsed = ParseManifest(bytes, { u"linux-x86_64"_s, false });
	QVERIFY(parsed.ok());
	const auto &manifest = parsed.manifest;
	QCOMPARE(manifest.version.toString(), u"0.2.0"_s);
	QCOMPARE(manifest.minSupported.toString(), u"0.1.5"_s);
	QCOMPARE(manifest.publishedAt, QDateTime(QDate(2026, 10, 11), QTime(10, 0), Qt::UTC));
	QCOMPARE(manifest.notesEn, u"What is new"_s);
	QVERIFY(manifest.package.has_value());
	QCOMPARE(manifest.package->size, qint64(package.size()));
	QCOMPARE(manifest.package->sha256, Crypto::Sha256(package));
	QCOMPARE(manifest.package->urls.size(), 2);

	// Another platform: the manifest is fine, there is just nothing for it.
	const auto other = ParseManifest(bytes, { u"windows-x64"_s, false });
	QVERIFY(other.ok());
	QVERIFY(!other.manifest.package.has_value());
}

void TestUpdater::manifestRejects_data() {
	QTest::addColumn<QByteArray>("bytes");
	QTest::addColumn<int>("error");

	const auto good = QJsonDocument::fromJson(ManifestJson({
		.package = "p",
		.urls = { u"https://eleza.me/a.zip"_s },
	})).object();
	const auto with = [&](const QString &key, const QJsonValue &value) {
		auto copy = good;
		copy.insert(key, value);
		return QJsonDocument(copy).toJson();
	};
	const auto withPackage = [&](const QString &key, const QJsonValue &value) {
		auto copy = good;
		auto packages = copy.value(u"packages"_s).toObject();
		auto package = packages.value(u"linux-x86_64"_s).toObject();
		package.insert(key, value);
		packages.insert(u"linux-x86_64"_s, package);
		copy.insert(u"packages"_s, packages);
		return QJsonDocument(copy).toJson();
	};
	QTest::newRow("not json") << QByteArray("<html>") << int(ManifestError::Syntax);
	QTest::newRow("array") << QByteArray("[1]") << int(ManifestError::Syntax);
	QTest::newRow("format 2") << with(u"format"_s, 2) << int(ManifestError::Format);
	QTest::newRow("format text") << with(u"format"_s, u"1"_s) << int(ManifestError::Format);
	QTest::newRow("beta channel") << with(u"channel"_s, u"beta"_s) << int(ManifestError::Channel);
	QTest::newRow("bad version") << with(u"version"_s, u"0.2"_s) << int(ManifestError::Version);
	QTest::newRow("version number") << with(u"version"_s, 2) << int(ManifestError::Version);
	QTest::newRow("bad minimum") << with(u"min_supported"_s, u"x"_s) << int(ManifestError::Version);
	QTest::newRow("minimum above") << with(u"min_supported"_s, u"0.3.0"_s) << int(ManifestError::Version);
	QTest::newRow("no date") << with(u"published_at"_s, QJsonValue()) << int(ManifestError::Date);
	QTest::newRow("local date") << with(u"published_at"_s, u"2026-10-11T10:00:00"_s) << int(ManifestError::Date);
	QTest::newRow("offset date") << with(u"published_at"_s, u"2026-10-11T10:00:00+03:30"_s) << int(ManifestError::Date);
	QTest::newRow("size zero") << withPackage(u"size"_s, 0) << int(ManifestError::Package);
	QTest::newRow("size negative") << withPackage(u"size"_s, -5) << int(ManifestError::Package);
	QTest::newRow("size fraction") << withPackage(u"size"_s, 10.5) << int(ManifestError::Package);
	QTest::newRow("size huge") << withPackage(u"size"_s, 1e18) << int(ManifestError::Package);
	QTest::newRow("size text") << withPackage(u"size"_s, u"1"_s) << int(ManifestError::Package);
	QTest::newRow("short hash") << withPackage(u"sha256"_s, u"abcd"_s) << int(ManifestError::Package);
	QTest::newRow("hash not hex") << withPackage(u"sha256"_s, QString(64, u'z')) << int(ManifestError::Package);
	QTest::newRow("no urls") << withPackage(u"urls"_s, QJsonArray()) << int(ManifestError::Package);
	QTest::newRow("http url") << withPackage(u"urls"_s, QJsonArray{ u"http://eleza.me/a.zip"_s }) << int(ManifestError::Package);
	QTest::newRow("file url") << withPackage(u"urls"_s, QJsonArray{ u"file:///etc/passwd"_s }) << int(ManifestError::Package);
	QTest::newRow("relative url") << withPackage(u"urls"_s, QJsonArray{ u"a.zip"_s }) << int(ManifestError::Package);
	QTest::newRow("credentials") << withPackage(u"urls"_s, QJsonArray{ u"https://user:pass@eleza.me/a.zip"_s }) << int(ManifestError::Package);
	QTest::newRow("one bad of two") << withPackage(u"urls"_s, QJsonArray{ u"https://eleza.me/a.zip"_s, u"ftp://eleza.me/a.zip"_s }) << int(ManifestError::Package);
	QTest::newRow("too many urls") << withPackage(u"urls"_s, QJsonArray::fromStringList(QStringList(9, u"https://eleza.me/a.zip"_s))) << int(ManifestError::Package);
	QTest::newRow("package not object") << with(u"packages"_s, QJsonObject{ { u"linux-x86_64"_s, 5 } }) << int(ManifestError::Package);
}

void TestUpdater::manifestRejects() {
	QFETCH(QByteArray, bytes);
	QFETCH(int, error);
	const auto parsed = ParseManifest(bytes, { u"linux-x86_64"_s, false });
	QCOMPARE(int(parsed.error), error);
}

void TestUpdater::manifestUrlRules() {
	QVERIFY(IsAllowedUrl(QUrl(u"https://eleza.me/x"_s), false));
	QVERIFY(IsAllowedUrl(QUrl(u"HTTPS://objects.githubusercontent.com/x?y=1"_s), false));
	QVERIFY(!IsAllowedUrl(QUrl(u"http://eleza.me/x"_s), false));
	QVERIFY(!IsAllowedUrl(QUrl(u"http://127.0.0.1:8000/x"_s), false));
	QVERIFY(!IsAllowedUrl(QUrl(u"https://user@eleza.me/x"_s), false));
	QVERIFY(!IsAllowedUrl(QUrl(u"file:///x"_s), true));
	QVERIFY(!IsAllowedUrl(QUrl(u"//eleza.me/x"_s), true));
	QVERIFY(!IsAllowedUrl(QUrl(), true));
	// Test servers, with the debug tools only.
	QVERIFY(IsAllowedUrl(QUrl(u"http://127.0.0.1:8000/x"_s), true));
	QVERIFY(IsAllowedUrl(QUrl(u"http://localhost:8000/x"_s), true));
	QVERIFY(IsAllowedUrl(QUrl(u"http://[::1]:8000/x"_s), true));
	QVERIFY(!IsAllowedUrl(QUrl(u"http://192.168.1.5/x"_s), true));
	QVERIFY(!IsAllowedUrl(QUrl(u"http://127.0.0.1.example.com/x"_s), true));
}

void TestUpdater::evaluateRules() {
	const auto make = [](const char *version, const char *minimum, const char *published) {
		auto manifest = Manifest();
		manifest.version = *Version::Parse(QString::fromLatin1(version));
		manifest.minSupported = *Version::Parse(QString::fromLatin1(minimum));
		manifest.publishedAt = QDateTime::fromString(
			QString::fromLatin1(published),
			Qt::ISODate);
		manifest.package = Package();
		return manifest;
	};
	const auto current = *Version::Parse(u"0.2.0"_s);
	const auto never = QDateTime();
	const auto t1 = "2026-10-11T10:00:00Z";
	const auto t2 = "2026-10-12T10:00:00Z";
	const auto at = [](const char *text) {
		return QDateTime::fromString(QString::fromLatin1(text), Qt::ISODate);
	};

	QCOMPARE(Evaluate(make("0.3.0", "0.0.0", t1), current, never).verdict, Verdict::Available);
	// Never the same version again, never an older one.
	QCOMPARE(Evaluate(make("0.2.0", "0.0.0", t1), current, never).verdict, Verdict::UpToDate);
	QCOMPARE(Evaluate(make("0.1.9", "0.0.0", t2), current, never).verdict, Verdict::UpToDate);
	QCOMPARE(Evaluate(make("0.1.9", "0.0.0", t2), current, at(t1)).verdict, Verdict::UpToDate);
	// A manifest published before the last accepted one is a replay, whatever
	// it offers.
	QCOMPARE(Evaluate(make("0.3.0", "0.0.0", t1), current, at(t2)).verdict, Verdict::Stale);
	QCOMPARE(Evaluate(make("0.2.0", "0.0.0", t1), current, at(t2)).verdict, Verdict::Stale);
	// The same manifest again is fine.
	QCOMPARE(Evaluate(make("0.3.0", "0.0.0", t2), current, at(t2)).verdict, Verdict::Available);

	QVERIFY(!Evaluate(make("0.3.0", "0.2.0", t1), current, never).mandatory);
	QVERIFY(Evaluate(make("0.3.0", "0.2.1", t1), current, never).mandatory);
	QVERIFY(!Evaluate(make("0.2.0", "0.2.0", t1), current, never).mandatory);

	auto bare = make("0.3.0", "0.0.0", t1);
	bare.package = std::nullopt;
	QCOMPARE(Evaluate(bare, current, never).verdict, Verdict::NoPackage);
}

// ---- archive

void TestUpdater::unpacks() {
	auto temp = QTemporaryDir();
	const auto archive = temp.filePath(u"package.zip"_s);
	WriteBytes(archive, MakeZip(GoodPackage()));
	const auto target = temp.filePath(u"out/0.2.0"_s);
	const auto result = Unpack(archive, target);
	QVERIFY2(result.ok(), qPrintable(u"%1 %2"_s.arg(int(result.error)).arg(result.entry)));
	QCOMPARE(result.files, 3);
	QCOMPARE(
		Listing(target),
		QStringList({ u"Eleza"_s, u"eleza-files.list"_s, u"lib"_s, u"lib/data.bin"_s }));
	QCOMPARE(ReadBytes(target + u"/lib/data.bin"_s), QByteArray(40000, 'x') + "new");
	QVERIFY(!QFileInfo::exists(target + u".tmp"_s));
#ifndef Q_OS_WIN
	QVERIFY(QFileInfo(target + u"/Eleza"_s).isExecutable());
	QVERIFY(!QFileInfo(target + u"/lib/data.bin"_s).isExecutable());
	// Nothing but rwxr-xr-x / rw-r--r-- comes out, whatever the archive says.
	WriteBytes(archive, MakeZip({
		{ .name = "setuid", .data = "x", .mode = 0104777 },
		{ .name = "private", .data = "x", .mode = 0100600 },
	}));
	const auto modes = temp.filePath(u"modes"_s);
	QVERIFY(Unpack(archive, modes).ok());
	QCOMPARE(
		int(QFileInfo(modes + u"/setuid"_s).permissions()),
		int(QFile::Permissions(0x7755)));
	QCOMPARE(
		int(QFileInfo(modes + u"/private"_s).permissions()),
		int(QFile::Permissions(0x6644)));
#endif // !Q_OS_WIN

	// The target must be new.
	QCOMPARE(Unpack(archive, target).error, UnpackError::Write);
}

void TestUpdater::unpackRejects_data() {
	QTest::addColumn<QByteArray>("archive");
	QTest::addColumn<int>("error");

	const auto one = [](ZipEntry entry) {
		auto entries = GoodPackage();
		entries.push_back(std::move(entry));
		return MakeZip(entries);
	};
	const auto unsafe = int(UnpackError::UnsafeName);
	QTest::newRow("traversal") << one({ .name = "../evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("traversal deep") << one({ .name = "lib/../../evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("traversal dir") << one({ .name = "../evil/", .data = "", .mode = 0040755 }) << unsafe;
	QTest::newRow("absolute") << one({ .name = "/tmp/evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("backslash") << one({ .name = "..\\evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("backslash absolute") << one({ .name = "\\evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("drive") << one({ .name = "C:/evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("drive relative") << one({ .name = "C:evil.txt", .data = "x" }) << unsafe;
	QTest::newRow("stream") << one({ .name = "Eleza:evil", .data = "x" }) << unsafe;
	QTest::newRow("device") << one({ .name = "NUL", .data = "x" }) << unsafe;
	QTest::newRow("device in dir") << one({ .name = "lib/com1.txt", .data = "x" }) << unsafe;
	QTest::newRow("trailing dot") << one({ .name = "evil.exe.", .data = "x" }) << unsafe;
	QTest::newRow("control") << one({ .name = "a\nb", .data = "x" }) << unsafe;
	QTest::newRow("zero in name") << one({ .name = QByteArray("ok\0/../evil", 11), .data = "x" }) << unsafe;
	QTest::newRow("empty name") << one({ .name = "", .data = "x" }) << unsafe;
	QTest::newRow("dot") << one({ .name = "./x", .data = "x" }) << unsafe;
	QTest::newRow("bad utf8") << one({ .name = "a\xFF\xFE", .data = "x" }) << unsafe;
	QTest::newRow("reserved for the swap") << one({ .name = "_backup_1/x", .data = "x" }) << unsafe;
	QTest::newRow("journal") << one({ .name = "_update_journal.txt", .data = "x" }) << unsafe;
	QTest::newRow("very long name") << one({ .name = QByteArray(600, 'a'), .data = "x" }) << unsafe;

	const auto notFile = int(UnpackError::NotAFile);
	QTest::newRow("symlink") << one({ .name = "link", .data = "/etc/passwd", .mode = 0120777 }) << notFile;
	QTest::newRow("symlink out") << one({ .name = "lib/up", .data = "../../..", .mode = 0120777 }) << notFile;
	QTest::newRow("fifo") << one({ .name = "pipe", .data = "", .mode = 0010644 }) << notFile;
	QTest::newRow("device node") << one({ .name = "dev", .data = "", .mode = 0060644 }) << notFile;
	QTest::newRow("socket") << one({ .name = "sock", .data = "", .mode = 0140644 }) << notFile;
	QTest::newRow("directory with content") << one({ .name = "dir/", .data = "payload", .mode = 0040755 }) << notFile;
	QTest::newRow("file that says directory") << one({ .name = "odd", .data = "x", .mode = 0040755 }) << notFile;
	QTest::newRow("directory that says file") << one({ .name = "odd/", .data = "", .mode = 0100644 }) << notFile;
	QTest::newRow("dos directory bit") << one({ .name = "odd", .data = "x", .host = 0, .dosAttributes = 0x10 }) << notFile;

	const auto duplicate = int(UnpackError::Duplicate);
	QTest::newRow("same name twice") << one({ .name = "Eleza", .data = "other" }) << duplicate;
	QTest::newRow("differs by case") << one({ .name = "ELEZA", .data = "other" }) << duplicate;
	QTest::newRow("differs by case in dir") << one({ .name = "LIB/Data.BIN", .data = "other" }) << duplicate;
	// U+00E9 against "e" + U+0301.
	QTest::newRow("differs by normalization") << MakeZip({
		{ .name = "caf\xC3\xA9.txt", .data = "a" },
		{ .name = "cafe\xCC\x81.txt", .data = "b" },
	}) << duplicate;
	QTest::newRow("file where a directory is") << one({ .name = "lib", .data = "x" }) << duplicate;
	QTest::newRow("file under a file") << one({ .name = "Eleza/inside", .data = "x" }) << duplicate;
	QTest::newRow("directory where a file is") << one({ .name = "Eleza/", .data = "", .mode = 0040755 }) << duplicate;

	const auto unsupported = int(UnpackError::Unsupported);
	QTest::newRow("encrypted") << one({ .name = "secret", .data = "x", .flags = 1 }) << unsupported;
}

void TestUpdater::unpackRejects() {
	QFETCH(QByteArray, archive);
	QFETCH(int, error);
	auto temp = QTemporaryDir();
	const auto path = temp.filePath(u"in/package.zip"_s);
	WriteBytes(path, archive);
	const auto target = temp.filePath(u"stage/out/0.2.0"_s);
	const auto before = Listing(temp.path());
	const auto result = Unpack(path, target);
	QCOMPARE(int(result.error), error);
	// Nothing was written: not the target, not a half of it, nothing beside.
	QVERIFY(!QFileInfo::exists(target));
	QVERIFY(!QFileInfo::exists(target + u".tmp"_s));
	auto after = Listing(temp.path());
	after.removeAll(u"stage"_s);
	after.removeAll(u"stage/out"_s);
	QCOMPARE(after, before);
}

void TestUpdater::unpackBomb() {
	auto temp = QTemporaryDir();
	const auto path = temp.filePath(u"bomb.zip"_s);
	// 64 MiB of zeros deflate to a few dozen kilobytes.
	const auto archive = MakeZip({
		{ .name = "Eleza", .data = QByteArray(64 << 20, '\0'), .deflate = true },
	});
	QVERIFY(archive.size() < (1 << 20));
	WriteBytes(path, archive);
	const auto target = temp.filePath(u"out"_s);
	const auto result = Unpack(path, target);
	QCOMPARE(result.error, UnpackError::TooLarge);
	QVERIFY(!QFileInfo::exists(target));
	QVERIFY(!QFileInfo::exists(target + u".tmp"_s));

	// Many small bombs: the sum is what counts.
	auto entries = QVector<ZipEntry>();
	for (auto i = 0; i != 40; ++i) {
		entries.push_back({
			.name = "f" + QByteArray::number(i),
			.data = QByteArray(2 << 20, '\0'),
			.deflate = true,
		});
	}
	WriteBytes(path, MakeZip(entries));
	QCOMPARE(Unpack(path, target).error, UnpackError::TooLarge);
	QVERIFY(!QFileInfo::exists(target));
}

void TestUpdater::unpackLyingSize() {
	auto temp = QTemporaryDir();
	const auto path = temp.filePath(u"lie.zip"_s);
	const auto target = temp.filePath(u"out"_s);
	// Says 10 bytes, inflates to 8 MiB: the limits pass on the declared size,
	// the bytes that really come out are counted again.
	WriteBytes(path, MakeZip({
		{ .name = "Eleza", .data = QByteArray(8 << 20, 'a'), .deflate = true, .declaredSize = 10 },
	}));
	const auto result = Unpack(path, target);
	QCOMPARE(result.error, UnpackError::Corrupt);
	QVERIFY(!QFileInfo::exists(target));
	QVERIFY(!QFileInfo::exists(target + u".tmp"_s));

	// Says more than there is.
	WriteBytes(path, MakeZip({
		{ .name = "Eleza", .data = QByteArray(1000, 'a'), .deflate = true, .declaredSize = 5000 },
	}));
	QCOMPARE(Unpack(path, target).error, UnpackError::Corrupt);
	QVERIFY(!QFileInfo::exists(target));

	// A stored entry whose bytes do not match the checksum.
	auto damaged = MakeZip({ { .name = "Eleza", .data = QByteArray(1000, 'a') } });
	damaged[100] = 'b';
	WriteBytes(path, damaged);
	QCOMPARE(Unpack(path, target).error, UnpackError::Corrupt);
	QVERIFY(!QFileInfo::exists(target));
}

void TestUpdater::unpackLimits() {
	auto temp = QTemporaryDir();
	const auto path = temp.filePath(u"package.zip"_s);
	const auto target = temp.filePath(u"out"_s);
	WriteBytes(path, MakeZip(GoodPackage()));

	auto limits = UnpackLimits();
	limits.maxEntries = 3;
	QCOMPARE(Unpack(path, target, limits).error, UnpackError::TooMany);
	limits = UnpackLimits();
	limits.maxFileBytes = 1000;
	QCOMPARE(Unpack(path, target, limits).error, UnpackError::TooLarge);
	limits = UnpackLimits();
	limits.maxTotalBytes = 40000;
	QCOMPARE(Unpack(path, target, limits).error, UnpackError::TooLarge);
	limits = UnpackLimits();
	limits.maxRatio = 1;
	QCOMPARE(Unpack(path, target, limits).error, UnpackError::TooLarge);
	QVERIFY(!QFileInfo::exists(target));
	QVERIFY(Unpack(path, target).ok());
}

void TestUpdater::unpackNotAZip() {
	auto temp = QTemporaryDir();
	const auto path = temp.filePath(u"package.zip"_s);
	const auto target = temp.filePath(u"out"_s);
	WriteBytes(path, "<html>not found</html>");
	QCOMPARE(Unpack(path, target).error, UnpackError::Format);
	WriteBytes(path, QByteArray());
	QCOMPARE(Unpack(path, target).error, UnpackError::Format);
	WriteBytes(path, MakeZip({}));
	QCOMPARE(Unpack(path, target).error, UnpackError::Format);
	// Cut in the middle.
	const auto whole = MakeZip(GoodPackage());
	WriteBytes(path, whole.left(whole.size() / 2));
	QCOMPARE(Unpack(path, target).error, UnpackError::Format);
	QCOMPARE(Unpack(temp.filePath(u"missing.zip"_s), target).error, UnpackError::Open);
	QVERIFY(!QFileInfo::exists(target));
}

// ---- checker

void TestUpdater::upToDate() {
	auto fixture = Fixture();
	fixture.publish(ManifestJson({ .version = u"0.1.0"_s }));
	auto checker = Checker(fixture.config());
	auto spy = QSignalSpy(&checker, &Checker::stateChanged);
	checker.start();
	QCOMPARE(checker.state(), State::Idle);
	QVERIFY(!checker.lastChecked().isValid());
	checker.checkNow();
	QCOMPARE(checker.state(), State::Checking);
	QVERIFY(WaitForState(checker, State::UpToDate));
	QVERIFY(checker.lastChecked().isValid());
	QVERIFY(spy.size() >= 2);
	// No account, no cookies: a plain request.
	const auto request = fixture.server.to("/update.json").value(0);
	QVERIFY(request.header("authorization").isEmpty());
	QVERIFY(request.header("cookie").isEmpty());
	QVERIFY(request.header("x-eleza-client").isEmpty());
	QVERIFY(request.header("user-agent").startsWith("Eleza-Desktop/0.1.0"));
}

void TestUpdater::downloadsAndUnpacks() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	auto states = QVector<State>();
	connect(&checker, &Checker::stateChanged, [&] {
		if (states.isEmpty() || states.back() != checker.state()) {
			states.push_back(checker.state());
		}
	});
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	QCOMPARE(states, QVector<State>({
		State::Checking,
		State::Downloading,
		State::Preparing,
		State::Ready,
	}));
	QCOMPARE(checker.info().version, u"0.2.0"_s);
	QCOMPARE(checker.info().notesEn, u"What is new"_s);
	QCOMPARE(checker.info().size, qint64(package.size()));
	QVERIFY(!checker.info().mandatory);
	const auto ready = checker.readyDir();
	QCOMPARE(ready, fixture.updatesDir() + u"/0.2.0"_s);
	QCOMPARE(ReadBytes(ready + u"/Eleza"_s), QByteArray("#!/bin/sh\necho new\n"));
	QCOMPARE(ReadBytes(ready + u"/lib/data.bin"_s).size(), 40003);
	// The archive is gone, the unpacked version and the state file stay.
	QCOMPARE(
		QDir(fixture.updatesDir()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
		QStringList({ u"0.2.0"_s, u"download"_s, u"state.json"_s }));
	QVERIFY(QDir(fixture.updatesDir() + u"/download"_s).isEmpty());
	QCOMPARE(fixture.server.count("/pkg.zip"), 1);
}

void TestUpdater::readySurvivesRestart() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	{
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
	}
	const auto requests = fixture.server.requests.size();
	{
		// The next start: ready at once, without the network.
		auto checker = Checker(fixture.config());
		checker.start();
		QCOMPARE(checker.state(), State::Ready);
		QCOMPARE(checker.info().version, u"0.2.0"_s);
		QCOMPARE(checker.info().notesFa, u"تازه‌ها"_s);
		QCOMPARE(fixture.server.requests.size(), requests);
		// A check behind it finds the same version and downloads nothing.
		checker.checkNow();
		QVERIFY(WaitSettled(checker));
		QCOMPARE(checker.state(), State::Ready);
		QCOMPARE(fixture.server.count("/pkg.zip"), 1);
	}
	{
		// The unpacked files were removed by somebody: not ready any more.
		QVERIFY(QFile::remove(fixture.updatesDir() + u"/0.2.0/Eleza"_s));
		auto checker = Checker(fixture.config());
		checker.start();
		QCOMPARE(checker.state(), State::Idle);
	}
	{
		// It was installed: this start is the new version.
		auto config = fixture.config();
		config.currentVersion = u"0.2.0"_s;
		auto checker = Checker(config);
		checker.start();
		QCOMPARE(checker.state(), State::Idle);
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::UpToDate));
	}
}

void TestUpdater::sourceFallbackWhenFirstIsDown() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	// Nothing at /first (404), the release at /second.
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}), "/second");
	auto config = fixture.config();
	config.sources = QStringList{
		fixture.url("/first/update.json"),
		fixture.url("/second/update.json"),
	};
	auto checker = Checker(config);
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	QCOMPARE(fixture.server.count("/first/update.json"), 1);
	QCOMPARE(fixture.server.count("/first/update.json.sig"), 0);
	QCOMPARE(fixture.server.count("/second/update.json"), 1);

	// A first source that does not answer at all (nothing listens there).
	auto dead = QTcpServer();
	QVERIFY(dead.listen(QHostAddress::LocalHost, 0));
	const auto port = dead.serverPort();
	dead.close();
	auto second = Fixture();
	second.serve("/pkg.zip", package);
	second.publish(ManifestJson({
		.package = package,
		.urls = { second.url("/pkg.zip") },
	}));
	config = second.config();
	config.sources = QStringList{
		u"http://127.0.0.1:%1/update.json"_s.arg(port),
		second.url("/update.json"),
	};
	auto other = Checker(config);
	other.start();
	other.checkNow();
	QVERIFY(WaitForState(other, State::Ready));
}

void TestUpdater::sourceFallbackWhenFirstIsTampered() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	const auto evil = MakeZip(GoodPackage("evil"));
	fixture.serve("/pkg.zip", package);
	fixture.serve("/evil.zip", evil);
	const auto good = ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	});
	// The first source serves a manifest that points at another package,
	// with the signature of the real one.
	const auto forged = ManifestJson({
		.version = u"0.9.0"_s,
		.package = evil,
		.urls = { fixture.url("/evil.zip") },
	});
	fixture.serve("/first/update.json", forged);
	fixture.serve("/first/update.json.sig", fixture.keys.sign(good));
	fixture.publish(good, "/second");
	auto config = fixture.config();
	config.sources = QStringList{
		fixture.url("/first/update.json"),
		fixture.url("/second/update.json"),
	};
	auto checker = Checker(config);
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	QCOMPARE(checker.info().version, u"0.2.0"_s);
	QCOMPARE(fixture.server.count("/evil.zip"), 0);
	QCOMPARE(ReadBytes(checker.readyDir() + u"/Eleza"_s), QByteArray("#!/bin/sh\necho new\n"));
}

void TestUpdater::tamperedEverywhere() {
	auto fixture = Fixture();
	const auto evil = MakeZip(GoodPackage("evil"));
	fixture.serve("/evil.zip", evil);
	const auto forged = ManifestJson({
		.package = evil,
		.urls = { fixture.url("/evil.zip") },
	});
	const auto attacker = Keys();
	// Signed, but not by a key this build trusts.
	fixture.serve("/first/update.json", forged);
	fixture.serve("/first/update.json.sig", attacker.sign(forged));
	// No signature at all.
	fixture.serve("/second/update.json", forged);
	// A signature that is not one.
	fixture.serve("/third/update.json", forged);
	fixture.serve("/third/update.json.sig", "AAAA");
	auto config = fixture.config();
	config.sources = QStringList{
		fixture.url("/first/update.json"),
		fixture.url("/second/update.json"),
		fixture.url("/third/update.json"),
	};
	auto checker = Checker(config);
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Failed));
	QCOMPARE(checker.failure(), Failure::Signature);
	QVERIFY(checker.info().version.isEmpty());
	QCOMPARE(fixture.server.count("/evil.zip"), 0);
	QVERIFY(!QFileInfo::exists(fixture.updatesDir() + u"/0.2.0"_s));

	// Nothing reachable at all is a network failure, not a signature one.
	config.sources = QStringList{ fixture.url("/nowhere/update.json") };
	auto other = Checker(config);
	other.start();
	other.checkNow();
	QVERIFY(WaitForState(other, State::Failed));
	QCOMPARE(other.failure(), Failure::Network);
}

void TestUpdater::manifestTooLarge() {
	auto fixture = Fixture();
	const auto huge = QByteArray(int(kMaxManifestBytes) + 1, ' ');
	fixture.serve("/update.json", huge);
	fixture.serve("/update.json.sig", fixture.keys.sign(huge));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Failed));
	QCOMPARE(fixture.server.count("/update.json.sig"), 0);
}

void TestUpdater::neverInstallsOlderOrSame() {
	for (const auto &version : { u"0.1.0"_s, u"0.0.9"_s, u"0.0.1"_s }) {
		auto fixture = Fixture();
		const auto package = MakeZip(GoodPackage());
		fixture.serve("/pkg.zip", package);
		fixture.publish(ManifestJson({
			.version = version,
			.package = package,
			.urls = { fixture.url("/pkg.zip") },
		}));
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::UpToDate));
		QCOMPARE(fixture.server.count("/pkg.zip"), 0);
	}
}

void TestUpdater::replayedManifestRefused() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	// The honest state of the world: 0.1.0 is the latest, published on the 12th.
	fixture.publish(ManifestJson({
		.version = u"0.1.0"_s,
		.published = u"2026-10-12T00:00:00Z"_s,
	}));
	{
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::UpToDate));
	}
	// Somebody on the way now serves an older manifest, correctly signed at
	// the time, that offers a version this client would take.
	fixture.publish(ManifestJson({
		.version = u"0.1.5"_s,
		.published = u"2026-10-01T00:00:00Z"_s,
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	{
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitSettled(checker));
		QCOMPARE(checker.state(), State::UpToDate);
		QCOMPARE(fixture.server.count("/pkg.zip"), 0);
		QVERIFY(checker.info().version.isEmpty());
	}
	// A lagging first source and a current second one: the second wins.
	fixture.publish(ManifestJson({
		.version = u"0.3.0"_s,
		.published = u"2026-10-20T00:00:00Z"_s,
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}), "/fresh");
	{
		auto config = fixture.config();
		config.sources.push_back(fixture.url("/fresh/update.json"));
		auto checker = Checker(config);
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
		QCOMPARE(checker.info().version, u"0.3.0"_s);
	}
}

void TestUpdater::mirrorFallback() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.routes.insert("/down/pkg.zip", [](const Request &) {
		return Response::Problem(503, "down");
	});
	fixture.serve("/mirror/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = {
			fixture.url("/missing/pkg.zip"),
			fixture.url("/down/pkg.zip"),
			fixture.url("/mirror/pkg.zip"),
		},
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	QCOMPARE(fixture.server.count("/missing/pkg.zip"), 1);
	QCOMPARE(fixture.server.count("/down/pkg.zip"), 1);
	QCOMPARE(fixture.server.count("/mirror/pkg.zip"), 1);
}

void TestUpdater::mirrorWithWrongBytes() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	// The same size, one byte different.
	auto wrong = package;
	wrong[wrong.size() / 2] = char(wrong[wrong.size() / 2] ^ 0x55);
	fixture.serve("/bad/pkg.zip", wrong);
	fixture.serve("/good/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/bad/pkg.zip"), fixture.url("/good/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	QCOMPARE(ReadBytes(checker.readyDir() + u"/lib/data.bin"_s), QByteArray(40000, 'x') + "new");
	// The good mirror was asked for the whole file, not for a continuation
	// of the bad one.
	const auto request = fixture.server.to("/good/pkg.zip").value(0);
	QVERIFY(request.header("range").isEmpty());
}

void TestUpdater::resumesOnNextMirror() {
	auto fixture = Fixture();
	const auto package = MakeZip({
		{ .name = "Eleza", .data = "#!/bin/sh\n", .mode = 0100755 },
		{ .name = "big.bin", .data = Crypto::RandomBytes(300000) },
	});
	const auto cut = 100000;
	fixture.routes.insert("/flaky/pkg.zip", [=](const Request &) {
		auto response = Response::Bytes(package);
		response.cutAfter = cut;
		return response;
	});
	fixture.serveRanged("/steady/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/flaky/pkg.zip"), fixture.url("/steady/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	auto progress = QVector<qint64>();
	connect(&checker, &Checker::progressChanged, [&] {
		progress.push_back(checker.received());
	});
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	const auto request = fixture.server.to("/steady/pkg.zip").value(0);
	QCOMPARE(request.header("range"), "bytes=" + QByteArray::number(cut) + '-');
	QCOMPARE(ReadBytes(checker.readyDir() + u"/big.bin"_s).size(), 300000);
	QVERIFY(progress.contains(qint64(cut)));
	QCOMPARE(checker.total(), qint64(package.size()));

	// A mirror that ignores the range and sends everything again.
	auto second = Fixture();
	second.routes.insert("/flaky/pkg.zip", [=](const Request &) {
		auto response = Response::Bytes(package);
		response.cutAfter = cut;
		return response;
	});
	second.serve("/plain/pkg.zip", package);
	second.publish(ManifestJson({
		.package = package,
		.urls = { second.url("/flaky/pkg.zip"), second.url("/plain/pkg.zip") },
	}));
	auto other = Checker(second.config());
	other.start();
	other.checkNow();
	QVERIFY(WaitForState(other, State::Ready));
	QCOMPARE(ReadBytes(other.readyDir() + u"/big.bin"_s).size(), 300000);

	// A mirror that answers 206 for another part than the one asked for.
	auto third = Fixture();
	third.routes.insert("/flaky/pkg.zip", [=](const Request &) {
		auto response = Response::Bytes(package);
		response.cutAfter = cut;
		return response;
	});
	third.routes.insert("/odd/pkg.zip", [=](const Request &request) {
		if (request.header("range").isEmpty()) {
			return Response::Bytes(package);
		}
		auto response = Response::Bytes(package.mid(5));
		response.status = 206;
		response.headers.push_back({
			"Content-Range",
			"bytes 5-" + QByteArray::number(package.size() - 1) + '/'
				+ QByteArray::number(package.size()),
		});
		return response;
	});
	third.publish(ManifestJson({
		.package = package,
		.urls = { third.url("/flaky/pkg.zip"), third.url("/odd/pkg.zip") },
	}));
	auto last = Checker(third.config());
	last.start();
	last.checkNow();
	QVERIFY(WaitForState(last, State::Ready));
	QCOMPARE(third.server.count("/odd/pkg.zip"), 2);
}

void TestUpdater::resumeFromWrongBeginning() {
	// The first mirror serves another, shorter file; the second one is right
	// and continues it on request. The mix does not match the signed hash:
	// the second mirror is asked once more, for the whole file.
	auto fixture = Fixture();
	const auto package = MakeZip({
		{ .name = "Eleza", .data = "#!/bin/sh\n", .mode = 0100755 },
		{ .name = "big.bin", .data = Crypto::RandomBytes(100000) },
	});
	fixture.serve("/wrong/pkg.zip", QByteArray(5000, 'w'));
	fixture.serveRanged("/right/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/wrong/pkg.zip"), fixture.url("/right/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Ready));
	const auto requests = fixture.server.to("/right/pkg.zip");
	QCOMPARE(requests.size(), 2);
	QCOMPARE(requests[0].header("range"), QByteArray("bytes=5000-"));
	QVERIFY(requests[1].header("range").isEmpty());
	QCOMPARE(ReadBytes(checker.readyDir() + u"/big.bin"_s).size(), 100000);
}

void TestUpdater::resumesPartFromDisk() {
	auto fixture = Fixture();
	const auto package = MakeZip({
		{ .name = "Eleza", .data = "#!/bin/sh\n", .mode = 0100755 },
		{ .name = "big.bin", .data = Crypto::RandomBytes(200000) },
	});
	fixture.serveRanged("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	const auto part = fixture.updatesDir()
		+ u"/download/"_s
		+ QString::fromLatin1(Crypto::Sha256(package).toHex().left(32))
		+ u".zip.part"_s;
	{
		// What an earlier run got before the app was closed.
		WriteBytes(part, package.left(150000));
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
		QCOMPARE(
			fixture.server.to("/pkg.zip").value(0).header("range"),
			QByteArray("bytes=150000-"));
		QCOMPARE(fixture.server.count("/pkg.zip"), 1);
	}
	{
		// A part that is not the beginning of the package: found out by the
		// hash, then everything again from the first byte.
		QDir(fixture.updatesDir()).removeRecursively();
		WriteBytes(part, QByteArray(150000, 'z'));
		fixture.server.requests.clear();
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
		QCOMPARE(fixture.server.count("/pkg.zip"), 2);
		QVERIFY(fixture.server.to("/pkg.zip").value(1).header("range").isEmpty());
	}
	{
		// A part longer than the package is not ours.
		QDir(fixture.updatesDir()).removeRecursively();
		WriteBytes(part, package + "more");
		fixture.server.requests.clear();
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
		QVERIFY(fixture.server.to("/pkg.zip").value(0).header("range").isEmpty());
	}
}

void TestUpdater::mirrorSendsMoreThanDeclared() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	// The signed size is that of the package; the mirror sends 4 MiB more.
	fixture.serve("/pkg.zip", package + QByteArray(4 << 20, 'x'));
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	auto most = qint64(0);
	connect(&checker, &Checker::progressChanged, [&] {
		most = qMax(most, checker.received());
	});
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Failed));
	QCOMPARE(checker.failure(), Failure::Package);
	QVERIFY(most <= package.size());
	QVERIFY(!QFileInfo::exists(fixture.updatesDir() + u"/0.2.0"_s));
	// Not a byte beyond the declared size ever reached the disk.
	const auto parts = QDir(fixture.updatesDir() + u"/download"_s).entryInfoList(QDir::Files);
	for (const auto &part : parts) {
		QVERIFY(part.size() <= package.size());
	}
}

void TestUpdater::wrongPackageEverywhere() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	const auto evil = MakeZip(GoodPackage("bad"));
	QCOMPARE(evil.size(), package.size());
	fixture.serve("/a/pkg.zip", evil);
	fixture.serve("/b/pkg.zip", evil);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/a/pkg.zip"), fixture.url("/b/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Failed));
	QCOMPARE(checker.failure(), Failure::Package);
	QCOMPARE(fixture.server.count("/a/pkg.zip"), 1);
	QCOMPARE(fixture.server.count("/b/pkg.zip"), 1);
	// Never unpacked, nothing kept.
	QVERIFY(!QFileInfo::exists(fixture.updatesDir() + u"/0.2.0"_s));
	QVERIFY(!QFileInfo::exists(fixture.updatesDir() + u"/0.2.0.tmp"_s));
	QVERIFY(QDir(fixture.updatesDir() + u"/download"_s).isEmpty());
	QVERIFY(checker.readyDir().isEmpty());

	// A package of another size.
	auto second = Fixture();
	second.serve("/pkg.zip", package.left(package.size() - 10));
	second.publish(ManifestJson({
		.package = package,
		.urls = { second.url("/pkg.zip") },
	}));
	auto other = Checker(second.config());
	other.start();
	other.checkNow();
	QVERIFY(WaitForState(other, State::Failed));
	QVERIFY(!QFileInfo::exists(second.updatesDir() + u"/0.2.0"_s));
}

void TestUpdater::unsafePackageRefused() {
	// Signed by the right key and still not unpacked: a package that tries
	// to leave the staging directory.
	auto fixture = Fixture();
	auto entries = GoodPackage();
	entries.push_back({ .name = "../../../escaped.txt", .data = "x" });
	const auto package = MakeZip(entries);
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Failed));
	QCOMPARE(checker.failure(), Failure::Unpack);
	QVERIFY(!QFileInfo::exists(fixture.temp.filePath(u"escaped.txt"_s)));
	QVERIFY(!QFileInfo::exists(fixture.temp.filePath(u"work/escaped.txt"_s)));
	QVERIFY(!QFileInfo::exists(fixture.updatesDir() + u"/0.2.0"_s));

	// A package without the executable is not an update either.
	auto second = Fixture();
	const auto empty = MakeZip({ { .name = "readme.txt", .data = "x" } });
	second.serve("/pkg.zip", empty);
	second.publish(ManifestJson({
		.package = empty,
		.urls = { second.url("/pkg.zip") },
	}));
	auto other = Checker(second.config());
	other.start();
	other.checkNow();
	QVERIFY(WaitForState(other, State::Failed));
	QCOMPARE(other.failure(), Failure::Unpack);
	QVERIFY(!QFileInfo::exists(second.updatesDir() + u"/0.2.0"_s));
	// Not fetched again by a later check on its own.
	const auto fetched = second.server.count("/pkg.zip");
	auto again = Checker(second.config());
	again.start();
	again.checkNow();
	// (checkNow is the user's «دوباره»: that does try once more.)
	QVERIFY(WaitForState(again, State::Failed));
	QCOMPARE(second.server.count("/pkg.zip"), fetched + 1);
}

void TestUpdater::automaticOff() {
	auto fixture = Fixture();
	fixture.autoDownload = false;
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Available));
	QCOMPARE(checker.info().version, u"0.2.0"_s);
	Test::Settle(100);
	QCOMPARE(fixture.server.count("/pkg.zip"), 0);
	// «دریافت».
	checker.download();
	QCOMPARE(checker.state(), State::Downloading);
	QVERIFY(WaitForState(checker, State::Ready));
}

void TestUpdater::announcesOnlyWithoutSelfUpdate() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	auto config = fixture.config();
	config.selfUpdate = false; // installed by the system's package manager
	auto checker = Checker(config);
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Available));
	QVERIFY(!checker.selfUpdate());
	checker.download();
	checker.setRequired(true);
	Test::Settle(200);
	QVERIFY(WaitSettled(checker));
	QCOMPARE(checker.state(), State::Available);
	QCOMPARE(fixture.server.count("/pkg.zip"), 0);
}

void TestUpdater::requiredDownloadsAnyway() {
	auto fixture = Fixture();
	fixture.autoDownload = false;
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.minSupported = u"0.2.0"_s,
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	auto checker = Checker(fixture.config());
	checker.start();
	checker.checkNow();
	QVERIFY(WaitForState(checker, State::Available));
	QVERIFY(checker.info().mandatory);
	// The blocker is shown: the setting does not hold it back any more.
	checker.setRequired(true);
	QVERIFY(WaitForState(checker, State::Ready));

	// Required before anything is known: checks and downloads.
	auto second = Fixture();
	second.autoDownload = false;
	second.serve("/pkg.zip", package);
	second.publish(ManifestJson({
		.package = package,
		.urls = { second.url("/pkg.zip") },
	}));
	auto other = Checker(second.config());
	other.start();
	other.setRequired(true);
	QVERIFY(WaitForState(other, State::Ready));
}

void TestUpdater::failedInstallNotRetried() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/pkg.zip", package);
	fixture.publish(ManifestJson({
		.package = package,
		.urls = { fixture.url("/pkg.zip") },
	}));
	{
		auto checker = Checker(fixture.config());
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
	}
	{
		// The restart: the install step reports a failure.
		auto config = fixture.config();
		config.firstCheckDelayMs = 0;
		auto checker = Checker(config);
		checker.installFinished(u"0.2.0"_s, false);
		checker.start();
		QCOMPARE(checker.state(), State::Idle);
		// The automatic check finds the same version and leaves it alone.
		QVERIFY(WaitForState(checker, State::Failed));
		QCOMPARE(checker.failure(), Failure::Install);
		QCOMPARE(checker.info().version, u"0.2.0"_s);
		QCOMPARE(fixture.server.count("/pkg.zip"), 1);
		// The user asks again.
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
		QCOMPARE(fixture.server.count("/pkg.zip"), 2);
	}
	{
		// The restart after a success: the new version runs.
		auto config = fixture.config();
		config.currentVersion = u"0.2.0"_s;
		auto checker = Checker(config);
		checker.installFinished(u"0.2.0"_s, true);
		checker.start();
		QCOMPARE(checker.installedNotes(false), u"What is new"_s);
		QCOMPARE(checker.installedNotes(true), u"تازه‌ها"_s);
		// What was staged is cleaned away.
		QVERIFY(Test::WaitFor([&] {
			return !QFileInfo::exists(fixture.updatesDir() + u"/0.2.0"_s);
		}));
	}
}

void TestUpdater::redirects() {
	auto fixture = Fixture();
	const auto package = MakeZip(GoodPackage());
	fixture.serve("/real/pkg.zip", package);
	const auto manifest = ManifestJson({
		.package = package,
		.urls = { fixture.url("/latest/pkg.zip") },
	});
	// "latest/download/…" -> the release -> the storage host.
	fixture.routes.insert("/latest/update.json", [](const Request &) {
		return Response::Redirect("/v0.2.0/update.json");
	});
	fixture.routes.insert("/latest/update.json.sig", [](const Request &) {
		return Response::Redirect("/v0.2.0/update.json.sig");
	});
	fixture.publish(manifest, "/v0.2.0");
	fixture.routes.insert("/latest/pkg.zip", [&](const Request &) {
		return Response::Redirect(fixture.url("/real/pkg.zip").toUtf8());
	});
	auto config = fixture.config();
	config.sources = QStringList{ fixture.url("/latest/update.json") };
	{
		auto checker = Checker(config);
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
	}

	// A redirect that leaves what is allowed (clear text to another machine,
	// another scheme) is not followed: the next source is asked.
	QDir(fixture.updatesDir()).removeRecursively();
	fixture.routes.insert("/out/update.json", [](const Request &) {
		return Response::Redirect("http://example.com/update.json");
	});
	fixture.routes.insert("/file/update.json", [](const Request &) {
		return Response::Redirect("file:///etc/passwd");
	});
	fixture.routes.insert("/loop/update.json", [](const Request &) {
		return Response::Redirect("/loop/update.json");
	});
	config.sources = QStringList{
		fixture.url("/out/update.json"),
		fixture.url("/file/update.json"),
		fixture.url("/loop/update.json"),
		fixture.url("/v0.2.0/update.json"),
	};
	{
		auto checker = Checker(config);
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Ready));
		QCOMPARE(fixture.server.count("/out/update.json"), 1);
		QCOMPARE(fixture.server.count("/loop/update.json"), 6);
	}

	// Without the debug switch a clear-text source is not even asked.
	QDir(fixture.updatesDir()).removeRecursively();
	fixture.server.requests.clear();
	config.allowInsecureLoopback = false;
	{
		auto checker = Checker(config);
		checker.start();
		checker.checkNow();
		QVERIFY(WaitForState(checker, State::Failed));
		QVERIFY(fixture.server.requests.isEmpty());
	}
}

// ---- install

void TestUpdater::detectInstall() {
	auto temp = QTemporaryDir();
	const auto directory = temp.filePath(u"Eleza"_s);
#ifdef Q_OS_WIN
	const auto executable = directory + u"/Eleza.exe"_s;
#else // Q_OS_WIN
	const auto executable = directory + u"/Eleza"_s;
#endif // !Q_OS_WIN
	WriteBytes(executable, "binary");

	// A build directory: no file list, never updated by itself.
	auto target = DetectInstall(executable);
	QVERIFY(!target.selfUpdate);
	QCOMPARE(target.installDir, QFileInfo(directory).canonicalFilePath());
	QCOMPARE(target.executable, QFileInfo(executable).fileName());

	WriteBytes(directory + u"/eleza-files.list"_s, "Eleza\n");
#ifdef Q_OS_WIN
	QVERIFY(!DetectInstall(executable).selfUpdate);
	WriteBytes(directory + u"/Updater.exe"_s, "updater");
	QVERIFY(DetectInstall(executable).selfUpdate);
#else // Q_OS_WIN
	QVERIFY(DetectInstall(executable).selfUpdate);

	// Started through a link: the real place counts.
	const auto link = temp.filePath(u"eleza-link"_s);
	QVERIFY(QFile::link(executable, link));
	target = DetectInstall(link);
	QVERIFY(target.selfUpdate);
	QCOMPARE(target.installDir, QFileInfo(directory).canonicalFilePath());

	// A directory the user cannot write (as a system-wide copy would be).
	if (::geteuid() != 0) {
		QVERIFY(QFile::setPermissions(
			directory,
			QFile::ReadOwner | QFile::ExeOwner));
		QVERIFY(!DetectInstall(executable).selfUpdate);
		QVERIFY(QFile::setPermissions(
			directory,
			QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
	}
	// A sandboxed package format.
	qputenv("FLATPAK_ID", "me.eleza.desktop");
	QVERIFY(!DetectInstall(executable).selfUpdate);
	qunsetenv("FLATPAK_ID");
	QVERIFY(DetectInstall(executable).selfUpdate);
	// What the system package manager installed.
	QVERIFY(!DetectInstall(u"/usr/bin/env"_s).selfUpdate);
#endif // !Q_OS_WIN
	QVERIFY(!DetectInstall(temp.filePath(u"missing/Eleza"_s)).selfUpdate);
}

QTEST_GUILESS_MAIN(TestUpdater)
#include "test_updater.moc"
