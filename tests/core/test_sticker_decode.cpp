// Data::SniffStickerFormat / OpenStickerAnimation: the three sticker kinds
// told apart from their bytes, an animated WebP and a Lottie (.tgs) fixture
// played frame by frame at the requested size, broken input refused.
#include "data/data_image_decode.h"
#include "data/data_sticker_decode.h"

#include <QtCore/QFile>
#include <QtTest/QtTest>

namespace {

[[nodiscard]] QByteArray Fixture(const char *name) {
	auto file = QFile(QStringLiteral(ELEZA_TESTDATA_DIR "/stickers/") + QString::fromLatin1(name));
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The colour at the centre of the frame's opaque part.
[[nodiscard]] QColor CenterOfInk(const QImage &image) {
	auto sumX = 0;
	auto sumY = 0;
	auto count = 0;
	for (auto y = 0; y != image.height(); ++y) {
		for (auto x = 0; x != image.width(); ++x) {
			if (qAlpha(image.pixel(x, y)) > 100) {
				sumX += x;
				sumY += y;
				++count;
			}
		}
	}
	return count
		? image.pixelColor(sumX / count, sumY / count)
		: QColor();
}

[[nodiscard]] int InkCenterX(const QImage &image) {
	auto sum = 0;
	auto count = 0;
	for (auto y = 0; y != image.height(); ++y) {
		for (auto x = 0; x != image.width(); ++x) {
			if (qAlpha(image.pixel(x, y)) > 100) {
				sum += x;
				++count;
			}
		}
	}
	return count ? (sum / count) : -1;
}

} // namespace

class TestStickerDecode final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void sniffing() {
		const auto webp = Fixture("anim.webp");
		const auto tgs = Fixture("anim.tgs");
		const auto still = Fixture("still.webp");
		QVERIFY(!webp.isEmpty() && !tgs.isEmpty() && !still.isEmpty());
		QCOMPARE(Data::SniffStickerFormat(webp), Data::StickerFormat::AnimatedWebp);
		QCOMPARE(Data::SniffStickerFormat(webp.left(21)), Data::StickerFormat::AnimatedWebp);
		QCOMPARE(Data::SniffStickerFormat(tgs), Data::StickerFormat::Lottie);
		QCOMPARE(Data::SniffStickerFormat(still), Data::StickerFormat::Still);
		QCOMPARE(Data::SniffStickerFormat(QByteArray("\x89PNG\r\n\x1a\n", 8)), Data::StickerFormat::Still);
		QCOMPARE(Data::SniffStickerFormat(QByteArray()), Data::StickerFormat::Still);
		// The still decoder never takes a Lottie file for a picture.
		QVERIFY(Data::SniffImageFormat(tgs).isEmpty());
		QVERIFY(Data::DecodeImage(QString(), tgs).isNull());
		// … and shows the first frame of an animated WebP.
		QCOMPARE(Data::DecodeImage(QString(), webp).size(), QSize(32, 24));
		QVERIFY(!Data::OpenStickerAnimation(still, QSize(64, 64)));
	}

	void gunzip() {
		const auto json = Data::GunzipSticker(Fixture("anim.tgs"));
		QVERIFY(json.startsWith("{\"v\":"));
		QVERIFY(json.endsWith("}"));
		QVERIFY(Data::GunzipSticker(Fixture("anim.tgs"), 16).isEmpty()); // over the limit
		QVERIFY(Data::GunzipSticker(QByteArray("\x1f\x8b\x08", 3)).isEmpty());
		auto broken = Fixture("anim.tgs");
		broken.truncate(broken.size() / 2);
		QVERIFY(Data::GunzipSticker(broken).isEmpty());
	}

