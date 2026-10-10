#include "window/window_shortcuts.h"

#include <QtCore/QHash>

namespace Window {
namespace {

[[nodiscard]] QHash<int, std::vector<CommandHandler>> &Handlers() {
	static auto result = QHash<int, std::vector<CommandHandler>>();
	return result;
}

} // namespace

const std::vector<ShortcutEntry> &Shortcuts() {
	static const auto result = std::vector<ShortcutEntry>{
		{ QKeySequence(Qt::CTRL | Qt::Key_1), Command::ShowTabEli },
		{ QKeySequence(Qt::CTRL | Qt::Key_2), Command::ShowTabChats },
		{ QKeySequence(Qt::CTRL | Qt::Key_3), Command::ShowTabSearch },
		{ QKeySequence(Qt::CTRL | Qt::Key_4), Command::ShowTabMe },
		{ QKeySequence(Qt::CTRL | Qt::Key_5), Command::ShowTabSettings },
		{ QKeySequence(Qt::CTRL | Qt::Key_Comma), Command::ShowTabSettings },
		{ QKeySequence(Qt::CTRL | Qt::Key_N), Command::NewMessage },
		{ QKeySequence(Qt::CTRL | Qt::Key_K), Command::SearchChats },
		{ QKeySequence(Qt::CTRL | Qt::Key_F), Command::Find },
		{ QKeySequence(Qt::Key_F5), Command::Refresh },
		{ QKeySequence(Qt::CTRL | Qt::Key_R), Command::Refresh },
		{ QKeySequence(Qt::CTRL | Qt::Key_Tab), Command::NextChat },
		{ QKeySequence(Qt::CTRL | Qt::Key_PageDown), Command::NextChat },
		{ QKeySequence(Qt::ALT | Qt::Key_Down), Command::NextChat },
		{ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), Command::PreviousChat },
		{ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), Command::PreviousChat },
		{ QKeySequence(Qt::CTRL | Qt::Key_PageUp), Command::PreviousChat },
		{ QKeySequence(Qt::ALT | Qt::Key_Up), Command::PreviousChat },
		{ QKeySequence(Qt::ALT | Qt::Key_Left), Command::Back },
		{ QKeySequence(Qt::CTRL | Qt::Key_W), Command::Close },
		{ QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), Command::ToggleTheme },
		{ QKeySequence(Qt::CTRL | Qt::Key_Q), Command::Quit },
	};
	return result;
}

void RegisterCommandHandler(Command command, CommandHandler handler) {
	if (handler) {
		Handlers()[int(command)].push_back(std::move(handler));
	}
}

bool RunCommandHandlers(Command command, SessionController *controller) {
	const auto i = Handlers().constFind(int(command));
	if (i == Handlers().cend()) {
		return false;
	}
	for (const auto &handler : *i) {
		if (handler(controller)) {
			return true;
		}
	}
	return false;
}

} // namespace Window
