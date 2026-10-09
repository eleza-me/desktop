#include "core/core_single_instance.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtNetwork/QLocalSocket>

namespace Core {
namespace {

constexpr auto kConnectTimeout = 400;
constexpr auto kWriteTimeout = 800;

} // namespace

SingleInstance::SingleInstance(const QString &workDir, QObject *parent)
: QObject(parent) {
	const auto hash = QCryptographicHash::hash(
		QDir(workDir).absolutePath().toUtf8(),
		QCryptographicHash::Sha1).toHex().left(16);
	_name = QStringLiteral("eleza-desktop-") + QString::fromLatin1(hash);
	QObject::connect(&_server, &QLocalServer::newConnection, this, [this] {
		handleConnection();
	});
}

bool SingleInstance::notifyRunning(const QString &message) {
	{
		auto socket = QLocalSocket();
		socket.connectToServer(_name);
		if (socket.waitForConnected(kConnectTimeout)) {
			auto payload = message.toUtf8();
			payload.append('\n');
			socket.write(payload);
			socket.flush();
			socket.waitForBytesWritten(kWriteTimeout);
			socket.disconnectFromServer();
			return true;
		}
	}
	// Nobody answered: a stale socket file may be left from a crash.
	QLocalServer::removeServer(_name);
	_server.setSocketOptions(QLocalServer::UserAccessOption);
	if (!_server.listen(_name)) {
		qWarning(
			"SingleInstance: could not listen on '%s': %s",
			qPrintable(_name),
			qPrintable(_server.errorString()));
	}
	return false;
}

void SingleInstance::handleConnection() {
	while (const auto socket = _server.nextPendingConnection()) {
		QObject::connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
			if (!socket->canReadLine()) {
				return;
			}
			const auto line = QString::fromUtf8(socket->readLine()).trimmed();
			emit activated(line);
			socket->disconnectFromServer();
		});
		QObject::connect(
			socket,
			&QLocalSocket::disconnected,
			socket,
			&QObject::deleteLater);
	}
}

} // namespace Core
