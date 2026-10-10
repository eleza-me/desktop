// The install step (lib/install): path rules, the swap with a failure and a
// "power cut" injected at every file operation, recovery from the journal, the
// request / result files.
#include "install/install_helper.h"
#include "install/install_swap.h"

#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

using Tree = std::map<std::string, std::string>; // relative path -> content

void WriteBytes(const fs::path &path, const std::string &content) {
	fs::create_directories(path.parent_path());
	auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
	stream << content;
}

void WriteTree(const fs::path &root, const Tree &tree) {
	fs::create_directories(root);
	for (const auto &[name, content] : tree) {
		WriteBytes(root / name, content);
	}
}

[[nodiscard]] std::string ReadBytes(const fs::path &path) {
	auto stream = std::ifstream(path, std::ios::binary);
	return std::string(
		std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>());
}

// Every file under the root; a link is recorded as "<link>", an empty
// directory as "<dir>".
[[nodiscard]] Tree Snapshot(const fs::path &root) {
	auto result = Tree();
	if (!fs::exists(root)) {
		return result;
	}
	for (auto i = fs::recursive_directory_iterator(root);
		i != fs::recursive_directory_iterator();
		++i) {
		const auto name = fs::relative(i->path(), root).generic_string();
		if (i->is_symlink()) {
			result[name] = "<link>";
			i.disable_recursion_pending();
		} else if (i->is_regular_file()) {
			result[name] = ReadBytes(i->path());
		} else if (i->is_directory() && fs::is_empty(i->path())) {
			result[name] = "<dir>";
		}
	}
	return result;
}

[[nodiscard]] QString Describe(const Tree &tree) {
	auto result = QStringList();
	for (const auto &[name, content] : tree) {
		result.push_back(QString::fromStdString(name + "=" + content.substr(0, 24)));
	}
	return result.join(QStringLiteral(", "));
}

struct Crash {
};

// The real operations with a counter: operation number `failAt` fails (once,
// or from then on), operation number `crashAt` ends the "process" (before it
// runs, or right after it did).
class FaultyOps final : public Install::FileOps {
public:
	int failAt = -1;
	bool failForever = false;
	int crashAt = -1;
	bool crashAfter = false;
	bool failStagedRenames = false; // "the staging directory is on another volume"
	fs::path stagedDir;
	int counter = 0;
	int copies = 0;

	bool makeDirectories(const Install::Path &path) override {
		return step([&] { return FileOps::makeDirectories(path); });
	}
	bool rename(const Install::Path &from, const Install::Path &to) override {
		if (failStagedRenames
			&& from.generic_string().rfind(stagedDir.generic_string(), 0) == 0) {
			return false;
		}
		return step([&] { return FileOps::rename(from, to); });
	}
	bool copyFile(const Install::Path &from, const Install::Path &to) override {
		++copies;
		return step([&] { return FileOps::copyFile(from, to); });
	}
	bool removeFile(const Install::Path &path) override {
		return step([&] { return FileOps::removeFile(path); });
	}
	bool appendLine(const Install::Path &file, const std::string &line) override {
		return step([&] { return FileOps::appendLine(file, line); });
	}
	void sleep(int) override {
	}

private:
	template <typename Operation>
	bool step(Operation &&operation) {
		const auto index = counter++;
		if (index == crashAt && !crashAfter) {
			throw Crash();
		} else if (index == failAt || (failForever && failAt >= 0 && index > failAt)) {
			return false;
		}
		const auto result = operation();
		if (index == crashAt) {
			throw Crash();
		}
		return result;
	}

};

struct Fixture {
	QTemporaryDir temp;
	fs::path install;
	fs::path staged;
	Tree before;
	Tree after;

