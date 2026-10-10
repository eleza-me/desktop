// What to tell the user when a request failed.
//
//   .fail([=](const Api::Error &error) {
//       _error->setText(Support::ErrorText(error, tr::lng_error_generic(tr::now)));
//   })
//   Support::ShowError(controller, error);                    // the same as a toast
//   Support::ShowError(controller, error, tr::lng_profile_couldnt_save(tr::now));
//
// ErrorText follows the web client's humanError: no connection, too many requests
// and server errors have their own wording; the well-known problem codes map to
// our phrases (ErrorPhrase); otherwise the server's localized message is used, and
// when there is none the fallback with the code in brackets.
//
// Fire-and-forget actions of the stores (Data::Session::actionFailed) are shown
// by Support::ErrorReporter, which the session services create once
// (support/session_services.h) — screens do not subscribe themselves.
#pragma once

#include "api/api_error.h"

#include <QtCore/QObject>

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
class MainWindow;
} // namespace Window

namespace Support {

// The phrase of a well-known problem code ("handle.taken", "otp.invalid", …);
// empty for a code we have no wording for.
[[nodiscard]] QString ErrorPhrase(const QString &code);

[[nodiscard]] QString ErrorText(const Api::Error &error, const QString &fallback);
// With the generic «چیزی اشتباه شد» as the fallback.
[[nodiscard]] QString ErrorText(const Api::Error &error);

// A toast with ErrorText; identical toasts within a few seconds are merged (a
// retry storm does not stack them). Cancelled requests are silent.
void ShowError(
	Window::SessionController *controller,
	const Api::Error &error,
	const QString &fallback = QString());
void ShowError(
	Window::MainWindow *window,
	const Api::Error &error,
	const QString &fallback = QString());

// Logs every failed request (method, path, code, request id) and shows a toast
// for the failed background actions of the stores.
class ErrorReporter final : public QObject {
public:
	ErrorReporter(Window::SessionController *controller, QObject *parent);

private:
	Window::SessionController *_controller = nullptr;

};

} // namespace Support
