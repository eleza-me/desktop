#include "support/error_text.h"

#include "api/api_client.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "ui/ui_utility.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"

namespace Support {
namespace {

constexpr auto kSameToastInterval = 4000;

struct LastToast {
	QString text;
	qint64 at = 0;
};

[[nodiscard]] LastToast &Last() {
	static auto result = LastToast();
	return result;
}

[[nodiscard]] bool Silent(const Api::Error &error) {
	return (error.code == u"cancelled");
}

// Drafts and typing are retried by the stores; nobody needs a toast for them.
[[nodiscard]] bool QuietAction(const QString &action) {
	return action.startsWith(u"draft.")
		|| action.startsWith(u"typing")
		|| action.startsWith(u"presence")
		|| action == u"chat.read"
		|| action == u"post.view";
}

} // namespace

void ShowError(
		Window::MainWindow *window,
		const Api::Error &error,
		const QString &fallback) {
	if (!window || Silent(error)) {
		return;
	}
	const auto text = fallback.isEmpty()
		? ErrorText(error)
		: ErrorText(error, fallback);
	const auto now = Ui::Now();
	auto &last = Last();
	if (last.text == text && (now - last.at) < kSameToastInterval) {
		return;
	}
	last.text = text;
	last.at = now;
	window->showToast(text);
}

void ShowError(
		Window::SessionController *controller,
		const Api::Error &error,
		const QString &fallback) {
	if (controller) {
		ShowError(controller->window(), error, fallback);
	}
}

ErrorReporter::ErrorReporter(
	Window::SessionController *controller,
	QObject *parent)
: QObject(parent)
, _controller(controller) {
	auto &session = controller->session();
	QObject::connect(
		&session.data(),
		&Data::Session::actionFailed,
		this,
		[this](
				const QString &action,
				const QString &subject,
				const Api::Error &error) {
			// The subject is a chat / user / post id: for the log of a build
			// with the debug tools only.
#ifdef ELEZA_DEBUG_TOOLS
			qWarning(
				"[eleza] %s (%s): %s",
				qPrintable(action),
				qPrintable(subject),
				qPrintable(error.forLog()));
#else // ELEZA_DEBUG_TOOLS
			Q_UNUSED(subject);
			qWarning(
				"[eleza] %s: %s",
				qPrintable(action),
				qPrintable(error.forLog()));
#endif // ELEZA_DEBUG_TOOLS
			if (!QuietAction(action)) {
				ShowError(_controller, error);
			}
		});
	QObject::connect(
		&session.account().api(),
		&Api::Client::requestFailed,
		this,
		[](
				const QString &method,
				const QString &path,
				const Api::Error &error) {
			if (Silent(error)) {
				return;
			}
			// The caller decides what to show; the log tells why it failed:
			// the method, the route (its template in a release build: no
			// chat / user / post ids, no handles), the status, the error
			// code and the request id.
			qInfo(
				"[eleza] %s %s: %s",
				qPrintable(method),
				qPrintable(Api::RouteForLog(path)),
				qPrintable(error.forLog()));
		});
}

} // namespace Support
