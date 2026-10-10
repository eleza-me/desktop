#include "core/core_launcher.h"

#include "api/api_client.h"
#include "core/core_application.h"
#include "core/core_single_instance.h"

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

namespace Core {
namespace {

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
		} else if (argument == QStringLiteral("--no-single-instance")) {
			result.noSingleInstance = true;
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
	// loopback interface. Clear text to another machine is for debug builds.
#ifdef QT_NO_DEBUG
	const auto allowInsecure = false;
#else // QT_NO_DEBUG
	const auto allowInsecure = true;
#endif // QT_NO_DEBUG
	if (!Api::Client::IsAcceptableBase(custom, allowInsecure)) {
		qWarning("The API address is not accepted (https, or http on the loopback interface); using the default.");
		return standard;
	}
	return custom;
}

void Launcher::prepareEnvironment() {
	// Must be set before the QApplication is created.
	if (_commandLine.debugScale > 0.) {
		qputenv(
			"QT_SCALE_FACTOR",
			QByteArray::number(_commandLine.debugScale));
	}
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
		const auto message = _commandLine.debugRoute.isEmpty()
			? QStringLiteral("activate")
			: (QStringLiteral("route:") + _commandLine.debugRoute);
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