	Fixture() {
		const auto root = fs::path(temp.path().toStdString());
		install = root / "app";
		staged = root / "data" / "updates" / "0.2.0";
		const auto oldFiles = Tree{
			{ "Eleza", "old binary" },
			{ "eleza-files.list", "Eleza\neleza-files.list\nlib/core.so\nlib/old/only-old.so\nplugins/a.so\n" },
			{ "lib/core.so", "old core" },
			{ "lib/old/only-old.so", "stale" },
			{ "plugins/a.so", "old plugin" },
		};
		const auto userFiles = Tree{
			{ "unins000.dat", "uninstaller" },
			{ "notes/mine.txt", "the user's own file" },
		};
		const auto newFiles = Tree{
			{ "Eleza", "new binary" },
			{ "eleza-files.list", "Eleza\neleza-files.list\nlib/core.so\nlib/new/added.so\nplugins/a.so\n" },
			{ "lib/core.so", "new core" },
			{ "lib/new/added.so", "added" },
			{ "plugins/a.so", "new plugin" },
		};
		WriteTree(install, oldFiles);
		WriteTree(install, userFiles);
		WriteTree(staged, newFiles);
		before = oldFiles;
		before.insert(userFiles.begin(), userFiles.end());
		after = newFiles;
		after.insert(userFiles.begin(), userFiles.end());
	}

	[[nodiscard]] Install::Request request() const {
		auto result = Install::Request();
		result.installDir = install;
		result.stagedDir = staged;
		result.version = "0.2.0";
		result.lastFile = "Eleza";
		return result;
	}

	// The installed tree without what a finished swap leaves for the caller.
	[[nodiscard]] Tree installed() const {
		auto result = Snapshot(install);
		for (auto i = result.begin(); i != result.end();) {
			if (i->first.rfind("_backup_", 0) == 0) {
				i = result.erase(i);
			} else {
				++i;
			}
		}
		return result;
	}
};

} // namespace

class TestInstallSwap final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void safePaths_data();
	void safePaths();
	void safeVersions();
	void installs();
	void installsWithoutFileList();
	void copiesFromAnotherVolume();
	void rollsBackAtEveryStep();
	void recoversFromBrokenRollback();
	void recoversFromCrashAtEveryStep();
	void journalWithTornLine();
	void refusesLinksInTheNewVersion();
	void refusesLinkedDirectoryInInstall();
	void ignoresUnsafeFileListEntries();
	void refusesWithoutExecutable();
	void paramsRoundTrip();
	void runInstallWritesResult();
	void removeLeftovers();

};

void TestInstallSwap::safePaths_data() {
	QTest::addColumn<QByteArray>("path");
	QTest::addColumn<bool>("safe");

	QTest::newRow("file") << QByteArray("Eleza.exe") << true;
	QTest::newRow("nested") << QByteArray("plugins/platforms/qwindows.dll") << true;
	QTest::newRow("unicode") << QByteArray("lang/\xD9\x81\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C.txt") << true;
	QTest::newRow("dots inside") << QByteArray("a..b/c.d.e") << true;
	QTest::newRow("empty") << QByteArray("") << false;
	QTest::newRow("absolute") << QByteArray("/etc/passwd") << false;
	QTest::newRow("parent") << QByteArray("../x") << false;
	QTest::newRow("parent inside") << QByteArray("a/../../x") << false;
	QTest::newRow("current") << QByteArray("./x") << false;
	QTest::newRow("double slash") << QByteArray("a//b") << false;
	QTest::newRow("trailing slash") << QByteArray("a/") << false;
	QTest::newRow("backslash") << QByteArray("a\\b") << false;
	QTest::newRow("backslash parent") << QByteArray("..\\x") << false;
	QTest::newRow("drive") << QByteArray("C:/x") << false;
	QTest::newRow("drive relative") << QByteArray("C:x") << false;
	QTest::newRow("stream") << QByteArray("a.txt:hidden") << false;
	QTest::newRow("unc") << QByteArray("//server/share/x") << false;
	QTest::newRow("device") << QByteArray("NUL") << false;
	QTest::newRow("device ext") << QByteArray("lib/con.txt") << false;
	QTest::newRow("device com") << QByteArray("COM1") << false;
	QTest::newRow("device lpt") << QByteArray("lpt9.log") << false;
	QTest::newRow("not a device") << QByteArray("console.txt") << true;
	QTest::newRow("not a device 2") << QByteArray("COM10") << true;
	QTest::newRow("trailing dot") << QByteArray("a.exe.") << false;
	QTest::newRow("trailing space") << QByteArray("a.exe ") << false;
	QTest::newRow("control") << QByteArray("a\nb") << false;
	QTest::newRow("zero") << QByteArray("a\0b", 3) << false;
	QTest::newRow("wildcard") << QByteArray("a*b") << false;
	QTest::newRow("bad utf8") << QByteArray("a\xFF") << false;
	QTest::newRow("overlong utf8") << QByteArray("\xC0\xAE\xC0\xAE/x") << false;
	QTest::newRow("backup dir") << QByteArray("_backup_0.2.0/x") << false;
	QTest::newRow("incoming dir") << QByteArray("_UPDATE_1/x") << false;
	QTest::newRow("journal") << QByteArray("_update_journal.txt") << false;
	QTest::newRow("backup deeper is fine") << QByteArray("lib/_backup_x") << true;
	QTest::newRow("too long component") << QByteArray(300, 'a') << false;
}

