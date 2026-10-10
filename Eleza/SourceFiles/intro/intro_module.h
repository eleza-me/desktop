// The signed-out flow (welcome -> phone -> code -> password / recovery -> profile).
// Registered as the window's intro; Core::Application shows it while nobody is
// signed in or the profile step is pending.
#pragma once

namespace Intro {

void Register();

} // namespace Intro
