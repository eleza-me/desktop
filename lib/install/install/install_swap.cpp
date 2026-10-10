#include "install/install_swap.h"

#include <algorithm>
#include <set>

namespace Install {
namespace {

constexpr auto kJournalHeader = "eleza-update-journal 1";
constexpr auto kFileListLimit = std::size_t(4) << 20;
constexpr auto kJournalLimit = std::size_t(16) << 20;
constexpr auto kMaxFiles = std::size_t(50000);
constexpr auto kMaxDepth = 32;

struct JournalEntry {
	bool in = false; // false: an old file moved out
	std::string path;
};

struct Journal {
	bool valid = false; // the header and the version are there
	bool committed = false;
	std::string version;
	std::vector<JournalEntry> entries;
};

void Write(const Log &log, const std::string &line) {
	if (log) {
		log(line);
	}
}

[[nodiscard]] std::vector<std::string> SplitLines(const std::string &text) {
	auto result = std::vector<std::string>();
	auto from = std::size_t(0);
	while (from < text.size()) {
		const auto end = text.find('\n', from);
		if (end == std::string::npos) {
			// A line without its end was being written when the power
			// went: what it announced has not been done.
			break;
		}
		auto line = text.substr(from, end - from);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		result.push_back(std::move(line));
		from = end + 1;
	}
	return result;
}

[[nodiscard]] Journal ParseJournal(const std::string &text) {
	auto result = Journal();
	const auto lines = SplitLines(text);
	if (lines.size() < 2 || lines[0] != kJournalHeader) {
		return result;
	}
	const auto versionPrefix = std::string("version ");
	if (lines[1].compare(0, versionPrefix.size(), versionPrefix) != 0) {
		return result;
	}
	result.version = lines[1].substr(versionPrefix.size());
	if (!IsSafeVersion(result.version)) {
		return result;
	}
	result.valid = true;
	for (auto i = std::size_t(2); i < lines.size(); ++i) {
		const auto &line = lines[i];
		if (line == "commit") {
			result.committed = true;
		} else if (line.compare(0, 4, "out ") == 0) {
			auto path = line.substr(4);
			if (IsSafeRelativePath(path)) {
				result.entries.push_back({ false, std::move(path) });
			}
		} else if (line.compare(0, 3, "in ") == 0) {
			auto path = line.substr(3);
			if (IsSafeRelativePath(path)) {
				result.entries.push_back({ true, std::move(path) });
			}
		}
	}
	return result;
}

[[nodiscard]] bool RenameWithRetries(
		FileOps &ops,
		const Path &from,
		const Path &to,
		int retries,
		int delayMs) {
	for (auto attempt = 0;; ++attempt) {
		if (ops.rename(from, to)) {
			return true;
		} else if (attempt >= retries) {
			return false;
		}
		ops.sleep(delayMs);
	}
}

// Every directory between `root` and the file is a real directory (or is not
// there yet): nothing we touch is reached through a link.
[[nodiscard]] bool ParentsAreDirectories(
		FileOps &ops,
		const Path &root,
		const std::string &relative) {
	auto current = root;
	auto from = std::size_t(0);
	for (;;) {
		const auto slash = relative.find('/', from);
		if (slash == std::string::npos) {
			return true;
		}
		current /= PathFromUtf8(relative.substr(from, slash - from));
		switch (ops.kind(current)) {
		case EntryKind::Directory: break;
		case EntryKind::Missing: return true;
		default: return false;
		}
		from = slash + 1;
	}
}

// Removes the directories above `relative` (inside `root`) that became empty.
void PruneEmptyParents(
		FileOps &ops,
		const Path &root,
		const std::string &relative) {
	auto path = relative;
	for (;;) {
		const auto slash = path.rfind('/');
		if (slash == std::string::npos) {
			return;
		}
		path.resize(slash);
		if (!ops.removeEmptyDirectory(root / PathFromUtf8(path))) {
			return;
		}
	}
}

// The regular files under `directory`, as sorted relative paths. False when
// something else is there (a link, a device), a name is not safe or the tree
// is absurdly large.
[[nodiscard]] bool CollectFiles(
		FileOps &ops,
		const Path &directory,
		const std::string &prefix,
		int depth,
		std::vector<std::string> &files,
		std::string &error) {
	if (depth > kMaxDepth) {
		error = "the new version is nested too deeply";
		return false;
	}
	auto names = std::vector<std::string>();
	if (!ops.list(directory, names)) {
		error = "could not list the new version";
		return false;
	}
	std::sort(names.begin(), names.end());
	for (const auto &name : names) {
		const auto relative = prefix.empty() ? name : (prefix + '/' + name);
		if (!IsSafeRelativePath(relative)) {
			error = "the new version has a file with an unsafe name";
			return false;
		}
		const auto path = directory / PathFromUtf8(name);
		switch (ops.kind(path)) {
		case EntryKind::File:
			if (files.size() >= kMaxFiles) {
				error = "the new version has too many files";
				return false;
			}
			files.push_back(relative);
			break;
		case EntryKind::Directory:
			if (!CollectFiles(ops, path, relative, depth + 1, files, error)) {
				return false;
			}
			break;
		default:
			error = "the new version has something that is not a file";
			return false;
		}
	}
	return true;
}

// Moves everything the journal names back. True when the old version is
// whole again.
[[nodiscard]] bool Undo(
		FileOps &ops,
		const Path &installDir,
		const Journal &journal,
		const Log &log,
		int retries,
		int delayMs) {
	const auto backup = installDir / (kBackupPrefix + journal.version);
	const auto incoming = installDir / (kIncomingPrefix + journal.version);
	auto ok = true;
	for (auto i = journal.entries.size(); i-- > 0;) {
		const auto &entry = journal.entries[i];
		const auto relative = PathFromUtf8(entry.path);
		const auto installed = installDir / relative;
		if (entry.in) {
			// Moved in when the new file is no longer where it waited.
			const auto waiting = incoming / relative;
			if (ops.kind(waiting) != EntryKind::Missing
				|| ops.kind(installed) == EntryKind::Missing) {
				continue;
			}
			if (!ops.makeDirectories(waiting.parent_path())
				|| !RenameWithRetries(ops, installed, waiting, retries, delayMs)) {
				Write(log, "undo: could not take out " + entry.path);
				ok = false;
			}
		} else {
			const auto saved = backup / relative;
			if (ops.kind(saved) == EntryKind::Missing) {
				continue; // was never moved out
			}
			if (ops.kind(installed) != EntryKind::Missing
				|| !ops.makeDirectories(installed.parent_path())
				|| !RenameWithRetries(ops, saved, installed, retries, delayMs)) {
				Write(log, "undo: could not put back " + entry.path);
				ok = false;
			}
		}
	}
	if (!ok) {
		return false;
	}
	// Directories that were made for files only the new version has.
	for (const auto &entry : journal.entries) {
		if (entry.in) {
			PruneEmptyParents(ops, installDir, entry.path);
		}
	}
	// The journal goes first: a run that stops here must not be undone a
	// second time over directories that are half removed already (what is
	// left of them is swept by RemoveLeftovers).
	if (!ops.removeFile(installDir / kJournalName)) {
		return false;
	}
	RemoveTree(ops, incoming);
	RemoveTree(ops, backup);
	return true;
}

} // namespace

std::vector<std::string> ParseFileList(const std::string &text) {
	auto result = std::vector<std::string>();
	auto from = std::size_t(0);
	while (from < text.size() && result.size() < kMaxFiles) {
		auto end = text.find('\n', from);
		if (end == std::string::npos) {
			end = text.size();
		}
		auto line = text.substr(from, end - from);
		from = end + 1;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!line.empty() && line.front() != '#' && IsSafeRelativePath(line)) {
			result.push_back(std::move(line));
		}
	}
	return result;
}

