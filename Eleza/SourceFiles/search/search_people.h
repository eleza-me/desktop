// The follow button of a person row and the suggestion reason line. No widgets
// here (the unit tests compile this file alone).
#pragma once

#include "data/data_social_types.h"

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

// «{A} و {B} دنبالش می‌کنند», «در مخاطبان تو», «از استان تو» ...
[[nodiscard]] QString SuggestionReason(const Data::Profile &profile);

} // namespace Search
