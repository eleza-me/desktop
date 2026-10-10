// What the app hands to the install step and what the step reports back.
//
// Both are small text files of `key=value` lines (UTF-8; "\\" and "\n" inside
// a value are written as "\\\\" and "\\n"):
//
//   the request   <workdir>/updates/install.params    written by the app
//   the result    <workdir>/updates/install.result    read by the next start
//
// RunInstall() is the whole install step on Linux (the exiting app calls it
// and starts the new binary) and the middle of the Windows updater, which adds
// waiting for the app, administrator rights when needed and the relaunch.
#pragma once

#include "install/install_swap.h"

namespace Install {

struct HelperParams {
	unsigned long parentPid = 0;
	Path installDir;
	Path stagedDir;
	std::string version;
	std::string executable; // relative to installDir
	Path resultPath;
	Path logPath;
	std::vector<std::string> arguments; // of the app, to start it again
};

struct HelperResult {
	std::string version;
	Status status = Status::Unchanged;
	std::string error;
};

[[nodiscard]] std::string SerializeParams(const HelperParams &params);
[[nodiscard]] bool ParseParams(const std::string &text, HelperParams &params);
[[nodiscard]] bool ReadParams(FileOps &ops, const Path &file, HelperParams &params);

[[nodiscard]] bool WriteResult(
	FileOps &ops,
	const Path &file,
	const HelperResult &result);
[[nodiscard]] bool ReadResult(
	FileOps &ops,
	const Path &file,
	HelperResult &result);

// Appends lines with the time in front to a file.
[[nodiscard]] Log FileLog(FileOps &ops, const Path &file);

// The swap, the result file, and after a success the removal of the backup
// and of what is left of the staging directory.
HelperResult RunInstall(
	const HelperParams &params,
	FileOps &ops,
	const Log &log,
	int retries,
	int retryDelayMs = 250);

} // namespace Install
