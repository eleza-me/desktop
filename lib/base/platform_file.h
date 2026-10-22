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

// Creates the directory if needed and makes sure it is really ours: a
// directory (not a link somebody put there), owned by this user, closed to
// everybody else (0700). False when it is not and cannot be made so - then
// nothing private may go into it. For places under a shared parent (the
// system temporary directory).
[[nodiscard]] bool EnsurePrivateDirectory(const QString &path);

// Removes a directory with everything in it. A link (or a junction) is removed
// itself, what it points to is left alone.
bool RemoveRecursively(const QString &path);

// A directory for short-lived files that nobody else on the machine can list
// or read (voice recordings, pasted images, cropped photos, media handed to
// the player): never the shared temporary directory itself, where names are
// visible to everyone, files are created world-readable and another user
// can plant a file or a link under a name we are about to use.
// By default it is created (0700, verified to be ours) in the per-user
// runtime directory or under the system temporary one; the application
// points it into its own data directory with SetPrivateTempRoot().
[[nodiscard]] QString PrivateTempDirectory();
void SetPrivateTempRoot(const QString &path);
// Deletes what earlier runs left in the directory.
void ClearPrivateTemp();

// A file name that came from somebody else (a message, a server header) made
// safe to create in a directory of ours: no path separators or characters
// the file systems refuse, no control or direction-override characters (so
// "photo<U+202E>gnp.exe" cannot be shown as "photoexe.png"), no leading
// dots, no trailing dots or spaces (Windows drops them: "a.exe." is
// "a.exe"), no reserved device names (CON, NUL, COM1...). Never empty.
[[nodiscard]] QString SafeFileName(const QString &name, const QString &fallback = QString());

// True for a file the system would run, install or interpret when it is
// "opened" (programs, installers, scripts, shortcuts, macro documents, local
// web pages), judged by the name as the file system sees it ("x.exe." and
// "x.exe:stream" are "x.exe") and, on Unix, by the executable bit of a file
// without an extension. Such files are shown in their folder, never launched.
[[nodiscard]] bool OpensAsProgram(const QString &path);

// "windows 11", "ubuntu 24.04", ... for the X-Eleza-Client header.
[[nodiscard]] QString SystemDescription();

// "Desktop · Windows" style device name shown in the sessions list.
[[nodiscard]] QString DeviceName();

} // namespace base::platform
