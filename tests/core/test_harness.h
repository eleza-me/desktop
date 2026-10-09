// A Data::Session on a FakeTransport (and optionally a real database) for
// the store tests. The transport answers the calls every session start makes
// from the captured fixtures; a test adds its own answers with `on()`.
#pragma once

#include "api/api_client.h"
#include "data/data_chats.h"
#include "data/data_messages.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_sync.h"
#include "storage/storage_database.h"
#include "test_fakes.h"

#include <QtCore/QTemporaryDir>

namespace Test {

class Harness final {
public:
	using Call = FakeTransport::Call;
	using Reply = FakeTransport::Reply;

	struct Options {
		QString databasePath;                       // empty: nothing is cached
		QString cacheDir;                           // empty: media stays in memory
		Api::ClientConfig *clientConfig = nullptr;
	};

	explicit Harness(const QString &databasePath = QString())
	: Harness(Options{ databasePath, QString(), nullptr }) {
	}
	explicit Harness(const Options &options)
	: api(http.base(), u"1.0.0"_s) {
		const auto &databasePath = options.databasePath;
		http.handler = [](const HttpServer::Request &) {
			return HttpServer::Response::Ok();
		};
		transport.handler = [this](const Call &call) {
			if (const auto i = _handlers.constFind(call.method); i != _handlers.cend()) {
				return (*i)(call);
			} else if (call.method == u"chat.inbox") {
				return Reply::Ok((call.body.value("section"_L1).toString() == u"message")
					? Fixture("inbox_message.json")
					: QJsonObject{ { u"chats"_s, QJsonArray() } });
			} else if (call.method == u"chat.history") {
				const auto chat = call.body.value("chat"_L1).toString();
				return Reply::Ok((chat == u"c1")
					? Fixture("history_c1.json")
					: (chat == u"c2")
					? Fixture("history_c2.json")
					: QJsonObject{ { u"messages"_s, QJsonArray() } });
			} else if (call.method == u"me.get") {
				return Reply::Ok(Fixture("me.json"));
			} else if (call.method == u"sync.state") {
				return Reply::Ok(QJsonObject{ { u"pts"_s, u"100"_s } });
			}
			return Reply::Ok();
		};
		if (!databasePath.isEmpty()) {
			database = std::make_unique<Storage::Database>(databasePath);
		}
		auto config = Data::Session::Config();
		config.userId = u"u_me"_s;
		config.transport = &transport;
		config.api = &api;
		config.database = database.get();
		config.cacheDir = options.cacheDir;
		config.clientConfig = options.clientConfig;
		session = std::make_unique<Data::Session>(config);
	}

	~Harness() {
		session->stop();
		session = nullptr;
		if (database) {
			database->flushSync();
		}
	}

	// Scripts the answer of one RPC method.
	void on(const QString &method, Fn<Reply(const Call &)> handler) {
		_handlers.insert(method, std::move(handler));
	}

	// Starts the session and waits until the first inbox is in the stores.
	[[nodiscard]] bool start() {
		auto ready = false;
		session->start([&] {
			ready = true;
		});
		return WaitFor([&] {
			return ready && session->chats().chat(u"c1"_s) != nullptr;
		});
	}

	[[nodiscard]] Data::Chats &chats() const {
		return session->chats();
	}
	[[nodiscard]] Data::Messages &messages() const {
		return session->messages();
	}
	[[nodiscard]] Data::Outbox &outbox() const {
		return session->outbox();
	}
	[[nodiscard]] Data::Sync &sync() const {
		return session->sync();
	}

	// The bodies of every call of `method`, in order.
	[[nodiscard]] QVector<QJsonObject> bodies(const QString &method) const {
		auto result = QVector<QJsonObject>();
		for (const auto &call : transport.calls) {
			if (call.method == method) {
				result.push_back(call.body);
			}
		}
		return result;
	}

	HttpServer http;
	Api::Client api;
	FakeTransport transport;
	std::unique_ptr<Storage::Database> database;
	std::unique_ptr<Data::Session> session;

private:
	QHash<QString, Fn<Reply(const Call &)>> _handlers;

};

} // namespace Test
