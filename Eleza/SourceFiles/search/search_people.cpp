#include "search/search_people.h"

#include "lang/lang_format.h"
#include "lang_auto.h"

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

QString SuggestionReason(const Data::Profile &profile) {
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
	} else if (reason == u"popular" || reason == u"active") {
		return tr::lng_search_suggested(tr::now);
	}
	// An unknown reason is the server's own wording of nothing: the handle.
	return HandleText(profile.handle);
}

} // namespace Search
