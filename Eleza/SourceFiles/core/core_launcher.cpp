#include "core/core_launcher.h"

#include "api/api_client.h"
#include "core/core_application.h"
#include "core/core_single_instance.h"

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Core {
namespace {

constexpr auto kVersionText = "Eleza Desktop " ELEZA_VERSION_STRING "\n";
constexpr auto kHelpText =
	"Eleza Desktop " ELEZA_VERSION_STRING "\n"
	"\n"
	"  --api=<url>            API address (https, or http on the loopback interface)\n"
	"  --workdir=<dir>        data directory\n"
	"  --lang=fa|en           interface language for this run\n"
	"  --theme=light|dark     palette for this run\n"
	"  --no-single-instance   allow a second process on the same data directory\n"
	"  --system-title-bar     keep the title bar of the system for this run\n"
	"  --version              print the version and exit\n"
	"  --help                 print this text and exit\n"
#ifdef ELEZA_DEBUG_TOOLS
	"\n"
	"This build has the debug tools (--debug-*, --gallery): docs/DEVELOPING.md.\n"
#endif // ELEZA_DEBUG_TOOLS
	;

// A GUI program has no console of its own on Windows: write to the one it was
// started from, when there is one.
void AttachOutput() {
#ifdef Q_OS_WIN
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		auto file = (FILE*)nullptr;
		freopen_s(&file, "CONOUT$", "w", stdout);
		freopen_s(&file, "CONOUT$", "w", stderr);
	}
#endif // Q_OS_WIN
}

[[nodiscard]] bool TakeValue(
		const QString &argument,
		const char *name,
		QString &to) {
	const auto prefix = QString::fromLatin1(name) + QLatin1Char('=');
	if (!argument.startsWith(prefix)) {
		return false;
	}
	to = argument.mid(prefix.size());
	return true;
}

} // namespace

CommandLine ParseCommandLine(const QStringList &arguments) {
	auto result = CommandLine();
	auto value = QString();
	for (auto i = 1; i < arguments.size(); ++i) {
		const auto &argument = arguments[i];
		if (TakeValue(argument, "--api", value)) {
			result.apiUrl = value;
		} else if (TakeValue(argument, "--workdir", value)) {
			result.workDir = value;
		} else if (TakeValue(argument, "--lang", value)) {
			if (value == QStringLiteral("fa")) {
				result.language = Lang::Language::Persian;
			} else if (value == QStringLiteral("en")) {
				result.language = Lang::Language::English;
			}
		} else if (TakeValue(argument, "--theme", value)) {
			if (value == QStringLiteral("light")) {
				result.theme = style::Scheme::Light;
			} else if (value == QStringLiteral("dark")) {
				result.theme = style::Scheme::Dark;
			}
		} else if (argument == QStringLiteral("--no-single-instance")) {
			result.noSingleInstance = true;
		} else if (argument == QStringLiteral("--system-title-bar")) {
			result.systemTitleBar = true;
		} else if (argument == QStringLiteral("--version")) {
			result.showVersion = true;
		} else if (argument == QStringLiteral("--help")) {
			result.showHelp = true;
#ifdef ELEZA_DEBUG_TOOLS
		} else if (argument == QStringLiteral("--gallery")) {
			result.gallery = true;
		} else if (TakeValue(argument, "--gallery", value)) {
			result.gallery = true;
			result.galleryPage = value;
		} else if (TakeValue(argument, "--debug-route", value)) {
			result.debugRoute = value;
		} else if (TakeValue(argument, "--debug-size", value)) {
			const auto parts = value.toLower().split(QLatin1Char('x'));
			if (parts.size() == 2) {
				const auto size = QSize(parts[0].toInt(), parts[1].toInt());
				if (size.width() > 0 && size.height() > 0) {
					result.debugSize = size;
				}
			}
		} else if (TakeValue(argument, "--debug-scale", value)) {
			result.debugScale = value.toDouble();
		} else if (TakeValue(argument, "--debug-grab", value)) {
			result.debugGrab = value;
		} else if (TakeValue(argument, "--debug-grab-delay", value)) {
			result.debugGrabDelay = qMax(value.toInt(), 0);
		} else if (TakeValue(argument, "--debug-login", value)) {
			const auto parts = value.split(QLatin1Char(':'));
			if (parts.size() >= 2) {
				result.debugLoginPhone = parts[0];
				result.debugLoginCode = parts[1];
				result.debugLoginPassword = parts.mid(2).join(QLatin1Char(':'));
			}
		} else if (TakeValue(argument, "--debug-actions", value)) {
			result.debugActions = value;
		} else if (argument == QStringLiteral("--debug-title-bar")) {
			result.debugTitleBar = true;
		} else if (TakeValue(argument, "--debug-title-bar", value)) {
			result.debugTitleBar = true;
			result.debugTitleState = value;
		} else if (TakeValue(argument, "--update-url", value)) {
			result.updateUrls.push_back(value);
		} else if (TakeValue(argument, "--update-key", value)) {
			result.updateKeys.push_back(value);
		} else if (argument == QStringLiteral("--update-now")) {
			result.updateNow = true;
		} else if (TakeValue(argument, "--update-state", value)) {
			result.updateState = value;
#else // ELEZA_DEBUG_TOOLS
		} else if (argument.startsWith(QStringLiteral("--debug-"))
			|| argument.startsWith(QStringLiteral("--gallery"))
			|| argument.startsWith(QStringLiteral("--update-"))) {
			const auto till = argument.indexOf(QLatin1Char('='));
			result.unavailable.push_back(argument.left(till));
#endif // ELEZA_DEBUG_TOOLS
		}
	}
	return result;
}

