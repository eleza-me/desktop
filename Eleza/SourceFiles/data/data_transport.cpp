#include "data/data_transport.h"

namespace Data {

RpcBuilder::RpcBuilder(Transport *transport, const QString &method)
: _transport(transport)
, _method(method) {
}

RpcBuilder &RpcBuilder::body(const QJsonObject &body) {
	_body = body;
	return *this;
}

RpcBuilder &RpcBuilder::idempotencyKey(const QString &key) {
	_idempotencyKey = key;
	return *this;
}

RpcBuilder &RpcBuilder::guard(QObject *object) {
	_guard = base::Guard(object);
	return *this;
}

RpcBuilder &RpcBuilder::guard(base::Guard guard) {
	_guard = std::move(guard);
	return *this;
}

RpcBuilder &RpcBuilder::done(Fn<void(const QJsonObject &)> callback) {
	_done = [callback = std::move(callback)](const QJsonValue &value) {
		callback(value.toObject());
	};
	return *this;
}

RpcBuilder &RpcBuilder::done(Fn<void()> callback) {
	_done = [callback = std::move(callback)](const QJsonValue &) {
		callback();
	};
	return *this;
}

RpcBuilder &RpcBuilder::doneValue(Fn<void(const QJsonValue &)> callback) {
	_done = std::move(callback);
	return *this;
}

RpcBuilder &RpcBuilder::fail(Fn<void(const Api::Error &)> callback) {
	_fail = std::move(callback);
	return *this;
}

void RpcBuilder::send() {
	_transport->call(
		_method,
		_body,
		_idempotencyKey,
		std::move(_guard),
		std::move(_done),
		std::move(_fail));
}

RpcBuilder Transport::rpc(const QString &method) {
	return RpcBuilder(this, method);
}

} // namespace Data
