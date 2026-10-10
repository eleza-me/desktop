#include "conversation/conversation_module.h"

#include "conversation/conversation_boxes.h"
#include "conversation/conversation_radar.h"
#include "conversation/conversation_topics.h"
#include "lang_auto.h"
#include "window/window_embeds.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"

namespace Conversation {
namespace {

const auto kTopicsSubTab = u"topics"_s;

// "debug/…" routes of the module (docs/DEVELOPING.md, section 2):
//   debug/topics                    the topics list as a main section
//   debug/conversation-new[?q=…]    the «گفتگوی تازه» box (optionally prefilled)
//   debug/conversation-join/<id>    the «پیوستن به گفتگو» box
//   debug/conversation-report/<id>  the report box
//   debug/conversation-invite/<id>  the «دعوت به گفتگو» box
//   debug/radar/<id>                the radar of a conversation, whatever its state
bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto name = route.startsWith(QLatin1Char('/')) ? route.mid(1) : route;
	if (!name.startsWith(u"debug/"_s)) {
		return false;
	}
	const auto query = name.indexOf(QLatin1Char('?'));
	const auto path = (query < 0) ? name : name.left(query);
	const auto argument = [&](const QString &prefix) {
		return path.mid(prefix.size());
	};
	if (path == u"debug/topics") {
		controller->showMainSection(
			TopicsSectionFactory(),
			{ .stack = false, .tab = Window::Tab::Chats });
		return true;
	} else if (path == u"debug/conversation-new") {
		const auto prefix = u"q="_s;
		const auto tail = (query < 0) ? QString() : name.mid(query + 1);
		ShowNewConversationBox(
			controller,
			tail.startsWith(prefix)
				? QString::fromUtf8(QByteArray::fromPercentEncoding(
					tail.mid(prefix.size()).toUtf8()))
				: QString());
		return true;
	} else if (path.startsWith(u"debug/conversation-join/"_s)) {
		ShowJoinBox(controller, argument(u"debug/conversation-join/"_s));
		return true;
	} else if (path.startsWith(u"debug/conversation-report/"_s)) {
		ShowReportBox(controller, argument(u"debug/conversation-report/"_s));
		return true;
	} else if (path.startsWith(u"debug/conversation-invite/"_s)) {
		ShowInviteBox(controller, argument(u"debug/conversation-invite/"_s));
		return true;
	} else if (path.startsWith(u"debug/radar/"_s)) {
		controller->showMainSection(
			RadarFactory(argument(u"debug/radar/"_s)),
			{ .tab = Window::Tab::Chats });
		return true;
	}
	return false;
}

} // namespace

void Register() {
	Window::RegisterSideSubTab(Window::Tab::Chats, {
		.id = kTopicsSubTab,
		.order = 10,
		.title = [] { return tr::lng_conv_topics(tr::now); },
		.factory = [](QWidget *parent, Window::SessionController *controller) {
			return Ui::Owned<Ui::RpWidget>(new TopicsList(parent, controller));
		},
	});
	Window::RegisterRouteHandler(Window::RouteType::ConversationTopics, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (Window::RegisteredTabFactory(Window::Tab::Chats)) {
			// The chat list hosts the «چت‌ها | موضوع‌ها» control. An open
			// chat stays where the list is visible beside it.
			controller->activateTab(Window::Tab::Chats);
			if (controller->isSingleColumn()) {
				controller->clearMainStack(Window::Tab::Chats);
			}
			controller->requestSideSubTab(Window::Tab::Chats, kTopicsSubTab);
		} else {
			controller->showMainSection(
				TopicsSectionFactory(),
				{ .stack = false, .tab = Window::Tab::Chats });
		}
		// "conversation/topics?new=<title>": a hint from another screen (a
		// topic page of the feed) to start a conversation about something.
		const auto hint = u"new"_s;
		if (route.query.contains(hint)) {
			ShowNewConversationBox(controller, route.queryValue(hint).trimmed());
		}
		return true;
	});
	Window::RegisterRouteHandler(Window::RouteType::Conversation, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (route.id.isEmpty()) {
			return false;
		}
		controller->activateTab(Window::Tab::Chats);
		OpenConversation(controller, route.id);
		return true;
	});
	Window::RegisterRouteHandler(HandleDebugRoute);
	Window::RegisterChatMenuFiller(FillChatMenu);
}

} // namespace Conversation
