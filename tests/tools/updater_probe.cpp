// A stand-in for Eleza.exe in the smoke test of the Windows updater
// (tests/tools/updater-smoke.py): it only reports that it was started.
//
//   <its directory>\probe-output.txt   UTF-8: the name it was built with
//                                      (PROBE_NAME: "old" / "new"), its process
//                                      id, then one argument per line
//   --probe-sleep=<ms>                 stays alive that long before exiting
//                                      (the app the updater has to wait for)
//
// Windows only, no dependencies beyond the system.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>

#ifndef PROBE_NAME
#define PROBE_NAME "probe"
#endif // !PROBE_NAME

namespace {

[[nodiscard]] std::string ToUtf8(const std::wstring &text) {
	if (text.empty()) {
		return std::string();
	}
	const auto size = WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	auto result = std::string(size_t(size > 0 ? size : 0), '\0');
	WideCharToMultiByte(
		CP_UTF8,
		0,
		text.data(),
		int(text.size()),
		result.data(),
		size,
		nullptr,
		nullptr);
	return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	auto path = std::wstring(32768, L'\0');
	const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
	path.resize(length);
	const auto slash = path.find_last_of(L"\\/");
	const auto directory = (slash == std::wstring::npos)
		? std::wstring(L".")
		: path.substr(0, slash);

	auto count = 0;
	const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
	auto text = std::string(PROBE_NAME "\n")
		+ std::to_string(GetCurrentProcessId())
		+ "\n";
	auto sleep = DWORD(0);
	const auto prefix = std::wstring(L"--probe-sleep=");
	for (auto i = 1; arguments && i < count; ++i) {
		const auto argument = std::wstring(arguments[i]);
		if (argument.compare(0, prefix.size(), prefix) == 0) {
			sleep = DWORD(_wtoi(argument.c_str() + prefix.size()));
		}
		text += ToUtf8(argument) + "\n";
	}
	const auto temporary = directory + L"\\probe-output.tmp";
	const auto final = directory + L"\\probe-output.txt";
	const auto file = CreateFileW(
		temporary.c_str(),
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return 1;
	}
	auto written = DWORD(0);
	WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
	CloseHandle(file);
	// Appears whole or not at all.
	MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING);
	if (sleep) {
		Sleep(sleep);
	}
	return 0;
}
