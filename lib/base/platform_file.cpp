#include "base/platform_file.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTemporaryDir>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif // !Q_OS_WIN

namespace base::platform {
namespace {

QMutex PrivateTempMutex;
QString PrivateTempRoot;
QString PrivateTempResolved;
std::unique_ptr<QTemporaryDir> PrivateTempFallback;

// A real directory (not a link), ours, closed to everybody else.
[[nodiscard]] bool MakePrivateDirectory(const QString &path) {
	if (!QDir().mkpath(path)) {
		return false;
	}
#ifndef Q_OS_WIN
	const auto native = QFile::encodeName(path);
	struct stat info;
	if (::lstat(native.constData(), &info) != 0
		|| !S_ISDIR(info.st_mode)
		|| info.st_uid != ::geteuid()) {
		return false;
	}
	if ((info.st_mode & 0777) != 0700 && ::chmod(native.constData(), 0700) != 0) {
		return false;
	}
#endif // !Q_OS_WIN
	return true;
}

} // namespace

bool EnsureDirectory(const QString &path, bool ownerOnly) {
	if (!QDir().mkpath(path)) {
		return false;
	}
#ifndef Q_OS_WIN
	if (ownerOnly) {
		QFile::setPermissions(
			path,
			QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
	}
#else // !Q_OS_WIN
	Q_UNUSED(ownerOnly);
#endif // Q_OS_WIN
	return true;
}

bool EnsurePrivateDirectory(const QString &path) {
	return MakePrivateDirectory(path);
}

bool WriteFileAtomic(const QString &path, const QByteArray &bytes, bool ownerOnly) {
	if (!EnsureDirectory(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
#ifndef Q_OS_WIN
	if (ownerOnly) {
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	}
#endif // !Q_OS_WIN
	if (file.write(bytes) != bytes.size()) {
		file.cancelWriting();
		return false;
	}
	if (!file.commit()) {
		return false;
	}
#ifndef Q_OS_WIN
	if (ownerOnly) {
		QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	}
#else // !Q_OS_WIN
	Q_UNUSED(ownerOnly);
#endif // Q_OS_WIN
	return true;
}

std::optional<QByteArray> ReadFile(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	auto result = file.readAll();
	if (result.isNull()) {
		result = QByteArray("", 0);
	}
	return result;
}

bool RemoveRecursively(const QString &path) {
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		return true;
	}
	return QDir(path).removeRecursively();
}

void SetPrivateTempRoot(const QString &path) {
	const auto locker = QMutexLocker(&PrivateTempMutex);
	PrivateTempRoot = path;
	PrivateTempResolved = QString();
}

QString PrivateTempDirectory() {
	const auto locker = QMutexLocker(&PrivateTempMutex);
	if (!PrivateTempResolved.isEmpty() && QFileInfo(PrivateTempResolved).isDir()) {
		return PrivateTempResolved;
	}
	auto candidates = QStringList();
	if (!PrivateTempRoot.isEmpty()) {
		candidates.push_back(PrivateTempRoot);
	}
	const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
	if (!runtime.isEmpty()) {
		candidates.push_back(QDir(runtime).filePath(u"eleza-desktop-tmp"_s));
	}
	for (const auto &candidate : candidates) {
		if (MakePrivateDirectory(candidate)) {
			PrivateTempResolved = QDir(candidate).absolutePath();
			return PrivateTempResolved;
		}
	}
	// A freshly made directory with a random name (0700), removed on exit.
	if (!PrivateTempFallback || !PrivateTempFallback->isValid()) {
		PrivateTempFallback = std::make_unique<QTemporaryDir>(
			QDir(QDir::tempPath()).filePath(u"eleza-XXXXXX"_s));
	}
	PrivateTempResolved = PrivateTempFallback->isValid()
		? PrivateTempFallback->path()
		: QString();
	return PrivateTempResolved;
}

void ClearPrivateTemp() {
	const auto directory = PrivateTempDirectory();
	if (directory.isEmpty()) {
		return;
	}
	const auto entries = QDir(directory).entryInfoList(
		QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	for (const auto &entry : entries) {
		if (entry.isDir() && !entry.isSymLink()) {
			QDir(entry.absoluteFilePath()).removeRecursively();
		} else {
			QFile::remove(entry.absoluteFilePath());
		}
	}
}

bool OpensAsProgram(const QString &path) {
	// What the system would run, install or interpret when the file is
	// "opened", on either platform (the list is not per OS: a file saved here
	// may be opened from a shared folder elsewhere).
	static const auto kExtensions = QSet<QString>{
		// Windows programs, installers, shortcuts, shell and script hosts.
		u"exe"_s, u"bat"_s, u"cmd"_s, u"com"_s, u"scr"_s, u"msi"_s,
		u"msp"_s, u"mst"_s, u"msix"_s, u"msixbundle"_s, u"appx"_s,
		u"appxbundle"_s, u"appinstaller"_s, u"application"_s, u"appref-ms"_s,
		u"ps1"_s, u"ps1xml"_s, u"ps2"_s, u"psm1"_s, u"psd1"_s, u"psc1"_s,
		u"vbs"_s, u"vbe"_s, u"vb"_s, u"js"_s, u"jse"_s, u"wsf"_s, u"wsh"_s,
		u"wsc"_s, u"sct"_s, u"hta"_s, u"jar"_s, u"jnlp"_s, u"lnk"_s,
		u"url"_s, u"scf"_s, u"reg"_s, u"cpl"_s, u"pif"_s, u"msc"_s,
		u"gadget"_s, u"inf"_s, u"ins"_s, u"isp"_s, u"chm"_s, u"hlp"_s,
		u"dll"_s, u"ocx"_s, u"sys"_s, u"drv"_s, u"library-ms"_s,
		u"search-ms"_s, u"searchconnector-ms"_s, u"settingcontent-ms"_s,
		u"diagcab"_s, u"diagcfg"_s, u"diagpkg"_s, u"website"_s, u"xll"_s,
		u"wll"_s, u"xbap"_s, u"iso"_s, u"img"_s, u"vhd"_s, u"vhdx"_s,
		u"theme"_s, u"themepack"_s, u"msu"_s, u"mht"_s, u"mhtml"_s,
		// Office documents that carry macros.
		u"docm"_s, u"dotm"_s, u"xlsm"_s, u"xltm"_s, u"xlam"_s, u"pptm"_s,
		u"potm"_s, u"ppam"_s, u"ppsm"_s, u"sldm"_s,
		// Unix programs, packages, launchers, scripts.
		u"sh"_s, u"bash"_s, u"zsh"_s, u"csh"_s, u"ksh"_s, u"fish"_s,
		u"run"_s, u"bin"_s, u"elf"_s, u"out"_s, u"so"_s, u"desktop"_s,
		u"appimage"_s, u"deb"_s, u"rpm"_s, u"snap"_s, u"flatpak"_s,
		u"flatpakref"_s, u"flatpakrepo"_s, u"service"_s, u"py"_s, u"pyc"_s,
		u"pyw"_s, u"pyz"_s, u"pl"_s, u"pm"_s, u"rb"_s, u"php"_s, u"lua"_s,
		u"tcl"_s, u"awk"_s, u"jsx"_s, u"mjs"_s, u"cjs"_s,
		// macOS.
		u"app"_s, u"command"_s, u"dmg"_s, u"pkg"_s, u"mpkg"_s, u"action"_s,
		u"workflow"_s, u"scpt"_s, u"scptd"_s, u"terminal"_s, u"webloc"_s,
		u"inetloc"_s, u"fileloc"_s,
		// Pages a browser opens with access to local files.
		u"htm"_s, u"html"_s, u"xhtml"_s, u"xht"_s, u"shtml"_s, u"svg"_s,
		u"svgz"_s, u"xml"_s, u"xsl"_s, u"xslt"_s,
	};
	// As the file system sees the name: Windows ignores trailing dots and
	// spaces ("x.exe." is "x.exe") and "x.exe:stream" names the same file;
	// elsewhere "a.txt:x.exe" is simply a file that ends in ".exe". Either
	// reading makes it a program.
	const auto byExtension = [](QString name) {
		while (name.endsWith(u'.') || name.endsWith(u' ')) {
			name.chop(1);
		}
		const auto dot = name.lastIndexOf(u'.');
		return (dot >= 0)
			&& kExtensions.contains(name.mid(dot + 1).trimmed().toLower());
	};
	const auto name = QFileInfo(path).fileName();
	const auto colon = name.indexOf(u':');
	if (byExtension(name) || (colon >= 0 && byExtension(name.left(colon)))) {
		return true;
	}
#ifndef Q_OS_WIN
	if (!name.contains(u'.')) {
		// No extension: a program if the file system says it may be run.
		const auto info = QFileInfo(path);
		return info.isFile() && info.isExecutable();
	}
#endif // !Q_OS_WIN
	return false;
}

QString SafeFileName(const QString &name, const QString &fallback) {
	static const auto kBad = u"\\/:*?\"<>|"_s;
	auto result = QString();
	result.reserve(name.size());
	for (auto i = qsizetype(0); i != name.size(); ++i) {
		const auto ch = name[i];
		if (ch.isHighSurrogate()
			&& i + 1 != name.size()
			&& name[i + 1].isLowSurrogate()) {
			const auto code = QChar::surrogateToUcs4(ch, name[i + 1]);
			const auto category = QChar::category(code);
			if (category != QChar::Other_Format
				&& category != QChar::Other_Control
				&& category != QChar::Other_NotAssigned
				&& category != QChar::Other_PrivateUse) {
				result.append(ch);
				result.append(name[i + 1]);
			}
			++i;
			continue;
		} else if (ch.isSurrogate()) {
			continue; // a lone half is not a character
		}
		const auto category = ch.category();
		if (category == QChar::Other_Control
			|| category == QChar::Separator_Line
			|| category == QChar::Separator_Paragraph
			|| kBad.contains(ch)) {
			result.append(u'_');
		} else if (category == QChar::Other_Format
			|| category == QChar::Other_PrivateUse
			|| category == QChar::Other_NotAssigned) {
			// Direction overrides, zero-width marks: dropped, not replaced.
			continue;
		} else if (ch.isSpace()) {
			result.append(u' ');
		} else {
			result.append(ch);
		}
	}
	auto skip = qsizetype(0);
	while (skip != result.size() && (result[skip] == u'.' || result[skip] == u' ')) {
		++skip;
	}
	result = result.mid(skip, 180);
	while (result.endsWith(u'.') || result.endsWith(u' ')) {
		result.chop(1);
	}
	// "CON", "nul.txt", "COM1.anything" are devices on Windows.
	static const auto kReserved = QStringList{
		u"con"_s, u"prn"_s, u"aux"_s, u"nul"_s, u"conin$"_s, u"conout$"_s,
		u"com0"_s, u"com1"_s, u"com2"_s, u"com3"_s, u"com4"_s, u"com5"_s,
		u"com6"_s, u"com7"_s, u"com8"_s, u"com9"_s,
		u"lpt0"_s, u"lpt1"_s, u"lpt2"_s, u"lpt3"_s, u"lpt4"_s, u"lpt5"_s,
		u"lpt6"_s, u"lpt7"_s, u"lpt8"_s, u"lpt9"_s,
	};
	const auto dot = result.indexOf(u'.');
	const auto stem = ((dot < 0) ? result : result.left(dot)).trimmed().toLower();
	if (kReserved.contains(stem)) {
		result.prepend(u'_');
		result = result.left(180);
		while (result.endsWith(u'.') || result.endsWith(u' ')) {
			result.chop(1);
		}
	}
	if (result.isEmpty()) {
		return fallback.isEmpty() ? u"file"_s : fallback;
	}
	return result;
}

QString SystemDescription() {
	const auto type = QSysInfo::productType();
	const auto version = QSysInfo::productVersion();
	auto result = (type.isEmpty() || type == u"unknown") ? QSysInfo::kernelType() : type;
	if (!version.isEmpty() && version != u"unknown") {
		result += u' ' + version;
	}
	return result.toLower();
}

QString DeviceName() {
#if defined Q_OS_WIN
	const auto os = QStringLiteral("Windows");
#elif defined Q_OS_MACOS // Q_OS_WIN
	const auto os = QStringLiteral("macOS");
#else // Q_OS_WIN || Q_OS_MACOS
	const auto os = QStringLiteral("Linux");
#endif // Q_OS_WIN || Q_OS_MACOS
	auto host = QSysInfo::machineHostName();
	auto result = QStringLiteral("Eleza Desktop · ") + os;
	if (!host.isEmpty()) {
		result += QStringLiteral(" (") + host + u')';
	}
	return result.left(64);
}

} // namespace base::platform
