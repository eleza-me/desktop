// Cross-module hooks: a module publishes a widget under a string id and another
// module embeds it without including the first one's headers.
//
// Embedded lists (post lists inside a profile, inside search results…):
//
//   // eli/eli_module.cpp
//   Window::RegisterEmbed(u"eli.posts"_s, [](QWidget *parent,
//           Window::SessionController *controller, const QVariantMap &params) {
//       return Ui::Owned<Window::EmbeddedList>(new Eli::PostsEmbed(parent, controller, params));
//   });
//   // profile/…
//   _posts = Window::CreateEmbed(u"eli.posts"_s, this, controller,
//       { { u"source"_s, u"user"_s }, { u"user"_s, userId } });   // null if not registered
//
// An EmbeddedList lives inside the host's scroll area: it has no scroll of its own, it
// reports its height through resizeGetHeight(width) + heightChanged(), paints only
// the range the host gives it and loads more when the host asks.
//
// Known ids and params:
//   "eli.posts"   source = "user" | "user_replies" | "user_media" | "bookmarks" | "search"
//                 user = <user id> (user* sources), query = <text> (search)
//
// Side sub-tabs (the «گفتگو» tab shows «چت‌ها» from dialogs and «موضوع‌ها» from
// conversation under one segmented control owned by dialogs):
//
//   Window::RegisterSideSubTab(Window::Tab::Chats, { .id = u"topics"_s, .order = 10,
//       .title = [] { return tr::lng_conversation_topics(tr::now); },
//       .factory = [](QWidget *parent, Window::SessionController *controller) {
//           return Ui::Owned<Ui::RpWidget>(new Conversation::TopicsList(parent, controller)); } });
//
// The widget of a sub-tab fills the area below the segmented control and scrolls itself.
//
// Chat menu fillers (the context menu of a chat row and the «…» menu of the chat
// screen get the same extra actions from the module that owns a kind of chat):
//
//   // conversation/conversation_module.cpp
//   Window::RegisterChatMenuFiller(Conversation::FillChatMenu);  // «پایان گفتگو», «مسدود کردن»
//   // dialogs/dialogs_menu.cpp, history/history_widget.cpp
//   Window::FillChatMenu(menu, controller, chatId);              // after their own actions
#pragma once

#include "ui/rp_widget.h"
#include "window/window_registry.h"

#include <QtCore/QVariantMap>
#include <QtCore/QVector>

#include <functional>

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {

class EmbeddedList : public Ui::RpWidget {
	Q_OBJECT

public:
	using Ui::RpWidget::RpWidget;

	// The host scrolled: [top, bottom) of this widget is visible (widget coordinates).
	virtual void setVisibleRange(int top, int bottom) {
		Q_UNUSED(top);
		Q_UNUSED(bottom);
	}
	// The host is near the end of the list.
	virtual void loadMore() {
	}
	// Pull-to-refresh / tab reselected.
	virtual void reload() {
	}

Q_SIGNALS:
	void heightChanged();
	// Rows are known to be absent (the host may show its own empty state).
	void emptyChanged(bool empty);

};

using EmbedFactory = std::function<Ui::object_ptr<EmbeddedList>(
	QWidget *parent,
	SessionController *controller,
	const QVariantMap &params)>;

void RegisterEmbed(const QString &id, EmbedFactory factory);
[[nodiscard]] bool HasEmbed(const QString &id);
// Null (an empty object_ptr) when nobody registered `id`.
[[nodiscard]] Ui::object_ptr<EmbeddedList> CreateEmbed(
	const QString &id,
	QWidget *parent,
	SessionController *controller,
	const QVariantMap &params = {});

struct SideSubTab {
	QString id;
	int order = 0; // ascending; the owner of the tab is 0
	std::function<QString()> title;
	std::function<Ui::object_ptr<Ui::RpWidget>(
		QWidget *parent,
		SessionController *controller)> factory;
};

using ChatMenuFiller = std::function<void(
	Ui::PopupMenu *menu,
	SessionController *controller,
	const QString &chatId)>;

void RegisterChatMenuFiller(ChatMenuFiller filler);
[[nodiscard]] bool HasChatMenuFillers();
// Every registered filler appends what it has for this chat (often nothing).
void FillChatMenu(
	Ui::PopupMenu *menu,
	SessionController *controller,
	const QString &chatId);

void RegisterSideSubTab(Tab tab, SideSubTab subTab);
// Sorted by order.
[[nodiscard]] QVector<SideSubTab> SideSubTabs(Tab tab);

} // namespace Window
