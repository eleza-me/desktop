// The bell: the unread badge (kept current by `notif` updates - no polling),
// the notifications list and «mark all read».
#pragma once

#include "data/data_social_types.h"

#include <QtCore/QJsonObject>

class QDateTime;

namespace Data {

class Session;
struct Chat;

// The desktop gets no pushes and makes its own toasts: they follow the rules
// the server applies before it pushes a chat's message
// (internal/notification/push.go onChat, groups.go groupRecipientOf), read
// from the synced settings document's `notifications` object.
enum class ChatAlert {
	None,      // nothing at all
	Anonymous, // «الزا» / «پیام جدید»: no sender, no text, no chat
	Full,
};
// Quiet hours ({enabled, start "23:00", end "07:00", tz}) at `now`.
[[nodiscard]] bool InQuietHours(const QJsonObject &quietHours, const QDateTime &now);
// A message in `chat` (not mine, the chat neither muted nor archived:
// those are the caller's).
[[nodiscard]] ChatAlert ChatAlertFor(
	const Chat &chat,
	const QJsonObject &notificationSettings,
	const QDateTime &now);

class Notifications final : public QObject {
	Q_OBJECT

public:
	explicit Notifications(Session *session);

	// Unread notifications (the badge).
	[[nodiscard]] int unread() const {
		return _unread;
	}
	// Per-kind people counts ("follow_request" -> 2, ...).
	[[nodiscard]] const QMap<QString, qint64> &kinds() const {
		return _kinds;
	}
	[[nodiscard]] int kind(const QString &name) const {
		return int(_kinds.value(name));
	}

	// Reads the counts once (start); afterwards `notif` updates keep them.
	void refreshUnread(base::Guard guard = base::Guard(), Fn<void()> done = nullptr);
	// tab: "" / "all" | "mentions" | "requests"; `before` pages by updated_at (ms).
	void load(const QString &tab, qint64 before, base::Guard guard, Fn<void(const QVector<NotificationEntry> &)> done, FailCallback fail = nullptr);
	void markAllRead();

	// From Data::Sync.
	void applyUnread(qint64 news, const QMap<QString, qint64> &kinds);

Q_SIGNALS:
	void unreadChanged();

private:
	Session *_session = nullptr;
	QMap<QString, qint64> _kinds;
	int _unread = 0;

};

} // namespace Data
