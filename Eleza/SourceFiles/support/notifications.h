// Desktop notifications for incoming messages: our own toast windows stacked at
// the corner of the screen (userpic, name, a preview of the text) and a short
// sound.
//
// The manager is created with the session services (support/session_services.h);
// feature code normally does not touch it. It listens to
// Data::Sync::messageReceived (chats) and Data::Groups::messageReceived
// (groups, which have a notify level of their own) and shows a toast when
//   * notifications are on (Core::Settings::notifications().desktopEnabled),
//   * the chat is not muted and is not «Saved messages»,
//   * the window is not active, or another chat is on screen
//     (Data::Chats::visibleChat) — unless `onlyWhenInactive` is set.
// The text is hidden when the privacy setting «نمایش متن پیام» (showPreview) is
// off, the name when `showSender` is off. A click activates the window and opens
// the route chat/<id>; hovering pauses the timer; at most three are stacked and
// the rest wait; reading the chat removes its toast.
//
//   Support::SessionServices::For(controller)->notifications()->showCustom({...});
//   route "debug/notification" shows a sample one.
#pragma once

#include "data/data_types.h"
#include "ui/effects/animations.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <deque>
#include <memory>
#include <vector>

class QSoundEffect;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class IconButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Support {

class UserpicButton;

struct NotificationData {
	QString key; // one toast per key (the chat id): a newer one replaces it
	QString title;
	QString text;
	QString route; // opened on click
	QString userId; // the userpic; empty with `anonymous` for a mask
	bool anonymous = false;
};

// What a message's notification says under the current settings. Nullopt:
// do not notify (mine, muted, saved messages, invisible).
[[nodiscard]] std::optional<NotificationData> NotificationFor(
	Main::Session *session,
	const Data::Message &message);
// The same for a message of a group that Data::Groups decided may alert (it
// applied the group's notify level and mute: a mention gets through a muted
// group). The toast names the sender and the group.
[[nodiscard]] std::optional<NotificationData> GroupNotificationFor(
	Main::Session *session,
	const Data::Message &message,
	bool addressesMe);

class NotificationWidget;

class NotificationsManager final : public QObject {
public:
	NotificationsManager(Window::SessionController *controller, QObject *parent);
	~NotificationsManager();

	// Shows (or queues) a toast regardless of the window state.
	void showCustom(const NotificationData &data);
	void playSound();
	void clearAll();
	void clearFor(const QString &key);

	static constexpr auto kMaxVisible = 3;

private:
	void messageReceived(const Data::Message &message);
	void groupMessageReceived(const Data::Message &message, bool addressesMe);
	void alert(const NotificationData &data);
	[[nodiscard]] bool shouldShow(const Data::Message &message) const;
	void show(const NotificationData &data);
	void showNext();
	void removed(NotificationWidget *widget);
	void relayout();
	[[nodiscard]] QRect anchorRect() const;
	void activated(const QString &route);

	Window::SessionController *_controller = nullptr;
	std::vector<NotificationWidget*> _visible; // bottom to top
	std::deque<NotificationData> _queue;
	std::unique_ptr<QSoundEffect> _sound;
	qint64 _lastSoundAt = 0;

};

} // namespace Support
