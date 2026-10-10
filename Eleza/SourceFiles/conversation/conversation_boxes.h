// The boxes and menus of the conversation layer, and the entry points other
// screens use (the topics list, the search results, the route handlers).
//
//   Conversation::ShowNewConversationBox(controller);            // «گفتگوی تازه»
//   Conversation::ShowJoinBox(controller, id);                   // «پیوستن به گفتگو»
//   Conversation::OpenConversation(controller, id);              // radar / chat / join, whatever fits
//   Conversation::ShowTopicMenu(this, controller, card, globalPos);
#pragma once

#include "data/data_social_types.h"

#include <QtCore/QObject>
#include <QtCore/QPoint>

class QWidget;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Conversation {

struct Card;

// What the boxes tell the lists (the open list itself is refreshed by the
// server's hints; these cover what this client did itself, at once).
class Notifier final : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;

Q_SIGNALS:
	// Hidden locally («دیگر نشان نده», a sent report, joined, closed).
	void topicHidden(const QString &id);
	// I opened a topic: the lists read the first page again.
	void topicCreated(const QString &id);

};
[[nodiscard]] Notifier *Events();

void ShowNewConversationBox(
	Window::SessionController *controller,
	const QString &prefill = QString());
void ShowJoinBox(Window::SessionController *controller, const QString &id);
void ShowReportBox(Window::SessionController *controller, const QString &id);
// «دعوت در الی»: publishes a post that invites people to the topic.
void ShowInviteBox(Window::SessionController *controller, const QString &id);
// «گفتگو را ببندی؟», then ends it; `done` after the server confirmed.
void ConfirmEnd(
	Window::SessionController *controller,
	const QString &id,
	std::function<void()> done = nullptr);
// «دیگر نشان نده» with its toast.
void DismissTopic(Window::SessionController *controller, const QString &id);
// The context menu of a topic card.
void ShowTopicMenu(
	QWidget *parent,
	Window::SessionController *controller,
	const Card &card,
	const QPoint &globalPosition);

// The conversation actions of a chat's menus (the chat list row, the chat's
// «…»): «پایان گفتگو» while it runs, «مسدود کردن» for a masked counterpart.
// Adds nothing for a direct chat. Registered as a Window::ChatMenuFiller.
void FillChatMenu(
	Ui::PopupMenu *menu,
	Window::SessionController *controller,
	const QString &chatId);

// Where a conversation id leads for me: my waiting topic -> the radar, a
// conversation I take part in -> its chat, somebody's open topic -> the join box.
void OpenConversation(Window::SessionController *controller, const QString &id);
// After joining / a match: finds the chat row (it is projected a moment
// later) and opens it; a toast when it did not appear in time.
void OpenChatOf(Window::SessionController *controller, const QString &conversationId);

} // namespace Conversation
