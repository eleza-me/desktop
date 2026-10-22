#include "dialogs/dialogs_menu.h"

#include "api/api_client_config.h"
#include "data/data_chats.h"
#include "data/data_graph.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_embeds.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"

namespace Dialogs {
namespace {

// The server keeps one limit for direct chats and another for groups.
[[nodiscard]] int PinnedCount(Data::Chats &chats, bool groups) {
	auto result = 0;
	for (const auto &chat : chats.all()) {
		if (chat.pinned && CanPin(chat) && chat.isGroup == groups) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] Ui::MenuItem DangerItem(
		const QString &text,
		std::function<void()> callback,
		const style::icon *icon) {
	auto result = Ui::MenuItem();
	result.text = text;
	result.callback = std::move(callback);
	result.icon = icon;
	result.destructive = true;
	return result;
}

[[nodiscard]] bool IsOpen(
		Window::SessionController *controller,
		const QString &chatId) {
	const auto section = controller->mainSection();
	if (!section) {
		return false;
	}
	const auto route = Window::ParseRoute(section->sectionKey());
	return (route.type == Window::RouteType::Chat) && (route.id == chatId);
}

} // namespace

void FillChatMenu(
		Ui::PopupMenu *menu,
		Window::SessionController *controller,
		const Data::Chat &chat,
		Folder folder) {
	const auto session = &controller->session();
	const auto chats = &session->data().chats();
	const auto id = chat.id;
	const auto name = Support::ChatTitle(session, chat);
	const auto peer = (!chat.isConversation
		&& !chat.saved
		&& !chat.peerDeleted
		&& !chat.anonymous)
		? chat.peerUserId
		: QString();

	if (folder == Folder::Requests) {
		menu->addAction(tr::lng_dialogs_accept(tr::now), [=] {
			chats->acceptRequest(id);
			controller->showToast(tr::lng_dialogs_request_accepted(tr::now));
		}, &st::dialogsMenuAccept);
		menu->addItem(DangerItem(tr::lng_dialogs_decline(tr::now), [=] {
			chats->declineRequest(id);
			if (IsOpen(controller, id)) {
				controller->clearMainStack(Window::Tab::Chats);
			}
		}, &st::dialogsMenuDecline));
		if (!peer.isEmpty()) {
			menu->addSeparator();
			menu->addAction(tr::lng_dialogs_view_profile(tr::now), [=] {
				controller->openRoute(Window::Routes::User(peer));
			}, &st::dialogsMenuProfile);
			menu->addItem(DangerItem(tr::lng_block_user(tr::now), [=] {
				ShowBlockUserBox(controller, peer, name);
			}, &st::menuIconBlock));
		}
		return;
	}

	if (chat.unread > 0) {
		const auto seq = chat.lastSeq;
		menu->addAction(tr::lng_dialogs_mark_read(tr::now), [=] {
			session->data().messages().markRead(id, seq);
		}, &st::dialogsMenuRead);
	}
	if (CanPin(chat)) {
		const auto pinned = chat.pinned;
		const auto group = chat.isGroup;
		menu->addAction(
			pinned ? tr::lng_unpin(tr::now) : tr::lng_pin(tr::now),
			[=] {
				const auto limit
					= session->account().clientConfig().maxPinnedChats();
				if (!pinned && limit > 0 && PinnedCount(*chats, group) >= limit) {
					controller->showToast(tr::lng_dialogs_pin_limit(
						tr::now,
						lt_count,
						limit));
					return;
				}
				chats->setPinned(id, !pinned);
			},
			&st::menuIconPin);
	}
	if (!chat.saved) {
		if (chat.muted) {
			menu->addAction(tr::lng_chat_unmute(tr::now), [=] {
				chats->muteFor(id, u"off"_s);
			}, &st::menuIconUnmute);
		} else {
			const auto durations
				= session->account().clientConfig().muteDurations();
			auto item = Ui::MenuItem();
			item.text = tr::lng_chat_mute(tr::now);
			item.icon = &st::menuIconMute;
			item.fillSubmenu = [=](Ui::PopupMenu *submenu) {
				for (const auto &duration : durations) {
					submenu->addAction(
						MuteDurationText(duration),
						[=] { chats->muteFor(id, duration); },
						(duration == u"forever")
							? &st::menuIconMute
							: &st::dialogsMenuClock);
				}
			};
			menu->addItem(std::move(item));
		}
	}
	const auto archived = chat.archived;
	menu->addAction(
		archived
			? tr::lng_chat_unarchive(tr::now)
			: tr::lng_chat_archive(tr::now),
		[=] {
			chats->setArchived(id, !archived);
			controller->showToast(archived
				? tr::lng_dialogs_unarchived_toast(tr::now)
				: tr::lng_dialogs_archived_toast(tr::now));
		},
		&st::menuIconArchive);
	if (!peer.isEmpty()) {
		menu->addSeparator();
		menu->addAction(tr::lng_dialogs_view_profile(tr::now), [=] {
			controller->openRoute(Window::Routes::User(peer));
		}, &st::dialogsMenuProfile);
		menu->addAction(tr::lng_block_user(tr::now), [=] {
			ShowBlockUserBox(controller, peer, name);
		}, &st::menuIconBlock);
	}
	menu->addSeparator();
	// What the owner of this kind of chat adds («پایان گفتگو», «اطلاعات
	// گروه», «ترک گروه»…).
	Window::FillChatMenu(menu, controller, id);
	if (chat.isGroup) {
		// A group is left, not deleted: its module offers that.
		return;
	}
	const auto copy = chat;
	menu->addItem(DangerItem(tr::lng_chat_delete_chat(tr::now), [=] {
		ShowDeleteChatBox(controller, copy);
	}, &st::menuIconDelete));
}

void ShowDeleteChatBox(
		Window::SessionController *controller,
		const Data::Chat &chat) {
	const auto session = &controller->session();
	const auto id = chat.id;
	const auto name = Support::ChatTitle(session, chat);
	const auto canForBoth = CanDeleteForBoth(chat)
		&& session->account().clientConfig().deleteChatForBothEnabled();
	const auto forBoth = std::make_shared<bool>(false);
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_chat_delete_chat(tr::now);
	args.text = chat.saved
		? tr::lng_dialogs_delete_saved_sure(tr::now)
		: chat.isConversation
		? (tr::lng_dialogs_delete_sure(tr::now, lt_name, name)
			+ QLatin1Char('\n')
			+ tr::lng_dialogs_delete_conversation_about(tr::now))
		: tr::lng_dialogs_delete_sure(tr::now, lt_name, name);
	args.confirmText = tr::lng_dialogs_delete_confirm(tr::now);
	args.destructive = true;
	if (canForBoth) {
		args.checkboxText = tr::lng_dialogs_delete_also_for(
			tr::now,
			lt_name,
			name);
		args.checkboxChanged = [=](bool checked) { *forBoth = checked; };
	}
	args.confirmed = [=](std::function<void()> close) {
		const auto open = IsOpen(controller, id);
		session->data().chats().deleteChat(id, canForBoth && *forBoth);
		close();
		if (open) {
			controller->clearMainStack(Window::Tab::Chats);
		}
		controller->showToast(
			tr::lng_dialogs_deleted_toast(tr::now, lt_name, name));
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowBlockUserBox(
		Window::SessionController *controller,
		const QString &userId,
		const QString &name) {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_block_user(tr::now);
	args.text = tr::lng_dialogs_block_sure(tr::now, lt_name, name);
	args.confirmText = tr::lng_block_user(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		controller->session().data().graph().block(
			userId,
			controller,
			[=](const Data::GraphState &) {
				controller->showToast(tr::lng_blocked_toast(tr::now));
			},
			[=](const Api::Error &error) {
				Support::ShowError(controller, error);
			});
		close();
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

} // namespace Dialogs
