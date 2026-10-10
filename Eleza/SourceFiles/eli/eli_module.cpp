#include "eli/eli_module.h"

#include "data/data_feed.h"
#include "data/data_session.h"
#include "eli/eli_actions.h"
#include "eli/eli_home.h"
#include "eli/eli_posts_embed.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "window/window_embeds.h"
#include "window/window_registry.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"

namespace Eli {
namespace {

// Routes of ours outside the canonical table, and the debug hooks used by the
// scripted screenshots.
[[nodiscard]] bool HandleRaw(
		Window::SessionController *controller,
		const QString &route) {
	auto path = route;
	if (path.startsWith(QChar('/'))) {
		path = path.mid(1);
	}
	const auto after = [&](QStringView prefix) {
		return path.startsWith(prefix)
			? path.mid(prefix.size())
			: QString();
	};
	if (path == Routes::Trends()) {
		OpenTrends(controller);
	} else if (path == Routes::Bookmarks()) {
		OpenBookmarks(controller);
	} else if (path == Routes::FollowRequests()) {
		OpenFollowRequests(controller);
	} else if (path == Routes::Compose()) {
		controller->showTab(Window::Tab::Eli);
		OpenCompose(controller);
	} else if (const auto id = after(u"eli/thread/"); !id.isEmpty()) {
		OpenThread(controller, id);
	} else if (const auto id = after(u"eli/sound/"); !id.isEmpty()) {
		OpenSound(controller, id);
	} else if (const auto count = after(u"debug/eli/pill/"); !count.isEmpty()) {
		controller->showTab(Window::Tab::Eli);
		Q_EMIT controller->session().data().feed().newPosts(count.toInt(), 0);
	} else if (const auto id = after(u"debug/eli/reactors/"); !id.isEmpty()) {
		OpenReactors(controller, id, false);
	} else if (const auto id = after(u"debug/eli/report/"); !id.isEmpty()) {
		OpenReport(controller, { u"post"_s, id, QString() });
	} else if (const auto id = after(u"debug/eli/pardeh/"); !id.isEmpty()) {
		OpenPardeh(controller, id);
	} else if (const auto id = after(u"debug/eli/note/"); !id.isEmpty()) {
		OpenVideoNote(controller, id);
	} else if (const auto id = after(u"debug/eli/reply/"); !id.isEmpty()) {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Reply;
		args.postId = id;
		OpenCompose(controller, args);
	} else if (const auto id = after(u"debug/eli/quote/"); !id.isEmpty()) {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Quote;
		args.postId = id;
		OpenCompose(controller, args);
	} else if (const auto id = after(u"debug/eli/edit/"); !id.isEmpty()) {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Edit;
		args.postId = id;
		OpenCompose(controller, args);
	} else if (const auto id = after(u"debug/eli/invite/"); !id.isEmpty()) {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Invite;
		args.conversationId = id;
		OpenCompose(controller, args);
	} else {
		return false;
	}
	return true;
}

} // namespace

void Register() {
	Window::RegisterTab(Window::Tab::Eli, [](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new HomeSection(parent, controller));
	});
	Window::RegisterRouteHandler(HandleRaw);
	Window::RegisterRouteHandler(Window::RouteType::Eli, [](
			Window::SessionController *controller,
			const Window::Route &) {
		controller->showTab(Window::Tab::Eli);
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Post, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		controller->showMainSection(
			PostFactory(route.id),
			{ .tab = Window::Tab::Eli });
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::SharedPost, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		// A share link names the post by a slug: ask which post it is, then
		// open it like any other.
		controller->session().data().feed().sharedPost(
			route.id,
			base::Guard(controller),
			[=](const Data::Post &post) {
				controller->openRoute(Window::Routes::Post(post.id));
			},
			[=](const Api::Error &error) {
				Support::ShowError(
					controller,
					error,
					tr::lng_eli_post_no_longer_available(tr::now));
			});
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Topic, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		controller->showMainSection(
			TopicFactory(route.id),
			{ .tab = Window::Tab::Eli });
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::EliNotifications, [](
			Window::SessionController *controller,
			const Window::Route &) {
		controller->showMainSection(
			NotificationsFactory(),
			{ .tab = Window::Tab::Eli });
		return true;
	});
	Window::RegisterEmbed(u"eli.posts"_s, [](
			QWidget *parent,
			Window::SessionController *controller,
			const QVariantMap &params) {
		return Ui::Owned<Window::EmbeddedList>(
			new PostsEmbed(parent, controller, params));
	});
}

} // namespace Eli
