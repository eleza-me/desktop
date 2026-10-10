// The widget-free rules of «الی»: geometry of media grids, poll percentages,
// the «نمایش بیشتر» clamp, compose limits and thread splitting, the replies
// tree, view beacons (a card counts as seen after a second on screen) and the
// freshness rule of counts (re-read only when a card re-appears with a copy
// older than a minute). Everything here takes its clock as an argument, so the
// unit tests (tests/app/test_eli_logic.cpp) drive it with a fake one.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QRect>
#include <QtCore/QSet>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace Eli::Logic {

// ---- media

inline constexpr auto kMinAspect = 0.8;
inline constexpr auto kMaxAspect = 1.91;
inline constexpr auto kMaxGridTiles = 4;

// width / height limited to what a card shows (4:5 .. 1.91:1); 1 when unknown.
[[nodiscard]] double ClampAspect(int width, int height);

struct MediaLayout {
	// Tiles in LTR coordinates inside (0, 0, width, height); at most four.
	QVector<QRect> tiles;
	int height = 0;
	// Items that did not get a tile («+N» over the last one).
	int more = 0;

	friend bool operator==(const MediaLayout &, const MediaLayout &) = default;
};

// One item keeps its (clamped) proportions, limited by `maxHeight`; two stand
// side by side; three are one tall tile and two stacked; four or more make a
// 2 x 2 grid.
[[nodiscard]] MediaLayout LayoutMedia(
	const QVector<QSize> &sizes,
	int width,
	int skip,
	int maxHeight);

// ---- polls

// Whole percents that add up to exactly 100 (largest remainder), all zeros
// when nobody voted.
[[nodiscard]] QVector<int> PollPercents(const QVector<qint64> &votes);

// ---- text

struct Clamp {
	int lines = 0; // how many lines to show
	bool clamped = false; // «نمایش بیشتر» is needed

	friend bool operator==(const Clamp &, const Clamp &) = default;
};

// Shows `maxLines` and the link, unless the link would hide fewer than
// `minHidden` lines (then everything is shown).
[[nodiscard]] Clamp ClampLines(int totalLines, int maxLines, int minHidden);

// Length the way the server counts it: Unicode code points.
[[nodiscard]] int TextLength(const QString &text);

// Reading time of a thread, in minutes (180 words a minute, at least one).
[[nodiscard]] int ReadingMinutes(const QStringList &parts);

// ---- compose

struct ComposeLimits {
	int maxText = 500;
	int maxThreadParts = 10;
	int maxMedia = 10;
	int pollMinOptions = 2;
	int pollMaxOptions = 4;
};

enum class ComposeKind {
	New,
	Reply,
	Quote,
	Invite,
	Edit,
};

struct ComposeDraft {
	ComposeKind kind = ComposeKind::New;
	QString text;
	QStringList parts; // the thread parts after the first one
	int mediaCount = 0;
	bool hasPoll = false;
	QStringList pollOptions;
};

enum class ComposeProblem {
	None,
	Empty,
	TooLong,
	PartTooLong,
	TooManyParts,
	TooManyMedia,
	PollOptions,
	PollWithMedia,
};

[[nodiscard]] ComposeProblem ValidateCompose(
	const ComposeDraft &draft,
	const ComposeLimits &limits);

// "text" | "media" | "poll" | "thread" | "quote" | "conversation_invite".
[[nodiscard]] QString ComposeType(const ComposeDraft &draft);

// The thread parts that go to the server: trimmed, empty ones dropped.
[[nodiscard]] QStringList CleanThreadParts(const QStringList &parts);

// Characters left before the limit (negative when over it).
[[nodiscard]] int Remaining(const QString &text, int limit);

// Cuts a long text into parts of at most `limit` code points each: at a
// paragraph break when there is one in the second half of the part, else at
// the end of a sentence, else between words, else in the middle of a word.
// Never returns more than `maxParts` parts (the last one keeps the rest).
[[nodiscard]] QStringList SplitIntoThread(
	const QString &text,
	int limit,
	int maxParts);

// ---- replies

struct ReplyNode {
	QString id;
	QString replyTo; // empty = a reply to the root
	bool deleted = false;
};

struct ReplyRow {
	QString id;
	int depth = 0; // 0 | 1
	QString parent; // the post a nested row answers
	bool line = false; // the next row hangs under this one

	friend bool operator==(const ReplyRow &, const ReplyRow &) = default;
};

// One level of nesting: everything under a top-level reply follows it depth
// first at depth 1. Deleted replies stay only while something hangs under
// them. `newestFirst` reverses the top-level order.
[[nodiscard]] QVector<ReplyRow> ArrangeReplies(
	const QVector<ReplyNode> &all,
	const QString &rootId,
	bool newestFirst);

// ---- view beacons

// A card is seen once at least half of it stayed on screen for a second.
class ViewTracker final {
public:
	static constexpr auto kDwellMs = qint64(1000);

	// The cards that are at least half visible right now.
	void setVisible(const QStringList &ids, qint64 now);
	// Cards whose second is up, each reported once per tracker.
	[[nodiscard]] QStringList takeSeen(qint64 now);
	// Milliseconds until the next card becomes due, -1 when nothing waits.
	[[nodiscard]] int nextCheckIn(qint64 now) const;
	void reset();

private:
	QHash<QString, qint64> _since;
	QSet<QString> _reported;

};

// Batches ids for one request each `intervalMs` (the store sends them).
class BeaconQueue final {
public:
	BeaconQueue(int intervalMs, int batchSize);

	// Returns true when a flush has to be scheduled (the queue was idle).
	bool push(const QStringList &ids);
	// The next batch, in the order the ids came; empty when there is none.
	[[nodiscard]] QStringList takeBatch();
	[[nodiscard]] bool empty() const {
		return _queue.isEmpty();
	}
	[[nodiscard]] int intervalMs() const {
		return _intervalMs;
	}

private:
	int _intervalMs = 0;
	int _batchSize = 0;
	QStringList _queue;
	QSet<QString> _known;

};

// ---- counts freshness

// Counts ride with post payloads; a card that re-appears asks for fresh ones
// only when its copy is older than a minute.
class CountsFreshness final {
public:
	static constexpr auto kMaxAgeMs = qint64(60) * 1000;

	// A payload with these posts just arrived.
	void noteFresh(const QStringList &ids, qint64 now);
	// Of the cards that appeared, the ones to re-read (they are marked as
	// asked for, so the same card is not asked again within the minute).
	[[nodiscard]] QStringList takeStale(const QStringList &appeared, qint64 now);
	void forget(const QString &id);

private:
	QHash<QString, qint64> _at;

};

} // namespace Eli::Logic
