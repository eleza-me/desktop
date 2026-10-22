// Test doubles shared by the store tests: a scriptable Data::Transport and a
// tiny in-process HTTP server.
#pragma once

#include "api/api_client.h"
#include "data/data_transport.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

namespace Test {

[[nodiscard]] inline QJsonObject Json(const QByteArray &text) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(text, &error);
	Q_ASSERT_X(error.error == QJsonParseError::NoError, "Test::Json", text.constData());
	return document.object();
}

[[nodiscard]] inline QJsonObject Fixture(const char *name) {
	auto file = QFile(QStringLiteral(ELEZA_TESTDATA_DIR "/") + QLatin1StringView(name));
	const auto opened = file.open(QIODevice::ReadOnly);
	Q_ASSERT_X(opened, "Test::Fixture", name);
	Q_UNUSED(opened);
	return Json(file.readAll());
}

// Spins the event loop until `condition` holds (or the timeout passes).
template <typename Condition>
[[nodiscard]] bool WaitFor(Condition &&condition, int timeoutMs = 5000) {
	return QTest::qWaitFor(std::forward<Condition>(condition), timeoutMs);
}

// Runs everything already queued on the event loop.
inline void Settle(int ms = 20) {
	QTest::qWait(ms);
}

// A Data::Transport whose answers the test scripts.
class FakeTransport final : public Data::Transport {
public:
	struct Call {
		QString method;
		QJsonObject body;
		QString idempotencyKey;
	};
	struct Reply {
		bool ok = true;
		QJsonValue value = QJsonObject();
		Api::Error error;
		bool hold = false; // answer later through resolve() / reject()

		static Reply Ok(const QJsonValue &value = QJsonObject()) {
			auto reply = Reply();
			reply.value = value;
			return reply;
		}
		static Reply Fail(int status, const QString &code, bool retryable = false) {
			auto reply = Reply();
			reply.ok = false;
			reply.error = Api::Error::Local(status, code, code, retryable);
			return reply;
		}
		static Reply Offline() {
			auto reply = Reply();
			reply.ok = false;
			reply.error = Api::Error::Offline();
			return reply;
		}
		static Reply Hold() {
			auto reply = Reply();
			reply.hold = true;
			return reply;
		}
	};

	// Decides the answer of every call; unset: every call succeeds with {}.
	Fn<Reply(const Call &)> handler;
	QVector<Call> calls;
	QVector<qint64> acks;
	QVector<QPair<QString, QString>> typing;
	QVector<QPair<QString, QString>> groupTyping;
	QStringList presenceSubscriptions;
	QStringList profileSubscriptions;
	std::optional<QString> topics;
	std::optional<qint64> feedSince;
	Fn<qint64()> ptsProvider;
	bool updating = false;
	bool online = false;

	void call(
			const QString &method,
			const QJsonObject &body,
			const QString &idempotencyKey,
			base::Guard guard,
			Fn<void(const QJsonValue &)> done,
			Fn<void(const Api::Error &)> fail) override {
		const auto index = int(calls.size());
		calls.push_back({ method, body, idempotencyKey });
		const auto reply = handler ? handler(calls.back()) : Reply();
		_pending.push_back({ guard, done, fail, false });
		if (!reply.hold) {
			QTimer::singleShot(0, this, [=, this] {
				finish(index, reply);
			});
		}
	}

	// Answers a held call (by its index in `calls`).
	void resolve(int index, const QJsonValue &value = QJsonObject()) {
		finish(index, Reply::Ok(value));
	}
	void reject(int index, const Api::Error &error) {
		auto reply = Reply();
		reply.ok = false;
		reply.error = error;
		finish(index, reply);
	}

	[[nodiscard]] int count(const QString &method) const {
		return int(std::count_if(calls.cbegin(), calls.cend(), [&](const Call &call) {
			return call.method == method;
		}));
	}
	[[nodiscard]] int lastIndexOf(const QString &method) const {
		for (auto i = int(calls.size()); i-- > 0;) {
			if (calls[i].method == method) {
				return i;
			}
		}
		return -1;
	}
	[[nodiscard]] QStringList methods() const {
		auto result = QStringList();
		for (const auto &call : calls) {
			result.push_back(call.method);
		}
		return result;
	}

	void setOnline(bool value) {
		if (online != value) {
			online = value;
			Q_EMIT connectedChanged(value);
		}
	}
	void emitFrame(const Rt::Frame &frame) {
		Q_EMIT frameReceived(frame);
	}

	bool connected() const override {
		return online;
	}
	void setPtsProvider(Fn<qint64()> provider) override {
		ptsProvider = std::move(provider);
	}
	void setUpdating(bool value) override {
		updating = value;
	}
	void ack(qint64 pts) override {
		acks.push_back(pts);
	}
	bool sendTyping(const QString &cap, const QString &kind) override {
		typing.push_back({ cap, kind });
		return online;
	}
	bool sendGroupTyping(const QString &group, const QString &kind) override {
		groupTyping.push_back({ group, kind });
		return online;
	}
	void subscribePresence(
			const QStringList &users,
			base::Guard guard,
			Fn<void(const QJsonArray &)> done) override {
		presenceSubscriptions += users;
		QTimer::singleShot(0, this, [=] {
			if (done && guard.alive()) {
				done(QJsonArray());
			}
		});
	}
	void unsubscribePresence(const QStringList &users) override {
		for (const auto &user : users) {
			presenceSubscriptions.removeAll(user);
		}
	}
	void followProfiles(const QStringList &users) override {
		profileSubscriptions += users;
	}
	void subscribeTopics(const QString &category) override {
		topics = category;
	}
	void unsubscribeTopics() override {
		topics = std::nullopt;
	}
	void subscribeFeed(qint64 since) override {
		feedSince = since;
	}
	void unsubscribeFeed() override {
		feedSince = std::nullopt;
	}

private:
	struct Pending {
		base::Guard guard;
		Fn<void(const QJsonValue &)> done;
		Fn<void(const Api::Error &)> fail;
		bool finished = false;
	};

