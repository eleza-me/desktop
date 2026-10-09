#include "base/platform_file.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>

namespace base::platform {

bool EnsureDirectory(const QString &path, bool ownerOnly) {
	if (!QDir().mkpath(path)) {
		return false;
	}
#ifndef Q_OS_WIN
	if (ownerOnly) {
		QFile::setPermissions(
			path,
			QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
	}
#else // !Q_OS_WIN
	Q_UNUSED(ownerOnly);
#endif // Q_OS_WIN
	return true;
}

bool WriteFileAtomic(const QString &path, const QByteArray &bytes, bool ownerOnly) {
	if (!EnsureDirectory(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
#ifndef Q_OS_WIN
	if (ownerOnly) {
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	}
#endif // !Q_OS_WIN
	if (file.write(bytes) != bytes.size()) {
		file.cancelWriting();
		return false;
	}
	if (!file.commit()) {
		return false;
	}
#ifndef Q_OS_WIN
	if (ownerOnly) {
		QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	}
#else // !Q_OS_WIN
	Q_UNUSED(ownerOnly);
#endif // Q_OS_WIN
	return true;
}

std::optional<QByteArray> ReadFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	auto result = file.readAll();
	if (result.isNull()) {
		result = QByteArray("", 0);
	}
	return result;
}

bool RemoveRecursively(const QString &path) {
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		return true;
	}
	return QDir(path).removeRecursively();
}

QString SystemDescription() {
	const auto type = QSysInfo::productType();
	const auto version = QSysInfo::productVersion();
	auto result = (type.isEmpty() || type == u"unknown") ? QSysInfo::kernelType() : type;
	if (!version.isEmpty() && version != u"unknown") {
		result += u' ' + version;
	}
	return result.toLower();
}

QString DeviceName() {
#if defined Q_OS_WIN
	const auto os = QStringLiteral("Windows");
#elif defined Q_OS_MACOS // Q_OS_WIN
	const auto os = QStringLiteral("macOS");
#else // Q_OS_WIN || Q_OS_MACOS
	const auto os = QStringLiteral("Linux");
#endif // Q_OS_WIN || Q_OS_MACOS
	auto host = QSysInfo::machineHostName();
	auto result = QStringLiteral("Eleza Desktop · ") + os;
	if (!host.isEmpty()) {
		result += QStringLiteral(" (") + host + u')';
	}
	return result.left(64);
}

} // namespace base::platform
