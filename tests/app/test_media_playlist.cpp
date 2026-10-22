// The order of the player's songs (media/media_playlist.h): a chat's songs
// newest first, ⏭ going round at the end, repeat, reverse, shuffle and the
// pages of older songs joining while one plays.
#include "media/media_playlist.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace Media;
using Advance = PlayOrder::Advance;

class TestMediaPlaylist final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void newestFirstAndTheEnd() {
		// Queue: index 0 is the newest song of the chat; the user clicked
		// the second newest.
		auto order = PlayOrder();
		order.reset(4, 1);
		QCOMPARE(order.current(), 1);
		QCOMPARE(order.remaining(), 2);
		// A song that ended goes on to the older one above it in the chat.
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::optional<int>(2));
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::optional<int>(3));
		// The last one ending stops there…
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::nullopt);
		QCOMPARE(order.current(), 3);
		// …while ⏭ on it goes round to the first, without repeat too.
		QCOMPARE(order.next(Advance::Manual, RepeatMode::Off), std::optional<int>(0));
		QCOMPARE(order.current(), 0);
	}

	void repeat() {
		auto order = PlayOrder();
		order.reset(3, 2);
		QCOMPARE(order.next(Advance::Ended, RepeatMode::All), std::optional<int>(0));
		// Repeat one plays the same song when it ends, ⏭ still moves on.
		QCOMPARE(order.next(Advance::Ended, RepeatMode::One), std::optional<int>(0));
		QCOMPARE(order.next(Advance::Manual, RepeatMode::One), std::optional<int>(1));
		// ⏮ at the first song: the last one with repeat «all», else it.
		QCOMPARE(order.previous(RepeatMode::Off), 0);
		QCOMPARE(order.previous(RepeatMode::Off), 0);
		QCOMPARE(order.previous(RepeatMode::All), 2);
		QCOMPARE(NextRepeatMode(RepeatMode::Off), RepeatMode::All);
		QCOMPARE(NextRepeatMode(RepeatMode::All), RepeatMode::One);
		QCOMPARE(NextRepeatMode(RepeatMode::One), RepeatMode::Off);
	}

	void olderPagesJoin() {
		auto order = PlayOrder();
		order.reset(2, 1);
		QCOMPARE(order.remaining(), 0);
		// A page of three older songs while the last one plays.
		order.append(3);
		QCOMPARE(order.current(), 1);
		QCOMPARE(order.remaining(), 3);
		QCOMPARE(order.order(), QVector<int>({ 0, 1, 2, 3, 4 }));
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::optional<int>(2));
		// Newer songs at the front move every index; the song playing stays.
		order.prepend(2);
		QCOMPARE(order.current(), 4);
		QCOMPARE(order.order(), QVector<int>({ 0, 1, 2, 3, 4, 5, 6 }));
	}

	void reversed() {
		auto order = PlayOrder();
		order.reset(4, 1);
		order.setReversed(true);
		// Oldest first: ⏭ goes to the newer song.
		QCOMPARE(order.order(), QVector<int>({ 3, 2, 1, 0 }));
		QCOMPARE(order.current(), 1);
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::optional<int>(0));
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::nullopt);
		// Older songs paged in go in front of the oldest.
		order.append(2);
		QCOMPARE(order.order(), QVector<int>({ 5, 4, 3, 2, 1, 0 }));
		QCOMPARE(order.current(), 0);
		order.setReversed(false);
		QCOMPARE(order.order(), QVector<int>({ 0, 1, 2, 3, 4, 5 }));
		QCOMPARE(order.current(), 0);
	}

	void shuffled() {
		auto order = PlayOrder();
		order.reset(6, 3);
		order.setShuffled(true, 7);
		// The song playing first, every song once.
		QCOMPARE(order.current(), 3);
		QCOMPARE(order.position(), 0);
		auto sorted = order.order();
		std::sort(sorted.begin(), sorted.end());
		QCOMPARE(sorted, QVector<int>({ 0, 1, 2, 3, 4, 5 }));
		const auto next = order.next(Advance::Ended, RepeatMode::Off);
		QVERIFY(next.has_value() && *next != 3);
		// Songs added later play after the one playing.
		order.append(2);
		QCOMPARE(order.size(), 8);
		const auto position = order.position();
		for (auto i = 0; i <= position; ++i) {
			QVERIFY(order.at(i) < 6);
		}
		// Back to the queue's order, at the same song.
		const auto playing = order.current();
		order.setShuffled(false);
		QCOMPARE(order.current(), playing);
		QCOMPARE(order.order(), QVector<int>({ 0, 1, 2, 3, 4, 5, 6, 7 }));
	}

	void emptyAndSingle() {
		auto order = PlayOrder();
		order.reset(0, 0);
		QCOMPARE(order.current(), -1);
		QCOMPARE(order.next(Advance::Manual, RepeatMode::All), std::nullopt);
		QCOMPARE(order.previous(RepeatMode::All), -1);
		order.reset(1, 5);
		QCOMPARE(order.current(), 0);
		QVERIFY(!order.hasOthers());
		QCOMPARE(order.next(Advance::Ended, RepeatMode::Off), std::nullopt);
		QCOMPARE(order.next(Advance::Manual, RepeatMode::Off), std::optional<int>(0));
		order.setCurrent(9); // not in the queue: nothing changes
		QCOMPARE(order.current(), 0);
	}
};

QTEST_GUILESS_MAIN(TestMediaPlaylist)
#include "test_media_playlist.moc"
