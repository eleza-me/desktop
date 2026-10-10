// Data::Media: addresses and cache keys, decoding, authenticated downloads
// through a redirect, the memory and disk caches, and handle sharing.
#include "data/data_media.h"
#include "test_harness.h"

#include <QtCore/QBuffer>
#include <QtCore/QDirIterator>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtTest/QSignalSpy>

using namespace Data;
using Test::Harness;
using Request = Test::HttpServer::Request;
using Response = Test::HttpServer::Response;

namespace {

[[nodiscard]] QByteArray Picture(int width, int height, const char *format = "PNG", bool alpha = false) {
	auto image = QImage(width, height, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
	for (auto y = 0; y < height; ++y) {
		for (auto x = 0; x < width; ++x) {
			image.setPixel(x, y, qRgba((x * 7) & 255, (y * 5) & 255, (x + y) & 255, alpha ? 128 : 255));
		}
	}
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, format);
	return bytes;
}

class Token final : public Api::TokenProvider {
public:
	void accessToken(Fn<void(QString)> done) override {
		done(u"access-token"_s);
	}
	void refreshAfterUnauthorized(const QString &, Fn<void(bool)> done) override {
		done(false);
	}
};

[[nodiscard]] int FilesIn(const QString &directory) {
	auto result = 0;
	auto iterator = QDirIterator(directory, QDir::Files, QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		iterator.next();
		++result;
	}
	return result;
}

[[nodiscard]] bool Loaded(const MediaHandlePtr &handle) {
	return Test::WaitFor([&] { return handle->state() != MediaHandle::State::Loading; });
}

} // namespace

class TestMedia final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void addresses();
	void decode();
	void downloadThroughRedirect();
	void handlesAreShared();
	void diskCache();
	void memoryOnly();
	void failureAndRetry();
	void evict();
	void releasedHandleStopsTheDownload();
	void urlsCarryNoToken();
	void diskLimit();

};

void TestMedia::addresses() {
	const auto avatar = MediaRef::Avatar(u"u sara"_s, u"av1"_s);
	QVERIFY(avatar.valid());
	QCOMPARE(avatar.location(MediaVariant::Thumb), u"/v1/users/u%20sara/avatar?variant=thumb"_s);
	QCOMPARE(avatar.cacheKey(MediaVariant::Thumb), u"avatar:u sara:av1:thumb"_s);
	QVERIFY(avatar.cacheKey(MediaVariant::Thumb) != avatar.cacheKey(MediaVariant::Feed));
	// A new avatar id is a new cache entry.
	QVERIFY(avatar.cacheKey(MediaVariant::Thumb)
		!= MediaRef::Avatar(u"u sara"_s, u"av2"_s).cacheKey(MediaVariant::Thumb));
	// No avatar: nothing to fetch.
	QVERIFY(!MediaRef::Avatar(u"u_sara"_s, QString()).valid());

	const auto attachment = MediaRef::Attachment(u"c1"_s, 6);
	QCOMPARE(attachment.location(MediaVariant::Full), u"/v1/chats/c1/messages/6/attachment?variant=full"_s);
	QCOMPARE(attachment.location(MediaVariant::Mp4), u"/v1/chats/c1/messages/6/attachment?variant=mp4"_s);
	QCOMPARE(attachment.cacheKey(MediaVariant::Poster), u"attachment:c1:6:poster"_s);
	QVERIFY(!MediaRef::Attachment(u"c1"_s, 0).valid());

	const auto post = MediaRef::PostMedia(u"p1"_s, u"m/1"_s);
	QCOMPARE(post.location(MediaVariant::Feed), u"/v1/posts/p1/media/m%2F1?variant=feed"_s);
	QCOMPARE(post.cacheKey(MediaVariant::Feed), u"post:p1:m/1:feed"_s);

	const auto url = MediaRef::Url(u"https://cdn.eleza.me/s/1.webp"_s);
	QCOMPARE(url.location(MediaVariant::Full), u"https://cdn.eleza.me/s/1.webp"_s);
	QCOMPARE(url.cacheKey(MediaVariant::Full), u"url:https://cdn.eleza.me/s/1.webp"_s);
	QVERIFY(!MediaRef().valid());

	QCOMPARE(MediaVariantName(MediaVariant::Audio), u"audio"_s);
	QCOMPARE(MediaVariantName(MediaVariant::File), u"file"_s);
	QCOMPARE(MediaVariantName(MediaVariant::Cover), u"cover"_s);
}

