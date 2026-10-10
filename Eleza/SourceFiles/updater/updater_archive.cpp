#include "updater/updater_archive.h"

#include "base/platform_file.h"
#include "install/install_paths.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>

#include <miniz.h>

namespace Updater {
namespace {

// The "version made by" host of entries whose external attributes hold a
// Unix mode in the upper 16 bits.
constexpr auto kHostUnix = 3;
constexpr auto kModeTypeMask = 0170000u;
constexpr auto kModeRegular = 0100000u;
constexpr auto kModeDirectory = 0040000u;
constexpr auto kModeExecutable = 0111u;

struct Entry {
	mz_uint index = 0;
	QString name; // relative, '/'-separated, no trailing slash
	bool directory = false;
	bool executable = false;
	qint64 size = 0;
};

struct Reader {
	QFile file;
};

size_t ReadCallback(void *opaque, mz_uint64 offset, void *buffer, size_t size) {
	const auto reader = static_cast<Reader*>(opaque);
	if (offset > mz_uint64(std::numeric_limits<qint64>::max())
		|| !reader->file.seek(qint64(offset))) {
		return 0;
	}
	const auto read = reader->file.read(static_cast<char*>(buffer), qint64(size));
	return (read < 0) ? 0 : size_t(read);
}

struct Writer {
	QFile file;
	qint64 written = 0;
	qint64 expected = 0;
};

size_t WriteCallback(void *opaque, mz_uint64 offset, const void *buffer, size_t size) {
	const auto writer = static_cast<Writer*>(opaque);
	// Sequential output, and never more than the entry declared.
	if (offset != mz_uint64(writer->written)
		|| qint64(size) > writer->expected - writer->written) {
		return 0;
	}
	const auto wrote = writer->file.write(
		static_cast<const char*>(buffer),
		qint64(size));
	if (wrote != qint64(size)) {
		return 0;
	}
	writer->written += wrote;
	return size;
}

// One name for everything the file systems we ship for could treat as the
// same file: composed form, case folded.
[[nodiscard]] QString CollisionKey(const QString &name) {
	return name.normalized(QString::NormalizationForm_C).toCaseFolded();
}

[[nodiscard]] UnpackResult Fail(UnpackError error, const QString &entry = QString()) {
	auto result = UnpackResult();
	result.error = error;
	result.entry = entry;
	return result;
}

class Archive {
public:
	Archive() {
		mz_zip_zero_struct(&_zip);
	}
	~Archive() {
		if (_opened) {
			mz_zip_reader_end(&_zip);
		}
	}
	Archive(const Archive &) = delete;
	Archive &operator=(const Archive &) = delete;

