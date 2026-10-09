// File helpers that differ per platform.
#pragma once

#include "base/basic_types.h"

namespace base::platform {

// Writes `bytes` to `path` atomically (temporary file + rename), creating the
// parent directory. With `ownerOnly` the file is readable by the user only
// (0600 on Unix; on Windows the per-user profile ACL already applies).
[[nodiscard]] bool WriteFileAtomic(
	const QString &path,
	const QByteArray &bytes,
	bool ownerOnly = false);

// Reads a whole file; nullopt when it does not exist or cannot be read.
[[nodiscard]] std::optional<QByteArray> ReadFile(const QString &path);

// Creates the directory (and parents); with `ownerOnly` 0700 on Unix.
bool EnsureDirectory(const QString &path, bool ownerOnly = false);

// Removes a directory with everything in it.
bool RemoveRecursively(const QString &path);

// "windows 11", "ubuntu 24.04", ... for the X-Eleza-Client header.
[[nodiscard]] QString SystemDescription();

// "Desktop · Windows" style device name shown in the sessions list.
[[nodiscard]] QString DeviceName();

} // namespace base::platform
