// «تأیید دومرحله‌ای»: the password asked after the SMS code, with a recovery
// e-mail. The whole management lives in boxes opened from the account page:
//
//   off      -> set up: password + repeat + hint + recovery e-mail -> the code
//               mailed to that address -> on
//   pending  -> enter the mailed code (or start again)
//   on       -> change the password / change the recovery e-mail (password,
//               new address, the code) / turn off (password)
//
// The server contract is the «D. Two-step verification» part of the phase 2
// wire contract.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <functional>

namespace Api {
struct Error;
} // namespace Api

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

struct TwoStepState {
	bool enabled = false;
	bool pending = false;
	QString hint;
	QString recoveryEmail; // masked
	QString pendingEmail;  // masked
};

// `GET /v1/account/two-step`.
void LoadTwoStepState(
	Window::SessionController *controller,
	QObject *guard,
	std::function<void(const TwoStepState &)> done,
	std::function<void(const Api::Error &)> fail);

// Opens the right box for the state; `changed` is called after every
// successful change (the caller refreshes its row).
void ShowTwoStepBox(
	Window::SessionController *controller,
	const TwoStepState &state,
	std::function<void()> changed);

} // namespace Settings
