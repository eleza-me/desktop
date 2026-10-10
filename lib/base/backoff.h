// Exponential backoff with jitter: base * 2^attempt, capped, multiplied by
// a random factor in [1 - jitter, 1 + jitter].
#pragma once

#include "base/random.h"

#include <algorithm>
#include <cmath>

namespace base {

class ExponentialBackoff {
public:
	// Delays in seconds. Defaults are the realtime reconnect policy (0.5 s -> 30 s, +-30%).
	explicit ExponentialBackoff(
		double first = 0.5,
		double max = 30.,
		double jitter = 0.3,
		int maxAttempt = 8)
	: _first(first)
	, _max(max)
	, _jitter(jitter)
	, _maxAttempt(maxAttempt) {
	}

	// The next delay in seconds; advances the attempt counter.
	[[nodiscard]] double next() {
		const auto value = nextWithoutJitter();
		return value * (1. - _jitter + RandomUnit() * 2. * _jitter);
	}
	[[nodiscard]] double nextWithoutJitter() {
		const auto value = std::min(_max, _first * std::pow(2., double(_attempt)));
		_attempt = std::min(_attempt + 1, _maxAttempt);
		return value;
	}
	void reset() {
		_attempt = 0;
	}
	[[nodiscard]] int attempt() const {
		return _attempt;
	}

private:
	double _first = 0.5;
	double _max = 30.;
	double _jitter = 0.3;
	int _maxAttempt = 8;
	int _attempt = 0;

};

} // namespace base
