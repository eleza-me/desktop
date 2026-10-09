// The signed-in session: created by Main::Account when tokens exist,
// destroyed when the session ends. It owns everything that belongs to one
// user: the local database, the realtime connection and the data stores.
//
//   if (const auto session = account.session()) {
//       auto &data = session->data();          // Data::Session: chats, messages, users, ...
//       session->api().request(Api::Get, u"/v1/..."_s)...   // plain HTTP
//       session->rpc(u"chat.pins"_s)...        // an RPC method: socket first, HTTP fallback
//       session->rt().state();                 // connection state for the title bar
//   }
//
// Start-up: the cached state is loaded from the database (ready()), then the
// realtime connection starts and the inbox is read once; after that the
// stores live on updates.
#pragma once

#include "data/data_transport.h"
#include "rt/rt_client.h"

namespace Api {
class Client;
} // namespace Api

namespace Storage {
class Database;
} // namespace Storage

namespace Data {
class Session;
} // namespace Data

namespace Main {

class Account;

class Session final : public QObject {
	Q_OBJECT

public:
	Session(Account *account, const QString &userId);
	~Session();

	[[nodiscard]] Account &account() const {
		return *_account;
	}
	[[nodiscard]] QString userId() const {
		return _userId;
	}
	// <workdir>/account/user-<id>
	[[nodiscard]] QString userDir() const {
		return _userDir;
	}

	[[nodiscard]] Api::Client &api() const;
	[[nodiscard]] Rt::Client &rt() const {
		return *_rt;
	}
	[[nodiscard]] Storage::Database &local() const {
		return *_database;
	}
	[[nodiscard]] Data::Session &data() const {
		return *_data;
	}
	[[nodiscard]] Data::Transport &transport() const;
	// Shortcut for transport().rpc(method).
	[[nodiscard]] Data::RpcBuilder rpc(const QString &method) const;

	// The cached state is in the stores.
	[[nodiscard]] bool isReady() const {
		return _ready;
	}
	[[nodiscard]] Rt::State connectionState() const {
		return _rt->state();
	}

	// Stops the connection and the stores (called before destruction).
	void shutdown();

Q_SIGNALS:
	void ready();
	void connectionStateChanged(Rt::State state);

private:
	class Transport;
	class RtDelegate;

	Account *_account = nullptr;
	QString _userId;
	QString _userDir;
	std::unique_ptr<Storage::Database> _database;
	std::unique_ptr<RtDelegate> _rtDelegate;
	std::unique_ptr<Rt::Client> _rt;
	std::unique_ptr<Transport> _transport;
	std::unique_ptr<Data::Session> _data;
	bool _ready = false;
	bool _shutdown = false;

};

} // namespace Main
