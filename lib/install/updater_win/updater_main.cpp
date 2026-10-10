// Updater.exe — installs a downloaded, verified and unpacked update of Eleza
// on Windows. No Qt and a static C++ runtime: it must not depend on the DLLs
// it replaces. The app copies it out of the install directory (so that this
// file can be replaced as well) and starts the copy:
//
//   Updater.exe --params=<install.params>
//
// What happens (the file work itself is Install::Apply, install_swap.h, shared
// with Linux and covered by the tests there):
//
//   1. One updater per install directory (a named mutex).
//   2. Waits for the app to exit (the process id from the request).
//   3. Install directory not writable (a copy under Program Files): starts
//      itself again with the "runas" verb — the system asks for administrator
//      rights — waits for that copy and takes its result. The elevated copy
//      only swaps the files; the app is started again by this one, so it never
//      runs with administrator rights.
//   4. Swaps the files, with a rollback when anything fails; writes the result
//      file for the next start of the app.
//   5. Starts Eleza.exe again with the arguments it had.
//
// There is no window: the swap is renames inside one volume and normally takes
// well under a second.
#include "install/install_helper.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif // !NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr auto kParentExitTimeoutMs = DWORD(60 * 1000);
// A file of the old version may be held for a moment by a scanner.
constexpr auto kRenameRetries = 40;
constexpr auto kRenameRetryDelayMs = 250;

enum ExitCode : int {
	kExitInstalled = 0,
	kExitFailed = 1,
	kExitBadArguments = 2,
	kExitAlreadyRunning = 3,
	kExitParentAlive = 4,
};

[[nodiscard]] std::wstring Utf8ToWide(const std::string &text) {
	if (text.empty()) {
		return std::wstring();
	}
	const auto size = MultiByteToWideChar(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		nullptr,
		0);
	if (size <= 0) {
		return std::wstring();
	}
	auto result = std::wstring(std::size_t(size), L'\0');
	MultiByteToWideChar(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		result.data(),
		size);
	return result;
}

// One argument the way CommandLineToArgvW and the C runtime read it back:
// quotes around anything with a space, backslashes doubled only where they
// precede a quote.
void AppendArgument(std::wstring &to, const std::wstring &argument) {
	if (!to.empty()) {
		to += L' ';
	}
	if (!argument.empty()
		&& argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		to += argument;
		return;
	}
	to += L'"';
	for (auto i = std::size_t(0);; ++i) {
		auto backslashes = std::size_t(0);
		while (i != argument.size() && argument[i] == L'\\') {
			++i;
			++backslashes;
		}
		if (i == argument.size()) {
			to.append(backslashes * 2, L'\\');
			break;
		} else if (argument[i] == L'"') {
			to.append(backslashes * 2 + 1, L'\\');
		} else {
			to.append(backslashes, L'\\');
		}
		to += argument[i];
	}
	to += L'"';
}

[[nodiscard]] std::wstring OwnPath() {
	auto buffer = std::wstring(32768, L'\0');
	const auto size = GetModuleFileNameW(
		nullptr,
		buffer.data(),
		DWORD(buffer.size()));
	// Zero is an error, the full buffer a truncated path.
	if (size == 0 || size >= buffer.size()) {
		return std::wstring();
	}
	buffer.resize(size);
	return buffer;
}

// The name of the mutex: the install directory without the characters a
// kernel object name cannot have.
[[nodiscard]] std::wstring MutexName(const Install::Path &installDir) {
	auto hash = std::uint64_t(1469598103934665603ULL);
	for (const auto ch : installDir.native()) {
		// The same directory spelled in another case is the same directory.
		const auto lower = (ch >= L'A' && ch <= L'Z') ? wchar_t(ch - L'A' + L'a') : ch;
		hash ^= std::uint64_t(lower);
		hash *= 1099511628211ULL;
	}
	wchar_t text[17] = L"";
	static constexpr wchar_t kDigits[] = L"0123456789abcdef";
	for (auto i = 0; i != 16; ++i) {
		text[i] = kDigits[(hash >> (60 - 4 * i)) & 0xF];
	}
	return std::wstring(L"Local\\ElezaUpdater-") + text;
}

// Waits until the app is gone. False when it is still there after the timeout.
[[nodiscard]] bool WaitForParent(unsigned long pid, const Install::Log &log) {
	if (!pid) {
		return true;
	}
	const auto process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
	if (!process) {
		// ERROR_INVALID_PARAMETER: no such process, it has exited already.
		// Anything else (no right to open it): give it a moment; a file
		// that is still in use fails the swap, which is then undone.
		if (GetLastError() != ERROR_INVALID_PARAMETER) {
			log("the app's process cannot be watched, waiting a moment");
			Sleep(3000);
		}
		return true;
	}
	const auto waited = WaitForSingleObject(process, kParentExitTimeoutMs);
	CloseHandle(process);
	return (waited == WAIT_OBJECT_0);
}

// Whether files can be created in the directory.
[[nodiscard]] bool IsWritable(const Install::Path &directory) {
	const auto probe = directory / L"_update_probe";
	if (CreateDirectoryW(probe.c_str(), nullptr)) {
		RemoveDirectoryW(probe.c_str());
		return true;
	}
	// A probe left by a run that was killed between the two calls.
	return (GetLastError() == ERROR_ALREADY_EXISTS)
		&& RemoveDirectoryW(probe.c_str());
}