void TestInstallSwap::safePaths() {
	QFETCH(QByteArray, path);
	QFETCH(bool, safe);
	QCOMPARE(
		Install::IsSafeRelativePath(std::string_view(path.constData(), path.size())),
		safe);
}

void TestInstallSwap::safeVersions() {
	QVERIFY(Install::IsSafeVersion("0.2.0"));
	QVERIFY(Install::IsSafeVersion("12.345.6"));
	QVERIFY(!Install::IsSafeVersion(""));
	QVERIFY(!Install::IsSafeVersion(".."));
	QVERIFY(!Install::IsSafeVersion("1..2"));
	QVERIFY(!Install::IsSafeVersion("1.2/"));
	QVERIFY(!Install::IsSafeVersion("1.2.0-beta"));
	QVERIFY(!Install::IsSafeVersion("../1"));
}

void TestInstallSwap::installs() {
	const auto fixture = Fixture();
	auto ops = Install::FileOps();
	auto lines = QStringList();
	const auto result = Install::Apply(fixture.request(), ops, [&](const std::string &line) {
		lines.push_back(QString::fromStdString(line));
	});
	QCOMPARE(result.status, Install::Status::Installed);
	QVERIFY(result.error.empty());
	QCOMPARE(Describe(fixture.installed()), Describe(fixture.after));
	// The old version is kept for the caller, the journal and the incoming
	// directory are gone, the directory of the stale file was pruned.
	const auto backup = Snapshot(fixture.install / "_backup_0.2.0");
	QCOMPARE(backup.at("Eleza"), std::string("old binary"));
	QCOMPARE(backup.at("lib/old/only-old.so"), std::string("stale"));
	QVERIFY(!backup.count("unins000.dat"));
	QVERIFY(!fs::exists(fixture.install / "_update_0.2.0"));
	QVERIFY(!fs::exists(fixture.install / Install::kJournalName));
	QVERIFY(!fs::exists(fixture.install / "lib" / "old"));
	QVERIFY(!Install::HasJournal(fixture.install, ops));
	QVERIFY(lines.contains(QStringLiteral("installed 0.2.0")));
}

void TestInstallSwap::installsWithoutFileList() {
	auto fixture = Fixture();
	fs::remove(fixture.install / "eleza-files.list");
	auto ops = Install::FileOps();
	const auto result = Install::Apply(fixture.request(), ops);
	QCOMPARE(result.status, Install::Status::Installed);
	// Nothing says that the stale file was ours: it stays.
	auto expected = fixture.after;
	expected["lib/old/only-old.so"] = "stale";
	QCOMPARE(Describe(fixture.installed()), Describe(expected));
}

void TestInstallSwap::copiesFromAnotherVolume() {
	const auto fixture = Fixture();
	auto ops = FaultyOps();
	ops.failStagedRenames = true;
	ops.stagedDir = fixture.staged;
	const auto result = Install::Apply(fixture.request(), ops);
	QCOMPARE(result.status, Install::Status::Installed);
	QCOMPARE(ops.copies, 5);
	QCOMPARE(Describe(fixture.installed()), Describe(fixture.after));
}

