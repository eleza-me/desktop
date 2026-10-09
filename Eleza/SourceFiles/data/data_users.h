// People cards (Data::User): batched reads (`users.cards`), the account
// database as the first answer, live `profile` frames refreshing one card.
// Also holds the signed-in user's own profile.
//
//   const auto card = users.get(id);   // the card if known; queues a fetch otherwise
//   connect(&users, &Data::Users::changed, this, [=](const QStringList &ids) { ... });
#pragma once

#include "api/api_error.h"
#include "base/timer.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

namespace Data {

class Session;

class Users final : public QObject {
	Q_OBJECT

public:
	explicit Users(Session *session);

	[[nodiscard]] QString myId() const;

	// The card if known; an invalid User (empty id) otherwise. Never fetches.
	[[nodiscard]] User peek(const QString &id) const;
	[[nodiscard]] bool has(const QString &id) const {
		return _cards.contains(id);
	}
	// The card if known; otherwise queues a batched fetch (changed() follows).
	[[nodiscard]] User get(const QString &id);
	// Queues a card read; calls made in the same 20 ms share one request.
	void request(const QString &id);
	void request(const QStringList &ids);

	// Makes the cards available (database, then one batched request per 50
	// ids) and calls `done`. Unknown / deleted people get a `deleted`
	// placeholder so rows stop asking.
	void ensure(const QStringList &ids, base::Guard guard, Fn<void()> done);
	// Refetches one card (a `profile` frame said it changed).
	void refresh(const QString &id, base::Guard guard = base::Guard(), Fn<void(const User &)> done = nullptr);

	// Cards that came with another answer (feed authors, search results, ...).
	void seed(const User &user);
	void seed(const QVector<User> &users);
	void seed(const QVector<Profile> &profiles);
	void seedJson(const QJsonValue &listOrMap);

	// ---- the signed-in user
	// Valid once loaded from the cache or the server.
	[[nodiscard]] const Profile &me() const {
		return _me;
	}
	void setMe(const Profile &profile);
	// `GET /v1/me` (over the connection when up).
	void refreshMe(
		base::Guard guard = base::Guard(),
		Fn<void(const Profile &)> done = nullptr,
		Fn<void(const Api::Error &)> fail = nullptr);
	// Reads the cached profile from the database (startup).
	void loadCachedMe(base::Guard guard, Fn<void()> done);

Q_SIGNALS:
	// Cards that were added or changed.
	void changed(const QStringList &ids);
	void meChanged();

private:
	void flush();
	void fetch(const QStringList &ids, Fn<void()> done);
	void fetchChunk(const QStringList &ids, int offset, Fn<void()> done);
	bool store(const User &user);

	Session *_session = nullptr;
	QHash<QString, User> _cards;
	QSet<QString> _pending;
	base::Timer _flushTimer;
	Profile _me;

};

} // namespace Data