bool HasJournal(const Path &installDir, FileOps &ops) {
	return ops.kind(installDir / kJournalName) != EntryKind::Missing;
}

Recovery Recover(
		const Path &installDir,
		FileOps &ops,
		const Log &log,
		int retries,
		int retryDelayMs) {
	const auto journalPath = installDir / kJournalName;
	if (ops.kind(journalPath) == EntryKind::Missing) {
		return Recovery::NotNeeded;
	}
	auto text = std::string();
	if (!ops.readText(journalPath, text, kJournalLimit)) {
		Write(log, "recover: the journal cannot be read");
		return Recovery::Failed;
	}
	const auto journal = ParseJournal(text);
	if (!journal.valid) {
		// Written before the first move: nothing has been touched.
		Write(log, "recover: an empty journal, nothing to undo");
		return ops.removeFile(journalPath)
			? Recovery::NotNeeded
			: Recovery::Failed;
	}
	if (journal.committed) {
		Write(log, "recover: version " + journal.version + " was installed, cleaning up");
		RemoveTree(ops, installDir / (kIncomingPrefix + journal.version));
		// Directories that only held files the new version does not have.
		for (const auto &entry : journal.entries) {
			if (!entry.in) {
				PruneEmptyParents(ops, installDir, entry.path);
			}
		}
		return ops.removeFile(journalPath)
			? Recovery::Finished
			: Recovery::Failed;
	}
	Write(log, "recover: undoing the unfinished install of " + journal.version);
	return Undo(ops, installDir, journal, log, retries, retryDelayMs)
		? Recovery::RolledBack
		: Recovery::Failed;
}

