#include "data/data_notifications.h"

#include "data/data_group_types.h"
#include "data/data_json.h"
#include "data/data_types.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>

namespace Data {
namespace {

// "HH:MM" -> minutes of the day (as the server reads it: junk is 0).
[[nodiscard]] int MinutesOf(const QString &text) {
	const auto parts = text.split(u':');
	const auto hours = parts.value(0).toInt();
	const auto minutes = parts.value(1).toInt();
	return hours * 60 + minutes;
}

} // namespace

bool InQuietHours(const QJsonObject &quietHours, const QDateTime &now) {
	if (!quietHours.value("enabled"_L1).toBool()) {
		return false;
	}
	// The server's defaults for what is not there (settings.Default).
	const auto start = MinutesOf(quietHours.value("start"_L1).toString(u"23:00"_s));
	const auto end = MinutesOf(quietHours.value("end"_L1).toString(u"07:00"_s));
	auto zone = QTimeZone(quietHours.value("tz"_L1).toString(u"Asia/Tehran"_s).toUtf8());
	if (!zone.isValid()) {
		zone = QTimeZone::utc(); // as the server falls back
	}
	const auto local = now.toTimeZone(zone).time();
	const auto current = local.hour() * 60 + local.minute();
	return (start <= end)
		? (current >= start && current < end)
		: (current >= start || current < end); // across midnight
}

ChatAlert ChatAlertFor(
		const Chat &chat,
		const QJsonObject &notificationSettings,
		const QDateTime &now) {
	// «Request threads never notify».
	if (chat.isRequest() || chat.request == u"incoming") {
		return ChatAlert::None;
	}
	const auto &settings = notificationSettings;
	if (InQuietHours(settings.value("quiet_hours"_L1).toObject(), now)) {
		return ChatAlert::None;
	}
	if (chat.isConversation || chat.section == u"conversation") {
		const auto conversations = settings.value("conversations"_L1).toObject();
		if (!conversations.value("enabled"_L1).toBool(true)
			|| !conversations.value("new_message"_L1).toBool(true)) {
			return ChatAlert::None;
		}
	}
	if (chat.info.hidden || (chat.group && chat.group->hidden)) {
		const auto mode = settings.value("hidden_chats"_L1).toString(u"anonymous"_s);
		if (mode == u"none") {
			return ChatAlert::None;
		} else if (mode != u"normal") {
			return ChatAlert::Anonymous;
		}
	}
	return ChatAlert::Full;
}

Notifications::Notifications(Session *session)
: QObject(session)
, _session(session) {
}

void Notifications::applyUnread(qint64 news, const QMap<QString, qint64> &kinds) {
	const auto unread = int(std::max<qint64>(0, news));
	if (_unread == unread && _kinds == kinds) {
		return;
	}
	_unread = unread;
	_kinds = kinds;
	Q_EMIT unreadChanged();
}

void Notifications::refreshUnread(base::Guard guard, Fn<void()> done) {
	_session->transport().rpc(u"notif.unread"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			applyUnread(Json::Int64(result, "news"_L1), Json::CountMap(result.value("kinds"_L1)));
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=](const Api::Error &) {
			if (done && guard.alive()) {
				done();
			}
		})
		.send();
}

void Notifications::load(
		const QString &tab,
		qint64 before,
		base::Guard guard,
		Fn<void(const QVector<NotificationEntry> &)> done,
		FailCallback fail) {
	auto body = QJsonObject();
	Json::Put(body, "tab"_L1, tab);
	if (before > 0) {
		body.insert("before"_L1, QString::number(before));
	}
	_session->transport().rpc(u"notif.list"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto list = Json::List<NotificationEntry>(result.value("notifications"_L1));
			auto actors = QStringList();
			for (const auto &entry : list) {
				for (const auto &actor : entry.actors) {
					if (!actors.contains(actor)) {
						actors.push_back(actor);
					}
				}
			}
			// The rows name people: have their cards first.
			_session->users().ensure(actors, this, [=] {
				if (done && guard.alive()) {
					done(list);
				}
			});
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Notifications::markAllRead() {
	const auto before = _unread;
	const auto kinds = _kinds;
	if (_unread) {
		_unread = 0;
		Q_EMIT unreadChanged();
	}
	_session->transport().rpc(u"notif.read"_s)
		.guard(this)
		.done([=, this] {
			refreshUnread();
		})
		.fail([=, this](const Api::Error &error) {
			applyUnread(before, kinds);
			_session->reportFailure(u"notifications.read"_s, QString(), error);
		})
		.send();
}

} // namespace Data
