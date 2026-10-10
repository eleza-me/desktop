#include "dialogs/dialogs_module.h"

#include "data/data_chats.h"
#include "data/data_session.h"
#include "dialogs/dialogs_menu.h"
#include "dialogs/dialogs_widget.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/ui_utility.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"
#include "window/window_shortcuts.h"

namespace Dialogs {
namespace {

[[nodiscard]] Widget *SideWidget(Window::SessionController *controller) {
	controller->showTab(Window::Tab::Chats);
	return dynamic_cast<Widget*>(
		controller->sideSection(Window::Tab::Chats));
}

void ShowFolder(Window::SessionController *controller, Folder folder) {
	if (const auto widget = SideWidget(controller)) {
		widget->showFolder(folder);
		if (folder != Folder::Main && controller->isSingleColumn()) {
			// The list is under the open chat in the single column mode.
			controller->clearMainStack(Window::Tab::Chats);
		}
	}
}

// "debug/dialogs/<what>": states that need a live peer or a second device.
bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto prefix = u"debug/dialogs/"_s;
	if (!route.startsWith(prefix)) {
		return false;
	}
	const auto what = route.mid(prefix.size());
	auto &chats = controller->session().data().chats();
	const auto list = chats.list(Data::Chats::Section::Messages);
	const auto at = [&](int index) {
		return (index < list.size()) ? list[index].id : QString();
	};
	if (what == u"new") {
		ShowNewMessageBox(controller);
	} else if (what == u"requests") {
		ShowFolder(controller, Folder::Requests);
	} else if (what == u"archived") {
		ShowFolder(controller, Folder::Archived);
	} else if (what == u"typing") {
		chats.setActivity(at(0), u"typing"_s, 30);
	} else if (what == u"recording") {
		chats.setActivity(at(1), u"recording_voice"_s, 30);
	} else if (what == u"draft") {
		auto draft = Data::Draft();
		draft.text = tr::lng_dialogs_empty_title(tr::now);
		draft.updatedAt = QDateTime::currentMSecsSinceEpoch();
		chats.saveDraft(at(1), draft);
	} else if (what == u"failed" || what == u"pending") {
		const auto failed = (what == u"failed");
		chats.patch(at(1), [&](Data::Chat &chat) {
			if (chat.lastMessage) {
				chat.lastMine = true;
				chat.lastMessage->mine = true;
				chat.lastMessage->status = failed
					? Data::MessageStatus::Failed
					: Data::MessageStatus::Sending;
			}
		});
	} else if (what == u"delete") {
		if (const auto chat = chats.chat(at(0))) {
			ShowDeleteChatBox(controller, *chat);
		}
	} else if (what == u"pin-all") {
		for (const auto &chat : list) {
			chats.setPinned(chat.id, true);
		}
	} else {
		return false;
	}
	return true;
}

} // namespace

void ShowRequests(Window::SessionController *controller) {
	ShowFolder(controller, Folder::Requests);
}

void Register() {
	Window::RegisterTab(Window::Tab::Chats, [](
			QWidget *parent,
			Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new Widget(parent, controller));
	});
	Window::RegisterRouteHandler(Window::RouteType::Chats, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		// "chats", "chats?folder=requests", "chats?folder=archived".
		const auto folder = route.queryValue(u"folder"_s);
		ShowFolder(
			controller,
			(folder == u"requests")
				? Folder::Requests
				: (folder == u"archived")
				? Folder::Archived
				: Folder::Main);
		return true;
	});
	Window::RegisterRouteHandler(HandleDebugRoute);

	// The window-wide shortcuts of the chat list (window_shortcuts.h).
	using Command = Window::Command;
	const auto list = [](Window::SessionController *controller) {
		// The open chat stays open.
		controller->activateTab(Window::Tab::Chats);
		return dynamic_cast<Widget*>(
			controller->sideSection(Window::Tab::Chats));
	};
	Window::RegisterCommandHandler(Command::SearchChats, [=](
			Window::SessionController *controller) {
		if (const auto widget = list(controller)) {
			if (controller->isSingleColumn()) {
				// The list is under the open chat in the single column mode.
				controller->clearMainStack(Window::Tab::Chats);
			}
			widget->focusSearch();
		}
		return true;
	});
	Window::RegisterCommandHandler(Command::NewMessage, [=](
			Window::SessionController *controller) {
		controller->activateTab(Window::Tab::Chats);
		ShowNewMessageBox(controller);
		return true;
	});
	const auto jump = [](int direction) {
		return [=](Window::SessionController *controller) {
			if (controller->currentTab() != Window::Tab::Chats) {
				return false;
			}
			const auto widget = dynamic_cast<Widget*>(
				controller->sideSection(Window::Tab::Chats));
			if (widget) {
				widget->jumpToChat(direction);
			}
			return (widget != nullptr);
		};
	};
	Window::RegisterCommandHandler(Command::NextChat, jump(1));
	Window::RegisterCommandHandler(Command::PreviousChat, jump(-1));
}

} // namespace Dialogs
