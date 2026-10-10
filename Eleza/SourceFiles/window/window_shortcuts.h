// The keyboard shortcuts of the session window: one table, one dispatcher.
//
//   Ctrl+1 … Ctrl+5   the tabs (الی, گفتگو, جستجو, من, تنظیمات)
//   Ctrl+,            settings
//   Ctrl+N            new message
//   Ctrl+K            search the chats
//   Ctrl+F            search in what is on screen (the chat, the list, the search tab)
//   F5, Ctrl+R        refresh the visible list
//   Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PgDown / Ctrl+PgUp, Alt+Down / Alt+Up
//                     next / previous chat
//   Esc, Alt+Left     back (closes a box, a search, a section)
//   Ctrl+W            close the current box or section (the window when there is none)
//   Ctrl+Shift+D      light / dark
//   Ctrl+Q            quit
//
// The main window creates the QShortcuts from Shortcuts() and runs a command:
// what the window knows itself (tabs, back, close, quit, theme) is done there;
// the rest is offered to the section that has the focus, then to the other
// visible one (SectionWidget::handleCommand), then to the handlers modules
// registered for the whole window:
//
//   Window::RegisterCommandHandler(Window::Command::NewMessage, [](
//           Window::SessionController *controller) {
//       Dialogs::ShowNewMessageBox(controller);
//       return true;
//   });
//
// A module never creates a window-wide QShortcut of its own: two shortcuts on
// one sequence are ambiguous in Qt and neither fires. Keys that only mean
// something inside one widget stay in that widget's keyPressEvent.
#pragma once

#include <QtGui/QKeySequence>

#include <functional>
#include <vector>

namespace Window {

class SessionController;

enum class Command {
	ShowTabEli,
	ShowTabChats,
	ShowTabSearch,
	ShowTabMe,
	ShowTabSettings,
	NewMessage,
	SearchChats,
	Find,
	Refresh,
	NextChat,
	PreviousChat,
	Back,
	Close,
	ToggleTheme,
	Quit,
};

struct ShortcutEntry {
	QKeySequence keys;
	Command command = Command::Back;
};

// Every sequence appears once.
[[nodiscard]] const std::vector<ShortcutEntry> &Shortcuts();

// Returns true if it did something.
using CommandHandler = std::function<bool(SessionController *controller)>;

void RegisterCommandHandler(Command command, CommandHandler handler);
[[nodiscard]] bool RunCommandHandlers(
	Command command,
	SessionController *controller);

} // namespace Window
