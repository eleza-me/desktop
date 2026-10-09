#include "conversation/conversation_time.h"

#include "lang/lang_format.h"
#include "lang_auto.h"

#include <QtCore/QRegularExpression>

#include <algorithm>

namespace Conversation {
namespace {

struct ParsedDuration {
	qint64 count = 0;
	QChar unit;

	[[nodiscard]] bool valid() const {
		return count > 0;
	}
	[[nodiscard]] qint64 seconds() const {
		return (unit == QLatin1Char('d'))
			? (count * 86400)
			: (unit == QLatin1Char('h'))
			? (count * 3600)
			: (count * 60);
	}
};

[[nodiscard]] ParsedDuration ParseDuration(const QString &duration) {
	static const auto kExpression = QRegularExpression(
		QStringLiteral("^(\\d+)([hmd])$"));
	const auto match = kExpression.match(duration);
	if (!match.hasMatch()) {
		return {};
	}
	return { match.captured(1).toLongLong(), match.captured(2).at(0) };
}

[[nodiscard]] QString TwoDigits(qint64 value) {
	return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
}

} // namespace

Remaining RemainingParts(qint64 expiresAt, qint64 nowMs) {
	const auto left = expiresAt * 1000 - nowMs;
	if (left <= 0) {
		return {};
	}
	const auto minutes = (left + 59'999) / 60'000;
	if (minutes >= 60 * 24 * 2) {
		return { Remaining::Unit::Days, int(minutes / (60 * 24)) };
	} else if (minutes >= 60) {
		return { Remaining::Unit::Hours, int(minutes / 60) };
	}
	return { Remaining::Unit::Minutes, int(minutes) };
}

QString RemainingText(qint64 expiresAt, qint64 nowMs) {
	const auto parts = RemainingParts(expiresAt, nowMs);
	switch (parts.unit) {
	case Remaining::Unit::Ended:
		return tr::lng_ended(tr::now);
	case Remaining::Unit::Days:
		return tr::lng_conv_days_left(tr::now, lt_count, parts.value);
	case Remaining::Unit::Hours:
		return tr::lng_conv_hours_left(tr::now, lt_count, parts.value);
	case Remaining::Unit::Minutes:
		return tr::lng_conv_minutes_left(tr::now, lt_count, parts.value);
	}
	return QString();
}

QString ClockRemaining(qint64 expiresAt, qint64 nowMs) {
	const auto left = std::max(expiresAt * 1000 - nowMs, qint64(0));
	const auto hours = left / 3'600'000;
	const auto minutes = (left % 3'600'000) / 60'000;
	return tr::lng_conv_time_left(
		tr::now,
		lt_time,
		Lang::Num(TwoDigits(hours) + QLatin1Char(':') + TwoDigits(minutes)));
}

QString LongRemaining(qint64 expiresAt, qint64 nowMs) {
	const auto left = std::max(expiresAt * 1000 - nowMs, qint64(0));
	const auto hours = int(left / 3'600'000);
	const auto minutes = int((left % 3'600'000) / 60'000);
	return (hours > 0)
		? tr::lng_conversation_hours_minutes(
			tr::now,
			lt_hours,
			Lang::Num(hours),
			lt_minutes,
			Lang::Num(minutes))
		: tr::lng_minutes_short(tr::now, lt_count, minutes);
}

bool Expired(qint64 expiresAt, qint64 nowMs) {
	return expiresAt * 1000 <= nowMs;
}

double ExpiryFraction(qint64 expiresAt, qint64 createdAt, qint64 nowMs) {
	const auto total = std::max((expiresAt - createdAt) * 1000, qint64(1));
	const auto left = std::max(expiresAt * 1000 - nowMs, qint64(0));
	return std::min(double(left) / double(total), 1.);
}

bool ExpiryFinal(qint64 expiresAt, qint64 nowMs) {
	const auto left = expiresAt * 1000 - nowMs;
	return (left > 0) && (left <= kFinalWindowMs);
}

int NextTickDelay(const QList<qint64> &expiresAt, qint64 nowMs) {
	constexpr auto kMin = qint64(1000);
	constexpr auto kMax = qint64(30'000);
	auto result = kMax;
	for (const auto expires : expiresAt) {
		const auto left = expires * 1000 - nowMs;
		if (left <= 0) {
			continue;
		}
		// The caption changes when the rounded-up minutes change.
		const auto untilMinute = ((left - 1) % 60'000) + 1;
		result = std::min(result, untilMinute);
	}
	return int(std::clamp(result, kMin, kMax));
}

QString DurationTitle(const QString &duration) {
	const auto parsed = ParseDuration(duration);
	if (!parsed.valid()) {
		return duration;
	} else if (parsed.unit == QLatin1Char('d')) {
		return tr::lng_conversation_days(tr::now, lt_count, int(parsed.count));
	} else if (parsed.unit == QLatin1Char('h')) {
		return (parsed.count >= 24 && parsed.count % 24 == 0)
			? tr::lng_conversation_days(
				tr::now,
				lt_count,
				int(parsed.count / 24))
			: tr::lng_hours_short(tr::now, lt_count, int(parsed.count));
	}
	return tr::lng_minutes_short(tr::now, lt_count, int(parsed.count));
}

QStringList SortedDurations(QStringList durations) {
	const auto seconds = [](const QString &duration) {
		const auto parsed = ParseDuration(duration);
		return parsed.valid() ? parsed.seconds() : qint64(24 * 3600);
	};
	durations.removeDuplicates();
	std::stable_sort(
		durations.begin(),
		durations.end(),
		[&](const QString &a, const QString &b) {
			return seconds(a) > seconds(b);
		});
	return durations;
}

QString DefaultDuration(
		const QStringList &sorted,
		const QString &preferred,
		const QString &fallback) {
	if (!preferred.isEmpty() && sorted.contains(preferred)) {
		return preferred;
	} else if (!sorted.isEmpty()) {
		return sorted.front();
	}
	return fallback;
}

QString NormalizedTopic(const QString &text) {
	return text.trimmed();
}

TopicProblem CheckTopic(const QString &text, int maxChars) {
	const auto topic = NormalizedTopic(text);
	if (topic.isEmpty()) {
		return TopicProblem::Empty;
	} else if (maxChars > 0 && topic.size() > maxChars) {
		return TopicProblem::TooLong;
	}
	return TopicProblem::None;
}

} // namespace Conversation
