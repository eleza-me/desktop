#include "data/data_notifications.h"

#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"

namespace Data {

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