	[[nodiscard]] UnpackError open(const QString &path) {
		_reader.file.setFileName(path);
		if (!_reader.file.open(QIODevice::ReadOnly)) {
			return UnpackError::Open;
		}
		_zip.m_pRead = &ReadCallback;
		_zip.m_pIO_opaque = &_reader;
		if (!mz_zip_reader_init(&_zip, mz_uint64(_reader.file.size()), 0)) {
			return UnpackError::Format;
		}
		_opened = true;
		return UnpackError::None;
	}
	[[nodiscard]] mz_zip_archive *zip() {
		return &_zip;
	}
	[[nodiscard]] qint64 size() const {
		return _reader.file.size();
	}

private:
	mz_zip_archive _zip;
	Reader _reader;
	bool _opened = false;

};

// Reads and checks the whole table of contents. Nothing is written.
[[nodiscard]] UnpackResult ReadEntries(
		Archive &archive,
		const UnpackLimits &limits,
		QVector<Entry> &entries) {
	const auto zip = archive.zip();
	const auto count = mz_zip_reader_get_num_files(zip);
	if (count == 0) {
		return Fail(UnpackError::Format);
	} else if (count > mz_uint(limits.maxEntries)) {
		return Fail(UnpackError::TooMany);
	}
	auto files = QSet<QString>();
	auto directories = QSet<QString>();
	auto total = qint64(0);
	entries.reserve(int(count));
	for (auto i = mz_uint(0); i != count; ++i) {
		auto stat = mz_zip_archive_file_stat();
		if (!mz_zip_reader_file_stat(zip, i, &stat)) {
			return Fail(UnpackError::Format);
		}
		// The name as the bytes that are in the archive: a name longer than
		// the reader's buffer or with a zero byte inside is not what the
		// entry is really called.
		const auto length = mz_zip_reader_get_filename(zip, i, nullptr, 0);
		const auto raw = QByteArray(stat.m_filename);
		const auto shown = QString::fromUtf8(raw).left(120);
		if (length == 0 || mz_uint(raw.size()) + 1 != length) {
			return Fail(UnpackError::UnsafeName, shown);
		}
		auto name = raw;
		const auto directory = name.endsWith('/');
		if (directory) {
			name.chop(1);
		}
		if (!Install::IsSafeRelativePath(
				std::string_view(name.constData(), size_t(name.size())))) {
			return Fail(UnpackError::UnsafeName, shown);
		}

		auto entry = Entry();
		entry.index = i;
		entry.name = QString::fromUtf8(name);
		entry.directory = directory;

		const auto host = (stat.m_version_made_by >> 8);
		const auto mode = (host == kHostUnix)
			? (stat.m_external_attr >> 16)
			: 0u;
		const auto type = (mode & kModeTypeMask);
		// "Directory" may be said by the name, by the Unix mode and by the
		// DOS attribute (which is what m_is_directory also looks at): all
		// of them must agree, and the only other thing allowed is a file.
		if (directory) {
			if ((type != 0 && type != kModeDirectory)
				|| stat.m_uncomp_size != 0
				|| stat.m_comp_size > 2) {
				return Fail(UnpackError::NotAFile, shown);
			}
		} else if ((type != 0 && type != kModeRegular)
			|| stat.m_is_directory) {
			return Fail(UnpackError::NotAFile, shown);
		}
		if (stat.m_is_encrypted || !stat.m_is_supported) {
			return Fail(UnpackError::Unsupported, shown);
		}
		entry.executable = !directory && ((mode & kModeExecutable) != 0);

		if (stat.m_uncomp_size > mz_uint64(limits.maxFileBytes)) {
			return Fail(UnpackError::TooLarge, shown);
		}
		entry.size = qint64(stat.m_uncomp_size);
		total += entry.size;
		if (total > limits.maxTotalBytes
			|| total / qMax(limits.maxRatio, 1) > archive.size()) {
			return Fail(UnpackError::TooLarge, shown);
		}

		// Collisions: the same name twice, a file where a directory is
		// needed, a directory where a file already is.
		const auto key = CollisionKey(entry.name);
		if (directory) {
			if (files.contains(key)) {
				return Fail(UnpackError::Duplicate, shown);
			}
			directories.insert(key);
		} else {
			if (files.contains(key) || directories.contains(key)) {
				return Fail(UnpackError::Duplicate, shown);
			}
			files.insert(key);
		}
		for (auto slash = key.lastIndexOf(u'/');
			slash > 0;
			slash = key.lastIndexOf(u'/', slash - 1)) {
			const auto parent = key.left(slash);
			if (files.contains(parent)) {
				return Fail(UnpackError::Duplicate, shown);
			}
			directories.insert(parent);
		}
		entries.push_back(std::move(entry));
	}
	return UnpackResult();
}

[[nodiscard]] UnpackResult Extract(
		Archive &archive,
		const QVector<Entry> &entries,
		const QString &root) {
	auto result = UnpackResult();
	const auto rootDir = QDir(root);
	for (const auto &entry : entries) {
		const auto path = rootDir.filePath(entry.name);
		if (entry.directory) {
			if (!QDir().mkpath(path)) {
				return Fail(UnpackError::Write, entry.name);
			}
			continue;
		}
		if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
			return Fail(UnpackError::Write, entry.name);
		}
		auto writer = Writer();
		writer.expected = entry.size;
		writer.file.setFileName(path);
		// NewOnly: a name that is there already (the checks above missed a
		// collision this file system sees) is an error, never an overwrite.
		if (!writer.file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
			return Fail(UnpackError::Write, entry.name);
		}
		const auto extracted = mz_zip_reader_extract_to_callback(
			archive.zip(),
			entry.index,
			&WriteCallback,
			&writer,
			0);
		if (!extracted || writer.written != entry.size) {
			return Fail(
				(writer.file.error() != QFileDevice::NoError)
					? UnpackError::Write
					: UnpackError::Corrupt,
				entry.name);
		}
		if (!writer.file.flush()) {
			return Fail(UnpackError::Write, entry.name);
		}
#ifndef Q_OS_WIN
		// Exactly two kinds of files come out: programs and data. No other
		// bit of the archive's mode (setuid, group / world writable) is used.
		const auto permissions = QFileDevice::ReadOwner
			| QFileDevice::WriteOwner
			| QFileDevice::ReadGroup
			| QFileDevice::ReadOther
			| (entry.executable
				? (QFileDevice::ExeOwner
					| QFileDevice::ExeGroup
					| QFileDevice::ExeOther)
				: QFileDevice::Permissions());
		if (!writer.file.setPermissions(permissions)) {
			return Fail(UnpackError::Write, entry.name);
		}
#endif // !Q_OS_WIN
		writer.file.close();
		++result.files;
		result.bytes += entry.size;
	}
	return result;
}

} // namespace

UnpackResult Unpack(
		const QString &archivePath,
		const QString &target,
		const UnpackLimits &limits) {
	if (target.isEmpty() || QFileInfo::exists(target)) {
		return Fail(UnpackError::Write);
	}
	auto archive = Archive();
	if (const auto error = archive.open(archivePath);
		error != UnpackError::None) {
		return Fail(error);
	}
	auto entries = QVector<Entry>();
	if (const auto checked = ReadEntries(archive, limits, entries);
		!checked.ok()) {
		return checked;
	}

	const auto temporary = target + u".tmp"_s;
	if (!base::platform::RemoveRecursively(temporary)
		|| !QDir().mkpath(temporary)) {
		return Fail(UnpackError::Write);
	}
	auto result = Extract(archive, entries, temporary);
	if (result.ok() && !QDir().rename(temporary, target)) {
		result = Fail(UnpackError::Write);
	}
	if (!result.ok()) {
		base::platform::RemoveRecursively(temporary);
	}
	return result;
}

} // namespace Updater
