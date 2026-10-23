#include "search/search_people.h"

#include "lang/lang_format.h"
#include "lang_auto.h"

#include <algorithm>

namespace Search {

FollowState FollowStateOf(const Data::Profile &profile) {
	if (profile.followState == u"following") {
		return FollowState::Following;
	} else if (profile.followState == u"requested") {
		return FollowState::Requested;
	}
	return profile.followsYou ? FollowState::FollowsYou : FollowState::None;
}

FollowState FollowStateOf(const Data::GraphState &state) {
	if (state.follow == u"following") {
		return FollowState::Following;
	} else if (state.follow == u"requested") {
		return FollowState::Requested;
	}
	return state.followsYou ? FollowState::FollowsYou : FollowState::None;
}

bool ClickFollows(FollowState current) {
	return (current == FollowState::None)
		|| (current == FollowState::FollowsYou);
}

FollowState NextFollowState(
		FollowState current,
		bool isPrivate,
		bool followsYou) {
	if (ClickFollows(current)) {
		return isPrivate ? FollowState::Requested : FollowState::Following;
	}
	return followsYou ? FollowState::FollowsYou : FollowState::None;
}

void ApplyFollowState(Data::Profile &profile, FollowState state) {
	switch (state) {
	case FollowState::Following:
		profile.followState = u"following"_s;
		break;
	case FollowState::Requested:
		profile.followState = u"requested"_s;
		break;
	case FollowState::FollowsYou:
		profile.followState = u"none"_s;
		profile.followsYou = true;
		break;
	case FollowState::None:
		profile.followState = u"none"_s;
		profile.followsYou = false;
		break;
	}
}

QString FollowText(FollowState state) {
	switch (state) {
	case FollowState::Following: return tr::lng_following(tr::now);
	case FollowState::Requested: return tr::lng_follow_requested(tr::now);
	case FollowState::FollowsYou: return tr::lng_follow_back(tr::now);
	case FollowState::None: break;
	}
	return tr::lng_follow(tr::now);
}

QString HandleText(const QString &handle) {
	// Isolated: the "@" stays in front of the name in RTL.
	return Lang::Handle(handle);
}

namespace {

[[nodiscard]] QString ReasonLine(const Data::Profile &profile);

} // namespace

bool IsPersonalReason(const QString &reason) {
	return (reason == u"mutual")
		|| (reason == u"contact")
		|| (reason == u"follows_you")
		|| (reason == u"groups");
}

QString SuggestionReason(const Data::Profile &profile) {
	const auto line = ReasonLine(profile);
	// Somebody I know who just joined: the tie, and that they are new.
	if (profile.joinedRecently
		&& IsPersonalReason(profile.reason)
		&& !line.isEmpty()
		&& line != tr::lng_search_suggested(tr::now)) {
		return tr::lng_search_reason_and_new(tr::now, lt_reason, line);
	}
	return line;
}

namespace {

QString ReasonLine(const Data::Profile &profile) {
	// Names are isolated: a Latin name in a Persian sentence (or the other
	// way round) must not reorder the words around it.
	auto names = QStringList();
	for (const auto &mutual : profile.mutuals) {
		if (!mutual.displayName.isEmpty()) {
			names.push_back(QChar(0x2068) + mutual.displayName + QChar(0x2069));
		}
	}
	const auto count = (profile.mutualCount > 0)
		? profile.mutualCount
		: (profile.mutualsTotal > 0)
		? profile.mutualsTotal
		: int(names.size());
	const auto &reason = profile.reason;
	if (reason == u"mutual") {
		if (names.size() >= 2 && count > 2) {
			return tr::lng_search_reason_mutual_more(
				tr::now,
				lt_count,
				count - 1,
				lt_name,
				names[0]);
		} else if (names.size() >= 2) {
			return tr::lng_search_reason_mutual_two(
				tr::now,
				lt_first,
				names[0],
				lt_second,
				names[1]);
		} else if (names.size() == 1 && count > 1) {
			return tr::lng_search_reason_mutual_more(
				tr::now,
				lt_count,
				count - 1,
				lt_name,
				names[0]);
		} else if (names.size() == 1) {
			return tr::lng_search_reason_mutual_one(
				tr::now,
				lt_name,
				names[0]);
		} else if (count > 0) {
			return tr::lng_search_reason_mutual_count(tr::now, lt_count, count);
		}
		return tr::lng_search_suggested(tr::now);
	} else if (reason == u"contact") {
		return tr::lng_search_reason_contact(tr::now);
	} else if (reason == u"follows_you") {
		return tr::lng_search_reason_follows_you(tr::now);
	} else if (reason == u"interest") {
		return tr::lng_search_reason_interest(tr::now);
	} else if (reason == u"province") {
		return tr::lng_search_reason_province(tr::now);
	} else if (reason == u"groups") {
		// Without a count (it is only ever added): the generic line.
		return (profile.groupCount > 0)
			? tr::lng_search_reason_groups(tr::now, lt_count, profile.groupCount)
			: tr::lng_search_suggested(tr::now);
	} else if (reason == u"new") {
		return tr::lng_search_reason_new(tr::now);
	} else if (reason == u"popular" || reason == u"active") {
		return tr::lng_search_suggested(tr::now);
	}
	// An unknown reason is the server's own wording of nothing: the handle.
	return HandleText(profile.handle);
}

} // namespace

SuggestionQueue::SuggestionQueue(int visibleCount, int lowWater)
: _visibleCount(std::max(visibleCount, 1))
, _lowWater(std::max(lowWater, 0)) {
}

QVector<Data::Profile> SuggestionQueue::visible() const {
	return _people.mid(0, _visibleCount);
}

int SuggestionQueue::waiting() const {
	return std::max(int(_people.size()) - _visibleCount, 0);
}

Data::Profile *SuggestionQueue::find(const QString &userId) {
	for (auto &person : _people) {
		if (person.id == userId) {
			return &person;
		}
	}
	return nullptr;
}

bool SuggestionQueue::takes(
		const Data::Profile &person,
		const QSet<QString> &excluding,
		const QString &me) const {
	if (person.id.isEmpty()
		|| person.id == me
		|| person.me
		|| _handled.contains(person.id)
		|| excluding.contains(person.id)) {
		return false;
	}
	return std::none_of(_people.cbegin(), _people.cend(), [&](
			const Data::Profile &known) {
		return known.id == person.id;
	});
}

void SuggestionQueue::replace(
		const QVector<Data::Profile> &fresh,
		const QSet<QString> &excluding,
		const QString &me) {
	_people.clear();
	refill(fresh, excluding, me);
}

int SuggestionQueue::refill(
		const QVector<Data::Profile> &more,
		const QSet<QString> &excluding,
		const QString &me) {
	auto added = 0;
	for (const auto &person : more) {
		if (takes(person, excluding, me)) {
			_people.push_back(person);
			++added;
		}
	}
	return added;
}

bool SuggestionQueue::remove(const QString &userId) {
	_handled.insert(userId);
	const auto was = _people.size();
	_people.erase(
		std::remove_if(_people.begin(), _people.end(), [&](
				const Data::Profile &person) {
			return person.id == userId;
		}),
		_people.end());
	return _people.size() != was;
}

void SuggestionQueue::forget(const QString &userId) {
	_handled.remove(userId);
}

bool LeavesSuggestions(FollowState state) {
	return (state == FollowState::Following)
		|| (state == FollowState::Requested);
}

bool RefreshThrottle::due(qint64 now) const {
	if (_loadedAt < 0) {
		return true;
	}
	const auto elapsed = now - _loadedAt;
	return (elapsed < 0) || (elapsed >= _intervalMs);
}

} // namespace Search
