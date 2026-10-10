// The part of an update that happens around the restart.
//
//   const auto target = Updater::DetectInstall();
//   // on «به‌روزرسانی الزا»:
//   if (Updater::PrepareInstall(target, request)) {
//       Core::App().quit();       // main() calls FinishPendingInstall() last
//   }
//   // at the next start:
//   if (const auto result = Updater::TakeInstallResult(updatesDir)) { … }
//
// Linux: the exiting process swaps the files itself (Install::Apply, the same
// code that the Windows updater runs and that the tests cover) and executes
// the new binary with the arguments it was started with — a running binary
// may be renamed there.
// Windows: the app copies Updater.exe out of the install directory, starts
// the copy and exits; the updater waits for it, swaps the files (asking for
// administrator rights when the directory needs them) and starts Eleza.exe.
#pragma once

#include "base/basic_types.h"

namespace Updater {

struct InstallTarget {
	// False: this copy belongs to a package manager, cannot be written by
	// the user, or is not a packaged copy at all (a build directory). New
	// versions are only announced.
	bool selfUpdate = false;
	QString installDir;
	QString executable; // relative to installDir: "Eleza.exe" / "Eleza"
};

// Looks at where the running executable lives. A copy updates itself when
// the package's file list (eleza-files.list) lies next to the executable
// and, on Linux, the directory belongs to the user, is writable and is not
// under /usr (a .deb installs no file list and stays with apt); on Windows
// Updater.exe must be there as well.
[[nodiscard]] InstallTarget DetectInstall();
// The same rule for any directory (tests).
[[nodiscard]] InstallTarget DetectInstall(const QString &executablePath);

struct InstallRequest {
	QString version;
	QString stagedDir;  // the unpacked new version
	QString updatesDir; // <workdir>/updates: the request, the result, the log
	QStringList arguments; // of this process, without the program name
};

// Writes the request and remembers it for FinishPendingInstall(). False when
// the install step cannot be started (the reason goes to the log).
[[nodiscard]] bool PrepareInstall(
	const InstallTarget &target,
	const InstallRequest &request);
[[nodiscard]] bool HasPendingInstall();
// The last thing main() does, after the application object is gone. Does not
// return when the new version was started in place of this process (Linux);
// otherwise returns the exit code to use.
int FinishPendingInstall(int exitCode);

struct InstallResult {
	QString version;
	bool installed = false;
	QString error; // for the log only
};
// What the install step left for this start; the file is removed.
[[nodiscard]] std::optional<InstallResult> TakeInstallResult(
	const QString &updatesDir);

// Deletes what a finished install step could not delete itself (the backup of
// files that were still in use). Blocking: call it from a worker thread.
void RemoveInstallLeftovers(const InstallTarget &target);

// A swap that was cut off (power loss, a killed updater) left a journal next
// to the executable: puts the old version back before anything else runs,
// and then starts the program again with the same arguments, because files
// this process has loaded were moved. Called first thing in main(), before
// there is an application object. Returns when there was nothing to undo (or
// the restart failed).
void RecoverInterruptedInstall(int argc, char *argv[]);

} // namespace Updater