Result Apply(const Request &request, FileOps &ops, const Log &log) {
	const auto fail = [&](std::string error) {
		Write(log, "failed: " + error);
		return Result{ Status::Unchanged, std::move(error) };
	};
	const auto &installDir = request.installDir;
	if (!IsSafeVersion(request.version)) {
		return fail("the version is not valid");
	} else if (ops.kind(installDir) != EntryKind::Directory) {
		return fail("the install directory is not a directory");
	} else if (ops.kind(request.stagedDir) != EntryKind::Directory) {
		return fail("the new version is not there");
	} else if (!request.lastFile.empty()
		&& !IsSafeRelativePath(request.lastFile)) {
		return fail("the executable name is not valid");
	}

	// ---- 1. an earlier swap that did not finish
	switch (Recover(installDir, ops, log, request.retries, request.retryDelayMs)) {
	case Recovery::Failed:
		return Result{
			Status::Broken,
			"an earlier update could not be undone",
		};
	default: break;
	}

	// ---- 2. what is new, what is old
	auto fresh = std::vector<std::string>();
	auto error = std::string();
	if (!CollectFiles(ops, request.stagedDir, std::string(), 0, fresh, error)) {
		return fail(error);
	} else if (fresh.empty()) {
		return fail("the new version is empty");
	}
	const auto freshSet = std::set<std::string>(fresh.begin(), fresh.end());
	if (!request.lastFile.empty() && !freshSet.count(request.lastFile)) {
		return fail("the new version has no executable");
	}
	for (const auto &relative : fresh) {
		if (!ParentsAreDirectories(ops, installDir, relative)) {
			return fail("a directory of the installed copy is a link: " + relative);
		} else if (ops.kind(installDir / PathFromUtf8(relative))
			== EntryKind::Directory) {
			return fail("a directory is where a file must go: " + relative);
		}
	}
	// Files of the old version the new one does not have.
	auto stale = std::vector<std::string>();
	{
		auto text = std::string();
		if (ops.readText(installDir / kFileListName, text, kFileListLimit)) {
			for (auto &relative : ParseFileList(text)) {
				if (freshSet.count(relative)
					|| !ParentsAreDirectories(ops, installDir, relative)) {
					continue;
				}
				const auto kind = ops.kind(installDir / PathFromUtf8(relative));
				if (kind == EntryKind::File || kind == EntryKind::Other) {
					stale.push_back(std::move(relative));
				}
			}
		}
	}
	std::sort(stale.begin(), stale.end());
	stale.erase(std::unique(stale.begin(), stale.end()), stale.end());

	// ---- 3. the new files come to the volume of the install directory
	const auto backup = installDir / (kBackupPrefix + request.version);
	const auto incoming = installDir / (kIncomingPrefix + request.version);
	if (!RemoveTree(ops, incoming) || !RemoveTree(ops, backup)) {
		return fail("could not clear what an earlier update left");
	} else if (!ops.makeDirectories(incoming)) {
		return fail("the install directory is not writable");
	}
	for (const auto &relative : fresh) {
		const auto path = PathFromUtf8(relative);
		const auto from = request.stagedDir / path;
		const auto to = incoming / path;
		if (!ops.makeDirectories(to.parent_path())
			|| (!ops.rename(from, to) && !ops.copyFile(from, to))) {
			RemoveTree(ops, incoming);
			return fail("could not copy the new version: " + relative);
		}
	}

	// ---- 4. the swap
	const auto journalPath = installDir / kJournalName;
	auto journal = Journal();
	journal.valid = true;
	journal.version = request.version;
	if (!ops.appendLine(journalPath, kJournalHeader)
		|| !ops.appendLine(journalPath, "version " + request.version)) {
		[[maybe_unused]] const auto removed = ops.removeFile(journalPath);
		RemoveTree(ops, incoming);
		return fail("could not start the journal");
	}
	const auto moveOut = [&](const std::string &relative) {
		const auto path = PathFromUtf8(relative);
		const auto to = backup / path;
		journal.entries.push_back({ false, relative });
		return ops.appendLine(journalPath, "out " + relative)
			&& ops.makeDirectories(to.parent_path())
			&& RenameWithRetries(
				ops,
				installDir / path,
				to,
				request.retries,
				request.retryDelayMs);
	};
	const auto moveIn = [&](const std::string &relative) {
		const auto path = PathFromUtf8(relative);
		const auto to = installDir / path;
		journal.entries.push_back({ true, relative });
		return ops.appendLine(journalPath, "in " + relative)
			&& ops.makeDirectories(to.parent_path())
			&& RenameWithRetries(
				ops,
				incoming / path,
				to,
				request.retries,
				request.retryDelayMs);
	};
	const auto swapOne = [&](const std::string &relative) {
		const auto present = ops.kind(installDir / PathFromUtf8(relative));
		return (present == EntryKind::Missing || moveOut(relative))
			&& moveIn(relative);
	};
	auto failedAt = std::string();
	const auto run = [&] {
		for (const auto &relative : fresh) {
			if (relative != request.lastFile && !swapOne(relative)) {
				failedAt = relative;
				return false;
			}
		}
		for (const auto &relative : stale) {
			if (!moveOut(relative)) {
				failedAt = relative;
				return false;
			}
		}
		if (!request.lastFile.empty() && !swapOne(request.lastFile)) {
			failedAt = request.lastFile;
			return false;
		}
		if (!ops.appendLine(journalPath, "commit")) {
			failedAt = "the journal";
			return false;
		}
		return true;
	};
	if (!run()) {
		Write(log, "could not replace " + failedAt + ", moving everything back");
		if (Undo(
				ops,
				installDir,
				journal,
				log,
				request.retries,
				request.retryDelayMs)) {
			return fail("could not replace a file: " + failedAt);
		}
		Write(log, "the old version could not be put back");
		return Result{
			Status::Broken,
			"could not replace a file and could not undo: " + failedAt,
		};
	}

	// ---- 5. committed: the rest is cleaning
	RemoveTree(ops, incoming);
	for (const auto &relative : stale) {
		PruneEmptyParents(ops, installDir, relative);
	}
	[[maybe_unused]] const auto removed = ops.removeFile(journalPath);
	Write(log, "installed " + request.version);
	return Result{ Status::Installed, std::string() };
}

void RemoveLeftovers(const Path &installDir, FileOps &ops) {
	if (HasJournal(installDir, ops)) {
		return;
	}
	auto names = std::vector<std::string>();
	if (!ops.list(installDir, names)) {
		return;
	}
	const auto backup = std::string(kBackupPrefix);
	const auto incoming = std::string(kIncomingPrefix);
	for (const auto &name : names) {
		const auto ours = (name.compare(0, backup.size(), backup) == 0
				&& IsSafeVersion(name.substr(backup.size())))
			|| (name.compare(0, incoming.size(), incoming) == 0
				&& IsSafeVersion(name.substr(incoming.size())));
		if (ours) {
			RemoveTree(ops, installDir / PathFromUtf8(name));
		}
	}
}

} // namespace Install