	void animatedWebp() {
		auto animation = Data::OpenStickerAnimation(Fixture("anim.webp"), QSize(64, 64));
		QVERIFY(animation != nullptr);
		QCOMPARE(animation->frameCount(), 3);
		QCOMPARE(animation->frameSize(), QSize(64, 48));
		const int delays[] = { 50, 60, 70 };
		const QColor colors[] = { Qt::red, Qt::green, Qt::blue };
		for (auto loop = 0; loop != 2; ++loop) {
			for (auto i = 0; i != 3; ++i) {
				auto delay = 0;
				const auto frame = animation->renderFrame(i, &delay);
				QCOMPARE(frame.size(), QSize(64, 48));
				QCOMPARE(frame.format(), QImage::Format_ARGB32_Premultiplied);
				QCOMPARE(delay, delays[i]);
				const auto color = CenterOfInk(frame);
				QCOMPARE(color.red() > 200, colors[i].red() > 200);
				QCOMPARE(color.green() > 200, colors[i].green() > 200);
				QCOMPARE(color.blue() > 100, colors[i].blue() > 100);
			}
		}
		// Back to an earlier frame: the file is read again.
		auto delay = 0;
		QVERIFY(!animation->renderFrame(1, &delay).isNull());
		QVERIFY(animation->renderFrame(3, &delay).isNull());
	}

	void lossyWebp() {
		// Lossy (VP8) sub-frames placed on a transparent canvas.
		auto animation = Data::OpenStickerAnimation(Fixture("anim_lossy.webp"), QSize(48, 48));
		QVERIFY(animation != nullptr);
		QCOMPARE(animation->frameCount(), 3);
		QCOMPARE(animation->frameSize(), QSize(48, 32));
		auto previous = -1;
		for (auto i = 0; i != 3; ++i) {
			auto delay = 0;
			const auto frame = animation->renderFrame(i, &delay);
			QVERIFY(!frame.isNull());
			QCOMPARE(delay, 40);
			// The box moves right; the corners stay transparent.
			QVERIFY(qAlpha(frame.pixel(0, 0)) < 20);
			const auto x = InkCenterX(frame);
			QVERIFY(x > previous);
			previous = x;
		}
	}

	void lottie() {
		auto animation = Data::OpenStickerAnimation(Fixture("anim.tgs"), QSize(128, 128));
		if (!Data::LottieSupported()) {
			QVERIFY(animation == nullptr);
			QSKIP("Built without rlottie: Lottie stickers show their emoji.");
		}
		QVERIFY(animation != nullptr);
		QCOMPARE(animation->frameCount(), 12);
		QCOMPARE(animation->frameSize(), QSize(128, 96));
		auto delay = 0;
		const auto first = animation->renderFrame(0, &delay);
		QCOMPARE(delay, 33);
		QCOMPARE(first.size(), QSize(128, 96));
		const auto last = animation->renderFrame(11, &delay);
		// The red box moves from the left to the right.
		const auto from = InkCenterX(first);
		const auto till = InkCenterX(last);
		QVERIFY(from >= 0 && till >= 0);
		QVERIFY2(till - from > 40, qPrintable(QStringLiteral("%1 -> %2").arg(from).arg(till)));
		QVERIFY(CenterOfInk(first).red() > 200);
		QVERIFY(animation->renderFrame(12, &delay).isNull());
	}

	void lottieWithFilesIsRefused() {
		if (!Data::LottieSupported()) {
			QSKIP("Built without rlottie.");
		}
		// An image asset read from a path on the disk: never played.
		const auto json = QByteArray(
			R"({"v":"5.5.2","fr":30,"ip":0,"op":10,"w":64,"h":64,)"
			R"("assets":[{"id":"i","w":10,"h":10,"u":"/etc/","p":"passwd","e":0}],)"
			R"("layers":[{"ty":2,"refId":"i","ind":1,"ip":0,"op":10,"st":0,)"
			R"("ks":{"o":{"a":0,"k":100},"p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},)"
			R"("s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0}}}]})");
		auto gz = qCompress(json, 9);
		// qCompress gives a zlib stream: wrap its deflate data as gzip.
		auto gzip = QByteArray("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
		gzip += gz.mid(4 + 2, gz.size() - 4 - 2 - 4);
		gzip += QByteArray(8, '\0');
		QVERIFY(!Data::GunzipSticker(gzip).isEmpty());
		QVERIFY(!Data::OpenStickerAnimation(gzip, QSize(64, 64)));
	}
};

QTEST_GUILESS_MAIN(TestStickerDecode)
#include "test_sticker_decode.moc"