void TestInstallSwap::rollsBackAtEveryStep() {
	auto installedRuns = 0;
	auto unchangedRuns = 0;
	for (auto step = 0;; ++step) {
		const auto fixture = Fixture();
		auto ops = FaultyOps();
		ops.failAt = step;
		const auto result = Install::Apply(fixture.request(), ops);
		if (ops.counter <= step) {
			// The run was over before this step: every step was covered.
			QCOMPARE(result.status, Install::Status::Installed);
			break;
		}
		if (result.status == Install::Status::Installed) {
			// Failed in the cleaning after the commit.
			++installedRuns;
			auto healthy = Install::FileOps();
			const auto recovery = Install::Recover(fixture.install, healthy);
			QVERIFY(recovery == Install::Recovery::NotNeeded
				|| recovery == Install::Recovery::Finished);
			Install::RemoveLeftovers(fixture.install, healthy);
			QCOMPARE(Describe(Snapshot(fixture.install)), Describe(fixture.after));
		} else {
			++unchangedRuns;
			QVERIFY2(
				result.status == Install::Status::Unchanged,
				qPrintable(QStringLiteral("step %1: %2").arg(step).arg(
					QString::fromStdString(result.error))));
			QVERIFY(!result.error.empty());
			// Exactly the old version, with nothing left behind.
			QVERIFY2(
				Snapshot(fixture.install) == fixture.before,
				qPrintable(QStringLiteral("step %1: %2").arg(step).arg(
					Describe(Snapshot(fixture.install)))));
		}
		QVERIFY(step < 500);
	}
	QVERIFY(unchangedRuns > 20);
	QVERIFY(installedRuns >= 1);
}

void TestInstallSwap::recoversFromBrokenRollback() {
	auto brokenRuns = 0;
	for (auto step = 0;; ++step) {
		const auto fixture = Fixture();
		auto ops = FaultyOps();
		ops.failAt = step;
		ops.failForever = true; // the rollback cannot move anything either
		const auto result = Install::Apply(fixture.request(), ops);
		if (ops.counter <= step) {
			break;
		}
		QVERIFY(result.status != Install::Status::Installed
			|| Snapshot(fixture.install) != fixture.before);
		if (result.status == Install::Status::Broken) {
			++brokenRuns;
			QVERIFY(fs::exists(fixture.install / Install::kJournalName));
		}
		// The next start, with a disk that works again.
		auto healthy = Install::FileOps();
		const auto recovery = Install::Recover(fixture.install, healthy);
		QVERIFY(recovery != Install::Recovery::Failed);
		Install::RemoveLeftovers(fixture.install, healthy);
		const auto now = Snapshot(fixture.install);
		if (recovery == Install::Recovery::Finished
			|| result.status == Install::Status::Installed) {
			QCOMPARE(Describe(now), Describe(fixture.after));
		} else {
			QVERIFY2(
				now == fixture.before,
				qPrintable(QStringLiteral("step %1: %2").arg(step).arg(Describe(now))));
		}
		QVERIFY(step < 500);
	}
	QVERIFY(brokenRuns > 5);
}

