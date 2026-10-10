#include "settings/settings_module.h"

#include "core/core_application.h"
#include "data/data_media.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "settings/settings_pages.h"
#include "settings/settings_two_step.h"
#include "settings/settings_widget.h"
#include "support/error_text.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"

namespace Settings {
namespace {

// The user's cache limit is a preference of this computer: the media store
// of every new session starts with it.
void ApplyMediaCacheLimit() {
	const auto limit = Core::App().settings().mediaCacheLimit();
	if (const auto session = Core::App().session(); session && limit > 0) {
		session->data().media().setDiskLimit(limit);
	}
}

bool OpenSection(Window::SessionController *controller, const QString &id) {
	const auto section = SectionFromId(id);
	if (!section) {
		controller->showTab(Window::Tab::Settings);
		return true;
	}
	// «دستگاه‌ها» from the account page goes over it; a section picked from
	// the list replaces whatever was open.
	auto nested = false;
	if (*section == Section::Devices
		&& controller->currentTab() == Window::Tab::Settings) {
		if (const auto current = controller->mainSection()) {
			nested = (current->sectionKey()
				== Window::Routes::Settings(SectionId(Section::Account)));
		}
	}
	controller->showMainSection(
		PageFactory(*section),
		{ .stack = nested, .tab = Window::Tab::Settings });
	return true;
}

#ifdef ELEZA_DEBUG_TOOLS
// "debug/settings/two-step", "debug/settings/export": the boxes directly.
bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto prefix = u"debug/settings/"_s;
	if (!route.startsWith(prefix)) {
		return false;
	}
	const auto what = route.mid(prefix.size());
	if (what == u"two-step") {
		LoadTwoStepState(controller, controller, [=](
				const TwoStepState &state) {
			ShowTwoStepBox(controller, state, nullptr);
		}, [=](const Api::Error &error) {
			Support::ShowError(controller, error);
		});
	} else if (what == u"export") {
		ShowExportBox(controller);
	} else {
		return false;
	}
	return true;
}

#endif // ELEZA_DEBUG_TOOLS

} // namespace

void Register() {
	Window::RegisterTab(Window::Tab::Settings, [](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new Widget(parent, controller));
	});
	Window::RegisterRouteHandler(Window::RouteType::Settings, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (route.id.isEmpty()) {
			controller->showTab(Window::Tab::Settings);
			return true;
		}
		return OpenSection(controller, route.id);
	});
#ifdef ELEZA_DEBUG_TOOLS
	Window::RegisterRouteHandler(HandleDebugRoute);
#endif // ELEZA_DEBUG_TOOLS
	QObject::connect(
		&Core::App(),
		&Core::Application::sessionShownChanged,
		&Core::App(),
		[] { ApplyMediaCacheLimit(); });
}

} // namespace Settings
