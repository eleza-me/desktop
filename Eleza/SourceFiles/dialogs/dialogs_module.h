#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Dialogs {

void Register();

// Shows the «گفتگو» tab with the message requests list (settings link to it).
void ShowRequests(Window::SessionController *controller);

} // namespace Dialogs