void TestInstallSwap::recoversFromCrashAtEveryStep() {
	for (const auto after : { false, true }) {
		auto rolledBack = 0;
		auto finished = 0;
		for (auto step = 0;; ++step) {
			const auto fixture = Fixture();
			auto ops = FaultyOps();
			ops.crashAt = step;
			ops.crashAfter = after;
			auto crashed = false;
			try {
				[[maybe_unused]] const auto result = Install::Apply(
					fixture.request(),
					ops);
			} catch (const Crash &) {
				crashed = true;
			}
			if (!crashed) {
				break;
			}
			// Whatever the moment: at most one file of the app is missing
			// (the one that was being swapped), and every file that is there
			// is whole — the old or the new one.
			auto missing = 0;
			for (const auto &[name, content] : fixture.before) {
				const auto path = fixture.install / name;
				if (!fixture.after.count(name)) {
					continue; // leaves with the old version
				} else if (!fs::exists(path)) {
					++missing;
					continue;
				}
				const auto now = ReadBytes(path);
				const auto fresh = fixture.after.at(name);
				QVERIFY2(
					now == content || now == fresh,
					qPrintable(QStringLiteral("step %1: %2").arg(step).arg(
						QString::fromStdString(name))));
			}
			QVERIFY2(missing <= 1, qPrintable(QStringLiteral("step %1").arg(step)));

			// The next start.
			auto healthy = Install::FileOps();
			const auto recovery = Install::Recover(fixture.install, healthy);
			QVERIFY(recovery != Install::Recovery::Failed);
			// Running it twice changes nothing.
			QCOMPARE(
				Install::Recover(fixture.install, healthy),
				Install::Recovery::NotNeeded);
			Install::RemoveLeftovers(fixture.install, healthy);
			const auto now = Snapshot(fixture.install);
			if (recovery == Install::Recovery::Finished) {
				++finished;
				QCOMPARE(Describe(now), Describe(fixture.after));
			} else if (now != fixture.before) {
				// Only a crash after the journal was removed (the swap was
				// complete) may leave the new version without a journal.
				QCOMPARE(recovery, Install::Recovery::NotNeeded);
				QCOMPARE(Describe(now), Describe(fixture.after));
				++finished;
			} else {
				++rolledBack;
			}
			QVERIFY(step < 500);
		}
		QVERIFY(rolledBack > 20);
		QVERIFY(finished >= 1);
	}
}

void TestInstallSwap::journalWithTornLine() {
	// "out" was announced and made, the "in" line was cut by the power loss:
	// the file is in the backup and must come back.
	const auto fixture = Fixture();
	auto ops = Install::FileOps();
	const auto backup = fixture.install / "_backup_0.2.0";
	fs::create_directories(backup / "lib");
	fs::rename(fixture.install / "lib" / "core.so", backup / "lib" / "core.so");
	WriteBytes(
		fixture.install / Install::kJournalName,
		"eleza-update-journal 1\nversion 0.2.0\nout lib/core.so\nin lib/co");
	QCOMPARE(
		Install::Recover(fixture.install, ops),
		Install::Recovery::RolledBack);
	QVERIFY(Snapshot(fixture.install) == fixture.before);

	// Only the header: nothing was moved.
	WriteBytes(fixture.install / Install::kJournalName, "eleza-update-jour");
	QCOMPARE(Install::Recover(fixture.install, ops), Install::Recovery::NotNeeded);
	QVERIFY(Snapshot(fixture.install) == fixture.before);

	// Entries that we could not have written are not followed.
	WriteBytes(
		fixture.install / Install::kJournalName,
		"eleza-update-journal 1\nversion 0.2.0\nin ../../outside\nout /etc/passwd\n");
	QCOMPARE(Install::Recover(fixture.install, ops), Install::Recovery::RolledBack);
	QVERIFY(Snapshot(fixture.install) == fixture.before);

	// A version that would leave the directory is not a journal of ours.
	WriteBytes(
		fixture.install / Install::kJournalName,
		"eleza-update-journal 1\nversion ../../x\nout Eleza\n");
	QCOMPARE(Install::Recover(fixture.install, ops), Install::Recovery::NotNeeded);
	QVERIFY(Snapshot(fixture.install) == fixture.before);
}

void TestInstallSwap::refusesLinksInTheNewVersion() {
#ifdef Q_OS_WIN
	QSKIP("Creating links needs a privilege on Windows.");
#else // Q_OS_WIN
	const auto fixture = Fixture();
	const auto outside = fs::path(fixture.temp.path().toStdString()) / "outside.txt";
	WriteBytes(outside, "outside");
	fs::create_symlink(outside, fixture.staged / "lib" / "link.so");
	auto ops = Install::FileOps();
	const auto result = Install::Apply(fixture.request(), ops);
	QCOMPARE(result.status, Install::Status::Unchanged);
	QVERIFY(Snapshot(fixture.install) == fixture.before);
	QCOMPARE(ReadBytes(outside), std::string("outside"));
#endif // !Q_OS_WIN
}

