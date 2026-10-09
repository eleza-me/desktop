// How the data stores talk to the server.
//
// `call()` / `rpc()` run one of the realtime RPC methods ("chat.send",
// "chat.history", "sync.difference", ... - the table in rt/rt_schema): over
// the connection when it is up, and as the equivalent HTTP request (same
// Idempotency-Key) when it is down or the call got no answer. The answer has
// the same JSON shape either way.
//
// Main::Session implements this over Rt::Client + Api::Client; tests plug in
// a fake. Routes that are not RPC methods (feed, graph, uploads, media) go
// through Api::Client directly.
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "rt/rt_client.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace Data {

class Transport;

//   transport.rpc(u"chat.pins"_s)
//       .body({ { u"chat"_s, id } })
//       .guard(this)
//       .done([=](const QJsonObject &result) { ... })
//       .fail([=](const Api::Error &error) { ... })
//       .send();
class RpcBuilder {
public:
	RpcBuilder(Transport *transport, const QString &method);

	RpcBuilder &body(const QJsonObject &body);
	// Defaults to a random UUID for mutating methods.
	RpcBuilder &idempotencyKey(const QString &key);
	RpcBuilder &guard(QObject *object);
	RpcBuilder &guard(base::Guard guard);
	RpcBuilder &done(Fn<void(const QJsonObject &)> callback);
	RpcBuilder &done(Fn<void()> callback);
	RpcBuilder &doneValue(Fn<void(const QJsonValue &)> callback);
	RpcBuilder &fail(Fn<void(const Api::Error &)> callback);
	void send();

private:
	Transport *_transport = nullptr;
	QString _method;
	QJsonObject _body;
	QString _idempotencyKey;
	base::Guard _guard;
	Fn<void(const QJsonValue &)> _done;
	Fn<void(const Api::Error &)> _fail;

};

class Transport : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;

	virtual void call(
		const QString &method,
		const QJsonObject &body,
		const QString &idempotencyKey,
		base::Guard guard,
		Fn<void(const QJsonValue &)> done,
		Fn<void(const Api::Error &)> fail) = 0;
	[[nodiscard]] RpcBuilder rpc(const QString &method);

	// The realtime connection is up (Hello received).
	[[nodiscard]] virtual bool connected() const = 0;

	virtual void setPtsProvider(Fn<qint64()> provider) = 0;
	virtual void setUpdating(bool updating) = 0;
	virtual void ack(qint64 pts) = 0;

	// False when the signal could not be sent (not connected).
	virtual bool sendTyping(const QString &cap, const QString &kind) = 0;
	// `done` gets the PresenceOut list the server answered ([] when not connected).
	virtual void subscribePresence(
		const QStringList &users,
		base::Guard guard,
		Fn<void(const QJsonArray &)> done) = 0;
	virtual void unsubscribePresence(const QStringList &users) = 0;
	virtual void followProfiles(const QStringList &users) = 0;
	virtual void subscribeTopics(const QString &category) = 0;
	virtual void unsubscribeTopics() = 0;
	virtual void subscribeFeed(qint64 since) = 0;
	virtual void unsubscribeFeed() = 0;

Q_SIGNALS:
	void frameReceived(const Rt::Frame &frame);
	void connectedChanged(bool connected);

};

} // namespace Data