void TestMedia::decode() {
	const auto png = Picture(400, 200);
	const auto full = Media::Decode(QString(), png, QSize());
	QCOMPARE(full.size(), QSize(400, 200));
	QVERIFY(full.format() == QImage::Format_RGB32);
	QCOMPARE(full.pixel(10, 20), qRgb(70, 100, 30));

	// Fits the box, aspect kept; never scaled up.
	QCOMPARE(Media::Decode(QString(), png, QSize(64, 64)).size(), QSize(64, 32));
	QCOMPARE(Media::Decode(QString(), png, QSize(100, 20)).size(), QSize(40, 20));
	QCOMPARE(Media::Decode(QString(), png, QSize(1000, 1000)).size(), QSize(400, 200));

	const auto jpeg = Media::Decode(QString(), Picture(300, 300, "JPG"), QSize(100, 50));
	QCOMPARE(jpeg.size(), QSize(50, 50));

	const auto alpha = Media::Decode(QString(), Picture(8, 8, "PNG", true), QSize());
	QVERIFY(alpha.format() == QImage::Format_ARGB32_Premultiplied);

	// From a file, whatever its name says.
	auto dir = QTemporaryDir();
	const auto path = dir.filePath(u"picture.bin"_s);
	{
		auto file = QFile(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(png);
	}
	QCOMPARE(Media::Decode(path, QByteArray(), QSize(200, 200)).size(), QSize(200, 100));

	QVERIFY(Media::Decode(QString(), "not an image at all", QSize()).isNull());
	QVERIFY(Media::Decode(QString(), QByteArray(), QSize()).isNull());
	QVERIFY(Media::Decode(dir.filePath(u"missing"_s), QByteArray(), QSize()).isNull());

	// Remote bytes are decoded as JPEG / PNG / GIF / WebP only, recognised by
	// their first bytes. An SVG is a document (external references, text
	// layout) and whatever other image plugin is installed is attack surface:
	// none of them is ever tried.
	const auto svg = QByteArray(
		"<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
		"<rect width='64' height='64' fill='red'/></svg>");
	QVERIFY(Media::Decode(QString(), svg, QSize()).isNull());
	QVERIFY(Media::Decode(QString(), "<?xml version='1.0'?>" + svg, QSize(32, 32)).isNull());
	const auto svgPath = dir.filePath(u"picture.svg"_s);
	{
		auto file = QFile(svgPath);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(svg);
	}
	QVERIFY(Media::Decode(svgPath, QByteArray(), QSize()).isNull());
	for (const auto format : { "BMP", "PPM", "XPM" }) {
		const auto other = Picture(16, 16, format);
		if (!other.isEmpty()) {
			QVERIFY2(Media::Decode(QString(), other, QSize()).isNull(), format);
		}
	}
	for (const auto format : { "GIF", "WEBP" }) {
		const auto known = Picture(16, 8, format);
		if (!known.isEmpty()) {
			QCOMPARE(Media::Decode(QString(), known, QSize()).size(), QSize(16, 8));
		}
	}

	// The header is read before any pixel is allocated: a picture that
	// claims 20000 x 20000 (1.6 GB decoded) or 2^31 - 1 a side is refused,
	// also when a small box was asked for.
	const auto header = [](quint32 width, quint32 height) {
		auto ihdr = QByteArray::fromHex("49484452") + QByteArray(13, char(0));
		for (auto i = 0; i != 4; ++i) {
			ihdr[4 + i] = char((width >> (8 * (3 - i))) & 0xFF);
			ihdr[8 + i] = char((height >> (8 * (3 - i))) & 0xFF);
		}
		ihdr[12] = 8;
		ihdr[13] = 6;
		// The decoder checks the CRC of the header chunk.
		auto crc = quint32(0xFFFFFFFF);
		for (const auto ch : ihdr) {
			crc ^= uchar(ch);
			for (auto bit = 0; bit != 8; ++bit) {
				crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0U);
			}
		}
		crc = ~crc;
		auto sum = QByteArray(4, char(0));
		for (auto i = 0; i != 4; ++i) {
			sum[i] = char((crc >> (8 * (3 - i))) & 0xFF);
		}
		return QByteArray::fromHex("89504e470d0a1a0a0000000d") + ihdr + sum
			+ QByteArray::fromHex("0000000049454e44ae426082");
	};
	QVERIFY(Media::Decode(QString(), header(20000, 20000), QSize()).isNull());
	QVERIFY(Media::Decode(QString(), header(20000, 20000), QSize(64, 64)).isNull());
	QVERIFY(Media::Decode(QString(), header(0x7FFFFFFF, 0x7FFFFFFF), QSize(64, 64)).isNull());
	QVERIFY(Media::Decode(QString(), header(17000, 1), QSize()).isNull());
	QVERIFY(Media::Decode(QString(), header(0, 0), QSize()).isNull());
}

