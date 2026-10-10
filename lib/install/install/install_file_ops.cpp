#include "install/install_file_ops.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif // !NOMINMAX
#include <windows.h>
#else // _WIN32
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif // !_WIN32

namespace Install {
namespace {

#ifdef _WIN32

[[nodiscard]] std::string WideToUtf8(const wchar_t *text) {
	const auto size = WideCharToMultiByte(
		CP_UTF8,
		0,
		text,
		-1,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (size <= 1) {
		return std::string();
	}
	// The count includes the terminating zero.
	auto result = std::string(std::size_t(size), '\0');
	WideCharToMultiByte(
		CP_UTF8,
		0,
		text,
		-1,
		result.data(),
		size,
		nullptr,
		nullptr);
	result.resize(std::size_t(size - 1));
	return result;
}

// A handle that is closed when the scope ends.
class Handle {
public:
	explicit Handle(HANDLE value) : _value(value) {
	}
	~Handle() {
		if (valid()) {
			CloseHandle(_value);
		}
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;

	[[nodiscard]] bool valid() const {
		return _value != INVALID_HANDLE_VALUE && _value != nullptr;
	}
	[[nodiscard]] HANDLE get() const {
		return _value;
	}

private:
	HANDLE _value = INVALID_HANDLE_VALUE;

};

// A regular file behind the handle: not a directory and not a link that was
// opened as itself (FILE_FLAG_OPEN_REPARSE_POINT).
[[nodiscard]] bool IsRegularFile(HANDLE file) {
	auto info = BY_HANDLE_FILE_INFORMATION();
	return GetFileInformationByHandle(file, &info)
		&& !(info.dwFileAttributes
			& (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

#else // _WIN32

class Descriptor {
public:
	explicit Descriptor(int value) : _value(value) {
	}
	~Descriptor() {
		if (valid()) {
			::close(_value);
		}
	}
	Descriptor(const Descriptor &) = delete;
	Descriptor &operator=(const Descriptor &) = delete;

	[[nodiscard]] bool valid() const {
		return _value >= 0;
	}
	[[nodiscard]] int get() const {
		return _value;
	}

private:
	int _value = -1;

};

[[nodiscard]] bool WriteAll(int descriptor, const char *data, std::size_t size) {
	while (size > 0) {
		const auto written = ::write(descriptor, data, size);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		data += written;
		size -= std::size_t(written);
	}
	return true;
}

#endif // !_WIN32

} // namespace

EntryKind FileOps::kind(const Path &path) {
#ifdef _WIN32
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		const auto error = GetLastError();
		return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			? EntryKind::Missing
			: EntryKind::Other;
	} else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		return EntryKind::Other;
	} else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
		return EntryKind::Directory;
	} else if (attributes & FILE_ATTRIBUTE_DEVICE) {
		return EntryKind::Other;
	}
	return EntryKind::File;
#else // _WIN32
	struct stat info;
	if (::lstat(path.c_str(), &info) != 0) {
		return (errno == ENOENT || errno == ENOTDIR)
			? EntryKind::Missing
			: EntryKind::Other;
	} else if (S_ISREG(info.st_mode)) {
		return EntryKind::File;
	} else if (S_ISDIR(info.st_mode)) {
		return EntryKind::Directory;
	}
	return EntryKind::Other;
#endif // !_WIN32
}

bool FileOps::list(const Path &directory, std::vector<std::string> &names) {
	names.clear();
#ifdef _WIN32
	auto data = WIN32_FIND_DATAW();
	const auto pattern = directory / L"*";
	const auto find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) {
		// A directory always has "." in it: an empty result is an error too.
		return false;
	}
	do {
		const auto name = std::wstring_view(data.cFileName);
		if (name != L"." && name != L"..") {
			names.push_back(WideToUtf8(data.cFileName));
		}
	} while (FindNextFileW(find, &data));
	const auto finished = (GetLastError() == ERROR_NO_MORE_FILES);
	FindClose(find);
	return finished;
#else // _WIN32
	const auto stream = ::opendir(directory.c_str());
	if (!stream) {
		return false;
	}
	for (;;) {
		errno = 0;
		const auto entry = ::readdir(stream);
		if (!entry) {
			break;
		}
		const auto name = std::string_view(entry->d_name);
		if (name != "." && name != "..") {
			names.emplace_back(name);
		}
	}
	const auto finished = (errno == 0);
	::closedir(stream);
	return finished;
#endif // !_WIN32
}

bool FileOps::makeDirectories(const Path &path) {
	switch (kind(path)) {
	case EntryKind::Directory: return true;
	case EntryKind::Missing: break;
	default: return false;
	}
	const auto parent = path.parent_path();
	if (!parent.empty() && parent != path && !makeDirectories(parent)) {
		return false;
	}
#ifdef _WIN32
	return CreateDirectoryW(path.c_str(), nullptr)
		|| (GetLastError() == ERROR_ALREADY_EXISTS
			&& kind(path) == EntryKind::Directory);
#else // _WIN32
	return (::mkdir(path.c_str(), 0755) == 0)
		|| (errno == EEXIST && kind(path) == EntryKind::Directory);
#endif // !_WIN32
}

bool FileOps::rename(const Path &from, const Path &to) {
#ifdef _WIN32
	// Without MOVEFILE_REPLACE_EXISTING an existing target fails the call
	// (ERROR_ALREADY_EXISTS); without MOVEFILE_COPY_ALLOWED so does a target
	// on another volume (ERROR_NOT_SAME_DEVICE): the move is a rename, never
	// a copy that could stop halfway. MOVEFILE_WRITE_THROUGH returns only
	// when the change is on the disk.
	return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else // _WIN32
#if defined(__linux__) && defined(RENAME_NOREPLACE)
	if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
		return true;
	} else if (errno != EINVAL && errno != ENOSYS) {
		return false;
	}
	// A file system without the flag: check and rename (one updater runs at
	// a time).
#endif // __linux__ && RENAME_NOREPLACE
	struct stat info;
	if (::lstat(to.c_str(), &info) == 0 || errno != ENOENT) {
		return false;
	}
	return ::rename(from.c_str(), to.c_str()) == 0;
#endif // !_WIN32
}

bool FileOps::copyFile(const Path &from, const Path &to) {
	if (kind(from) != EntryKind::File) {
		return false;
	}
#ifdef _WIN32
	// bFailIfExists: an existing target is never overwritten.
	if (!CopyFileW(from.c_str(), to.c_str(), TRUE)) {
		return false;
	}
	const auto file = Handle(CreateFileW(
		to.c_str(),
		GENERIC_WRITE,
		0,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr));
	if (!file.valid() || !FlushFileBuffers(file.get())) {
		DeleteFileW(to.c_str());
		return false;
	}
	return true;
#else // _WIN32
	const auto source = Descriptor(::open(
		from.c_str(),
		O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
	struct stat info;
	if (!source.valid()
		|| ::fstat(source.get(), &info) != 0
		|| !S_ISREG(info.st_mode)) {
		return false;
	}
	const auto mode = mode_t((info.st_mode & 0111) ? 0755 : 0644);
	auto ok = false;
	{
		const auto target = Descriptor(::open(
			to.c_str(),
			O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			mode));
		if (!target.valid()) {
			return false;
		}
		char buffer[64 * 1024];
		ok = true;
		for (;;) {
			const auto read = ::read(source.get(), buffer, sizeof(buffer));
			if (read < 0) {
				if (errno == EINTR) {
					continue;
				}
				ok = false;
				break;
			} else if (read == 0) {
				break;
			} else if (!WriteAll(target.get(), buffer, std::size_t(read))) {
				ok = false;
				break;
			}
		}
		// The umask may have taken bits away.
		ok = ok
			&& (::fchmod(target.get(), mode) == 0)
			&& (::fsync(target.get()) == 0);
	}
	if (!ok) {
		::unlink(to.c_str());
	}
	return ok;
#endif // !_WIN32
}

bool FileOps::removeFile(const Path &path) {
#ifdef _WIN32
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		const auto error = GetLastError();
		return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
	}
	if (attributes & FILE_ATTRIBUTE_READONLY) {
		SetFileAttributesW(
			path.c_str(),
			attributes & ~DWORD(FILE_ATTRIBUTE_READONLY));
	}
	if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
		// Only a link to a directory gets here (a junction, a directory
		// symbolic link): RemoveDirectoryW removes the link itself.
		return (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
			&& RemoveDirectoryW(path.c_str());
	}
	return DeleteFileW(path.c_str()) != 0;
#else // _WIN32
	struct stat info;
	if (::lstat(path.c_str(), &info) != 0) {
		return (errno == ENOENT);
	} else if (S_ISDIR(info.st_mode)) {
		return false;
	}
	return ::unlink(path.c_str()) == 0;
#endif // !_WIN32
}

bool FileOps::removeEmptyDirectory(const Path &path) {
	if (kind(path) != EntryKind::Directory) {
		return false;
	}
#ifdef _WIN32
	return RemoveDirectoryW(path.c_str()) != 0;
#else // _WIN32
	return ::rmdir(path.c_str()) == 0;
#endif // !_WIN32
}

bool FileOps::appendLine(const Path &file, const std::string &line) {
	const auto text = line + '\n';
#ifdef _WIN32
	// GENERIC_WRITE is what FlushFileBuffers asks for; the position is moved
	// to the end by hand (one writer at a time). The reparse point flag
	// opens a link as itself, and then it is refused.
	const auto handle = Handle(CreateFileW(
		file.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
	if (!handle.valid() || !IsRegularFile(handle.get())) {
		return false;
	}
	auto end = LARGE_INTEGER();
	end.QuadPart = 0;
	if (!SetFilePointerEx(handle.get(), end, nullptr, FILE_END)) {
		return false;
	}
	auto written = DWORD(0);
	return WriteFile(
			handle.get(),
			text.data(),
			DWORD(text.size()),
			&written,
			nullptr)
		&& (written == DWORD(text.size()))
		&& FlushFileBuffers(handle.get());
#else // _WIN32
	const auto descriptor = Descriptor(::open(
		file.c_str(),
		O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
		0644));
	struct stat info;
	return descriptor.valid()
		&& (::fstat(descriptor.get(), &info) == 0)
		&& S_ISREG(info.st_mode)
		&& WriteAll(descriptor.get(), text.data(), text.size())
		&& (::fsync(descriptor.get()) == 0);
#endif // !_WIN32
}

bool FileOps::readText(const Path &file, std::string &text, std::size_t limit) {
	text.clear();
#ifdef _WIN32
	const auto handle = Handle(CreateFileW(
		file.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
	if (!handle.valid() || !IsRegularFile(handle.get())) {
		return false;
	}
	char buffer[16 * 1024];
	for (;;) {
		auto read = DWORD(0);
		if (!ReadFile(handle.get(), buffer, DWORD(sizeof(buffer)), &read, nullptr)) {
			return false;
		} else if (read == 0) {
			return true;
		} else if (text.size() + read > limit) {
			return false;
		}
		text.append(buffer, read);
	}
#else // _WIN32
	const auto descriptor = Descriptor(::open(
		file.c_str(),
		O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
	struct stat info;
	if (!descriptor.valid()
		|| ::fstat(descriptor.get(), &info) != 0
		|| !S_ISREG(info.st_mode)) {
		return false;
	}
	char buffer[16 * 1024];
	for (;;) {
		const auto read = ::read(descriptor.get(), buffer, sizeof(buffer));
		if (read < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		} else if (read == 0) {
			return true;
		} else if (text.size() + std::size_t(read) > limit) {
			return false;
		}
		text.append(buffer, std::size_t(read));
	}
#endif // !_WIN32
}

void FileOps::sleep(int milliseconds) {
#ifdef _WIN32
	Sleep(DWORD(milliseconds > 0 ? milliseconds : 0));
#else // _WIN32
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif // !_WIN32
}

bool RemoveTree(FileOps &ops, const Path &directory) {
	switch (ops.kind(directory)) {
	case EntryKind::Missing: return true;
	case EntryKind::Directory: break;
	default: return ops.removeFile(directory);
	}
	auto names = std::vector<std::string>();
	if (!ops.list(directory, names)) {
		return false;
	}
	auto ok = true;
	for (const auto &name : names) {
		const auto path = directory / PathFromUtf8(name);
		if (ops.kind(path) == EntryKind::Directory) {
			ok = RemoveTree(ops, path) && ok;
		} else {
			ok = ops.removeFile(path) && ok;
		}
	}
	return ok && ops.removeEmptyDirectory(directory);
}

} // namespace Install
