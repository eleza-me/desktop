// Ui::Emoji: the EPK3 reader against the real emoji.pack (every emoji decodes,
// pixel-exact samples), the tables, which runs of a text are emoji and which
// emoji each one is, skin tones, qualifying for sending.
#include "ui/emoji/emoji_index.h"
#include "ui/emoji/emoji_pack.h"
#include "ui/emoji/emoji_pack_reader.h"
#include "ui/emoji/emoji_tables.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QResource>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtTest/QtTest>

#include <memory>

using namespace Ui::Emoji;

class TestEmoji : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

	// The text of each range, in order, separated by '|'.
	static QString Runs(const QString &text) {
		auto result = QStringList();
		for (const auto &range : FindAll(text)) {
			result.push_back(text.mid(range.from, range.till - range.from));
		}
		return result.join(u'|');
	}

	QImage decode(const QString &emoji) {
		const auto id = Find(emoji);
		if (id < 0 || IsHandshake(id)) {
			qWarning("not in the index: %s", qPrintable(emoji));
			return QImage();
		}
		return _reader->decode(Page(id), Slot(id));
	}

	// Opaque pixels in the middle, a transparent rim, more than a few colours.
	static bool LooksLikeAnEmoji(const QString &emoji, const QImage &image) {
		if (image.size() != QSize(64, 64)) {
			return false;
		}
		auto opaque = 0;
		auto clear = 0;
		auto colors = QSet<QRgb>();
		for (auto y = 0; y != 64; ++y) {
			for (auto x = 0; x != 64; ++x) {
				const auto c = image.pixel(x, y);
				if (qAlpha(c) == 255) {
					++opaque;
				} else if (qAlpha(c) == 0) {
					++clear;
				}
				if (qAlpha(c) > 0) {
					colors.insert(c);
				}
			}
		}
		// (A keycap fills its square to the corners.)
		const auto keycap = emoji.contains(QChar(0x20E3));
		return opaque > 600
			&& (keycap || clear > 50)
			&& colors.size() > 8
			&& qAlpha(image.pixel(32, 32)) > 0;
	}

	QByteArray _pack;
	std::unique_ptr<PackReader> _reader;