void TestMedia::downloadThroughRedirect() {
	auto dir = QTemporaryDir();
	auto storage = Test::HttpServer();
	const auto png = Picture(120, 80);
	storage.handler = [&](const Request &) {
		return Response::Bytes(png, "image/png");
	};
	auto options = Harness::Options();
	options.cacheDir = dir.filePath(u"media"_s);
	auto harness = Harness(options);
	auto token = Token();
	harness.api.setTokenProvider(&token);
	harness.http.handler = [&](const Request &request) {
		return (request.path == "/v1/users/u_sara/avatar")
			? Response::Redirect((storage.base() + u"/bucket/a1.png?sig=abc"_s).toLatin1())
			: Response::Ok();
	};
	auto &media = harness.session->media();
	const auto ref = MediaRef::Avatar(u"u_sara"_s, u"av1"_s);
	QVERIFY(media.cached(ref, MediaVariant::Thumb).isNull());

	const auto handle = media.request(ref, MediaVariant::Thumb);
	QVERIFY(handle);
	QVERIFY(handle->state() == MediaHandle::State::Loading);
	QVERIFY(handle->image().isNull());
	auto ready = QSignalSpy(handle.get(), &MediaHandle::ready);
	QVERIFY(Loaded(handle));
	QVERIFY(handle->isReady());
	QCOMPARE(ready.size(), 1);
	QCOMPARE(handle->image().size(), QSize(120, 80));
	QCOMPARE(handle->progress(), 1.);
	QCOMPARE(handle->ref().id, u"u_sara"_s);

	// The API saw the token and the variant; the storage host saw neither.
	const auto asked = harness.http.to("/v1/users/u_sara/avatar");
	QCOMPARE(asked.size(), 1);
	QCOMPARE(asked[0].target, QByteArray("/v1/users/u_sara/avatar?variant=thumb"));
	QCOMPARE(asked[0].header("authorization"), QByteArray("Bearer access-token"));
	QCOMPARE(storage.requests.size(), 1);
	QCOMPARE(storage.requests[0].target, QByteArray("/bucket/a1.png?sig=abc"));
	QVERIFY(storage.requests[0].header("authorization").isEmpty());

	// The bytes are in the disk cache under the logical address.
	QCOMPARE(handle->filePath(), media.filePathFor(ref, MediaVariant::Thumb));
	QVERIFY(handle->filePath().startsWith(options.cacheDir));
	auto file = QFile(handle->filePath());
	QVERIFY(file.open(QIODevice::ReadOnly));
	QCOMPARE(file.readAll(), png);
	QCOMPARE(FilesIn(options.cacheDir), 1);
	QCOMPARE(media.diskUsage(), qint64(png.size()));
	QCOMPARE(media.cached(ref, MediaVariant::Thumb).size(), QSize(120, 80));
}

