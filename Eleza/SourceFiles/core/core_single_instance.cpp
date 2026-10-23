#include "core/core_single_instance.h"

#include "base/platform_file.h"
#include "base/random.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
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

// On Windows the endpoint is a named pipe, in one namespace for the whole
// machine, and Qt cannot ask for the first instance of a name only: another
// user who can work the name out could create it first and swallow every
// launch. A random secret kept in the data directory (closed to other users)
// goes into the name. Empty when it cannot be stored: the name is as before.
[[maybe_unused]] [[nodiscard]] QByteArray InstanceSecret(const QString &workDir) {
	constexpr auto kSize = 32; // hex characters
	const auto path = QDir(workDir).filePath(QStringLiteral("instance.secret"));
	const auto read = [&] {
		auto file = QFile(path);
		const auto secret = file.open(QIODevice::ReadOnly)
			? file.read(kSize + 1)
			: QByteArray();
		return (secret.size() == kSize) ? secret : QByteArray();
	};
	if (const auto secret = read(); !secret.isEmpty()) {
		return secret;
	} else if (QFile::exists(path)) {
		QFile::remove(path); // damaged: made anew
	}
	// Written aside and renamed (which never replaces a file): two first
	// launches at once end up with the same secret.
	const auto temp = path + u'.' + QString::fromLatin1(base::RandomBytes(4).toHex());
	auto file = QFile(temp);
	if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
		const auto written = file.write(base::RandomBytes(kSize / 2).toHex());
		file.close();
		if (written != kSize || !QFile::rename(temp, path)) {
			QFile::remove(temp);
		}
	}
	return read();
}

// Where the endpoint lives. A bare name is a socket in the shared temporary
// directory on Unix: another user of the machine could take the (predictable)
// name first and swallow every launch. The per-user runtime directory (0700)
// cannot be squatted; the name itself is still per user and per data directory.
[[nodiscard]] QString EndpointName(const QString &workDir) {
	auto seed = (UserName() + QLatin1Char('\n') + QDir(workDir).absolutePath()).toUtf8();
#ifdef Q_OS_WIN
	if (const auto secret = InstanceSecret(workDir); !secret.isEmpty()) {
		seed += '\n';
		seed += secret;
	}
#endif // Q_OS_WIN
	const auto hash = QCryptographicHash::hash(
		seed,
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