// Runs this program again as administrator and waits for it. False when the
// rights were not given (or it could not be started).
[[nodiscard]] bool RunElevated(const std::wstring &paramsPath, const Install::Log &log) {
	const auto self = OwnPath();
	if (self.empty()) {
		return false;
	}
	auto parameters = std::wstring();
	AppendArgument(parameters, L"--params=" + paramsPath);
	AppendArgument(parameters, L"--elevated");

	// The shell may hand the verb to a COM object: it wants an apartment.
	const auto initialized = SUCCEEDED(CoInitializeEx(
		nullptr,
		COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
	struct Uninitialize {
		bool needed = false;
		~Uninitialize() {
			if (needed) {
				CoUninitialize();
			}
		}
	};
	const auto uninitialize = Uninitialize{ initialized };

	auto info = SHELLEXECUTEINFOW();
	info.cbSize = sizeof(info);
	// The process handle is wanted back; no error boxes from the shell.
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
	info.lpVerb = L"runas";
	info.lpFile = self.c_str();
	info.lpParameters = parameters.c_str();
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info)) {
		// ERROR_CANCELLED: the user answered "No".
		log((GetLastError() == ERROR_CANCELLED)
			? "administrator rights were declined"
			: "could not ask for administrator rights");
		return false;
	} else if (!info.hProcess) {
		return false;
	}
	WaitForSingleObject(info.hProcess, INFINITE);
	CloseHandle(info.hProcess);
	return true;
}

void StartApp(const Install::HelperParams &params, const Install::Log &log) {
	const auto executable = (params.installDir
		/ Install::PathFromUtf8(params.executable)).native();
	auto commandLine = std::wstring();
	AppendArgument(commandLine, executable);
	for (const auto &argument : params.arguments) {
		AppendArgument(commandLine, Utf8ToWide(argument));
	}
	auto startup = STARTUPINFOW();
	startup.cb = sizeof(startup);
	auto process = PROCESS_INFORMATION();
	// CreateProcessW may write into the command line buffer: it is our own
	// (std::wstring::data(), zero-terminated).
	if (!CreateProcessW(
			executable.c_str(),
			commandLine.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			params.installDir.c_str(),
			&startup,
			&process)) {
		log("could not start the app again");
		return;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
}

void WriteFailure(
		Install::FileOps &ops,
		const Install::HelperParams &params,
		const std::string &error) {
	auto result = Install::HelperResult();
	result.version = params.version;
	result.status = Install::Status::Unchanged;
	result.error = error;
	[[maybe_unused]] const auto written = Install::WriteResult(
		ops,
		params.resultPath,
		result);
}

[[nodiscard]] int Run() {
	auto paramsPath = std::wstring();
	auto elevated = false;
	{
		auto count = 0;
		const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
		if (!arguments) {
			return kExitBadArguments;
		}
		const auto prefix = std::wstring(L"--params=");
		for (auto i = 1; i < count; ++i) {
			const auto argument = std::wstring(arguments[i]);
			if (argument.compare(0, prefix.size(), prefix) == 0) {
				paramsPath = argument.substr(prefix.size());
			} else if (argument == L"--elevated") {
				elevated = true;
			}
		}
		LocalFree(arguments);
	}
	if (paramsPath.empty()) {
		return kExitBadArguments;
	}

	auto ops = Install::FileOps();
	auto params = Install::HelperParams();
	if (!Install::ReadParams(ops, Install::Path(paramsPath), params)) {
		return kExitBadArguments;
	}
	const auto fileLog = Install::FileLog(ops, params.logPath);
	const auto log = Install::Log([&](const std::string &line) {
		if (fileLog) {
			fileLog((elevated ? "[elevated] " : "") + line);
		}
	});
	log("updater started for " + params.version);

	if (elevated) {
		// The first copy holds the mutex, has waited for the app and starts
		// it again: here only the files.
		const auto result = Install::RunInstall(
			params,
			ops,
			log,
			kRenameRetries,
			kRenameRetryDelayMs);
		return (result.status == Install::Status::Installed)
			? kExitInstalled
			: kExitFailed;
	}

	// Kept until the process ends; the system closes the handle.
	const auto mutex = CreateMutexW(
		nullptr,
		TRUE,
		MutexName(params.installDir).c_str());
	if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
		log("another updater is working on this directory");
		return kExitAlreadyRunning;
	}

	if (!WaitForParent(params.parentPid, log)) {
		log("the app did not exit");
		WriteFailure(ops, params, "Eleza did not exit");
		return kExitParentAlive;
	}

	auto installed = false;
	if (!IsWritable(params.installDir)) {
		log("the install directory is not writable, asking for administrator rights");
		if (RunElevated(paramsPath, log)) {
			auto result = Install::HelperResult();
			installed = Install::ReadResult(ops, params.resultPath, result)
				&& (result.version == params.version)
				&& (result.status == Install::Status::Installed);
		} else {
			WriteFailure(ops, params, "administrator rights are needed");
		}
	} else {
		const auto result = Install::RunInstall(
			params,
			ops,
			log,
			kRenameRetries,
			kRenameRetryDelayMs);
		installed = (result.status == Install::Status::Installed);
	}
	StartApp(params, log);
	return installed ? kExitInstalled : kExitFailed;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	return Run();
}