void TestMedia::handlesAreShared() {
	auto harness = Harness();
	const auto png = Picture(64, 64);
	harness.http.handler = [&](const Request &) {
		return Response::Bytes(png, "image/png");
	};
	auto &media = harness.session->media();
	const auto ref = MediaRef::Attachment(u"c1"_s, 6);
	const auto first = media.request(ref, MediaVariant::Thumb);
	const auto second = media.request(ref, MediaVariant::Thumb);
	QCOMPARE(first.get(), second.get());
	// Another variant or another box is another piece of work.
	const auto full = media.request(ref, MediaVariant::Full);
	QVERIFY(full.get() != first.get());
	auto fitted = Media::Options();
	fitted.fit = QSize(16, 16);
	const auto small = media.request(ref, MediaVariant::Thumb, fitted);
	QVERIFY(small.get() != first.get());
	QVERIFY(Loaded(first) && Loaded(full) && Loaded(small));
	QCOMPARE(small->image().size(), QSize(16, 16));
	QCOMPARE(first->image().size(), QSize(64, 64));
	QCOMPARE(harness.http.count("/v1/chats/c1/messages/6/attachment"), 3);

	// While somebody holds the handle, everybody gets the same one.
	QCOMPARE(media.request(ref, MediaVariant::Thumb).get(), first.get());
	// Once decoded, a new handle is Ready at once (no flicker in a list).
	auto released = small;
	const auto weak = std::weak_ptr<MediaHandle>(small);
	released = nullptr;
	QVERIFY(!weak.expired()); // `small` still holds it
	media.evict(MediaRef::Attachment(u"c9"_s, 1), MediaVariant::Thumb); // unrelated: nothing happens
	auto dropped = media.request(MediaRef::Attachment(u"c1"_s, 8), MediaVariant::Thumb);
	QVERIFY(Loaded(dropped));
	dropped = nullptr;
	Test::Settle();
	const auto again = media.request(MediaRef::Attachment(u"c1"_s, 8), MediaVariant::Thumb);
	QVERIFY(again->isReady());
	QCOMPARE(again->image().size(), QSize(64, 64));
	QCOMPARE(harness.http.count("/v1/chats/c1/messages/8/attachment"), 1);
	Q_UNUSED(fitted);
	QCOMPARE(harness.http.count("/v1/chats/c1/messages/6/attachment"), 3);
}

void TestMedia::diskCache() {
	auto dir = QTemporaryDir();
	const auto png = Picture(90, 60);
	auto options = Harness::Options();
	options.cacheDir = dir.filePath(u"media"_s);
	const auto ref = MediaRef::PostMedia(u"p1"_s, u"m1"_s);
	{
		auto harness = Harness(options);
		harness.http.handler = [&](const Request &) {
			return Response::Bytes(png, "image/png");
		};
		auto &media = harness.session->media();
		auto handle = media.request(ref, MediaVariant::Feed);
		QVERIFY(Loaded(handle));
		QVERIFY(handle->isReady());
		QCOMPARE(harness.http.count("/v1/posts/p1/media/m1"), 1);

		// Out of memory, still on disk: decoded again without the network.
		handle = nullptr;
		Test::Settle();
		media.clearMemory();
		QVERIFY(media.cached(ref, MediaVariant::Feed).isNull());
		handle = media.request(ref, MediaVariant::Feed);
		QVERIFY(handle->state() == MediaHandle::State::Loading);
		QVERIFY(Loaded(handle));
		QCOMPARE(handle->image().size(), QSize(90, 60));
		QCOMPARE(harness.http.count("/v1/posts/p1/media/m1"), 1);

		// `force` fetches again.
		auto force = Media::Options();
		force.force = true;
		const auto forced = media.request(ref, MediaVariant::Feed, force);
		QVERIFY(Loaded(forced));
		QCOMPARE(harness.http.count("/v1/posts/p1/media/m1"), 2);
		QCOMPARE(FilesIn(options.cacheDir), 1);
	}
	{
		// The next run of the application.
		auto harness = Harness(options);
		auto &media = harness.session->media();
		QVERIFY(Test::WaitFor([&] { return media.diskUsage() == png.size(); }));
		const auto handle = media.request(ref, MediaVariant::Feed);
		QVERIFY(Loaded(handle));
		QVERIFY(handle->isReady());
		QCOMPARE(handle->image().size(), QSize(90, 60));
		QCOMPARE(harness.http.requests.size(), 0);

		// A file only (a document to open): no decoding.
		auto fileOnly = Media::Options();
		fileOnly.decode = false;
		const auto file = media.request(ref, MediaVariant::Feed, fileOnly);
		QVERIFY(Loaded(file));
		QVERIFY(file->isReady());
		QVERIFY(file->image().isNull());
		QVERIFY(QFile::exists(file->filePath()));
		QVERIFY(file->bytes().isEmpty());

		media.clearDisk();
		QVERIFY(Test::WaitFor([&] { return FilesIn(options.cacheDir) == 0; }));
		QVERIFY(Test::WaitFor([&] { return media.diskUsage() == 0; }));
	}
}