Launcher::Launcher(int argc, char *argv[])
: _argc(argc)
, _argv(argv) {
	auto arguments = QStringList();
	for (auto i = 0; i != argc; ++i) {
		arguments.push_back(QString::fromLocal8Bit(argv[i]));
	}
	_commandLine = ParseCommandLine(arguments);
}

QString Launcher::apiUrl() const {
	const auto standard = QStringLiteral("https://api.eleza.me");
	const auto custom = !_commandLine.apiUrl.isEmpty()
		? _commandLine.apiUrl
		: qEnvironmentVariable("ELEZA_API");
	if (custom.isEmpty()) {
		return standard;
	}
	// The tokens go wherever this points: TLS, or a test server on the
	// loopback interface. Clear text to another machine is for builds with the
	// debug tools only.
#ifdef ELEZA_DEBUG_TOOLS
	const auto allowInsecure = true;
#else // ELEZA_DEBUG_TOOLS
	const auto allowInsecure = false;
#endif // ELEZA_DEBUG_TOOLS
	if (!Api::Client::IsAcceptableBase(custom, allowInsecure)) {
		qWarning("The API address is not accepted (https, or http on the loopback interface); using the default.");
		return standard;
	}
	return custom;
}

void Launcher::prepareEnvironment() {
#ifdef ELEZA_DEBUG_TOOLS
	// Must be set before the QApplication is created.
	if (_commandLine.debugScale > 0.) {
		qputenv(
			"QT_SCALE_FACTOR",
			QByteArray::number(_commandLine.debugScale));
	}
#endif // ELEZA_DEBUG_TOOLS
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
		Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
	QCoreApplication::setOrganizationName(QStringLiteral("eleza"));
	QCoreApplication::setOrganizationDomain(QStringLiteral("eleza.me"));
	QCoreApplication::setApplicationName(QStringLiteral("Eleza"));
	QCoreApplication::setApplicationVersion(
		QStringLiteral(ELEZA_VERSION_STRING));
	QGuiApplication::setDesktopFileName(QStringLiteral("me.eleza.desktop"));
}

QString Launcher::computeWorkDir() const {
	if (!_commandLine.workDir.isEmpty()) {
		return QDir(_commandLine.workDir).absolutePath();
	}
	const auto base = QStandardPaths::writableLocation(
		QStandardPaths::AppDataLocation);
	return base.isEmpty()
		? QDir::home().filePath(QStringLiteral(".eleza"))
		: base;
}

int Launcher::exec() {
	// Answered without a display or a data directory.
	if (_commandLine.showVersion || _commandLine.showHelp) {
		AttachOutput();
		std::fputs(_commandLine.showHelp ? kHelpText : kVersionText, stdout);
		std::fflush(stdout);
		return 0;
	}
	if (!_commandLine.unavailable.isEmpty()) {
		AttachOutput();
		std::fprintf(
			stderr,
			"Eleza: %s: not available in this build (ignored).\n",
			qPrintable(_commandLine.unavailable.join(QStringLiteral(", "))));
		std::fflush(stderr);
	}
	prepareEnvironment();

	auto application = QApplication(_argc, _argv);
#ifdef Q_OS_WIN
	// argv is in the ANSI code page on Windows: a --workdir or a route with
	// characters outside of it (Persian on an English system) arrives as
	// "?". Qt reads the wide command line.
	_commandLine = ParseCommandLine(QCoreApplication::arguments());
#endif // Q_OS_WIN
	// One look on every platform for the few native pieces we keep (the inner
	// text editors, the tray menu).
	if (const auto fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
		QApplication::setStyle(fusion);
	}
	QApplication::setQuitOnLastWindowClosed(false);
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/gui/art/icon256.png")));

	_workDir = computeWorkDir();

	auto instance = std::unique_ptr<SingleInstance>();
	if (!_commandLine.ephemeral() && !_commandLine.noSingleInstance) {
		QDir().mkpath(_workDir);
		instance = std::make_unique<SingleInstance>(_workDir);
#ifdef ELEZA_DEBUG_TOOLS
		const auto message = _commandLine.debugRoute.isEmpty()
			? QStringLiteral("activate")
			: (QStringLiteral("route:") + _commandLine.debugRoute);
#else // ELEZA_DEBUG_TOOLS
		const auto message = QStringLiteral("activate");
#endif // ELEZA_DEBUG_TOOLS
		if (instance->notifyRunning(message)) {
			return 0;
		}
	}

	auto core = Application(this);
	if (instance) {
		QObject::connect(
			instance.get(),
			&SingleInstance::activated,
			&core,
			[&core](const QString &message) {
				core.handleActivation(message);
			});
	}
	core.start();
	return QApplication::exec();
}

} // namespace Core
