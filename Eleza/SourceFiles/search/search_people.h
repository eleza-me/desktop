// The follow button of a person row and the suggestion reason line. No widgets
// here (the unit tests compile this file alone).
#pragma once

#include "data/data_social_types.h"

#include <QtCore/QSet>

namespace Search {

enum class FollowState {
	None,       // «دنبال کردن»
	Following,  // «دنبال می‌کنی»
	Requested,  // «درخواست فرستاده شد»
	FollowsYou, // «تو هم دنبال کن»
};

[[nodiscard]] FollowState FollowStateOf(const Data::Profile &profile);
[[nodiscard]] FollowState FollowStateOf(const Data::GraphState &state);
// What the button shows right after a click, before the server answered: a
// private account is «requested»; unfollowing returns to none / follows you.
[[nodiscard]] FollowState NextFollowState(
	FollowState current,
	bool isPrivate,
	bool followsYou);
// Whether a click in this state follows (otherwise it unfollows / withdraws).
[[nodiscard]] bool ClickFollows(FollowState current);
// Writes a state back into the row's profile.
void ApplyFollowState(Data::Profile &profile, FollowState state);
[[nodiscard]] QString FollowText(FollowState state);

// "@handle", always left-to-right inside a line of either direction; empty
// without a handle.
[[nodiscard]] QString HandleText(const QString &handle);

// «{A} و {B} دنبالش می‌کنند», «در مخاطبان تو», «۳ گروه مشترک», «تازه به الزا
// پیوسته» ...; a personal tie with `joined_recently` adds «· تازه پیوسته».
[[nodiscard]] QString SuggestionReason(const Data::Profile &profile);
// A tie to me personally (not an interest, a region or the crowd).
[[nodiscard]] bool IsPersonalReason(const QString &reason);

// The people of «آدم‌هایی که شاید بشناسی»: the first `visibleCount` show, the
// rest of what was fetched waits its turn. A followed or dismissed person
// leaves (after kFollowLeaveMs / kDismissLeaveMs on screen) and the next one
// takes the place; when fewer than `lowWater` wait, the screen asks the
// server for more and refill()s.
class SuggestionQueue final {
public:
	// How long a followed row stays («دنبال می‌کنی») before it gives its place;
	// a dismissed one goes almost at once.
	static constexpr auto kFollowLeaveMs = 1200;
	static constexpr auto kDismissLeaveMs = 200;

	explicit SuggestionQueue(int visibleCount = 5, int lowWater = 3);

	[[nodiscard]] const QVector<Data::Profile> &people() const {
		return _people;
	}
	[[nodiscard]] QVector<Data::Profile> visible() const;
	[[nodiscard]] int waiting() const;
	[[nodiscard]] bool empty() const {
		return _people.isEmpty();
	}
	[[nodiscard]] bool needsRefill() const {
		return waiting() < _lowWater;
	}
	[[nodiscard]] Data::Profile *find(const QString &userId);

	// A fresh first page (the load, the periodic refresh): it replaces the
	// line, without who was handled here, `excluding` and `me`.
	void replace(
		const QVector<Data::Profile> &fresh,
		const QSet<QString> &excluding,
		const QString &me);
	// More for the line: the new ones go to its end, the rows on screen keep
	// their places. Returns how many were added.
	int refill(
		const QVector<Data::Profile> &more,
		const QSet<QString> &excluding,
		const QString &me);
	// Followed or dismissed: out of the line for good. True when it was there.
	bool remove(const QString &userId);
	// The follow was taken back (or failed) before the row left: a later
	// page may bring the person again.
	void forget(const QString &userId);
	[[nodiscard]] bool handled(const QString &userId) const {
		return _handled.contains(userId);
	}

private:
	[[nodiscard]] bool takes(
		const Data::Profile &person,
		const QSet<QString> &excluding,
		const QString &me) const;

	int _visibleCount = 5;
	int _lowWater = 3;
	QVector<Data::Profile> _people;
	QSet<QString> _handled;

};

// Whether a person leaves the suggestions after this follow state.
[[nodiscard]] bool LeavesSuggestions(FollowState state);

// When a list that refreshes by itself (the suggestions) is due again: when
// it is shown or the app comes back, at most once per `intervalMs` after the
// last load that went through. A failed load leaves it due; a clock that
// went back makes it due too.
class RefreshThrottle final {
public:
	static constexpr auto kSuggestionsMs = qint64(10) * 60 * 1000;

	explicit RefreshThrottle(qint64 intervalMs = kSuggestionsMs)
	: _intervalMs(intervalMs) {
	}

	[[nodiscard]] bool due(qint64 now) const;
	void loaded(qint64 now) {
		_loadedAt = now;
	}

private:
	qint64 _intervalMs = 0;
	qint64 _loadedAt = -1;

};

} // namespace Search
