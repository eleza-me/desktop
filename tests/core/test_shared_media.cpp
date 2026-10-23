// The shared media of a chat or a group (data/data_shared_media.h): the
// counts of the tabs, which tabs show, how the server's pages and the cached
// rows make one list, and the media pages on the wire.
#include "data/data_shared_media.h"

#include "test_harness.h"

#include <QtCore/QJsonArray>
#include <QtTest/QtTest>

using namespace Data;

namespace {

using Seqs = QVector<qint64>;
using Tabs = QVector<SharedMediaTab>;
using Flags = std::array<bool, kSharedMediaTabs>;

[[nodiscard]] QJsonObject View(qint64 seq, const QString &kind) {
	return QJsonObject{
		{ u"seq"_s, seq },
		{ u"kind"_s, kind },
		{ u"sender"_s, u"u_sara"_s },
		{ u"attachment"_s, u"f%1"_s.arg(seq) },
		{ u"created_at"_s, 1760000000000 + seq },
	};
}

} // namespace

class TestSharedMedia final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void countsAreTheTabs() {
		const auto counts = SharedMediaCounts::FromJson(QJsonObject{
			{ u"photo_video"_s, 10000 },
			{ u"file"_s, 4 },
			{ u"link"_s, 17 },
			{ u"audio"_s, 0 },
			{ u"voice"_s, 31 },
			{ u"gif"_s, 2 },
			{ u"video_note"_s, 3 },
			{ u"capped"_s, QJsonArray{ u"photo_video"_s, u"video_note"_s, u"future"_s } },
		});
		QVERIFY(counts.has_value());
		QCOMPARE(counts->of(SharedMediaTab::Media), 10000);
		QCOMPARE(counts->of(SharedMediaTab::Files), 4);
		QCOMPARE(counts->of(SharedMediaTab::Links), 17);
		QCOMPARE(counts->of(SharedMediaTab::Music), 0);
		// Voice messages and round videos are one tab.
		QCOMPARE(counts->of(SharedMediaTab::Voice), 34);
		QCOMPARE(counts->of(SharedMediaTab::Gifs), 2);
		QVERIFY(counts->cappedAt(SharedMediaTab::Media));
		QVERIFY(counts->cappedAt(SharedMediaTab::Voice));
		QVERIFY(!counts->cappedAt(SharedMediaTab::Files));
		// Negative numbers are nothing; an answer without the counts is none.
		const auto odd = SharedMediaCounts::FromJson(QJsonObject{
			{ u"photo_video"_s, -5 },
		});
		QVERIFY(odd.has_value());
		QCOMPARE(odd->of(SharedMediaTab::Media), 0);
		QVERIFY(!SharedMediaCounts::FromJson(QJsonObject{ { u"file"_s, 1 } }));
	}

	void visibleTabs() {
		auto counts = SharedMediaCounts();
		counts.count[int(SharedMediaTab::Files)] = 3;
		counts.count[int(SharedMediaTab::Gifs)] = 1;
		auto rows = Flags{};
		auto unknown = Flags{};
		// Counted tabs only, in the order of the screen.
		QCOMPARE(
			SharedMediaVisibleTabs(counts, rows, unknown),
			(Tabs{ SharedMediaTab::Files, SharedMediaTab::Gifs }));
		// A tab with cached rows shows even when the server counts none
		// (links from before the server's release).
		rows[int(SharedMediaTab::Links)] = true;
		QCOMPARE(
			SharedMediaVisibleTabs(counts, rows, unknown),
			(Tabs{ SharedMediaTab::Files, SharedMediaTab::Links, SharedMediaTab::Gifs }));
		// Without counts: what has rows and what is still being read.
		unknown[int(SharedMediaTab::Music)] = true;
		QCOMPARE(
			SharedMediaVisibleTabs(std::nullopt, rows, unknown),
			(Tabs{ SharedMediaTab::Links, SharedMediaTab::Music }));
		// Never empty.
		QCOMPARE(
			SharedMediaVisibleTabs(SharedMediaCounts(), Flags{}, Flags{}),
			(Tabs{ SharedMediaTab::Media }));
	}

	void mergeServerAndCache() {
		// Before the server answered: the cache as it is, newest first.
		QCOMPARE(MergeSharedMedia({}, false, { 3, 9, 5, 9 }, false), (Seqs{ 9, 5, 3 }));
		// The server is the truth for what it covers: 7 is gone (deleted
		// or cleared), 12 arrived after the page.
		QCOMPARE(
			MergeSharedMedia({ 10, 8, 4 }, true, { 12, 10, 7, 4, 2 }, false),
			(Seqs{ 12, 10, 8, 4 }));
		// The server has nothing: nothing cached stays.
		QCOMPARE(MergeSharedMedia({}, true, { 5, 3 }, false), Seqs{});
		// Links: every cached row joins (the server knows only new ones).
		QCOMPARE(
			MergeSharedMedia({ 10, 8 }, true, { 7, 10, 1 }, true),
			(Seqs{ 10, 8, 7, 1 }));
		// No repeats, nothing without a seq.
		QCOMPARE(MergeSharedMedia({ 8, 8, 0 }, true, { 8 }, false), (Seqs{ 8 }));
	}

	void matchesAndLinks() {
		auto message = Message();
		message.seq = 5;
		message.kind = MessageKind::Text;
		message.text = u"see https://eleza.me/x?y=1 now"_s;
		QVERIFY(SharedMediaMatches(SharedMediaTab::Links, message));
		QCOMPARE(SharedMediaLink(message), u"https://eleza.me/x?y=1"_s);
		QVERIFY(!SharedMediaMatches(SharedMediaTab::Media, message));
		message.text = u"no address"_s;
		QVERIFY(!SharedMediaMatches(SharedMediaTab::Links, message));
		auto preview = LinkPreview();
		preview.url = u"https://example.com"_s;
		message.linkPreview = preview;
		QCOMPARE(SharedMediaLink(message), u"https://example.com"_s);
		message.kind = MessageKind::VideoNote;
		QVERIFY(SharedMediaMatches(SharedMediaTab::Voice, message));
		message.kind = MessageKind::Gif;
		QVERIFY(SharedMediaMatches(SharedMediaTab::Gifs, message));
		QVERIFY(!SharedMediaMatches(SharedMediaTab::Media, message));
		// A message still being sent is not listed.
		message.seq = 0;
		QVERIFY(!SharedMediaMatches(SharedMediaTab::Gifs, message));
		QCOMPARE(SharedMediaKinds(SharedMediaTab::Media), (QStringList{ u"photo"_s, u"video"_s }));
		QCOMPARE(SharedMediaKinds(SharedMediaTab::Voice), (QStringList{ u"voice"_s, u"video_note"_s }));
		QVERIFY(SharedMediaIsGrid(SharedMediaTab::Gifs));
		QVERIFY(!SharedMediaIsGrid(SharedMediaTab::Music));
	}

	void unsupported() {
		const auto error = [](int status, const QString &code) {
			return Api::Error::Local(status, code, QString());
		};
		QVERIFY(SharedMediaUnsupported(error(404, u"not_found"_s)));
		QVERIFY(SharedMediaUnsupported(error(404, QString())));
		QVERIFY(SharedMediaUnsupported(error(404, u"chat.media_not_supported"_s)));
		QVERIFY(!SharedMediaUnsupported(error(404, u"chat.not_found"_s)));
		QVERIFY(!SharedMediaUnsupported(error(404, u"group.not_found"_s)));
		QVERIFY(!SharedMediaUnsupported(error(503, u"service.unavailable"_s)));
		QVERIFY(!SharedMediaUnsupported(Api::Error::Offline()));
	}

	void pagesOnTheWire() {
		auto harness = Test::Harness();
		harness.on(u"chat.media"_s, [](const Test::Harness::Call &) {
			return Test::Harness::Reply::Ok(QJsonObject{
				{ u"messages"_s, QJsonArray{
					View(37, u"video"_s),
					View(41, u"photo"_s),
					View(41, u"photo"_s),
				} },
				{ u"has_more_before"_s, true },
			});
		});
		auto page = std::optional<SharedMediaPage>();
		auto guard = QObject();
		LoadSharedMedia(
			harness.session.get(),
			u"c_unknown"_s,
			SharedMediaKinds(SharedMediaTab::Media),
			50,
			30,
			base::Guard(&guard),
			[&](const SharedMediaPage &result) { page = result; });
		QVERIFY(Test::WaitFor([&] { return page.has_value(); }));
		const auto body = harness.bodies(u"chat.media"_s).back();
		QCOMPARE(body.value("chat"_L1).toString(), u"c_unknown"_s);
		QCOMPARE(body.value("kinds"_L1).toString(), u"photo,video"_s);
		QCOMPARE(body.value("before"_L1).toString(), u"50"_s);
		QCOMPARE(body.value("limit"_L1).toString(), u"30"_s);
		// Newest first, once each, for a chat that is not in the list.
		QCOMPARE(page->messages.size(), 2);
		QCOMPARE(page->messages[0].seq, 41);
		QCOMPARE(page->messages[1].seq, 37);
		QCOMPARE(page->messages[0].chat, u"c_unknown"_s);
		QVERIFY(page->hasMoreBefore);
		// Nothing of it went into the chat's history.
		QVERIFY(harness.messages().history(u"c_unknown"_s).items.isEmpty());

		// A group asks its own route; an empty page is the end.
		harness.on(u"groups.media"_s, [](const Test::Harness::Call &) {
			return Test::Harness::Reply::Ok(QJsonObject{
				{ u"messages"_s, QJsonArray() },
				{ u"has_more_before"_s, true },
			});
		});
		page = std::nullopt;
		LoadSharedMedia(
			harness.session.get(),
			u"j18P1DKWNqdem"_s,
			{ u"audio"_s },
			0,
			0,
			base::Guard(&guard),
			[&](const SharedMediaPage &result) { page = result; });
		QVERIFY(Test::WaitFor([&] { return page.has_value(); }));
		const auto groupBody = harness.bodies(u"groups.media"_s).back();
		QCOMPARE(groupBody.value("group"_L1).toString(), u"j18P1DKWNqdem"_s);
		QVERIFY(!groupBody.contains("before"_L1));
		QVERIFY(!groupBody.contains("limit"_L1));
		QVERIFY(!page->hasMoreBefore);
	}

	void countsOnTheWire() {
		auto harness = Test::Harness();
		harness.on(u"groups.media.counts"_s, [](const Test::Harness::Call &) {
			return Test::Harness::Reply::Ok(QJsonObject{
				{ u"photo_video"_s, 2 },
				{ u"file"_s, 0 },
				{ u"link"_s, 0 },
				{ u"audio"_s, 5 },
				{ u"voice"_s, 0 },
				{ u"gif"_s, 0 },
				{ u"video_note"_s, 0 },
			});
		});
		harness.on(u"chat.media.counts"_s, [](const Test::Harness::Call &) {
			return Test::Harness::Reply::Fail(404, u"chat.not_found"_s);
		});
		auto guard = QObject();
		auto counts = std::optional<SharedMediaCounts>();
		LoadSharedMediaCounts(
			harness.session.get(),
			u"j18P1DKWNqdem"_s,
			base::Guard(&guard),
			[&](const SharedMediaCounts &result) { counts = result; });
		QVERIFY(Test::WaitFor([&] { return counts.has_value(); }));
		QCOMPARE(harness.bodies(u"groups.media.counts"_s).back()
			.value("group"_L1).toString(), u"j18P1DKWNqdem"_s);
		QCOMPARE(counts->of(SharedMediaTab::Music), 5);
		auto error = std::optional<Api::Error>();
		LoadSharedMediaCounts(
			harness.session.get(),
			u"c1"_s,
			base::Guard(&guard),
			[&](const SharedMediaCounts &) {},
			[&](const Api::Error &result) { error = result; });
		QVERIFY(Test::WaitFor([&] { return error.has_value(); }));
		QCOMPARE(error->code, u"chat.not_found"_s);
		QVERIFY(!SharedMediaUnsupported(*error));
	}
};

QTEST_GUILESS_MAIN(TestSharedMedia)
#include "test_shared_media.moc"
