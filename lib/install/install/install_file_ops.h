// The few file system calls the swap is made of, behind an interface: the
// tests wrap the real ones to fail (or "lose power") at any step.
//
// Every call works on the entry itself and never through a link: a symbolic
// link, a junction or any other reparse point is reported as Other and is
// never opened, listed or copied.
#pragma once

#include "install/install_paths.h"

#include <string>
#include <vector>

namespace Install {

enum class EntryKind {
	Missing,
	File,      // a regular file
	Directory, // a real directory
	Other,     // a link, a reparse point, a device… or an error
};

class FileOps {
public:
	virtual ~FileOps() = default;

	[[nodiscard]] virtual EntryKind kind(const Path &path);
	// The names (UTF-8) in a directory, without "." and "..".
	[[nodiscard]] virtual bool list(
		const Path &directory,
		std::vector<std::string> &names);
	// Creates the directory and its missing parents.
	[[nodiscard]] virtual bool makeDirectories(const Path &path);
	// Moves a file inside one volume. Fails when `to` exists (nothing is
	// ever replaced) and when the two are on different volumes.
	[[nodiscard]] virtual bool rename(const Path &from, const Path &to);
	// Copies a regular file to a name that does not exist yet and flushes it
	// to the disk; the executable bits are kept.
	[[nodiscard]] virtual bool copyFile(const Path &from, const Path &to);
	// Removes a file or a link (the link, not its target).
	[[nodiscard]] virtual bool removeFile(const Path &path);
	[[nodiscard]] virtual bool removeEmptyDirectory(const Path &path);
	// Appends one line to a text file (created when missing) and flushes it
	// to the disk before returning.
	[[nodiscard]] virtual bool appendLine(const Path &file, const std::string &line);
	// The whole of a small file; false when it is missing, is not a regular
	// file or is larger than `limit`.
	[[nodiscard]] virtual bool readText(
		const Path &file,
		std::string &text,
		std::size_t limit);
	virtual void sleep(int milliseconds);
	// Whether two names that differ only in letter case are the same file
	// (Windows).
	[[nodiscard]] virtual bool caseInsensitiveNames();
};

// Removes a directory with everything in it, without ever following a link.
// True when nothing is left.
bool RemoveTree(FileOps &ops, const Path &directory);

} // namespace Install
