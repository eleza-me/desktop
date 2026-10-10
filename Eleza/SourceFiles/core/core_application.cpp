#include "core/core_application.h"

#include "api/api_auth.h"
#include "api/api_client_config.h"
#include "base/platform_file.h"
#include "core/core_modules.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/media_audio_player.h"
#include "storage/storage_key_value.h"
#include "support/session_services.h"
#include "ui/style/style_core.h"
#include "ui/ui_phrases.h"
#include "ui/ui_utility.h"
#include "ui/widgets/tooltip.h"
#include "window/window_main_window.h"
#include "window/window_registry.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

#ifdef ELEZA_DEBUG_TOOLS
#include "core/core_debug.h"
#include "gallery/gallery_widget.h"
#endif // ELEZA_DEBUG_TOOLS

namespace Core {
namespace {

constexpr auto kBackgroundDelay = 5000;
const auto kNeedsProfileKey = QStringLiteral("intro.needs_profile");

Application *InstanceValue = nullptr;

[[nodiscard]] QString FormatDigits(const QString &latin) {
	return Lang::Num(latin);
}

} // namespace

Application::Application(Launcher *launcher)
: _launcher(launcher) {
	InstanceValue = this;
#ifdef ELEZA_DEBUG_TOOLS
	_debugRoute = launcher->commandLine().debugRoute;
#endif // ELEZA_DEBUG_TOOLS
	const auto &arguments = launcher->commandLine();
	if (arguments.ephemeral() && arguments.workDir.isEmpty()) {
		// A screenshot run must not touch the real data directory.
		_temporaryDir = std::make_unique<QTemporaryDir>();
	}
	// Recordings, pasted images and the like never go to the shared
	// temporary directory: the data directory is ours alone.
	if (arguments.workDir.isEmpty()) {
		// The default location is the app's own: closed to other users (a
		// directory named with --workdir keeps the mode its owner gave it;
		// everything secret inside is 0700 / 0600 on its own).
		base::platform::EnsureDirectory(workDir(), true);
	}
	base::platform::SetPrivateTempRoot(
		QDir(workDir()).filePath(QStringLiteral("temp")));
	if (!arguments.noSingleInstance) {
		// Nobody else runs on this directory: what is left there is stale.
		base::platform::ClearPrivateTemp();
	}
	_settings = std::make_unique<Settings>(arguments.ephemeral()
		? QString()
		: QDir(workDir()).filePath(QStringLiteral("settings.json")));
	_backgroundTimer.setSingleShot(true);
	QObject::connect(&_backgroundTimer, &QTimer::timeout, this, [this] {
		applyBackground();
	});
}

Application::~Application() {
#ifdef ELEZA_DEBUG_TOOLS
	_debugActions = nullptr;
#endif // ELEZA_DEBUG_TOOLS
	// The window's sections use the session: they go first.
	_window = nullptr;
	_account = nullptr;
	_settings = nullptr;
	InstanceValue = nullptr;
}

Application &Application::Instance() {
	Q_ASSERT(InstanceValue != nullptr);
	return *InstanceValue;
}

bool Application::Exists() {
	return InstanceValue != nullptr;
}

QString Application::workDir() const {
	return (_temporaryDir && _temporaryDir->isValid())
		? _temporaryDir->path()
		: _launcher->workDir();
}

Main::Account &Application::account() const {
	Q_ASSERT(_account != nullptr);
	return *_account;
}

Main::Session *Application::session() const {
	return _account ? _account->session() : nullptr;
}

void Application::start() {
	const auto &arguments = _launcher->commandLine();

	// Fonts, palette and st:: values must exist before the first widget.
	style::Start(style::Scheme::Light);
	if (arguments.theme) {
		style::SetThemeMode((*arguments.theme == style::Scheme::Dark)
			? style::ThemeMode::Dark
			: style::ThemeMode::Light);
	} else {
		style::SetThemeMode(_settings->themeMode());
	}
	applyLanguage(arguments.language.value_or(_settings->language()));
	if (arguments.ephemeral()) {
		// Screenshots must not depend on the timing of animations.
		Ui::SetAnimationsDisabled(true);
		// Nor on where the pointer of the platform happens to rest: hover
		// tooltips appear only after a scripted "move:".
		Ui::SetHoverTooltipsSuppressed(true);
	}

	RegisterModules();

#ifdef ELEZA_DEBUG_TOOLS
	const auto gallery = arguments.gallery;
#else // ELEZA_DEBUG_TOOLS
	constexpr auto gallery = false;
#endif // ELEZA_DEBUG_TOOLS
	if (!gallery) {
		createAccount();
	}
	_window = std::make_unique<Window::MainWindow>();
#ifdef ELEZA_DEBUG_TOOLS
	if (arguments.debugSize.isValid()) {
		_window->setFixedDebugSize(arguments.debugSize);
	}
#endif // ELEZA_DEBUG_TOOLS
	QObject::connect(
		_window.get(),
		&Window::MainWindow::activeChanged,
		this,
		[this] { windowActiveChanged(); });

#ifdef ELEZA_DEBUG_TOOLS
	if (gallery) {
		showGallery(arguments.galleryPage);
		_window->showFirstTime();
		runDebugScenario();
		return;
	}
#endif // ELEZA_DEBUG_TOOLS
	_account->start();
	if (_account->session() && !introNeedsProfile()) {
		showSession();
	} else {
		showIntro();
#ifdef ELEZA_DEBUG_TOOLS
		if (!_account->session() && arguments.debugLogin()) {
			startDebugLogin();
		}
#endif // ELEZA_DEBUG_TOOLS
	}
	checkUpgradeRequired();
	_window->showFirstTime();

#ifdef ELEZA_DEBUG_TOOLS
	if (!_debugLoginRunning) {
		runDebugScenario();
	}
#endif // ELEZA_DEBUG_TOOLS
}

void Application::createAccount() {
	auto config = Main::Account::Config();
	config.workdir = workDir();
	config.apiBase = apiUrl();
	config.version = QStringLiteral(ELEZA_VERSION_STRING);
	config.language = Lang::CurrentId();
	_account = std::make_unique<Main::Account>(std::move(config));
	// The stored language of the account may be older than the settings.
	if (_account->language() != Lang::CurrentId()) {
		_account->setLanguage(Lang::CurrentId());
	}
	QObject::connect(
		_account.get(),
		&Main::Account::sessionChanged,
		this,
		[this] { sessionChanged(); });
	QObject::connect(
		_account.get(),
		&Main::Account::sessionEnded,
		this,
		[this](Main::SessionEndReason reason) { sessionEnded(reason); });
	QObject::connect(
		&_account->api(),
		&Api::Client::upgradeRequired,
		this,
		[this] {
			if (_window) {
				_window->showUpgradeRequired(_account->clientConfig().updateUrl());
			}
		});
}

void Application::checkUpgradeRequired() {
	// The cached document answers at once, the fresh one a moment later.
	const auto check = [this] {
		const auto &config = _account->clientConfig();
		if (_window
			&& config.updateRequired(QStringLiteral(ELEZA_VERSION_STRING))) {
			_window->showUpgradeRequired(config.updateUrl());
		}
	};
	check();
	_account->clientConfig().load(this, [check](bool) { check(); });
}

void Application::sessionChanged() {
	if (_account->session()) {
		// Signed in. The intro stays until it reports that the profile step is
		// done (introFinished); the debug sign-in does the same.
		return;
	}
	// The session is going away: nothing may keep using it after we return.
	Media::Player().stop();
	if (_content == Content::Session) {
		_window->destroySession();
		_content = Content::None;
		emit sessionShownChanged();
	}
}

void Application::sessionEnded(Main::SessionEndReason reason) {
	if (_quitting || !_window) {
		return;
	}
	setIntroNeedsProfile(false);
	// Whoever signs in next starts in the default tab, not where the last
	// account was left (its settings page).
	_settings->setLastTab(QString());
	// Also when the intro was on screen (the profile step): start it over.
	showIntro();
	switch (reason) {
	case Main::SessionEndReason::Expired:
	case Main::SessionEndReason::Revoked:
		_window->showToast(tr::lng_app_session_has_ended_sign_again(tr::now));
		break;
	case Main::SessionEndReason::AccountDeleted:
		_window->showToast(tr::lng_window_account_deleted(tr::now));
		break;
	case Main::SessionEndReason::SignedOut:
		break;
	}
}

bool Application::introNeedsProfile() const {
	return _account && _account->settings().boolean(kNeedsProfileKey);
}

void Application::setIntroNeedsProfile(bool needs) {
	if (!_account || introNeedsProfile() == needs) {
		return;
	}
	if (needs) {
		_account->settings().set(kNeedsProfileKey, true);
	} else {
		_account->settings().remove(kNeedsProfileKey);
	}
	_account->settings().flush();
}

void Application::introFinished() {
	setIntroNeedsProfile(false);
	if (_account && _account->session()) {
		showSession();
	}
}

void Application::applyLanguage(Lang::Language language) {
	Lang::SetCurrent(language);
	style::SetRightToLeft(Lang::RightToLeft());
	QApplication::setLayoutDirection(Lang::RightToLeft()
		? Qt::RightToLeft
		: Qt::LeftToRight);
	refreshPhrases();
}

void Application::refreshPhrases() {
	auto phrases = Ui::Phrases();
	phrases.undo = tr::lng_menu_undo(tr::now);
	phrases.redo = tr::lng_menu_redo(tr::now);
	phrases.cut = tr::lng_menu_cut(tr::now);
	phrases.copy = tr::lng_copy(tr::now);
	phrases.paste = tr::lng_menu_paste(tr::now);
	phrases.remove = tr::lng_delete(tr::now);
	phrases.selectAll = tr::lng_menu_select_all(tr::now);
	phrases.copyLink = tr::lng_copy_link(tr::now);
	phrases.openLink = tr::lng_menu_open_link(tr::now);
	phrases.copied = tr::lng_copied(tr::now);
	phrases.ok = tr::lng_ok(tr::now);
	phrases.cancel = tr::lng_cancel(tr::now);
	phrases.close = tr::lng_close(tr::now);
	phrases.back = tr::lng_back(tr::now);
	phrases.show = tr::lng_show(tr::now);
	phrases.hide = tr::lng_hide(tr::now);
	phrases.formatNumber = &FormatDigits;
	Ui::SetPhrases(std::move(phrases));
}

void Application::setLanguage(Lang::Language language) {
	_settings->setLanguage(language);
	if (Lang::Current() == language) {
		return;
	}
	applyLanguage(language);
	if (_account) {
		// Accept-Language of every request from now on.
		_account->setLanguage(Lang::Id(language));
	}
	// The layout direction changed: rebuild everything the window shows.
	rebuildContent();
	emit languageChanged();
}

void Application::setThemeMode(style::ThemeMode mode) {
	_settings->setThemeMode(mode);
	style::SetThemeMode(mode);
	if (_window) {
		// The palette notifier repaints widgets; cached snapshots (a running
		// transition, the window icon) are refreshed by a full update.
		_window->update();
	}
	emit themeChanged();
}

void Application::toggleTheme() {
	setThemeMode(style::IsDark()
		? style::ThemeMode::Light
		: style::ThemeMode::Dark);
}

void Application::rebuildContent() {
	if (!_window) {
		return;
	}
	_window->refreshTitle();
	switch (_content) {
#ifdef ELEZA_DEBUG_TOOLS
	case Content::Gallery: showGallery(_galleryPage); break;
#endif // ELEZA_DEBUG_TOOLS
	case Content::Intro: showIntro(); break;
	case Content::Session: {
		// Back to where the user was: the tab and what every tab had open.
		const auto was = _window->sessionController();
		const auto state = was
			? std::make_optional(was->saveState())
			: std::nullopt;
		showSession();
		const auto now = _window->sessionController();
		if (state && now) {
			now->restoreState(*state);
		}
	} break;
	case Content::None: break;
	}
}

#ifdef ELEZA_DEBUG_TOOLS
void Application::showGallery(const QString &page) {
	_content = Content::Gallery;
	if (!page.isEmpty()) {
		_galleryPage = page;
	}
	const auto window = _window.get();
	const auto initial = _galleryPage;
	const auto overlay = _debugRoute;
	const auto widget = new Gallery::Widget(
		window->bodyWidget(),
		window,
		initial,
		overlay);
	QObject::connect(
		widget,
		&Gallery::Widget::pageChanged,
		this,
		[this](const QString &now) { _galleryPage = now; });
	window->setContent(Ui::Owned<Ui::RpWidget>(widget));
}
#endif // ELEZA_DEBUG_TOOLS

void Application::showIntro() {
	const auto wasSession = (_content == Content::Session);
	_content = Content::Intro;
	_window->destroySession();
	_window->setUnreadCount(0);
	_window->setContent(Window::CreateIntro(_window->bodyWidget(), _window.get()));
	if (wasSession) {
		emit sessionShownChanged();
	}
}

void Application::showSession() {
	const auto session = _account ? _account->session() : nullptr;
	if (!session) {
		showIntro();
		return;
	}
	_content = Content::Session;
	_window->showSession(session);
	// Error toasts for background actions and desktop notifications.
	[[maybe_unused]] const auto services = Support::SessionServices::For(
		_window->sessionController());
	applyBackground();
	emit sessionShownChanged();
	if (const auto route = std::exchange(_pendingRoute, QString());
		!route.isEmpty()) {
		openRoute(route);
	}
}

void Application::openRoute(const QString &route) {
	if (route.isEmpty()) {
		return;
	}
	const auto controller = _window ? _window->sessionController() : nullptr;
	if (controller) {
		controller->openRoute(route);
		return;
	}
#ifdef ELEZA_DEBUG_TOOLS
	if (_content == Content::Gallery) {
		return;
	}
#endif // ELEZA_DEBUG_TOOLS
	_pendingRoute = route;
}

bool Application::windowActive() const {
	return _window
		&& _window->isVisible()
		&& !_window->isMinimized()
		&& _window->isActiveWindow();
}

void Application::windowActiveChanged() {
	if (windowActive()) {
		_backgroundTimer.stop();
		applyBackground();
	} else if (_window->isMinimized() || !_window->isVisible()) {
		_backgroundTimer.stop();
		applyBackground();
	} else {
		// Switching to another window for a moment does not make us "away".
		_backgroundTimer.start(kBackgroundDelay);
	}
}

void Application::applyBackground() {
	if (const auto session = this->session()) {
		// Screenshot runs have no window manager to activate the window.
		const auto active = windowActive() || commandLine().ephemeral();
		session->rt().setBackground(!active);
	}
}

void Application::handleActivation(const QString &message) {
	if (_window) {
		_window->activateAndShow();
	}
#ifdef ELEZA_DEBUG_TOOLS
	// The message comes from another process: only a place of the app may be
	// opened through it - never a "debug/..." hook (those end the session or
	// load files by path) and nothing that is not a canonical route.
	const auto prefix = QStringLiteral("route:");
	if (message.startsWith(prefix)) {
		const auto route = Window::ParseRoute(message.mid(prefix.size()));
		if (route.valid()) {
			openRoute(route.toString());
		}
	}
#else // ELEZA_DEBUG_TOOLS
	// Nothing but "come to the front" is taken from another process.
	Q_UNUSED(message);
#endif // ELEZA_DEBUG_TOOLS
}

void Application::quit() {
	_quitting = true;
	if (_settings) {
		_settings->flush();
	}
	QCoreApplication::quit();
}

// ---- debug hooks
#ifdef ELEZA_DEBUG_TOOLS

void Application::startDebugLogin() {
	const auto &arguments = _launcher->commandLine();
	_debugLoginRunning = true;
	const auto phone = Lang::E164(arguments.debugLoginPhone);
	if (phone.isEmpty()) {
		debugLoginFailed(QStringLiteral("the phone number is not valid"));
		return;
	}
	const auto code = arguments.debugLoginCode;
	const auto password = arguments.debugLoginPassword;
	const auto fail = [this](const Api::Error &error) {
		debugLoginFailed(error.describe());
	};
	const auto done = [this](const Api::VerifyOutcome &outcome) {
		if (outcome.kind != Api::VerifyOutcome::Kind::SignedIn) {
			debugLoginFailed(QStringLiteral("a second password step"));
			return;
		}
		_debugLoginRunning = false;
		if (outcome.needsProfile) {
			// The profile step is the intro's: it is on screen already.
			setIntroNeedsProfile(true);
			showIntro();
		} else {
			introFinished();
		}
		runDebugScenario();
	};
	auto &auth = _account->auth();
	auth.requestCode(phone, this, [=, this, &auth](
			const Api::OtpChallenge &challenge) {
		auth.verifyCode(challenge.challengeId, code, true, this, [=, this, &auth](
				const Api::VerifyOutcome &outcome) {
			if (outcome.kind != Api::VerifyOutcome::Kind::TwoStep) {
				done(outcome);
			} else if (password.isEmpty()) {
				debugLoginFailed(QStringLiteral(
					"the account has a two-step password; "
					"pass --debug-login=<phone>:<code>:<password>"));
			} else {
				auth.signInWithPassword(outcome.form, password, this, done, fail);
			}
		}, fail);
	}, fail);
}

void Application::debugLoginFailed(const QString &why) {
	qWarning("--debug-login failed: %s", qPrintable(why));
	_debugLoginRunning = false;
	if (commandLine().ephemeral()) {
		QCoreApplication::exit(2);
	}
}

void Application::runDebugScenario() {
	if (_debugScenarioStarted) {
		return;
	}
	_debugScenarioStarted = true;
	const auto &arguments = _launcher->commandLine();
	const auto route = _debugRoute;
	// On the next turn of the loop: the window is laid out and shown by then.
	QTimer::singleShot(0, this, [=, this] {
		if (_content == Content::Session) {
			openRoute(route);
		}
		if (arguments.debugActions.isEmpty()) {
			finishDebugScenario();
			return;
		}
		_debugActions = std::make_unique<DebugActions>(
			_window.get(),
			arguments.debugActions,
			[this](const QString &value) { openRoute(value); });
		_debugActions->start([this] { finishDebugScenario(); });
	});
}

void Application::finishDebugScenario() {
	const auto &arguments = _launcher->commandLine();
	if (!arguments.ephemeral()) {
		return;
	}
	const auto path = arguments.debugGrab;
	QTimer::singleShot(arguments.debugGrabDelay, this, [this, path] {
		if (!path.isEmpty()
			&& !GrabWindowWithPopups(_window.get()).save(path)) {
			qWarning("Could not save the grab to '%s'.", qPrintable(path));
			QCoreApplication::exit(1);
			return;
		}
		QCoreApplication::exit(0);
	});
}

#endif // ELEZA_DEBUG_TOOLS

} // namespace Core