void TestMedia::memoryOnly() {
	auto harness = Harness();
	const auto png = Picture(32, 32);
	harness.http.handler = [&](const Request &) {
		return Response::Bytes(png, "image/png");
	};
	auto &media = harness.session->media();
	QVERIFY(media.directory().isEmpty());
	const auto ref = MediaRef::Attachment(u"c1"_s, 6);
	QVERIFY(media.filePathFor(ref, MediaVariant::Full).isEmpty());
	const auto handle = media.request(ref, MediaVariant::Full);
	QVERIFY(Loaded(handle));
	QVERIFY(handle->isReady());
	QVERIFY(handle->filePath().isEmpty());
	QCOMPARE(handle->bytes(), png);
	QCOMPARE(handle->image().size(), QSize(32, 32));
	QCOMPARE(media.diskUsage(), 0);

	// A tiny memory budget keeps only what fits.
	media.setMemoryLimit(1);
	media.clearMemory();
	const auto other = media.request(MediaRef::Attachment(u"c1"_s, 7), MediaVariant::Full);
	QVERIFY(Loaded(other));
	QVERIFY(other->isReady());
	QCOMPARE(other->image().size(), QSize(32, 32)); // the handle keeps its image
	QVERIFY(media.cached(MediaRef::Attachment(u"c1"_s, 7), MediaVariant::Full).isNull());
}

void TestMedia::failureAndRetry() {
	auto dir = QTemporaryDir();
	auto options = Harness::Options();
	options.cacheDir = dir.filePath(u"media"_s);
	auto harness = Harness(options);
	auto missing = true;
	const auto png = Picture(20, 20);
	harness.http.handler = [&](const Request &) {
		return missing
			? Response::Problem(404, "media.not_found")
			: Response::Bytes(png, "image/png");
	};
	auto &media = harness.session->media();
	const auto ref = MediaRef::Attachment(u"c1"_s, 6);
	const auto handle = media.request(ref, MediaVariant::Thumb);
	auto failed = QSignalSpy(handle.get(), &MediaHandle::failed);
	auto ready = QSignalSpy(handle.get(), &MediaHandle::ready);
	QVERIFY(Loaded(handle));
	QVERIFY(handle->isFailed());
	QCOMPARE(failed.size(), 1);
	QCOMPARE(ready.size(), 0);
	QCOMPARE(handle->error().status, 404);
	QCOMPARE(handle->error().code, u"media.not_found"_s);
	QVERIFY(handle->image().isNull());
	QCOMPARE(FilesIn(options.cacheDir), 0);

	// A failed handle is not handed out again: asking again tries again.
	missing = false;
	const auto retry = media.request(ref, MediaVariant::Thumb);
	QVERIFY(retry.get() != handle.get());
	QVERIFY(Loaded(retry));
	QVERIFY(retry->isReady());
	QVERIFY(handle->isFailed());

	// No media at all fails at once, without a request.
	const auto before = harness.http.requests.size();
	const auto none = media.request(MediaRef(), MediaVariant::Thumb);
	QVERIFY(none->isFailed());
	QCOMPARE(none->error().code, u"media.none"_s);
	QCOMPARE(harness.http.requests.size(), before);
}

void TestMedia::evict() {
	auto dir = QTemporaryDir();
	auto options = Harness::Options();
	options.cacheDir = dir.filePath(u"media"_s);
	auto harness = Harness(options);
	const auto png = Picture(20, 20);
	harness.http.handler = [&](const Request &) {
		return Response::Bytes(png, "image/png");
	};
	auto &media = harness.session->media();
	const auto ref = MediaRef::Avatar(u"u_ali"_s, u"av7"_s);
	auto handle = media.request(ref, MediaVariant::Thumb);
	QVERIFY(Loaded(handle));
	const auto path = handle->filePath();
	QVERIFY(QFile::exists(path));
	handle = nullptr;
	Test::Settle();

	media.evict(ref, MediaVariant::Thumb);
	QVERIFY(!QFile::exists(path));
	QVERIFY(media.cached(ref, MediaVariant::Thumb).isNull());
	QCOMPARE(media.diskUsage(), 0);
	handle = media.request(ref, MediaVariant::Thumb);
	QVERIFY(Loaded(handle));
	QCOMPARE(harness.http.count("/v1/users/u_ali/avatar"), 2);
}

