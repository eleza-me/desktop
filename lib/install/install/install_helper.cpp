#include "install/install_helper.h"

#include <cstdlib>
#include <ctime>

namespace Install {
namespace {

constexpr auto kParamsLimit = std::size_t(1) << 20;

[[nodiscard]] std::string Escape(const std::string &value) {
	auto result = std::string();
	result.reserve(value.size());
	for (const auto ch : value) {
		if (ch == '\\') {
			result += "\\\\";
		} else if (ch == '\n') {
			result += "\\n";
		} else if (ch == '\r') {
			result += "\\r";
		} else {
			result += ch;
		}
	}
	return result;
}

[[nodiscard]] std::string Unescape(const std::string &value) {
	auto result = std::string();
	result.reserve(value.size());
	for (auto i = std::size_t(0); i != value.size(); ++i) {
		if (value[i] != '\\' || i + 1 == value.size()) {
			result += value[i];
			continue;
		}
		const auto next = value[++i];
		result += (next == 'n') ? '\n' : (next == 'r') ? '\r' : next;
	}
	return result;
}

void AddLine(std::string &to, const char *key, const std::string &value) {
	to += key;
	to += '=';
	to += Escape(value);
	to += '\n';
}

template <typename Callback>
void ForEachPair(const std::string &text, Callback &&callback) {
	auto from = std::size_t(0);
	while (from < text.size()) {
		auto end = text.find('\n', from);
		if (end == std::string::npos) {
			end = text.size();
		}
		auto line = text.substr(from, end - from);
		from = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const auto equals = line.find('=');
		if (equals != std::string::npos) {
			callback(line.substr(0, equals), Unescape(line.substr(equals + 1)));
		}
	}
}

[[nodiscard]] const char *StatusName(Status status) {
	switch (status) {
	case Status::Installed: return "installed";
	case Status::Unchanged: return "unchanged";
	case Status::Broken: return "broken";
	}
	return "unchanged";
}

} // namespace

std::string SerializeParams(const HelperParams &params) {
	auto result = std::string();
	AddLine(result, "pid", std::to_string(params.parentPid));
	AddLine(result, "install", PathToUtf8(params.installDir));
	AddLine(result, "staged", PathToUtf8(params.stagedDir));
	AddLine(result, "version", params.version);
	AddLine(result, "executable", params.executable);
	AddLine(result, "result", PathToUtf8(params.resultPath));
	AddLine(result, "log", PathToUtf8(params.logPath));
	for (const auto &argument : params.arguments) {
		AddLine(result, "argument", argument);
	}
	return result;
}

bool ParseParams(const std::string &text, HelperParams &params) {
	params = HelperParams();
	ForEachPair(text, [&](const std::string &key, std::string value) {
		if (key == "pid") {
			params.parentPid = std::strtoul(value.c_str(), nullptr, 10);
		} else if (key == "install") {
			params.installDir = PathFromUtf8(value);
		} else if (key == "staged") {
			params.stagedDir = PathFromUtf8(value);
		} else if (key == "version") {
			params.version = std::move(value);
		} else if (key == "executable") {
			params.executable = std::move(value);
		} else if (key == "result") {
			params.resultPath = PathFromUtf8(value);
		} else if (key == "log") {
			params.logPath = PathFromUtf8(value);
		} else if (key == "argument") {
			params.arguments.push_back(std::move(value));
		}
	});
	return params.installDir.is_absolute()
		&& params.stagedDir.is_absolute()
		&& params.resultPath.is_absolute()
		&& IsSafeVersion(params.version)
		&& IsSafeRelativePath(params.executable);
}

bool ReadParams(FileOps &ops, const Path &file, HelperParams &params) {
	auto text = std::string();
	return ops.readText(file, text, kParamsLimit) && ParseParams(text, params);
}

bool WriteResult(FileOps &ops, const Path &file, const HelperResult &result) {
	if (!ops.removeFile(file)) {
		return false;
	}
	// One append: the file is whole or it is not there.
	auto text = std::string();
	AddLine(text, "version", result.version);
	AddLine(text, "status", StatusName(result.status));
	AddLine(text, "error", result.error);
	text += "end=1";
	return ops.appendLine(file, text);
}

bool ReadResult(FileOps &ops, const Path &file, HelperResult &result) {
	auto text = std::string();
	if (!ops.readText(file, text, kParamsLimit)) {
		return false;
	}
	result = HelperResult();
	auto complete = false;
	auto known = false;
	ForEachPair(text, [&](const std::string &key, std::string value) {
		if (key == "version") {
			result.version = std::move(value);
		} else if (key == "status") {
			known = true;
			if (value == "installed") {
				result.status = Status::Installed;
			} else if (value == "broken") {
				result.status = Status::Broken;
			} else {
				result.status = Status::Unchanged;
			}
		} else if (key == "error") {
			result.error = std::move(value);
		} else if (key == "end") {
			complete = true;
		}
	});
	return complete && known && IsSafeVersion(result.version);
}

Log FileLog(FileOps &ops, const Path &file) {
	if (file.empty()) {
		return nullptr;
	}
	return [&ops, file](const std::string &line) {
		char stamp[32] = "";
		const auto now = std::time(nullptr);
		auto parts = std::tm();
#ifdef _WIN32
		gmtime_s(&parts, &now);
#else // _WIN32
		gmtime_r(&now, &parts);
#endif // !_WIN32
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ ", &parts);
		[[maybe_unused]] const auto written = ops.appendLine(file, stamp + line);
	};
}

HelperResult RunInstall(
		const HelperParams &params,
		FileOps &ops,
		const Log &log,
		int retries,
		int retryDelayMs) {
	auto request = Request();
	request.installDir = params.installDir;
	request.stagedDir = params.stagedDir;
	request.version = params.version;
	request.lastFile = params.executable;
	request.retries = retries;
	request.retryDelayMs = retryDelayMs;
	const auto applied = Apply(request, ops, log);

	auto result = HelperResult();
	result.version = params.version;
	result.status = applied.status;
	result.error = applied.error;
	if (!WriteResult(ops, params.resultPath, result) && log) {
		log("could not write the result file");
	}
	if (applied.status == Status::Installed) {
		// Nothing of the old version is in use any more.
		RemoveTree(ops, params.installDir / (kBackupPrefix + params.version));
		RemoveTree(ops, params.stagedDir);
	}
	return result;
}

} // namespace Install
