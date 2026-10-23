#include "search/search_module.h"

#include "search/search_widget.h"
#include "window/window_registry.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"

namespace Search {
namespace {

// Opens the tab and hands the query to its side section.
void ShowSearch(Window::SessionController *controller, const QString &query) {
	controller->showTab(Window::Tab::Search);
	const auto section = controller->sideSection(Window::Tab::Search);
	if (const auto widget = dynamic_cast<Widget*>(section)) {
		widget->setQuery(query);
	}
}

} // namespace

void Register() {
	Window::RegisterTab(Window::Tab::Search, [](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(new Widget(parent, controller));
	});
	// «دیدن همه» of the people you may know.
	Window::RegisterRouteHandler([](
			Window::SessionController *controller,
			const QString &route) {
		const auto clean = route.startsWith(QLatin1Char('/'))
			? route.mid(1)
			: route;
		if (clean != u"search/people") {
			return false;
		}
		controller->showMainSection([](
				QWidget *parent,
				Window::SessionController *controller) {
			return Ui::Owned<Window::SectionWidget>(
				new Widget(parent, controller, Widget::Mode::AllPeople));
		}, { .tab = Window::Tab::Search });
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Search, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		ShowSearch(controller, route.queryValue(u"q"_s));
		return true;
	});
}

} // namespace Search
