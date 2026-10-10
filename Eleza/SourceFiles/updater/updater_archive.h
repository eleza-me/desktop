// Unpacks an update package (a zip archive) into a new directory.
//
// The archive has passed its SHA-256 check against the signed manifest before
// it gets here; it is still read as hostile input, so that one mistake in the
// release tooling cannot write outside the staging directory:
//
//   * names must be safe relative paths (Install::IsSafeRelativePath): nothing
//     absolute, no "..", no drive letters, device names or backslashes;
//   * only regular files and directories: an entry that says it is a symbolic
//     link, a device or anything else is refused (and nothing is ever created
//     as a link);
//   * no two entries for one name, also when they differ only by case or by
//     Unicode normalization, and no file where another entry needs a directory;
//   * no encrypted entries, no compression method other than stored / deflate;
//   * limits on the number of entries, the size of one file, the size of
//     everything together and the ratio to the size of the archive (zip bombs):
//     checked on the declared sizes first and again on the bytes that really
//     come out of the decompressor.
//
// Everything is checked before the first byte is written. The files go to
// "<target>.tmp", which becomes <target> only when all of them are there.
// Blocking: call it from a worker thread.
#pragma once

#include "base/basic_types.h"

namespace Updater {

struct UnpackLimits {
	int maxEntries = 20000;
	qint64 maxFileBytes = qint64(1) << 30;
	qint64 maxTotalBytes = qint64(3) << 30;
	// Everything unpacked may be at most this many times the archive.
	int maxRatio = 100;
};

enum class UnpackError {
	None,
	Open,        // the archive cannot be read
	Format,      // not a zip archive (or a damaged one)
	TooMany,     // more entries than allowed
	UnsafeName,
	Duplicate,
	NotAFile,    // a link, a device…
	Unsupported, // encrypted, or a compression method we do not read
	TooLarge,    // the size limits or the ratio
	Corrupt,     // an entry does not decompress to what it declares
	Write,       // the target cannot be written
};

struct UnpackResult {
	UnpackError error = UnpackError::None;
	QString entry; // the entry that was refused, for the log
	int files = 0;
	qint64 bytes = 0;

	[[nodiscard]] bool ok() const {
		return error == UnpackError::None;
	}
};

// `target` must not exist; its parent is created when missing.
[[nodiscard]] UnpackResult Unpack(
	const QString &archivePath,
	const QString &target,
	const UnpackLimits &limits = UnpackLimits());

} // namespace Updater
