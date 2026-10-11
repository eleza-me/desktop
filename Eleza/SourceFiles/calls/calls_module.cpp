#include "calls/calls_module.h"

#include "calls/calls_media.h"
#include "calls/calls_service.h"
#include "calls/calls_settings.h"
#include "core/core_application.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "window/window_calls.h"
#include "window/window_main_window.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"

#include <QtCore/QCoreApplication>

namespace Calls {
namespace {

// The service of the signed-in session (one account, one session).
[[nodiscard]] std::unique_ptr<Service> &Owned() {
	static auto result = std::unique_ptr<Service>();
	return result;
}

// The session layout appeared, was rebuilt (a language switch) or went away.
void SessionShownChanged() {
	auto &app = Core::App();
	const auto session = app.session();
	const auto window = app.window();
	const auto controller = window ? window->sessionController() : nullptr;
	auto &service = Owned();
	if (!session || !controller) {
		if (service) {
			service->setController(nullptr);
		}
		return;
	}
	if (!service || service->session() != session) {
		service = nullptr;
		service = std::make_unique<Service>(session);
		// The service goes before the session's stores do: the account
		// says so while the session object still exists.
		static auto watching = false;
		if (!std::exchange(watching, true)) {
			QObject::connect(
				&app.account(),
				&Main::Account::sessionChanged,
				&app,
				[] {
					if (!Core::App().account().session()) {
						Owned() = nullptr;
					}
				});
		}
	}
	service->setController(controller);
}

[[nodiscard]] Service *EngagedService() {
	const auto service = Owned().get();
	return (service && service->engaged()) ? service : nullptr;
}

// Closing the window would quit the app in the middle of a call: ask.
[[nodiscard]] bool GuardClose() {
	const auto service = EngagedService();
	const auto window = Core::App().window();
	if (!service || !window) {
		return false;
	}
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_calls_quit_title(tr::now);
	args.text = tr::lng_calls_quit_text(tr::now);
	args.confirmText = tr::lng_calls_quit_confirm(tr::now);
	args.cancelText = tr::lng_calls_quit_stay(tr::now);
	args.destructive = true;
	args.confirmed = [](std::function<void()> close) {
		close();
		Core::App().quit();
	};
	window->show(Ui::MakeConfirmBox(std::move(args)));
	return true;
}

} // namespace

void Register() {
	Window::RegisterCallHooks({
		.start = [](
				Window::SessionController *controller,
				const QString &userId) {
			if (const auto service = Service::For(&controller->session())) {
				service->start(userId);
			}
		},
	});
	RegisterSettingsPage();

	auto &app = Core::App();
	QObject::connect(
		&app,
		&Core::Application::sessionShownChanged,
		&app,
		[] { SessionShownChanged(); });
	// Quitting (and signing out) ends the call for the other side first.
	app.registerExitTask([](std::function<void()> done) {
		if (const auto service = Owned().get()) {
			service->finishOnExit(std::move(done));
		} else {
			done();
		}
	});
	app.registerRestartBlocker([] {
		return EngagedService()
			? tr::lng_calls_update_blocked(tr::now)
			: QString();
	});
	app.registerCloseGuard([] { return GuardClose(); });
	QObject::connect(
		QCoreApplication::instance(),
		&QCoreApplication::aboutToQuit,
		&app,
		[] {
			Owned() = nullptr;
			ShutdownMedia();
		});
#ifdef ELEZA_DEBUG_TOOLS
	Window::RegisterRouteHandler([](
			Window::SessionController *controller,
			const QString &route) {
		const auto prefix = u"debug/calls/"_s;
		if (!route.startsWith(prefix)) {
			return false;
		}
		const auto service = Service::For(&controller->session());
		return service && service->showDemo(route.mid(prefix.size()));
	});
#endif // ELEZA_DEBUG_TOOLS
}

} // namespace Calls
