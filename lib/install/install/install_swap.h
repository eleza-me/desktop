// Replaces the files of an installed copy with an unpacked new version.
//
//   auto ops = Install::FileOps();
//   const auto result = Install::Apply({
//       .installDir = "C:/Users/me/AppData/Local/Programs/Eleza",
//       .stagedDir = ".../updates/0.2.0",
//       .version = "0.2.0",
//       .lastFile = "Eleza.exe",
//   }, ops, log);
//
// The same function is the Linux install step (called by the app as it exits)
// and the body of the Windows updater; the tests run it with file operations
// that fail or stop at every step.
//
// What it does, in this order:
//   1. An unfinished earlier swap (a journal in the install directory) is
//      undone first.
//   2. The staged tree is checked: only regular files with safe names
//      (install_paths.h), no links. Nothing in the install directory that the
//      swap would touch may sit behind a link either.
//   3. The new files are brought next to the old ones, into
//      <install>/_update_<version>/ (moved, or copied when the staging
//      directory is on another volume). Until here a failure changes nothing.
//   4. File by file: the old file moves to <install>/_backup_<version>/ and
//      the new one takes its place. Every move is a rename inside one volume
//      and is written to the journal (flushed to the disk) before it is made.
//      The executable (`lastFile`) is swapped last. Files of the old version
//      that the new one no longer has (by the old eleza-files.list) are moved
//      out as well; everything else in the directory (the uninstaller, the
//      user's own files) is left alone.
//   5. "commit" goes to the journal, _update_… and the journal are removed.
//      The backup stays for the caller to delete.
//
// A step that fails is retried (files locked by a scanner on Windows), then
// everything is moved back: the result is Unchanged and the old version runs.
// Broken means that moving back failed too; the journal stays, and Recover()
// tries again at the next start.
//
// A process that dies in step 4 leaves a directory in which every file is
// there — the old or the new one — and the journal. Recover() moves the old
// files back (or, when "commit" was written, only cleans up).
#pragma once

#include "install/install_file_ops.h"

#include <functional>

namespace Install {

using Log = std::function<void(const std::string &line)>;

struct Request {
	Path installDir;
	Path stagedDir;
	std::string version;
	// Relative name of the main executable: required in the new version and
	// swapped after everything else.
	std::string lastFile;
	// How many more times a rename is tried, and the pause between the tries.
	int retries = 0;
	int retryDelayMs = 250;
};

enum class Status {
	Installed, // the new version is in place
	Unchanged, // failed; the old version is intact
	Broken,    // failed and could not be undone completely
};

struct Result {
	Status status = Status::Unchanged;
	std::string error;
};

[[nodiscard]] Result Apply(
	const Request &request,
	FileOps &ops,
	const Log &log = nullptr);

enum class Recovery {
	NotNeeded,  // no journal
	RolledBack, // the old version was put back
	Finished,   // the swap had been committed: only cleaned up
	Failed,     // could not be undone (yet); the journal stays
};

[[nodiscard]] bool HasJournal(const Path &installDir, FileOps &ops);
[[nodiscard]] Recovery Recover(
	const Path &installDir,
	FileOps &ops,
	const Log &log = nullptr,
	int retries = 0,
	int retryDelayMs = 250);

// Deletes the _backup_… and _update_… directories a finished swap left
// behind. Does nothing while a journal exists.
void RemoveLeftovers(const Path &installDir, FileOps &ops);

// The lines of an eleza-files.list that are safe relative paths.
[[nodiscard]] std::vector<std::string> ParseFileList(const std::string &text);

} // namespace Install