void TestInstallSwap::refusesLinkedDirectoryInInstall() {
#ifdef Q_OS_WIN
	QSKIP("Creating links needs a privilege on Windows.");
#else // Q_OS_WIN
	// <install>/plugins is a link to a directory elsewhere: nothing may be
	// moved out of or into that other directory.
	const auto fixture = Fixture();
	const auto outside = fs::path(fixture.temp.path().toStdString()) / "elsewhere";
	WriteTree(outside, { { "a.so", "not ours" } });
	fs::remove_all(fixture.install / "plugins");
	fs::create_directory_symlink(outside, fixture.install / "plugins");
	const auto was = Snapshot(fixture.install);
	auto ops = Install::FileOps();
	const auto result = Install::Apply(fixture.request(), ops);
	QCOMPARE(result.status, Install::Status::Unchanged);
	QVERIFY(Snapshot(fixture.install) == was);
	QVERIFY(Snapshot(outside) == Tree({ { "a.so", "not ours" } }));

	// The same for a stale entry of the old list: skipped, not followed.
	const auto second = Fixture();
	fs::remove_all(second.install / "lib" / "old");
	fs::create_directory_symlink(outside, second.install / "lib" / "old");
	WriteBytes(outside / "only-old.so", "not ours either");
	const auto applied = Install::Apply(second.request(), ops);
	QCOMPARE(applied.status, Install::Status::Installed);
	QCOMPARE(ReadBytes(outside / "only-old.so"), std::string("not ours either"));
#endif // !Q_OS_WIN
}

void TestInstallSwap::ignoresUnsafeFileListEntries() {
	auto fixture = Fixture();
	const auto root = fs::path(fixture.temp.path().toStdString());
	WriteBytes(root / "victim.txt", "victim");
	WriteBytes(
		fixture.install / "eleza-files.list",
		"../victim.txt\n/etc/hostname\nC:\\x\nnotes/mine.txt\r\n# comment\n\nEleza\n");
	auto ops = Install::FileOps();
	const auto result = Install::Apply(fixture.request(), ops);
	QCOMPARE(result.status, Install::Status::Installed);
	QCOMPARE(ReadBytes(root / "victim.txt"), std::string("victim"));
	// A file the old list names is the app's: it went away with the version.
	QVERIFY(!fs::exists(fixture.install / "notes" / "mine.txt"));
	QVERIFY(fs::exists(fixture.install / "_backup_0.2.0" / "notes" / "mine.txt"));

	const auto parsed = Install::ParseFileList("a\n../b\nc/d\r\n#e\n\n");
	QCOMPARE(parsed.size(), std::size_t(2));
	QCOMPARE(parsed[0], std::string("a"));
	QCOMPARE(parsed[1], std::string("c/d"));
}

void TestInstallSwap::refusesWithoutExecutable() {
	const auto fixture = Fixture();
	fs::remove(fixture.staged / "Eleza");
	auto ops = Install::FileOps();
	const auto result = Install::Apply(fixture.request(), ops);
	QCOMPARE(result.status, Install::Status::Unchanged);
	QVERIFY(Snapshot(fixture.install) == fixture.before);

	auto request = fixture.request();
	request.version = "../0.2.0";
	QCOMPARE(Install::Apply(request, ops).status, Install::Status::Unchanged);
	QVERIFY(Snapshot(fixture.install) == fixture.before);
}