private Q_SLOTS:
	void initTestCase() {
		auto file = QFile(QStringLiteral(ELEZA_EMOJI_PACK));
		QVERIFY(file.open(QIODevice::ReadOnly));
		_pack = file.readAll();
		_reader = std::make_unique<PackReader>(
			reinterpret_cast<const uchar*>(_pack.constData()),
			_pack.size());
		QVERIFY2(_reader->valid(), qPrintable(_reader->error()));
	}

	void header() {
		QCOMPARE(_reader->version(), 3);
		QVERIFY(_reader->recordCount() > _reader->emojiCount());
		QCOMPARE(_reader->emojiCount(), 3606);
	}

	void refusesWhatIsNotAPack() {
		const auto junk = QByteArray(64, 'x');
		auto reader = PackReader(
			reinterpret_cast<const uchar*>(junk.constData()),
			junk.size());
		QVERIFY(!reader.valid());
		QVERIFY(reader.decode(0, 0).isNull());
		// A cut-off pack: the file length in the header does not match.
		const auto cut = _pack.left(_pack.size() / 2);
		auto half = PackReader(
			reinterpret_cast<const uchar*>(cut.constData()),
			cut.size());
		QVERIFY(!half.valid());
	}

	void tablesMatchThePack() {
		// The generated tables and the pack come from the same Telegram revision.
		auto total = 0;
		for (auto p = 0; p != Tables::kPageCount; ++p) {
			const auto n = PageSize(p);
			QCOMPARE(_reader->pageSize(p), n);
			QVERIFY(_reader->has(p, n - 1));
			QVERIFY(!_reader->has(p, n));
			total += n;
		}
		QCOMPARE(total, _reader->emojiCount());
	}

	void notInThePack() {
		QVERIFY(_reader->decode(9, 0).isNull());
		QVERIFY(_reader->decode(0, 4000).isNull());
		QVERIFY(_reader->decode(-1, 0).isNull());
	}

	void decodesKnownEmoji() {
		// A face (an index picture against a root), a masked one, a heart, a
		// keycap, flags (a whole WebP file with alpha), a couple, a hair
		// style, a skin tone…
		const char *list[] = { "😀", "😂", "❤", "1⃣", "🇮🇷", "🇺🇳", "👍🏽",
			"👩🏻‍❤‍👨🏿", "🧑‍🦰", "🏳‍🌈", "🐻", "🍔", "⚽", "🚗", "💡" };
		for (const auto emoji : list) {
			QVERIFY2(LooksLikeAnEmoji(U(emoji), decode(U(emoji))), emoji);
		}
	}

	void grinningFaceIsYellow() {
		const auto c = decode(U("😀")).pixel(32, 20); // The forehead.
		QCOMPARE(qAlpha(c), 255);
		QVERIFY(qRed(c) > 200 && qGreen(c) > 150 && qBlue(c) < 120);
	}

	void skinTonesDiffer() {
		const auto light = decode(U("👍🏻"));
		const auto dark = decode(U("👍🏿"));
		auto diff = 0;
		for (auto y = 0; y != 64; ++y) {
			for (auto x = 0; x != 64; ++x) {
				diff += (light.pixel(x, y) != dark.pixel(x, y)) ? 1 : 0;
			}
		}
		QVERIFY(diff > 500);
	}

	void matchesTheReferenceDecoder() {
		// Pixel-exact against the reference of the Android client's tests (an
		// independent reader of the format on libwebp): SHA-1 of the
		// little-endian ARGB words, the first 8 bytes. Deflated index maps,
		// index pictures against roots, the predictors, masks, a flag.
		struct Sample {
			int page;
			int index;
			const char *hash;
		};
		const Sample samples[] = {
			{ 0, 0, "d4befda66ee8cf99" }, // 😀
			{ 0, 7, "3ab6c4e0cd910f82" }, // 😂
			{ 0, 391, "8e71ba7ce44ee304" }, // DEFLATE only
			{ 3, 10, "a115da0da26edc59" },
			{ 5, 196, "a48176f46c6ca69f" },
			{ 6, 214, "bf6c6b252d6f42e1" },
			{ 7, 1, "3f323e2515894e6b" }, // A flag.
		};
		for (const auto &sample : samples) {
			const auto image = _reader->decode(sample.page, sample.index);
			QCOMPARE(image.format(), QImage::Format_ARGB32);
			auto bytes = QByteArray();
			for (auto y = 0; y != 64; ++y) {
				for (auto x = 0; x != 64; ++x) {
					const auto c = quint32(image.pixel(x, y));
					bytes.append(char(c & 0xFF));
					bytes.append(char((c >> 8) & 0xFF));
					bytes.append(char((c >> 16) & 0xFF));
					bytes.append(char((c >> 24) & 0xFF));
				}
			}
			const auto hash = QCryptographicHash::hash(
				bytes,
				QCryptographicHash::Sha1).left(8).toHex();
			QCOMPARE(hash, QByteArray(sample.hash));
		}
	}

	void everyEmojiDecodes() {
		// Every record path of the pack (palettes, masks, all predictors, both
		// codecs, whole files) and every emoji of the tables.
		auto failed = QStringList();
		for (auto p = 0; p != Tables::kPageCount; ++p) {
			for (auto i = 0; i != PageSize(p); ++i) {
				auto error = QString();
				const auto image = _reader->decode(p, i, &error);
				const auto name = Code((p << 12) | i)
					+ QStringLiteral(" (%1, %2)").arg(p).arg(i);
				if (image.isNull()) {
					failed.push_back(name + u' ' + error);
					continue;
				}
				auto drawn = 0;
				for (auto y = 0; y != 64; ++y) {
					for (auto x = 0; x != 64; ++x) {
						drawn += (qAlpha(image.pixel(x, y)) != 0) ? 1 : 0;
					}
				}
				if (drawn <= 64) {
					failed.push_back(name + QStringLiteral(" is empty"));
				}
			}
		}
		QVERIFY2(failed.isEmpty(), qPrintable(failed.join(u'\n')));
	}

	void embeddedUncompressed() {
		// The app reads the pack in place from its resources.
		Q_INIT_RESOURCE(emoji);
		const auto resource = QResource(QStringLiteral(":/emoji/emoji.pack"));
		QVERIFY(resource.isValid());
		QCOMPARE(resource.compressionAlgorithm(), QResource::NoCompression);
		QCOMPARE(qsizetype(resource.size()), _pack.size());
		for (auto tone = -1; tone != 5; ++tone) {
			for (auto side = 0; side != 2; ++side) {
				const auto half = QImage(
					QStringLiteral(":/emoji/compound/0_%1_%2.png").arg(tone).arg(side));
				QVERIFY(!half.isNull());
			}
		}
		QVERIFY(Available());
	}

	// ---- the parser

	void plainTextHasNone() {
		QVERIFY(FindAll(U("سلام، حالت چطوره؟ Hello 123 #tag *bold* (c) -> <- 1/2")).empty());
		QVERIFY(FindAll(QString()).empty());
		QVERIFY(FindAll(U("‌نیم‌فاصله ‍ و ۱۲۳ ؟ ، «سلام»")).empty());
	}

	void simpleEmojiAmongText() {
		const auto s = U("سلام 😀 خوبی؟ 👍");
		const auto r = FindAll(s);
		QCOMPARE(int(r.size()), 2);
		QCOMPARE(s.mid(r[0].from, r[0].till - r[0].from), U("😀"));
		QCOMPARE(r[0].id, Find(U("😀")));
		QCOMPARE(s.mid(r[1].from, r[1].till - r[1].from), U("👍"));
	}

	void variationSelectors() {
		QCOMPARE(Runs(U("❤️")), U("❤️"));
		QCOMPARE(FindAll(U("❤️"))[0].id, Find(U("❤")));
		QCOMPARE(FindAll(U("❤"))[0].id, Find(U("❤"))); // The bare heart too.
		// Asked to be text (U+FE0E): left to the fonts.
		QVERIFY(FindAll(U("❤︎")).empty());
		QVERIFY(FindAll(U("☺︎")).empty());
		QCOMPARE(Runs(U("a☺︎b❤️c")), U("❤️"));
	}

	void skinTones() {
		const auto s = U("👍🏽");
		const auto r = FindAll(s);
		QCOMPARE(int(r.size()), 1);
		QCOMPARE(r[0].from, 0);
		QCOMPARE(r[0].till, int(s.size()));
		QVERIFY(r[0].id != Find(U("👍")));
		QCOMPARE(r[0].id, Find(U("👍🏽")));
	}

	void zwjSequencesAreOneEmoji() {
		const char *list[] = { "👩‍❤️‍👨", "👨‍👩‍👧‍👦", "🧑🏻‍🦰", "🏳️‍🌈", "🏴‍☠️",
			"😮‍💨", "❤️‍🔥", "👱🏻‍♀️", "👁️‍🗨️", "🙂‍↔️" };
		for (const auto item : list) {
			const auto e = U(item);
			const auto r = FindAll(u'a' + e + u'b');
			QCOMPARE(int(r.size()), 1);
			QCOMPARE(r[0].from, 1);
			QCOMPARE(r[0].till, 1 + int(e.size()));
			QVERIFY2(r[0].id >= 0, item);
		}
	}

	void unknownZwjSequenceFallsApart() {
		// 😀 + ZWJ + 🐻 is no emoji: two pictures, the ZWJ between them stays.
		const auto s = U("😀‍🐻");
		const auto r = FindAll(s);
		QCOMPARE(int(r.size()), 2);
		QCOMPARE(r[0].id, Find(U("😀")));
		QCOMPARE(r[1].id, Find(U("🐻")));
		QCOMPARE(r[1].from, int(s.indexOf(QChar(0x200D))) + 1);
	}

	void flags() {
		QCOMPARE(Runs(U("🇮🇷🇩🇪")), U("🇮🇷|🇩🇪"));
		QVERIFY(Find(U("🇮🇷")) >= 0);
		// A lone regional indicator is no flag.
		QVERIFY(FindAll(U("🇮")).empty());
		// England: the black flag + tag letters + the cancel tag.
		const auto england = U("🏴󠁧󠁢󠁥󠁮󠁧󠁿");
		const auto r = FindAll(u'x' + england + u'y');
		QCOMPARE(int(r.size()), 1);
		QCOMPARE(r[0].till, 1 + int(england.size()));
		QVERIFY(r[0].id >= 0);
	}

	void keycaps() {
		QCOMPARE(Runs(U("1️⃣ #⃣ 12 #")), U("1️⃣|#⃣"));
		QCOMPARE(FindAll(U("1️⃣"))[0].id, Find(U("1⃣")));
		QVERIFY(FindAll(U("1 2 3 * #")).empty());
	}

	void symbolsAsTelegramDrawsThem() {
		QCOMPARE(Runs(U("☺")), U("☺"));
		QCOMPARE(Runs(U("✌🏻")), U("✌🏻"));
		// Persian / Arabic letters and punctuation: no emoji.
		QVERIFY(FindAll(U("۱۲۳ ؟ ، «سلام»")).empty());
	}

	void handshakes() {
		const auto one = Find(U("🤝"));
		QVERIFY(one >= 0);
		QVERIFY(!IsHandshake(one)); // The plain one is a picture of the pack.
		const auto toned = Find(U("🤝🏽"));
		QVERIFY(IsHandshake(toned));
		QCOMPARE(HandshakeTone(toned, false), 2);
		QCOMPARE(HandshakeTone(toned, true), 2);
		const auto mixed = Find(U("🫱🏻‍🫲🏿"));
		QVERIFY(IsHandshake(mixed));
		QCOMPARE(HandshakeTone(mixed, false), 0);
		QCOMPARE(HandshakeTone(mixed, true), 4);
		QCOMPARE(int(FindAll(U("🫱🏻‍🫲🏿")).size()), 1);
	}

	void facingRightIsMirrored() {
		const auto id = Find(U("🏃‍➡️"));
		QVERIFY(id >= 0);
		QVERIFY(IsMirrored(id));
		QCOMPARE(id & ~kMirrored, Find(U("🏃")));
		QCOMPARE(int(FindAll(U("🏃‍➡️")).size()), 1);
	}

	void aliasesOfOldSpellings() {
		QVERIFY(Find(U("👱")) >= 0);
		QVERIFY(Find(U("👷🏽")) >= 0);
	}

	void limit() {
		auto s = QString();
		for (auto i = 0; i != 600; ++i) {
			s += U("😀");
		}
		QCOMPARE(int(FindAll(s).size()), kMaxPerText);
		QCOMPARE(int(FindAll(s, 10).size()), 10);
	}

	void everyPanelEmojiIsFound() {
		for (auto c = 0; c != kPanelCategoryCount; ++c) {
			QVERIFY(!PanelCategory(c).isEmpty());
			for (const auto &e : PanelCategory(c)) {
				const auto r = FindAll(e);
				QCOMPARE(int(r.size()), 1);
				QCOMPARE(r[0].till, int(e.size()));
				// The form put into a message (with U+FE0F) is the same picture.
				const auto q = Qualify(e);
				const auto rq = FindAll(q);
				QCOMPARE(int(rq.size()), 1);
				QCOMPARE(rq[0].id, r[0].id);
			}
		}
	}

	void tones() {
		QVERIFY(TakesTone(U("👍")));
		QVERIFY(TakesTone(U("🤝")));
		QVERIFY(!TakesTone(U("😀")));
		QCOMPARE(WithTone(U("👍"), 2), U("👍🏽"));
		// The tone goes after the person, before a profession / gender part.
		QCOMPARE(WithTone(U("🧑‍🚀"), 4), U("🧑🏿‍🚀"));
		QCOMPARE(WithTone(U("🙋‍♀"), 0), U("🙋🏻‍♀"));
		QCOMPARE(WithTone(U("👍"), -1), U("👍"));
		for (auto c = 0; c != kPanelCategoryCount; ++c) {
			for (const auto &base : PanelCategory(c)) {
				if (TakesTone(base)) {
					const auto toned = WithTone(base, 1);
					QVERIFY2(Find(toned) >= 0, qPrintable(toned));
				}
			}
		}
	}

	void qualify() {
		QCOMPARE(Qualify(U("❤")), U("❤️"));
		QCOMPARE(Qualify(U("1⃣")), U("1️⃣"));
		QCOMPARE(Qualify(U("🙋🏻‍♀")), U("🙋🏻‍♀️"));
		QCOMPARE(Qualify(U("✌🏻")), U("✌🏻")); // No U+FE0F before a tone.
		QCOMPARE(Qualify(U("😀")), U("😀"));
		QCOMPARE(Qualify(U("🏳‍🌈")), U("🏳️‍🌈"));
		QCOMPARE(Qualify(U("❤‍🔥")), U("❤️‍🔥"));
	}

	void panelTabsAreQualified() {
		// Without the pack the category tabs are drawn by the fonts: as emoji.
		QCOMPARE(PanelCategoryIcon(0), U("😀"));
		QCOMPARE(PanelCategoryIcon(3), U("⚽️"));
		QCOMPARE(PanelCategoryIcon(7), U("🏳️"));
		for (auto i = 0; i != kPanelCategoryCount; ++i) {
			QVERIFY(Find(PanelCategoryIcon(i)) >= 0);
		}
	}

};

QTEST_MAIN(TestEmoji)
#include "test_emoji.moc"