void TestMedia::releasedHandleStopsTheDownload() {
	auto harness = Harness();
	auto hang = true;
	const auto png = Picture(20, 20);
	harness.http.handler = [&](const Request &) {
		auto response = Response::Bytes(png, "image/png");
		response.hang = hang;
		return response;
	};
	auto &media = harness.session->media();
	const auto ref = MediaRef::Attachment(u"c1"_s, 6);
	auto handle = media.request(ref, MediaVariant::Thumb);
	QVERIFY(Test::WaitFor([&] { return harness.http.requests.size() == 1; }));
	// The row scrolled away.
	handle = nullptr;
	Test::Settle(50);
	// Asked again later, it starts over and completes.
	hang = false;
	handle = media.request(ref, MediaVariant::Thumb);
	QVERIFY(handle->state() == MediaHandle::State::Loading);
	QVERIFY(Loaded(handle));
	QVERIFY(handle->isReady());
	QCOMPARE(harness.http.requests.size(), 2);
}

void TestMedia::urlsCarryNoToken() {
	auto harness = Harness();
	auto token = Token();
	harness.api.setTokenProvider(&token);
	auto cdn = Test::HttpServer();
	const auto png = Picture(48, 48);
	cdn.handler = [&](const Request &) {
		return Response::Bytes(png, "image/png");
	};
	const auto handle = harness.session->media().request(
		MediaRef::Url(cdn.base() + u"/stickers/1.png"_s),
		MediaVariant::Full);
	QVERIFY(Loaded(handle));
	QVERIFY(handle->isReady());
	QCOMPARE(handle->image().size(), QSize(48, 48));
	QCOMPARE(cdn.requests.size(), 1);
	QVERIFY(cdn.requests[0].header("authorization").isEmpty());
	QCOMPARE(harness.http.requests.size(), 0);
}

void TestMedia::diskLimit() {
	auto dir = QTemporaryDir();
	auto options = Harness::Options();
	options.cacheDir = dir.filePath(u"media"_s);
	auto harness = Harness(options);
	const auto blob = QByteArray(4000, 'x');
	harness.http.handler = [&](const Request &) {
		return Response::Bytes(blob);
	};
	auto &media = harness.session->media();
	QCOMPARE(media.diskLimit(), qint64(512) << 20);
	auto fileOnly = Media::Options();
	fileOnly.decode = false;
	auto paths = QStringList();
	for (auto seq = 1; seq <= 5; ++seq) {
		const auto handle = media.request(MediaRef::Attachment(u"c1"_s, seq), MediaVariant::File, fileOnly);
		QVERIFY(Loaded(handle));
		QVERIFY(handle->isReady());
		paths.push_back(handle->filePath());
		// Distinct "last used" times, oldest first.
		auto file = QFile(handle->filePath());
		QVERIFY(file.open(QIODevice::ReadWrite));
		file.setFileTime(QDateTime::currentDateTime().addSecs(-1000 + seq * 100), QFileDevice::FileModificationTime);
	}
	QCOMPARE(media.diskUsage(), 20000);
	QCOMPARE(FilesIn(options.cacheDir), 5);

	// Over the limit: the least recently used files go first.
	media.setDiskLimit(10000);
	QVERIFY(Test::WaitFor([&] { return media.diskUsage() <= 10000; }));
	QVERIFY(media.diskUsage() > 0);
	QVERIFY(!QFile::exists(paths[0]));
	QVERIFY(!QFile::exists(paths[1]));
	QVERIFY(QFile::exists(paths[4]));
	QCOMPARE(qint64(FilesIn(options.cacheDir)) * 4000, media.diskUsage());
}

QTEST_GUILESS_MAIN(TestMedia)
#include "test_media.moc"
