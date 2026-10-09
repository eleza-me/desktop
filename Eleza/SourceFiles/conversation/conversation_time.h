// Time math of the anonymous conversations: the expiry ring, the «N ساعت مانده»
// captions, the radar's clock and the duration choices. No widgets here (the unit
// tests compile this file alone).
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Conversation {

// A topic stays open for a day unless the speaker chose less; the open list
// does not carry the creation time, so the ring is drawn against this window
// (as the web client does).
inline constexpr auto kDefaultWindowSeconds = qint64(24 * 3600);
inline constexpr auto kFinalWindowMs = qint64(3600) * 1000;

struct Remaining {
	enum class Unit {
		Ended,
		Minutes,
		Hours,
		Days,
	};
	Unit unit = Unit::Ended;
	int value = 0;

	friend bool operator==(const Remaining &, const Remaining &) = default;
};

// What is left until `expiresAt` (unix seconds) at `nowMs`: minutes are rounded
// up, hours and days down; days start at two whole days.
[[nodiscard]] Remaining RemainingParts(qint64 expiresAt, qint64 nowMs);
// «۲۳ ساعت مانده» / «۴۵ دقیقه مانده» / «تمام شد».
[[nodiscard]] QString RemainingText(qint64 expiresAt, qint64 nowMs);
// «۰۴:۵۹ مانده» (the radar).
[[nodiscard]] QString ClockRemaining(qint64 expiresAt, qint64 nowMs);
// «۴ ساعت و ۵۹ دقیقه» / «۵۹ دقیقه» (the matched radar).
[[nodiscard]] QString LongRemaining(qint64 expiresAt, qint64 nowMs);

[[nodiscard]] bool Expired(qint64 expiresAt, qint64 nowMs);
// The part of the ring that is still filled, 0..1.
[[nodiscard]] double ExpiryFraction(
	qint64 expiresAt,
	qint64 createdAt,
	qint64 nowMs);
// The last hour: the ring turns amber.
[[nodiscard]] bool ExpiryFinal(qint64 expiresAt, qint64 nowMs);
// Milliseconds until the caption or the look of any of the cards changes
// next (the next whole minute of the soonest one), clamped to [1 s, 30 s]:
// the list repaints on a local timer, the network is not involved.
[[nodiscard]] int NextTickDelay(const QList<qint64> &expiresAt, qint64 nowMs);

// «۱ روز» / «۶ ساعت» / «۳۰ دقیقه» for a server duration like "24h".
[[nodiscard]] QString DurationTitle(const QString &duration);
// The choices, longest first.
[[nodiscard]] QStringList SortedDurations(QStringList durations);
// The duration selected by default: the user's preference when it is one of
// the choices, else the longest one, else `fallback`.
[[nodiscard]] QString DefaultDuration(
	const QStringList &sorted,
	const QString &preferred,
	const QString &fallback);

// The topic as it will be sent, and whether it may be sent.
enum class TopicProblem {
	None,
	Empty,
	TooLong,
};
[[nodiscard]] QString NormalizedTopic(const QString &text);
[[nodiscard]] TopicProblem CheckTopic(const QString &text, int maxChars);

} // namespace Conversation
