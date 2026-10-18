// Chat albums: the mosaic (the vectors shared with the Android and iOS apps),
// which messages of a timeline make one bubble and what it says, how picked
// files are split into albums and where the caption goes, forwarding.
#include "history/history_album.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QtTest>

#include <cmath>
#include <limits>

namespace {

constexpr auto kAlbum = "0f8d6a4e-55aa-4b1c-9e21-3c4d5e6f7a8b";

[[nodiscard]] Data::Message Item(
		qint64 seq,
		Data::MessageKind kind = Data::MessageKind::Photo,
		const QString &album = QString::fromLatin1(kAlbum),
		bool mine = false,
		const QString &sender = u"peer"_s) {
	auto result = Data::Message();
	result.chat = u"c"_s;
	result.seq = seq;
	result.kind = kind;
	result.album = album;
	result.mine = mine;
	result.sender = mine ? u"me"_s : sender;
	result.createdAt = 1000 * seq;
	return result;
}

using History::AlbumFamily;

} // namespace

class TestHistoryAlbum final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void sharedVectors() {
		auto file = QFile(QStringLiteral(ELEZA_TESTDATA_DIR "/album_layout_vectors.json"));
		QVERIFY(file.open(QIODevice::ReadOnly));
		const auto root = QJsonDocument::fromJson(file.readAll()).object();
		QCOMPARE(root.value(u"max_w"_s).toDouble(), History::kAlbumMaxWidth);
		QCOMPARE(root.value(u"max_h"_s).toDouble(), History::kAlbumMaxHeight);
		QCOMPARE(root.value(u"min_w"_s).toDouble(), History::kAlbumMinWidth);
		QCOMPARE(root.value(u"min_h"_s).toDouble(), History::kAlbumMinHeight);
		const auto cases = root.value(u"cases"_s).toArray();
		QVERIFY(cases.size() > 10);
		for (const auto &value : cases) {
			const auto one = value.toObject();
			auto ratios = std::vector<double>();
			for (const auto &ratio : one.value(u"ratios"_s).toArray()) {
				ratios.push_back(ratio.toDouble());
			}
			const auto want = one.value(u"frames"_s).toArray();
			const auto got = History::AlbumLayout(ratios);
			QCOMPARE(int(got.size()), int(want.size()));
			for (auto i = 0; i != int(got.size()); ++i) {
				const auto f = want[i].toArray();
				const auto &g = got[i];
				const auto near = [](double a, double b) {
					return std::abs(a - b) < 0.01;
				};
				QVERIFY2(near(f[0].toDouble(), g.x), qPrintable(QJsonDocument(one).toJson()));
				QVERIFY(near(f[1].toDouble(), g.y));
				QVERIFY(near(f[2].toDouble(), g.w));
				QVERIFY(near(f[3].toDouble(), g.h));
				QCOMPARE(f[4].toInt(), g.sides);
			}
		}
	}

	void unknownSizesAreSquares() {
		const auto a = History::AlbumLayout({ 0., std::numeric_limits<double>::quiet_NaN() });
		const auto b = History::AlbumLayout({ 1., 1. });
		QCOMPARE(a.size(), b.size());
		for (auto i = 0; i != int(a.size()); ++i) {
			QCOMPARE(a[i].w, b[i].w);
			QCOMPARE(a[i].h, b[i].h);
		}
	}

	void everyCountFillsTheBox() {
		for (auto n = 2; n <= 10; ++n) {
			auto ratios = std::vector<double>();
			for (auto i = 0; i != n; ++i) {
				ratios.push_back((i % 3 == 0) ? 1.6 : (i % 3 == 1) ? 0.7 : 1.);
			}
			const auto frames = History::AlbumLayout(ratios);
			QCOMPARE(int(frames.size()), n);
			QVERIFY(History::AlbumHeight(frames) <= History::kAlbumMaxHeight + 0.5);
			auto top = 0;
			for (const auto &frame : frames) {
				QVERIFY(frame.w > 0. && frame.h > 0.);
				QVERIFY(frame.x >= 0. && frame.x + frame.w <= History::kAlbumMaxWidth + 0.5);
				top |= frame.sides;
			}
			QCOMPARE(top, 15);
		}
	}

	void grouping() {
		auto items = QVector<Data::Message>{
			Item(1, Data::MessageKind::Text, QString()),
			Item(2),
			Item(3, Data::MessageKind::Video),
			Item(4),
			Item(5, Data::MessageKind::Photo, QString()),
		};
		auto runs = History::GroupAlbums(items, false, 10);
		QCOMPARE(int(runs.size()), 1);
		QCOMPARE(runs[0].from, 1);
		QCOMPARE(runs[0].count, 3);

		// At most maxItems in one bubble: the rest makes the next one.
		runs = History::GroupAlbums(items, false, 2);
		QCOMPARE(int(runs.size()), 1);
		QCOMPARE(runs[0].count, 2);
		items.insert(4, Item(4, Data::MessageKind::Photo));
		runs = History::GroupAlbums(items, false, 2);
		QCOMPARE(int(runs.size()), 2);
		QCOMPARE(runs[1].from, 3);

		// A file in the middle, another sender in a group, my side.
		auto mixed = QVector<Data::Message>{
			Item(1), Item(2, Data::MessageKind::File), Item(3),
		};
		QVERIFY(History::GroupAlbums(mixed, false, 10).empty());
		auto group = QVector<Data::Message>{
			Item(1, Data::MessageKind::Photo, QString::fromLatin1(kAlbum), false, u"a"_s),
			Item(2, Data::MessageKind::Photo, QString::fromLatin1(kAlbum), false, u"b"_s),
		};
		QVERIFY(History::GroupAlbums(group, true, 10).empty());
		QCOMPARE(int(History::GroupAlbums(group, false, 10).size()), 1);

		// My items still in the outbox are one bubble already.
		auto local = QVector<Data::Message>{ Item(0), Item(0) };
		local[0].mine = local[1].mine = true;
		QCOMPARE(int(History::GroupAlbums(local, false, 10).size()), 1);
		local[0].mine = local[1].mine = false;
		QVERIFY(History::GroupAlbums(local, false, 10).empty());
	}

	void legacyClientMessageIds() {
		auto a = Item(1, Data::MessageKind::Photo, QString());
		auto b = Item(2, Data::MessageKind::Photo, QString());
		a.clientMessageId = u"11111111-2222-3333-a1b4-555555555555"_s;
		b.clientMessageId = u"11111111-2222-3333-a1b4-666666666666"_s;
		QCOMPARE(History::AlbumKey(a), History::AlbumKey(b));
		QCOMPARE(int(History::GroupAlbums({ a, b }, false, 10).size()), 1);
		b.clientMessageId = u"11111111-2222-3333-4444-666666666666"_s;
		QVERIFY(History::AlbumKey(b).isEmpty());
		// The album id wins, in any case of letters.
		a.album = u"ABC"_s;
		QCOMPARE(History::AlbumKey(a), u"u:abc"_s);
	}

	void head() {
		auto items = QVector<Data::Message>{ Item(1), Item(2), Item(3) };
		items[1].text = u"caption"_s;
		items[1].captionAbove = true;
		auto head = History::AlbumHeadMessage(items);
		QCOMPARE(head.seq, qint64(1));
		QCOMPARE(head.key(), items[0].key());
		QCOMPARE(head.text, u"caption"_s);
		QVERIFY(head.captionAbove);
		QCOMPARE(head.createdAt, items[2].createdAt);
		QCOMPARE(History::AlbumCaptionIndex(items), 1);

		for (auto &item : items) {
			item.mine = true;
			item.status = Data::MessageStatus::Read;
		}
		items[2].status = Data::MessageStatus::Sending;
		QCOMPARE(History::AlbumHeadMessage(items).status, Data::MessageStatus::Sending);
		items[0].status = Data::MessageStatus::Failed;
		QCOMPARE(History::AlbumHeadMessage(items).status, Data::MessageStatus::Failed);
		items[0].status = items[2].status = Data::MessageStatus::Read;
		QCOMPARE(History::AlbumHeadMessage(items).status, Data::MessageStatus::Read);
		items[1].text.clear();
		QVERIFY(!History::AlbumHeadMessage(items).captionAbove);
		QCOMPARE(History::AlbumCaptionIndex(items), -1);
	}

	void sendPlan() {
		const auto V = AlbumFamily::Visual;
		const auto F = AlbumFamily::File;
		auto plan = History::PlanAlbumSend({ V, V, V }, true, 10);
		QCOMPARE(plan.albumCount, 1);
		QCOMPARE(plan.albums, (std::vector<int>{ 0, 0, 0 }));
		QVERIFY(plan.captionOnFirst);

		// Twelve pictures with ten at most: ten and two.
		plan = History::PlanAlbumSend(std::vector<AlbumFamily>(12, V), true, 10);
		QCOMPARE(plan.albumCount, 2);
		QCOMPARE(plan.albums[9], 0);
		QCOMPARE(plan.albums[10], 1);

		// Families do not mix; a lone file goes on its own; the caption of
		// a mixed set is a message of its own.
		plan = History::PlanAlbumSend({ V, V, F, V }, true, 10);
		QCOMPARE(plan.albums, (std::vector<int>{ 0, 0, -1, -1 }));
		QVERIFY(!plan.captionOnFirst);

		plan = History::PlanAlbumSend({ V, V }, false, 10);
		QCOMPARE(plan.albumCount, 0);
		QCOMPARE(plan.albums, (std::vector<int>{ -1, -1 }));
		QVERIFY(plan.captionOnFirst);

		plan = History::PlanAlbumSend({ F, F }, true, 10);
		QCOMPARE(plan.albumCount, 1);
		QVERIFY(!plan.captionOnFirst);
	}

	void forwarding() {
		const auto other = u"9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d"_s;
		const auto messages = QVector<Data::Message>{
			Item(1), Item(2),
			Item(3, Data::MessageKind::Text, QString()),
			Item(4, Data::MessageKind::Photo, other),
			Item(5, Data::MessageKind::Photo, other),
			Item(6, Data::MessageKind::Photo, other),
		};
		QCOMPARE(
			History::ForwardAlbumGroups(messages, 10),
			(std::vector<int>{ 0, 0, -1, 1, 1, 1 }));
		QCOMPARE(
			History::ForwardAlbumGroups(messages, 2),
			(std::vector<int>{ 0, 0, -1, 1, 1, -1 }));
	}
};

QTEST_GUILESS_MAIN(TestHistoryAlbum)
#include "test_history_album.moc"
