#include "media/media_playlist.h"

#include <algorithm>

namespace Media {

RepeatMode NextRepeatMode(RepeatMode mode) {
	switch (mode) {
	case RepeatMode::Off: return RepeatMode::All;
	case RepeatMode::All: return RepeatMode::One;
	case RepeatMode::One: return RepeatMode::Off;
	}
	return RepeatMode::Off;
}

void PlayOrder::reset(int count, int current) {
	_count = std::max(count, 0);
	rebuild((current >= 0 && current < _count) ? current : (_count ? 0 : -1));
}

void PlayOrder::rebuild(int current) {
	_order.clear();
	_order.reserve(_count);
	if (_shuffled) {
		// The song playing first, the rest in a random order after it.
		auto rest = QVector<int>();
		for (auto i = 0; i != _count; ++i) {
			if (i != current) {
				rest.push_back(i);
			}
		}
		std::shuffle(rest.begin(), rest.end(), _random);
		if (current >= 0) {
			_order.push_back(current);
		}
		_order += rest;
	} else {
		for (auto i = 0; i != _count; ++i) {
			_order.push_back(_reversed ? (_count - 1 - i) : i);
		}
	}
	_position = (current >= 0) ? int(_order.indexOf(current)) : -1;
}

void PlayOrder::append(int count) {
	if (count <= 0) {
		return;
	}
	const auto from = _count;
	_count += count;
	if (_shuffled) {
		// Somewhere after the song playing.
		for (auto i = from; i != _count; ++i) {
			const auto low = _position + 1;
			const auto high = int(_order.size());
			auto spread = std::uniform_int_distribution<int>(
				std::min(low, high),
				high);
			_order.insert(spread(_random), i);
		}
	} else if (_reversed) {
		// Oldest first: what is older still goes in front.
		auto front = QVector<int>();
		for (auto i = _count; i != from;) {
			front.push_back(--i);
		}
		_order = front + _order;
		if (_position >= 0) {
			_position += count;
		}
	} else {
		for (auto i = from; i != _count; ++i) {
			_order.push_back(i);
		}
	}
	if (_position < 0 && !_order.isEmpty()) {
		_position = 0;
	}
}

void PlayOrder::prepend(int count) {
	if (count <= 0) {
		return;
	}
	for (auto &index : _order) {
		index += count;
	}
	_count += count;
	if (_shuffled) {
		for (auto i = 0; i != count; ++i) {
			const auto low = _position + 1;
			const auto high = int(_order.size());
			auto spread = std::uniform_int_distribution<int>(
				std::min(low, high),
				high);
			_order.insert(spread(_random), i);
		}
	} else if (_reversed) {
		// Newest last.
		for (auto i = count; i != 0;) {
			_order.push_back(--i);
		}
	} else {
		auto front = QVector<int>();
		for (auto i = 0; i != count; ++i) {
			front.push_back(i);
		}
		_order = front + _order;
		if (_position >= 0) {
			_position += count;
		}
	}
	if (_position < 0 && !_order.isEmpty()) {
		_position = 0;
	}
}

void PlayOrder::setReversed(bool reversed) {
	if (_reversed == reversed) {
		return;
	}
	_reversed = reversed;
	if (!_shuffled) {
		rebuild(current());
	}
}

void PlayOrder::setShuffled(bool shuffled, unsigned seed) {
	if (seed) {
		_random.seed(seed);
	} else if (shuffled && !_shuffled) {
		_random.seed(std::random_device()());
	}
	if (_shuffled == shuffled) {
		return;
	}
	_shuffled = shuffled;
	rebuild(current());
}

int PlayOrder::current() const {
	return (_position >= 0 && _position < int(_order.size()))
		? _order[_position]
		: -1;
}

int PlayOrder::at(int position) const {
	return (position >= 0 && position < int(_order.size()))
		? _order[position]
		: -1;
}

void PlayOrder::setCurrent(int index) {
	const auto position = int(_order.indexOf(index));
	if (position >= 0) {
		_position = position;
	}
}

int PlayOrder::remaining() const {
	return (_position >= 0)
		? std::max(int(_order.size()) - _position - 1, 0)
		: int(_order.size());
}

std::optional<int> PlayOrder::next(Advance advance, RepeatMode repeat) {
	if (_order.isEmpty()) {
		return std::nullopt;
	} else if (advance == Advance::Ended && repeat == RepeatMode::One) {
		return current();
	} else if (_position + 1 < int(_order.size())) {
		return _order[++_position];
	} else if (advance == Advance::Manual || repeat != RepeatMode::Off) {
		// ⏭ on the last song goes round to the first (Telegram).
		_position = 0;
		return _order[_position];
	}
	return std::nullopt;
}

int PlayOrder::previous(RepeatMode repeat) {
	if (_order.isEmpty()) {
		return -1;
	} else if (_position > 0) {
		--_position;
	} else if (repeat == RepeatMode::All) {
		_position = int(_order.size()) - 1;
	} else {
		_position = 0;
	}
	return _order[_position];
}

} // namespace Media
