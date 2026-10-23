// Media::PlayOrder — the order the songs of the player's queue play in, as in
// Telegram, without widgets or audio (tests/app/test_media_playlist.cpp).
//
// The queue is what the player was handed: a chat's songs newest first (⏭ is
// the older song above in the chat), older pages joining at its end. The
// order is that queue, reversed (oldest first), or shuffled from the song
// playing (songs added later play after it). ⏭ on the last song goes round
// to the first even without repeat; only a song that ends by itself stops
// there (repeat «all» goes round, repeat «one» plays it again).
//
//   auto order = Media::PlayOrder();
//   order.reset(count, clicked);
//   if (const auto next = order.next(Media::PlayOrder::Advance::Ended, repeat)) { play(*next); }
//   order.append(older.size());            // a page of older songs (queue indices count up)
#pragma once

#include <QtCore/QVector>

#include <optional>
#include <random>

namespace Media {

enum class RepeatMode {
	Off,
	All,
	One,
};

[[nodiscard]] RepeatMode NextRepeatMode(RepeatMode mode); // off → all → one → off

class PlayOrder final {
public:
	enum class Advance {
		Ended, // the song played to its end
		Manual, // ⏭
	};

	// `count` songs, the one at queue index `current` playing.
	void reset(int count, int current);
	// `count` songs joined the end of the queue (older ones).
	void append(int count);
	// `count` songs joined the front of the queue (newer ones): every queue
	// index moves up by `count`.
	void prepend(int count);

	void setReversed(bool reversed);
	[[nodiscard]] bool reversed() const {
		return _reversed;
	}
	// `seed` makes the shuffle repeatable (tests).
	void setShuffled(bool shuffled, unsigned seed = 0);
	[[nodiscard]] bool shuffled() const {
		return _shuffled;
	}

	[[nodiscard]] int size() const {
		return int(_order.size());
	}
	// The queue index of the song playing (-1: none).
	[[nodiscard]] int current() const;
	// The queue index at a place of the order.
	[[nodiscard]] int at(int position) const;
	[[nodiscard]] int position() const {
		return _position;
	}
	// Makes the song at queue index `index` the one playing.
	void setCurrent(int index);
	// The queue indices in the order they play.
	[[nodiscard]] const QVector<int> &order() const {
		return _order;
	}
	// How many songs come after the one playing (to page older ones in
	// before the end is reached).
	[[nodiscard]] int remaining() const;

	// The queue index to play next, nullopt: stop. Moves the position.
	[[nodiscard]] std::optional<int> next(Advance advance, RepeatMode repeat);
	// ⏮: the song before; at the first one: the last with repeat «all»,
	// else the first again. Moves the position.
	[[nodiscard]] int previous(RepeatMode repeat);
	// Whether ⏭ / ⏮ lead anywhere but the song playing.
	[[nodiscard]] bool hasOthers() const {
		return _order.size() > 1;
	}

private:
	void rebuild(int current);

	QVector<int> _order;
	int _position = -1;
	int _count = 0;
	bool _reversed = false;
	bool _shuffled = false;
	std::mt19937 _random;

};

} // namespace Media
