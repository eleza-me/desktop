#include "core/core_single_instance.h"

#include "base/platform_file.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtNetwork/QLocalSocket>

namespace Core {
namespace {

constexpr auto kConnectTimeout = 400;
constexpr auto kWriteTimeout = 800;
// "activate" or "route:<route>": anything longer is not ours.
constexpr auto kMaxMessage = 4096;

[[nodiscard]] QString UserName() {
	auto result = qEnvironmentVariable("USERNAME");
	if (result.isEmpty()) {
		result = qEnvironmentVariable("USER");
	}
	return result.isEmpty() ? QDir::homePath() : result;
}

// Where the endpoint lives. A bare name is a socket in the shared temporary
// directory on Unix: another user of the machine could take the (predictable)
// name first and swallow every launch. The per-user runtime directory (0700)
// cannot be squatted; the name itself is still per user and per data directory.
[[nodiscard]] QString EndpointName(const QString &workDir) {
	const auto hash = QCryptographicHash::hash(
		(UserName() + QLatin1Char('\n') + QDir(workDir).absolutePath()).toUtf8(),
		QCryptographicHash::Sha256).toHex().left(16);
	const auto name = QStringLiteral("eleza-desktop-") + QString::fromLatin1(hash);
#ifdef Q_OS_UNIX
	const auto runtime = QStandardPaths::writableLocation(
		QStandardPaths::RuntimeLocation);
	const auto path = runtime.isEmpty() ? QString() : QDir(runtime).filePath(name);
	// sockaddr_un holds about a hundred bytes of path.
	if (!path.isEmpty() && path.toLocal8Bit().size() < 100) {
		return path;
	}
	// No (short enough) runtime directory: a directory of our own under the
	// temporary one, verified to be ours and closed to everybody else.
	const auto own = QDir(QDir::tempPath()).filePath(
		QStringLiteral("eleza-") + QString::fromLatin1(QCryptographicHash::hash(
			UserName().toUtf8(),
			QCryptographicHash::Sha256).toHex().left(12)));
	const auto inside = QDir(own).filePath(name);
	if (inside.toLocal8Bit().size() < 100
		&& base::platform::EnsurePrivateDirectory(own)) {
		return inside;
	}
#endif // Q_OS_UNIX
	return name;
}

} // namespace

SingleInstance::SingleInstance(const QString &workDir, QObject *parent)
: QObject(parent) {
	_name = EndpointName(workDir);
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
		// The socket name has the account name of the system in it.
		qWarning(
			"SingleInstance: could not listen (%d).",
			int(_server.serverError()));
	}
	return false;
}

void SingleInstance::handleConnection() {
	while (const auto socket = _server.nextPendingConnection()) {
		// A peer that never sends a line end must not grow the buffer.
		socket->setReadBufferSize(kMaxMessage);
		QObject::connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
			if (!socket->canReadLine()) {
				if (socket->bytesAvailable() >= kMaxMessage) {
					socket->abort();
				}
				return;
			}
			const auto line = QString::fromUtf8(
				socket->readLine(kMaxMessage)).trimmed();
			socket->disconnectFromServer();
			Q_EMIT activated(line);
		});
		QObject::connect(
			socket,
			&QLocalSocket::disconnected,
			socket,
			&QObject::deleteLater);
	}
}

} // namespace Core
