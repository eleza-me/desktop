#include "search/search_module.h"

#include "search/search_widget.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "window/window_embeds.h"
#include "window/window_registry.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_search.h"

namespace Search {
namespace {

const auto kPostsEmbed = u"eli.posts"_s;

// Opens the tab and hands the query to its side section.
void ShowSearch(Window::SessionController *controller, const QString &query) {
	controller->showTab(Window::Tab::Search);
	const auto section = controller->sideSection(Window::Tab::Search);
	if (const auto widget = dynamic_cast<Widget*>(section)) {
		widget->setQuery(query);
	}
}

// A stand-in for the posts list of the feed module: shows how the search
// results host an embedded list in a build without that module. The route
// "debug/search-embed[?q=…]" registers it (only when nobody published
// "eli.posts") and opens the search; a query with "none" in it has no posts.
class DebugPostsEmbed final : public Window::EmbeddedList {
public:
	DebugPostsEmbed(QWidget *parent, const QVariantMap &params)
	: EmbeddedList(parent)
	, _query(params.value(u"query"_s).toString())
	, _rows(_query.contains(u"none"_s) ? 0 : kPage) {
		// The host starts listening after the constructor returned.
		Ui::PostponeCall(this, [this] {
			Q_EMIT emptyChanged(_rows == 0);
		});
	}

	void setVisibleRange(int top, int bottom) override {
		_visibleTop = top;
		_visibleBottom = bottom;
		update();
	}
	void loadMore() override {
		if (_rows > 0 && _rows < kPage * 4) {
			_rows += kPage;
			Q_EMIT heightChanged();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return _rows * kRowHeight;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.setFont(st::searchPersonSubFont);
		for (auto i = 0; i != _rows; ++i) {
			const auto top = i * kRowHeight;
			if (top + kRowHeight <= _visibleTop || top >= _visibleBottom) {
				continue;
			}
			const auto padding = int(st::searchPadding);
			p.fillRect(
				QRect(padding, top + 6, width() - 2 * padding, kRowHeight - 12),
				st::bgSunken);
			p.setPen(st::textSecondary);
			p.drawTextLeft(
				2 * padding,
				top + 18,
				width(),
				_query + QLatin1Char(' ') + QString::number(i + 1));
		}
	}

private:
	static constexpr auto kRowHeight = 96;
	static constexpr auto kPage = 6;

	QString _query;
	int _rows = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;

};

bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto name = route.startsWith(QLatin1Char('/')) ? route.mid(1) : route;
	const auto prefix = u"debug/search-embed"_s;
	if (!name.startsWith(prefix)) {
		return false;
	}
	if (!Window::HasEmbed(kPostsEmbed)) {
		Window::RegisterEmbed(kPostsEmbed, [](
				QWidget *parent,
				Window::SessionController *,
				const QVariantMap &params) {
			return Ui::Owned<Window::EmbeddedList>(
				new DebugPostsEmbed(parent, params));
		});
	}
	const auto parsed = Window::ParseRoute(
		Window::Routes::Search() + name.mid(prefix.size()));
	ShowSearch(controller, parsed.queryValue(u"q"_s));
	return true;
}

} // namespace

void Register() {
	Window::RegisterTab(Window::Tab::Search, [](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(new Widget(parent, controller));
	});
	Window::RegisterRouteHandler(Window::RouteType::Search, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		ShowSearch(controller, route.queryValue(u"q"_s));
		return true;
	});
	Window::RegisterRouteHandler(HandleDebugRoute);
}

} // namespace Search
