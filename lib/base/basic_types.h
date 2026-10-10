// Common includes and aliases used across the non-GUI code.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

// Callback alias: Fn<void(int)>.
template <typename Signature>
using Fn = std::function<Signature>;

// u"text"_s (QString), "text"_L1 (QLatin1StringView), "text"_ba (QByteArray).
using namespace Qt::Literals::StringLiterals;

using TimeId = qint64; // unix time, seconds
using TimeMs = qint64; // unix time, milliseconds

namespace base {

// Numbers from the wire are doubles (JSON) and a cast of one that does not
// fit the integer type is undefined behaviour: these saturate (NaN -> 0).
[[nodiscard]] constexpr qint64 SafeInt64(double value) {
	return !(value == value)
		? 0
		: (value >= 9223372036854775807.)
		? std::numeric_limits<qint64>::max()
		: (value <= -9223372036854775808.)
		? std::numeric_limits<qint64>::min()
		: qint64(value);
}

[[nodiscard]] constexpr int SafeInt(double value) {
	return !(value == value)
		? 0
		: (value >= 2147483647.)
		? std::numeric_limits<int>::max()
		: (value <= -2147483648.)
		? std::numeric_limits<int>::min()
		: int(value);
}

[[nodiscard]] constexpr int SafeInt(qint64 value) {
	return (value > std::numeric_limits<int>::max())
		? std::numeric_limits<int>::max()
		: (value < std::numeric_limits<int>::min())
		? std::numeric_limits<int>::min()
		: int(value);
}

} // namespace base
