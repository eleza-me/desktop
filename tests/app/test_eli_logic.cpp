// The widget-free rules of «الی» (eli/eli_logic.h): album grid geometry, poll
// percentages, the text clamp, compose limits and thread splitting, the
// replies tree, view beacons and the 60 s counts-freshness rule (fake clock).
#include "eli/eli_logic.h"

#include <QtTest/QtTest>

using namespace Eli::Logic;

class TestEliLogic : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

private Q_SLOTS:
	void aspect() {
		QCOMPARE(ClampAspect(0, 0), 1.);
		QCOMPARE(ClampAspect(1200, 900), 1200. / 900.);
		QCOMPARE(ClampAspect(4000, 1000), kMaxAspect);
		QCOMPARE(ClampAspect(1000, 4000), kMinAspect);
	}

	void singleMedia() {
		const auto wide = LayoutMedia({ QSize(1200, 900) }, 400, 2, 0);
		QCOMPARE(wide.tiles.size(), 1);
		QCOMPARE(wide.tiles[0], QRect(0, 0, 400, 300));
		QCOMPARE(wide.height, 300);
		QCOMPARE(wide.more, 0);
		// A tall picture is limited by the height and keeps its proportions.
		const auto tall = LayoutMedia({ QSize(800, 1000) }, 400, 2, 300);
		QCOMPARE(tall.height, 300);
		QCOMPARE(tall.tiles[0], QRect(0, 0, 240, 300));
		// Unknown size: a square.
		QCOMPARE(LayoutMedia({ QSize() }, 320, 2, 0).tiles[0], QRect(0, 0, 320, 320));
		QVERIFY(LayoutMedia({}, 320, 2, 0).tiles.isEmpty());
		QVERIFY(LayoutMedia({ QSize(1, 1) }, 0, 2, 0).tiles.isEmpty());
	}

	void albumGrid() {
		const auto any = QSize(100, 100);
		const auto two = LayoutMedia({ any, any }, 402, 2, 0);
		QCOMPARE(two.tiles.size(), 2);
		QCOMPARE(two.tiles[0], QRect(0, 0, 200, 250));
		QCOMPARE(two.tiles[1], QRect(202, 0, 200, 250));
		QCOMPARE(two.height, 250);

		const auto three = LayoutMedia({ any, any, any }, 402, 2, 0);
		QCOMPARE(three.tiles.size(), 3);
		QCOMPARE(three.height, 226);
		QCOMPARE(three.tiles[0], QRect(0, 0, 200, 226));
		QCOMPARE(three.tiles[1], QRect(202, 0, 200, 112));
		QCOMPARE(three.tiles[2], QRect(202, 114, 200, 112));

		const auto four = LayoutMedia({ any, any, any, any }, 402, 2, 0);
		QCOMPARE(four.tiles.size(), 4);
		QCOMPARE(four.more, 0);
		QCOMPARE(four.tiles[3], QRect(202, 114, 200, 112));

		const auto seven = LayoutMedia(QVector<QSize>(7, any), 402, 2, 0);
		QCOMPARE(seven.tiles.size(), 4);
		QCOMPARE(seven.more, 3);
		// Odd widths: the tiles still cover the whole width without overlap.
		const auto odd = LayoutMedia({ any, any, any, any }, 277, 3, 0);
		QCOMPARE(odd.tiles[1].right(), 276);
		QCOMPARE(odd.tiles[0].right() + 1 + 3, odd.tiles[1].left());
		QCOMPARE(odd.tiles[2].bottom(), odd.height - 1);
		// Limited height.
		QCOMPARE(LayoutMedia({ any, any }, 402, 2, 180).height, 180);
	}

	void pollPercents() {
		QCOMPARE(PollPercents({ 0, 0, 0 }), (QVector<int>{ 0, 0, 0 }));
		QCOMPARE(PollPercents({ 1, 1, 1 }), (QVector<int>{ 34, 33, 33 }));
		QCOMPARE(PollPercents({ 12, 30, 5 }), (QVector<int>{ 25, 64, 11 }));
		QCOMPARE(PollPercents({ 1, 2 }), (QVector<int>{ 33, 67 }));
		QCOMPARE(PollPercents({ 5 }), (QVector<int>{ 100 }));
		QCOMPARE(PollPercents({ 0, 7 }), (QVector<int>{ 0, 100 }));
		QCOMPARE(PollPercents({ -3, 1 }), (QVector<int>{ 0, 100 }));
		QVERIFY(PollPercents({}).isEmpty());
		for (const auto &votes : { QVector<qint64>{ 1, 1, 1, 1, 1, 1 }, QVector<qint64>{ 3, 3, 1 }, QVector<qint64>{ 999, 1, 1, 1 } }) {
			auto sum = 0;
			for (const auto value : PollPercents(votes)) {
				sum += value;
			}
			QCOMPARE(sum, 100);
		}
	}

	void clamp() {
		QCOMPARE(ClampLines(3, 8, 3), (Clamp{ 3, false }));
		QCOMPARE(ClampLines(8, 8, 3), (Clamp{ 8, false }));
		// Hiding one or two lines behind a link is pointless.
		QCOMPARE(ClampLines(10, 8, 3), (Clamp{ 10, false }));
		QCOMPARE(ClampLines(11, 8, 3), (Clamp{ 8, true }));
		QCOMPARE(ClampLines(40, 8, 3), (Clamp{ 8, true }));
		QCOMPARE(ClampLines(40, 0, 3), (Clamp{ 40, false }));
		QCOMPARE(ClampLines(9, 8, 0), (Clamp{ 8, true }));
	}

	void textLength() {
		QCOMPARE(TextLength(QString()), 0);
		QCOMPARE(TextLength(U("سلام")), 4);
		// An emoji outside the BMP is one character, two UTF-16 units.
		const auto emoji = U("a\xF0\x9F\x98\x80z");
		QCOMPARE(emoji.size(), 4);
		QCOMPARE(TextLength(emoji), 3);
		QCOMPARE(Remaining(emoji, 500), 497);
		QCOMPARE(Remaining(QString(510, QChar('x')), 500), -10);
	}

	void readingMinutes() {
		QCOMPARE(ReadingMinutes({}), 1);
		QCOMPARE(ReadingMinutes({ U("یک دو سه") }), 1);
		auto words = QStringList();
		for (auto i = 0; i != 450; ++i) {
			words.push_back(QStringLiteral("w"));
		}
		QCOMPARE(ReadingMinutes({ words.join(' ') }), 3);
		QCOMPARE(ReadingMinutes({ words.mid(0, 180).join(' '), words.mid(0, 180).join('\n') }), 2);
	}

	void composeLimits() {
		const auto limits = ComposeLimits();
		auto draft = ComposeDraft();
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::Empty);
		draft.text = QStringLiteral("   ");
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::Empty);
		draft.text = QStringLiteral("hi");
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);
		QCOMPARE(ComposeType(draft), QStringLiteral("text"));
		draft.text = QString(500, QChar('x'));
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);
		draft.text = QString(501, QChar('x'));
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::TooLong);

		// Media alone is enough; too many is not.
		draft = ComposeDraft();
		draft.mediaCount = 1;
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);
		QCOMPARE(ComposeType(draft), QStringLiteral("media"));
		draft.mediaCount = 11;
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::TooManyMedia);

		// Polls: two to four filled options, never with media.
		draft = ComposeDraft();
		draft.text = QStringLiteral("q?");
		draft.hasPoll = true;
		draft.pollOptions = QStringList{ QStringLiteral("a"), QStringLiteral(" ") };
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::PollOptions);
		draft.pollOptions = QStringList{ QStringLiteral("a"), QStringLiteral("b") };
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);
		QCOMPARE(ComposeType(draft), QStringLiteral("poll"));
		draft.mediaCount = 1;
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::PollWithMedia);

		// Threads: every part within the limit, at most ten parts in all.
		draft = ComposeDraft();
		draft.text = QStringLiteral("one");
		draft.parts = QStringList{ QStringLiteral(" two "), QString(), QStringLiteral("three") };
		QCOMPARE(CleanThreadParts(draft.parts), (QStringList{ QStringLiteral("two"), QStringLiteral("three") }));
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);
		QCOMPARE(ComposeType(draft), QStringLiteral("thread"));
		draft.parts.push_back(QString(501, QChar('y')));
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::PartTooLong);
		draft.parts = QStringList(10, QStringLiteral("p"));
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::TooManyParts);
		draft.parts = QStringList(9, QStringLiteral("p"));
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);

		// An invite needs no text; a quote is a quote whatever it carries.
		draft = ComposeDraft();
		draft.kind = ComposeKind::Invite;
		QCOMPARE(ValidateCompose(draft, limits), ComposeProblem::None);
		QCOMPARE(ComposeType(draft), QStringLiteral("conversation_invite"));
		draft.kind = ComposeKind::Quote;
		draft.mediaCount = 2;
		QCOMPARE(ComposeType(draft), QStringLiteral("quote"));
	}

	void threadSplitting() {
		QVERIFY(SplitIntoThread(QString(), 10, 5).isEmpty());
		QCOMPARE(SplitIntoThread(QStringLiteral(" short "), 10, 5), QStringList{ QStringLiteral("short") });

		// Between words.
		const auto words = SplitIntoThread(QStringLiteral("aaaa bbbb cccc dddd"), 10, 5);
		QCOMPARE(words, (QStringList{ QStringLiteral("aaaa bbbb"), QStringLiteral("cccc dddd") }));

		// At the end of a sentence rather than in the middle of the next one.
		const auto sentences = SplitIntoThread(QStringLiteral("One two three four. Five six seven"), 24, 5);
		QCOMPARE(sentences.first(), QStringLiteral("One two three four."));
		// … unless that would leave a very short part.
		const auto early = SplitIntoThread(QStringLiteral("One two. Three four five six"), 20, 5);
		QCOMPARE(early.first(), QStringLiteral("One two. Three four"));

		// At a paragraph break in the second half.
		const auto paragraphs = SplitIntoThread(QStringLiteral("first line here\nsecond one that is long"), 22, 5);
		QCOMPARE(paragraphs.first(), QStringLiteral("first line here"));

		// A word longer than the limit is cut.
		const auto longWord = SplitIntoThread(QString(25, QChar('x')), 10, 5);
		QCOMPARE(longWord.size(), 3);
		QCOMPARE(longWord[0].size(), 10);
		QCOMPARE(longWord[2].size(), 5);

		// Never more than maxParts: the last one keeps the rest.
		const auto capped = SplitIntoThread(QString(100, QChar('x')), 10, 3);
		QCOMPARE(capped.size(), 3);
		QCOMPARE(capped[2].size(), 80);

		// Every part fits and nothing is lost.
		auto text = QString();
		for (auto i = 0; i != 300; ++i) {
			text += U("واژه") + QString::number(i) + ((i % 17 == 16) ? QStringLiteral(". ") : QStringLiteral(" "));
		}
		const auto parts = SplitIntoThread(text, 500, 10);
		QVERIFY(parts.size() > 1);
		for (const auto &part : parts) {
			QVERIFY(TextLength(part) <= 500);
			QVERIFY(!part.isEmpty());
		}
		auto joined = parts.join(' ');
		QCOMPARE(joined.simplified(), text.simplified());

		// Surrogate pairs are never split.
		const auto emoji = U("\xF0\x9F\x98\x80");
		auto many = QString();
		for (auto i = 0; i != 12; ++i) {
			many += emoji;
		}
		const auto cut = SplitIntoThread(many, 5, 5);
		QCOMPARE(cut.size(), 3);
		QCOMPARE(TextLength(cut[0]), 5);
		QVERIFY(!cut[0].back().isHighSurrogate());
	}

	void repliesTree() {
		const auto node = [](const char *id, const char *to, bool deleted = false) {
			return ReplyNode{ QString::fromLatin1(id), QString::fromLatin1(to), deleted };
		};
		const auto all = QVector<ReplyNode>{
			node("a", "root"),
			node("b", "root"),
			node("a1", "a"),
			node("a2", "a1"), // deeper levels flatten to depth 1
			node("c", "root", true), // deleted without children: hidden
			node("d", "root", true), // deleted with a living child: kept
			node("d1", "d"),
			node("x", "unknown"), // parent not loaded: under the root
		};
		const auto rows = ArrangeReplies(all, QStringLiteral("root"), false);
		auto ids = QStringList();
		for (const auto &row : rows) {
			ids.push_back(row.id + QChar(':') + QString::number(row.depth));
		}
		QCOMPARE(ids.join(' '), QStringLiteral("a:0 a1:1 a2:1 b:0 d:0 d1:1 x:0"));
		QCOMPARE(rows[1].parent, QStringLiteral("a"));
		QCOMPARE(rows[2].parent, QStringLiteral("a1"));
		QVERIFY(rows[0].parent.isEmpty());
		QVERIFY(rows[0].line);
		QVERIFY(rows[1].line);
		QVERIFY(!rows[2].line);
		QVERIFY(!rows[3].line);
		QVERIFY(rows[4].line);

		const auto newest = ArrangeReplies(all, QStringLiteral("root"), true);
		QCOMPARE(newest.first().id, QStringLiteral("x"));
		QCOMPARE(newest.last().id, QStringLiteral("a2"));
		QVERIFY(ArrangeReplies({}, QStringLiteral("root"), false).isEmpty());
	}

	void viewTracker() {
		auto tracker = ViewTracker();
		auto now = qint64(10'000);
		QCOMPARE(tracker.nextCheckIn(now), -1);
		tracker.setVisible({ QStringLiteral("p1"), QStringLiteral("p2") }, now);
		QCOMPARE(tracker.nextCheckIn(now), 1000);
		QVERIFY(tracker.takeSeen(now + 999).isEmpty());

		// p2 scrolled away before its second was up; p3 appeared.
		now += 600;
		tracker.setVisible({ QStringLiteral("p1"), QStringLiteral("p3") }, now);
		QCOMPARE(tracker.nextCheckIn(now), 400);
		now += 400;
		QCOMPARE(tracker.takeSeen(now), QStringList{ QStringLiteral("p1") });
		QCOMPARE(tracker.nextCheckIn(now), 600);
		now += 600;
		QCOMPARE(tracker.takeSeen(now), QStringList{ QStringLiteral("p3") });
		QCOMPARE(tracker.nextCheckIn(now), -1);

		// Seen once: staying on screen or coming back does not report again.
		tracker.setVisible({ QStringLiteral("p1"), QStringLiteral("p2") }, now);
		now += 5000;
		QCOMPARE(tracker.takeSeen(now), QStringList{ QStringLiteral("p2") });
		tracker.setVisible({ QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3") }, now);
		QVERIFY(tracker.takeSeen(now + 60'000).isEmpty());

		// p2's clock restarted when it came back (the first 600 ms did not count).
		tracker.reset();
		tracker.setVisible({ QStringLiteral("p9") }, now);
		tracker.setVisible({}, now + 900);
		tracker.setVisible({ QStringLiteral("p9") }, now + 950);
		QVERIFY(tracker.takeSeen(now + 1100).isEmpty());
		QCOMPARE(tracker.takeSeen(now + 1950), QStringList{ QStringLiteral("p9") });
	}

	void beaconBatching() {
		auto queue = BeaconQueue(5000, 3);
		QCOMPARE(queue.intervalMs(), 5000);
		QVERIFY(queue.empty());
		QVERIFY(queue.takeBatch().isEmpty());
		// The first ids arm the timer; more ids before the flush do not.
		QVERIFY(queue.push({ QStringLiteral("a"), QStringLiteral("b") }));
		QVERIFY(!queue.push({ QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d") }));
		QCOMPARE(queue.takeBatch(), (QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }));
		QVERIFY(!queue.empty());
		QCOMPARE(queue.takeBatch(), QStringList{ QStringLiteral("d") });
		QVERIFY(queue.empty());
		// An id is sent once per run.
		QVERIFY(!queue.push({ QStringLiteral("a"), QStringLiteral("d") }));
		QVERIFY(queue.empty());
		QVERIFY(queue.push({ QStringLiteral("e") }));
	}

	void countsFreshness() {
		auto fresh = CountsFreshness();
		auto now = qint64(1'000'000);
		fresh.noteFresh({ QStringLiteral("p1"), QStringLiteral("p2") }, now);

		// Re-appearing within the minute: the payload's counts are good.
		now += 59'000;
		QVERIFY(fresh.takeStale({ QStringLiteral("p1"), QStringLiteral("p2") }, now).isEmpty());
		now += 1'000;
		QVERIFY(fresh.takeStale({ QStringLiteral("p1") }, now).isEmpty());

		// Older than 60 s: re-read once …
		now += 1;
		QCOMPARE(fresh.takeStale({ QStringLiteral("p1") }, now), QStringList{ QStringLiteral("p1") });
		// … and not again while the answer is on its way or fresh.
		QVERIFY(fresh.takeStale({ QStringLiteral("p1") }, now + 10).isEmpty());
		QVERIFY(fresh.takeStale({ QStringLiteral("p1") }, now + 60'000).isEmpty());
		QCOMPARE(fresh.takeStale({ QStringLiteral("p1"), QStringLiteral("p2") }, now + 60'001), (QStringList{ QStringLiteral("p1"), QStringLiteral("p2") }));

		// A post we never got a payload for is not asked about at first sight.
		QVERIFY(fresh.takeStale({ QStringLiteral("p3") }, now).isEmpty());
		QCOMPARE(fresh.takeStale({ QStringLiteral("p3") }, now + 60'001), QStringList{ QStringLiteral("p3") });

		// A fresh payload (the feed was reloaded) resets the clock.
		fresh.noteFresh({ QStringLiteral("p2") }, now + 200'000);
		QVERIFY(fresh.takeStale({ QStringLiteral("p2") }, now + 250'000).isEmpty());
		fresh.forget(QStringLiteral("p2"));
		QVERIFY(fresh.takeStale({ QStringLiteral("p2") }, now + 900'000).isEmpty());
	}

};

QTEST_GUILESS_MAIN(TestEliLogic)
#include "test_eli_logic.moc"