void TestInstallSwap::paramsRoundTrip() {
	const auto fixture = Fixture();
	auto params = Install::HelperParams();
	params.parentPid = 4242;
	params.installDir = fixture.install;
	params.stagedDir = fixture.staged;
	params.version = "0.2.0";
	params.executable = "Eleza";
	params.resultPath = fixture.staged.parent_path() / "install.result";
	params.logPath = fixture.staged.parent_path() / "install.log";
	params.arguments = {
		"--workdir=/tmp/with space",
		"--note=a\\b\nsecond line",
		"--lang=fa",
		"",
	};
	auto parsed = Install::HelperParams();
	QVERIFY(Install::ParseParams(Install::SerializeParams(params), parsed));
	QCOMPARE(parsed.parentPid, 4242UL);
	QVERIFY(parsed.installDir == params.installDir);
	QVERIFY(parsed.stagedDir == params.stagedDir);
	QVERIFY(parsed.resultPath == params.resultPath);
	QVERIFY(parsed.logPath == params.logPath);
	QCOMPARE(parsed.version, params.version);
	QCOMPARE(parsed.executable, params.executable);
	QVERIFY(parsed.arguments == params.arguments);

	// A request that points outside or has no sane version is refused.
	auto bad = params;
	bad.executable = "../Eleza";
	QVERIFY(!Install::ParseParams(Install::SerializeParams(bad), parsed));
	bad = params;
	bad.version = "0.2.0/..";
	QVERIFY(!Install::ParseParams(Install::SerializeParams(bad), parsed));
	bad = params;
	bad.installDir = "relative/dir";
	QVERIFY(!Install::ParseParams(Install::SerializeParams(bad), parsed));
	QVERIFY(!Install::ParseParams("", parsed));
}

void TestInstallSwap::runInstallWritesResult() {
	const auto fixture = Fixture();
	auto params = Install::HelperParams();
	params.installDir = fixture.install;
	params.stagedDir = fixture.staged;
	params.version = "0.2.0";
	params.executable = "Eleza";
	params.resultPath = fixture.staged.parent_path() / "install.result";
	params.logPath = fixture.staged.parent_path() / "install.log";

	auto ops = Install::FileOps();
	const auto log = Install::FileLog(ops, params.logPath);
	const auto result = Install::RunInstall(params, ops, log, 0);
	QCOMPARE(result.status, Install::Status::Installed);
	// Nothing but the new version is left.
	QCOMPARE(Describe(Snapshot(fixture.install)), Describe(fixture.after));
	QVERIFY(!fs::exists(fixture.staged));
	auto read = Install::HelperResult();
	QVERIFY(Install::ReadResult(ops, params.resultPath, read));
	QCOMPARE(read.status, Install::Status::Installed);
	QCOMPARE(read.version, std::string("0.2.0"));
	QVERIFY(ReadBytes(params.logPath).find("installed 0.2.0") != std::string::npos);

	// A failure is reported with its reason.
	const auto second = Fixture();
	params.installDir = second.install;
	params.stagedDir = second.staged;
	params.resultPath = second.staged.parent_path() / "install.result";
	params.logPath = Install::Path();
	fs::remove(second.staged / "Eleza");
	const auto failed = Install::RunInstall(params, ops, nullptr, 0);
	QCOMPARE(failed.status, Install::Status::Unchanged);
	QVERIFY(Install::ReadResult(ops, params.resultPath, read));
	QCOMPARE(read.status, Install::Status::Unchanged);
	QVERIFY(!read.error.empty());
	QVERIFY(Snapshot(second.install) == second.before);

	// A result file that was cut is not a result.
	WriteBytes(params.resultPath, "version=0.2.0\nstatus=installed\n");
	QVERIFY(!Install::ReadResult(ops, params.resultPath, read));
}

void TestInstallSwap::removeLeftovers() {
	const auto fixture = Fixture();
	WriteTree(fixture.install / "_backup_0.1.5", { { "Eleza", "older" } });
	WriteTree(fixture.install / "_update_0.1.9", { { "Eleza", "half" } });
	WriteTree(fixture.install / "_backup_notes", { { "keep.txt", "the user's" } });
	auto ops = Install::FileOps();
	Install::RemoveLeftovers(fixture.install, ops);
	QVERIFY(!fs::exists(fixture.install / "_backup_0.1.5"));
	QVERIFY(!fs::exists(fixture.install / "_update_0.1.9"));
	QVERIFY(fs::exists(fixture.install / "_backup_notes" / "keep.txt"));
}

QTEST_GUILESS_MAIN(TestInstallSwap)
#include "test_install_swap.moc"
