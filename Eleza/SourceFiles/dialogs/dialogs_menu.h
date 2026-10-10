// The context menu of a chat row, the «حذف گفتگو» confirmation and the
// «پیام جدید» box.
#pragma once

#include "data/data_types.h"
#include "dialogs/dialogs_logic.h"

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Dialogs {

// Pin, mute, archive, mark as read, block, delete… for a row of `folder`.
void FillChatMenu(
	Ui::PopupMenu *menu,
	Window::SessionController *controller,
	const Data::Chat &chat,
	Folder folder);

// Telegram-style: «چت با … حذف شود؟» with the «برای … هم پاک شود» checkbox
// where both sides can be cleared.
void ShowDeleteChatBox(
	Window::SessionController *controller,
	const Data::Chat &chat);

void ShowBlockUserBox(
	Window::SessionController *controller,
	const QString &userId,
	const QString &name);

// «پیام جدید»: «پیام‌های ذخیره‌شده», the people of the recent chats and the
// ones I follow; typing searches people by name / @handle.
void ShowNewMessageBox(Window::SessionController *controller);

} // namespace Dialogs
