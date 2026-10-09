#include "data/data_users.h"

#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "storage/storage_database.h"

namespace Data {
namespace {

constexpr auto kFlushDelayMs = 20;

} // namespace

Users::Users(Session *session)
: QObject(session)
, _session(session)
, _flushTimer([this] { flush(); }) {
}

QString Users::myId() const {
	return _session->userId();
}

User Users::peek(const QString &id) const {
	return _cards.value(id);
}

User Users::get(const QString &id) {
	const auto i = _cards.constFind(id);
	if (i != _cards.cend()) {
		return *i;
	}
	request(id);
	return User();
}

void Users::request(const QString &id) {
	if (id.isEmpty() || _cards.contains(id) || _pending.contains(id)) {
		return;
	}
	_pending.insert(id);
	if (!_flushTimer.isActive()) {
		_flushTimer.callOnce(kFlushDelayMs);
	}
}

void Users::request(const QStringList &ids) {
	for (const auto &id : ids) {
		request(id);
	}
}

void Users::flush() {
	const auto ids = QStringList(_pending.cbegin(), _pending.cend());
	_pending.clear();
	// The database first, like ensure().
	ensure(ids, base::Guard(this), nullptr);
}

bool Users::store(const User &user) {
	if (!user.valid()) {
		return false;
	}
	const auto i = _cards.find(user.id);
	if (i != _cards.end() && *i == user) {
		return false;
	}
	_cards.insert(user.id, user);
	return true;
}

void Users::seed(const User &user) {
	seed(QVector<User>{ user });
}

void Users::seed(const QVector<User> &users) {
	auto ids = QStringList();
	auto rows = QVector<Storage::Database::Row>();
	for (const auto &user : users) {
		if (store(user)) {
			ids.push_back(user.id);
			rows.push_back({ user.id, user.toJson() });
		}
	}
	if (ids.isEmpty()) {
		return;
	}
	if (const auto db = _session->local()) {
		db->putUsers(rows);
	}
	Q_EMIT changed(ids);
}

void Users::seed(const QVector<Profile> &profiles) {
	auto users = QVector<User>();
	users.reserve(profiles.size());
	for (const auto &profile : profiles) {
		users.push_back(profile.card());
	}
	seed(users);
}

void Users::seedJson(const QJsonValue &listOrMap) {
	auto users = QVector<User>();
	if (listOrMap.isArray()) {
		users = Json::List<User>(listOrMap);
	} else {
		// `authors` maps: id -> card.
		const auto map = listOrMap.toObject();
		for (auto i = map.begin(); i != map.end(); ++i) {
			if (i.value().isObject()) {
				auto user = User::FromJson(i.value().toObject());
				if (user.id.isEmpty()) {
					user.id = i.key();
				}
				users.push_back(user);
			}
		}
	}
	seed(users);
}

void Users::ensure(const QStringList &ids, base::Guard guard, Fn<void()> done) {
	auto missing = QStringList();
	for (const auto &id : ids) {
		if (!id.isEmpty() && !_cards.contains(id) && !missing.contains(id)) {
			missing.push_back(id);
		}
	}
	const auto finish = [=] {
		if (done && guard.alive()) {
			done();
		}
	};
	if (missing.isEmpty()) {
		finish();
		return;
	}
	const auto fromNetwork = [=, this] {
		auto still = QStringList();
		for (const auto &id : missing) {
			if (!_cards.contains(id)) {
				still.push_back(id);
			}
		}
		if (still.isEmpty()) {
			finish();
		} else {
			fetch(still, finish);
		}
	};
	const auto db = _session->local();
	if (!db) {
		fromNetwork();
		return;
	}
	db->users(missing, this, [=, this](QHash<QString, QJsonObject> stored) {
		auto changedIds = QStringList();
		for (auto i = stored.cbegin(); i != stored.cend(); ++i) {
			auto user = User::FromJson(i.value());
			// A cached placeholder for a deleted / unknown user is not final:
			// ask the server again next time it matters.
			if (user.valid() && !_cards.contains(user.id)) {
				_cards.insert(user.id, user);
				changedIds.push_back(user.id);
			}
		}
		if (!changedIds.isEmpty()) {
			Q_EMIT changed(changedIds);
		}
		fromNetwork();
	});
}

void Users::fetch(const QStringList &ids, Fn<void()> done) {
	fetchChunk(ids, 0, std::move(done));
}

void Users::fetchChunk(const QStringList &ids, int offset, Fn<void()> done) {
	if (offset >= ids.size()) {
		if (done) {
			done();
		}
		return;
	}
	const auto batch = _session->cardsBatchSize();
	const auto chunk = ids.mid(offset, batch);
	const auto next = [=, this] {
		fetchChunk(ids, offset + batch, done);
	};
	_session->transport().rpc(u"users.cards"_s)
		.body(QJsonObject{ { u"ids"_s, chunk.join(u',') } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto users = Json::List<User>(result.value("users"_L1));
			// Deleted / unknown people: a placeholder so rows stop asking.
			for (const auto &id : chunk) {
				const auto found = std::any_of(users.cbegin(), users.cend(), [&](const User &user) {
					return user.id == id;
				});
				if (!found) {
					auto gone = User();
					gone.id = id;
					gone.deleted = true;
					users.push_back(gone);
				}
			}
			seed(users);
			next();
		})
		.fail([=](const Api::Error &error) {
			qWarning("Data: users.cards failed: %s", qPrintable(error.describe()));
			next();
		})
		.send();
}

void Users::refresh(const QString &id, base::Guard guard, Fn<void(const User &)> done) {
	if (id.isEmpty()) {
		return;
	}
	_session->transport().rpc(u"users.cards"_s)
		.body(QJsonObject{ { u"ids"_s, id } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto users = Json::List<User>(result.value("users"_L1));
			seed(users);
			if (done && guard.alive()) {
				done(peek(id));
			}
		})
		.fail([=, this](const Api::Error &) {
			if (done && guard.alive()) {
				done(peek(id));
			}
		})
		.send();
}

void Users::setMe(const Profile &profile) {
	if (!profile.valid()) {
		return;
	}
	_me = profile;
	seed(profile.card());
	if (const auto db = _session->local()) {
		db->setMeta(u"me"_s, profile.toJson());
	}
	Q_EMIT meChanged();
}

void Users::refreshMe(
		base::Guard guard,
		Fn<void(const Profile &)> done,
		Fn<void(const Api::Error &)> fail) {
	_session->transport().rpc(u"me.get"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			setMe(Profile::FromJson(result));
			if (done && guard.alive()) {
				done(_me);
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Users::loadCachedMe(base::Guard guard, Fn<void()> done) {
	const auto db = _session->local();
	if (!db) {
		if (done && guard.alive()) {
			done();
		}
		return;
	}
	db->meta(u"me"_s, this, [=, this](QJsonValue value) {
		if (value.isObject() && !_me.valid()) {
			_me = Profile::FromJson(value.toObject());
			if (_me.valid()) {
				store(_me.card());
				Q_EMIT meChanged();
			}
		}
		if (done && guard.alive()) {
			done();
		}
	});
}

} // namespace Data
