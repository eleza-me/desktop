#include "updater/updater_install.h"

#include "base/platform_file.h"
#include "install/install_helper.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#else // Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif // !Q_OS_WIN

namespace Updater {
namespace {

const auto kParamsFile = u"install.params"_s;
const auto kResultFile = u"install.result"_s;
const auto kLogFile = u"install.log"_s;
#ifdef Q_OS_WIN
const auto kUpdaterName = u"Updater.exe"_s;
#endif // Q_OS_WIN
constexpr auto kMaxLogBytes = qint64(256) * 1024;

// Everything FinishPendingInstall() needs, in types that outlive the
// application object.
struct Pending {
	Install::HelperParams params;
	Install::Path paramsPath;
	Install::Path updaterPath; // Windows: the copy to start
};
std::optional<Pending> PendingInstall;

[[nodiscard]] Install::Path ToPath(const QString &path) {
	return Install::PathFromUtf8(
		QDir::toNativeSeparators(path).toUtf8().toStdString());
}

#ifndef Q_OS_WIN
[[nodiscard]] bool IsUnder(const QString &path, const QString &root) {
	return path == root || path.startsWith(root + u'/');
}
#endif // !Q_OS_WIN

} // namespace

InstallTarget DetectInstall(const QString &executablePath) {
	auto result = InstallTarget();
	const auto info = QFileInfo(executablePath);
	const auto canonical = info.canonicalFilePath();
	if (canonical.isEmpty()) {
		return result;
	}
	const auto file = QFileInfo(canonical);
	result.installDir = file.absolutePath();
	result.executable = file.fileName();
	const auto directory = QDir(result.installDir);
	const auto list = QFileInfo(directory.filePath(
		QString::fromLatin1(Install::kFileListName)));
	if (!list.isFile() || list.isSymLink()) {
		// Not a packaged copy: a build directory, or a system package.
		return result;
	}
#ifdef Q_OS_WIN
	result.selfUpdate = QFileInfo(directory.filePath(kUpdaterName)).isFile();
#else // Q_OS_WIN
	// Sandboxed formats bring their own updates.
	if (qEnvironmentVariableIsSet("APPIMAGE")
		|| qEnvironmentVariableIsSet("FLATPAK_ID")
		|| qEnvironmentVariableIsSet("SNAP")) {
		return result;
	}
	for (const auto &root : { u"/usr"_s, u"/bin"_s, u"/sbin"_s, u"/lib"_s }) {
		if (IsUnder(result.installDir, root)) {
			return result;
		}
	}
	const auto native = QFile::encodeName(result.installDir);
	struct stat status;
	result.selfUpdate = (::stat(native.constData(), &status) == 0)
		&& S_ISDIR(status.st_mode)
		&& (status.st_uid == ::geteuid())
		&& (::access(native.constData(), W_OK | X_OK) == 0);
#endif // !Q_OS_WIN
	return result;
}

InstallTarget DetectInstall() {
	return DetectInstall(QCoreApplication::applicationFilePath());
}

bool PrepareInstall(
		const InstallTarget &target,
		const InstallRequest &request) {
	PendingInstall = std::nullopt;
	if (!target.selfUpdate) {
		return false;
	}
	const auto updates = QDir(request.updatesDir);
	auto pending = Pending();
	auto &params = pending.params;
	params.parentPid = static_cast<unsigned long>(
		QCoreApplication::applicationPid());
	params.installDir = ToPath(target.installDir);
	params.stagedDir = ToPath(request.stagedDir);
	params.version = request.version.toStdString();
	params.executable = target.executable.toUtf8().toStdString();
	params.resultPath = ToPath(updates.filePath(kResultFile));
	params.logPath = ToPath(updates.filePath(kLogFile));
	for (const auto &argument : request.arguments) {
		params.arguments.push_back(argument.toUtf8().toStdString());
	}
	pending.paramsPath = ToPath(updates.filePath(kParamsFile));

	// The log of the install steps does not grow for ever.
	const auto log = updates.filePath(kLogFile);
	if (QFileInfo(log).size() > kMaxLogBytes) {
		QFile::remove(log);
	}
	QFile::remove(updates.filePath(kResultFile));
	if (!base::platform::WriteFileAtomic(
			updates.filePath(kParamsFile),
			QByteArray::fromStdString(Install::SerializeParams(params)),
			true)) {
		qWarning("Updater: could not write the install request.");
		return false;
	}
	// What was written must be what the install step will accept.
	auto check = Install::HelperParams();
	auto ops = Install::FileOps();
	if (!Install::ReadParams(ops, pending.paramsPath, check)) {
		qWarning("Updater: the install request is not valid.");
		return false;
	}
#ifdef Q_OS_WIN
	// A copy of the updater: the one in the install directory is replaced
	// like every other file.
	const auto source = QDir(target.installDir).filePath(kUpdaterName);
	const auto copy = updates.filePath(kUpdaterName);
	QFile::remove(copy);
	if (!QFile::copy(source, copy)) {
		qWarning("Updater: could not copy the updater.");
		return false;
	}
	pending.updaterPath = ToPath(copy);
#endif // Q_OS_WIN
	PendingInstall = std::move(pending);
	return true;
}

bool HasPendingInstall() {
	return PendingInstall.has_value();
}

int FinishPendingInstall(int exitCode) {
	if (!PendingInstall) {
		return exitCode;
	}
	const auto pending = *std::exchange(PendingInstall, std::nullopt);
#ifdef Q_OS_WIN
	auto commandLine = std::wstring(L"\"")
		+ pending.updaterPath.native()
		+ L"\" \"--params="
		+ pending.paramsPath.native()
		+ L"\"";
	auto startup = STARTUPINFOW();
	startup.cb = sizeof(startup);
	auto process = PROCESS_INFORMATION();
	// The updater outlives this process: nothing is inherited, the current
	// directory is its own (not the install directory, which it renames in).
	if (!CreateProcessW(
			pending.updaterPath.c_str(),
			commandLine.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			pending.updaterPath.parent_path().c_str(),
			&startup,
			&process)) {
		return exitCode;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return exitCode;
#else // Q_OS_WIN
	auto ops = Install::FileOps();
	const auto log = Install::FileLog(ops, pending.params.logPath);
	Install::RunInstall(pending.params, ops, log, 0);

	// The new version on success, the old one (put back) after a failure:
	// either way the app comes up again and reads the result.
	const auto executable = (pending.params.installDir
		/ Install::PathFromUtf8(pending.params.executable)).native();
	auto arguments = std::vector<std::string>{ executable };
	arguments.insert(
		arguments.end(),
		pending.params.arguments.begin(),
		pending.params.arguments.end());
	auto pointers = std::vector<char*>();
	for (auto &argument : arguments) {
		pointers.push_back(argument.data());
	}
	pointers.push_back(nullptr);
	::execv(executable.c_str(), pointers.data());
	if (log) {
		log("could not start the app again");
	}
	return exitCode;
#endif // !Q_OS_WIN
}

std::optional<InstallResult> TakeInstallResult(const QString &updatesDir) {
	const auto path = QDir(updatesDir).filePath(kResultFile);
	if (!QFileInfo::exists(path)) {
		return std::nullopt;
	}
	auto ops = Install::FileOps();
	auto read = Install::HelperResult();
	const auto ok = Install::ReadResult(ops, ToPath(path), read);
	QFile::remove(path);
	QFile::remove(QDir(updatesDir).filePath(kParamsFile));
	if (!ok) {
		return std::nullopt;
	}
	auto result = InstallResult();
	result.version = QString::fromStdString(read.version);
	result.installed = (read.status == Install::Status::Installed);
	result.error = QString::fromStdString(read.error);
	return result;
}

void RemoveInstallLeftovers(const InstallTarget &target) {
	if (!target.selfUpdate || target.installDir.isEmpty()) {
		return;
	}
	auto ops = Install::FileOps();
	Install::RemoveLeftovers(ToPath(target.installDir), ops);
}

void RecoverInterruptedInstall(int argc, char *argv[]) {
	// Where this program is, asked before anything is moved.
#ifdef Q_OS_WIN
	Q_UNUSED(argc);
	Q_UNUSED(argv);
	auto buffer = std::wstring(32768, L'\0');
	const auto size = GetModuleFileNameW(
		nullptr,
		buffer.data(),
		DWORD(buffer.size()));
	if (size == 0 || size >= buffer.size()) {
		return;
	}
	buffer.resize(size);
	const auto self = Install::Path(buffer);
#else // Q_OS_WIN
	const auto resolved = QFileInfo(u"/proc/self/exe"_s).canonicalFilePath();
	if (resolved.isEmpty() || argc < 1) {
		return;
	}
	const auto self = ToPath(resolved);
#endif // !Q_OS_WIN
	const auto directory = self.parent_path();
	auto ops = Install::FileOps();
	if (!Install::HasJournal(directory, ops)) {
		return;
	}
	const auto recovery = Install::Recover(directory, ops, nullptr, 4, 250);
	if (recovery == Install::Recovery::RolledBack
		|| recovery == Install::Recovery::Finished) {
		Install::RemoveLeftovers(directory, ops);
	}
	if (recovery != Install::Recovery::RolledBack) {
		// A committed swap only needed cleaning: what runs is whole.
		return;
	}
#ifdef Q_OS_WIN
	// The command line of this process, as it is.
	auto commandLine = std::wstring(GetCommandLineW());
	auto startup = STARTUPINFOW();
	startup.cb = sizeof(startup);
	auto process = PROCESS_INFORMATION();
	if (CreateProcessW(
			self.c_str(),
			commandLine.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			nullptr,
			&startup,
			&process)) {
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		ExitProcess(0);
	}
#else // Q_OS_WIN
	auto pointers = std::vector<char*>(argv, argv + argc);
	pointers.push_back(nullptr);
	::execv(self.c_str(), pointers.data());
#endif // !Q_OS_WIN
}

} // namespace Updater
