// The bell: the unread badge (kept current by `notif` updates - no polling),
// the notifications list and «mark all read».
#pragma once

#include "data/data_social_types.h"

namespace Data {

class Session;

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