	void finish(int index, const Reply &reply) {
		if (index < 0 || index >= _pending.size() || _pending[index].finished) {
			return;
		}
		_pending[index].finished = true;
		const auto pending = _pending[index];
		if (!pending.guard.alive()) {
			return;
		} else if (reply.ok) {
			if (pending.done) {
				pending.done(reply.value);
			}
		} else if (pending.fail) {
			pending.fail(reply.error);
		}
	}

	QVector<Pending> _pending;

};

// A minimal HTTP/1.1 server on 127.0.0.1 for the API client tests.
class HttpServer final : public QObject {
public:
	struct Request {
		QByteArray method;
		QByteArray target; // path with query
		QByteArray path;
		QMap<QByteArray, QByteArray> headers; // lower-case names
		QByteArray body;

		[[nodiscard]] QByteArray header(const char *name) const {
			return headers.value(QByteArray(name).toLower());
		}
		[[nodiscard]] QJsonObject json() const {
			return QJsonDocument::fromJson(body).object();
		}
	};
	struct Response {
		int status = 200;
		QByteArray body;
		QByteArray contentType = "application/json";
		QVector<QPair<QByteArray, QByteArray>> headers;
		bool hang = false; // never answer (cancellation tests)
		// >= 0: announce the whole body, send only this many bytes of it
		// and drop the connection (a download that is cut off).
		int cutAfter = -1;
		int delayMs = 0;   // > 0: answered that much later (a slow request)

		static Response Ok(const QJsonObject &json = QJsonObject()) {
			auto response = Response();
			response.body = QJsonDocument(json).toJson(QJsonDocument::Compact);
			return response;
		}
		static Response Problem(int status, const QByteArray &code, const QByteArray &title = "problem") {
			auto response = Response();
			response.status = status;
			response.contentType = "application/problem+json";
			response.body = "{\"code\":\"" + code + "\",\"title\":\"" + title + "\"}";
			return response;
		}
		static Response Redirect(const QByteArray &location) {
			auto response = Response();
			response.status = 302;
			response.headers.push_back({ "Location", location });
			return response;
		}
		static Response Bytes(const QByteArray &bytes, const QByteArray &type = "application/octet-stream") {
			auto response = Response();
			response.body = bytes;
			response.contentType = type;
			return response;
		}
	};

	Fn<Response(const Request &)> handler;
	QVector<Request> requests;

	HttpServer() {
		const auto listening = _server.listen(QHostAddress::LocalHost, 0);
		Q_ASSERT(listening);
		Q_UNUSED(listening);
		connect(&_server, &QTcpServer::newConnection, this, [this] {
			while (const auto socket = _server.nextPendingConnection()) {
				accept(socket);
			}
		});
	}

	[[nodiscard]] QString base() const {
		return QStringLiteral("http://127.0.0.1:%1").arg(_server.serverPort());
	}
	[[nodiscard]] int count(const QByteArray &path) const {
		return int(std::count_if(requests.cbegin(), requests.cend(), [&](const Request &request) {
			return request.path == path;
		}));
	}
	[[nodiscard]] QVector<Request> to(const QByteArray &path) const {
		auto result = QVector<Request>();
		for (const auto &request : requests) {
			if (request.path == path) {
				result.push_back(request);
			}
		}
		return result;
	}

private:
	void accept(QTcpSocket *socket) {
		const auto buffer = std::make_shared<QByteArray>();
		connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
		connect(socket, &QTcpSocket::readyRead, this, [=, this] {
			buffer->append(socket->readAll());
			for (;;) {
				const auto end = buffer->indexOf("\r\n\r\n");
				if (end < 0) {
					return;
				}
				const auto lines = buffer->left(end).split('\n');
				auto request = Request();
				const auto first = lines.value(0).trimmed().split(' ');
				request.method = first.value(0);
				request.target = first.value(1);
				request.path = request.target.left(
					request.target.indexOf('?') < 0 ? request.target.size() : request.target.indexOf('?'));
				for (auto i = 1; i < lines.size(); ++i) {
					const auto line = lines[i].trimmed();
					const auto colon = line.indexOf(':');
					if (colon > 0) {
						request.headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
					}
				}
				const auto length = request.header("content-length").toLongLong();
				if (buffer->size() < end + 4 + length) {
					return;
				}
				request.body = buffer->mid(end + 4, length);
				buffer->remove(0, end + 4 + length);
				requests.push_back(request);
				const auto response = handler ? handler(request) : Response::Ok();
				if (response.hang) {
					return;
				}
				auto bytes = QByteArray("HTTP/1.1 ") + QByteArray::number(response.status) + " X\r\n";
				bytes += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
				if (!response.body.isEmpty()) {
					bytes += "Content-Type: " + response.contentType + "\r\n";
				}
				for (const auto &[name, value] : response.headers) {
					bytes += name + ": " + value + "\r\n";
				}
				bytes += "\r\n";
				if (response.cutAfter >= 0) {
					socket->write(bytes + response.body.left(response.cutAfter));
					socket->disconnectFromHost();
					return;
				}
				if (response.delayMs > 0) {
					const auto all = bytes + response.body;
					QTimer::singleShot(response.delayMs, socket, [=] {
						socket->write(all);
					});
					continue;
				}
				socket->write(bytes + response.body);
			}
		});
	}

	QTcpServer _server;

};

} // namespace Test
