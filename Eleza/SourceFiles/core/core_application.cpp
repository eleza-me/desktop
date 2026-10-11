#include "core/core_application.h"

#include "api/api_auth.h"
#include "api/api_client_config.h"
#include "base/platform_file.h"
#include "core/core_modules.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
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
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/widgets/tooltip.h"
#include "updater/updater_checker.h"
#include "updater/updater_install.h"
#include "updater/updater_keys.h"
#include "window/window_main_window.h"
#include "window/window_registry.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThreadPool>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QApplication>

#ifdef ELEZA_DEBUG_TOOLS
#include "core/core_debug.h"
#include "gallery/gallery_widget.h"
#endif // ELEZA_DEBUG_TOOLS

namespace Core {
namespace {

constexpr auto kBackgroundDelay = 5000;
// How long quitting (or signing out) waits for the exit tasks.
constexpr auto kExitTasksTimeout = 1500;
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
	_updater = nullptr;
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
		createUpdater();
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
	updaterStateChanged();
	_window->showFirstTime();
	showInstallResult();

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

void Application::createUpdater() {
	const auto &arguments = _launcher->commandLine();
	_installTarget = std::make_unique<Updater::InstallTarget>(
		Updater::DetectInstall());

	auto config = Updater::Config();
	config.currentVersion = QStringLiteral(ELEZA_VERSION_STRING);
	config.updatesDir = QDir(workDir()).filePath(QStringLiteral("updates"));
	config.platform = Updater::PlatformKey();
	for (const auto source : Updater::kManifestSources) {
		config.sources.push_back(QString::fromLatin1(source));
	}
	config.publicKeys = Updater::BakedPublicKeys();
	config.selfUpdate = _installTarget->selfUpdate;
	config.executable = _installTarget->executable;
	config.autoDownload = [this] { return _settings->autoUpdate(); };
#ifdef ELEZA_DEBUG_TOOLS
	// A test release on a local server, signed with a test key.
	if (!arguments.updateUrls.isEmpty()) {
		config.sources = arguments.updateUrls;
		config.allowInsecureLoopback = true;
	}
	if (!arguments.updateKeys.isEmpty()) {
		config.publicKeys.clear();
		for (const auto &text : arguments.updateKeys) {
			if (const auto key = Updater::DecodePublicKey(text)) {
				config.publicKeys.push_back(*key);
			} else {
				qWarning("--update-key: not an Ed25519 public key in base64.");
			}
		}
	}
	if (arguments.updateNow) {
		config.firstCheckDelayMs = 0;
	}
	if (!arguments.updateState.isEmpty()) {
		// The pages as a copy that updates itself shows them; "announced"
		// is the one that does not.
		config.selfUpdate = (arguments.updateState != QStringLiteral("announced"));
		if (config.platform.isEmpty()) {
			config.platform = QStringLiteral("linux-x86_64");
		}
	}
#endif // ELEZA_DEBUG_TOOLS
	const auto updatesDir = config.updatesDir;
	_updater = std::make_unique<Updater::Checker>(std::move(config));
	QObject::connect(
		_updater.get(),
		&Updater::Checker::stateChanged,
		this,
		[this] { updaterStateChanged(); });

#ifdef ELEZA_DEBUG_TOOLS
	if (!arguments.updateState.isEmpty()) {
		applyDebugUpdateState(arguments.updateState);
		return;
	}
#endif // ELEZA_DEBUG_TOOLS
	if (arguments.ephemeral()) {
		// A screenshot run neither checks nor installs anything.
		return;
	}
	if (const auto result = Updater::TakeInstallResult(updatesDir)) {
		_updater->installFinished(result->version, result->installed);
		// A backup that could not be deleted while its files were in use.
		QThreadPool::globalInstance()->start([target = *_installTarget] {
			Updater::RemoveInstallLeftovers(target);
		});
		if (result->installed) {
			_installedVersion = result->version;
		} else {
			_installFailedVersion = result->version;
			// The reason names files of the install directory.
#ifdef ELEZA_DEBUG_TOOLS
			qWarning(
				"Updater: installing %s failed: %s",
				qPrintable(result->version),
				qPrintable(result->error));
#else // ELEZA_DEBUG_TOOLS
			qWarning("Updater: the install step failed.");
#endif // !ELEZA_DEBUG_TOOLS
		}
	}
	_updater->start();
}

void Application::showInstallResult() {
	if (!_window) {
		return;
	}
	if (const auto version = std::exchange(_installedVersion, QString());
		!version.isEmpty()) {
		auto toast = Ui::Toast::Config();
		toast.text = tr::lng_update_done_toast(
			tr::now,
			lt_version,
			Lang::Num(version));
		toast.icon = &st::toastIconCheck;
		toast.duration = 6000;
		// A click on it opens the page with what is new.
		toast.clicked = [this] {
			openRoute(Window::Routes::Settings(QStringLiteral("update")));
		};
		Ui::Toast::Show(_window->bodyWidget(), std::move(toast));
	} else if (const auto failed = std::exchange(
			_installFailedVersion,
			QString()); !failed.isEmpty()) {
		auto toast = Ui::Toast::Config();
		toast.text = tr::lng_update_failed_toast(
			tr::now,
			lt_version,
			Lang::Num(failed));
		toast.icon = &st::toastIconWarning;
		toast.duration = 7000;
		Ui::Toast::Show(_window->bodyWidget(), std::move(toast));
	}
}

void Application::updaterStateChanged() {
	if (!_updater || !_window) {
		return;
	}
	// The manifest says that this version is not supported any more: the
	// same blocking screen the server's own signal brings, with the update
	// under way.
	const auto state = _updater->state();
	const auto known = (state == Updater::State::Available)
		|| (state == Updater::State::Downloading)
		|| (state == Updater::State::Preparing)
		|| (state == Updater::State::Ready)
		|| (state == Updater::State::Failed);
	if (known && _updater->info().mandatory) {
		_window->showUpgradeRequired(_account
			? _account->clientConfig().updateUrl()
			: QString());
	}
	// The answer to «بررسی به‌روزرسانی» where no settings page shows it.
	if (_announceUpdateCheck) {
		auto text = QString();
		switch (state) {
		case Updater::State::UpToDate:
			text = tr::lng_update_up_to_date(tr::now);
			break;
		case Updater::State::Available:
			text = tr::lng_update_new_available(tr::now);
			break;
		case Updater::State::Failed:
			text = tr::lng_update_failed_network(tr::now);
			break;
		case Updater::State::Ready:
			text = tr::lng_update_ready(
				tr::now,
				lt_version,
				Lang::Num(_updater->info().version));
			break;
		default: break;
		}
		if (!text.isEmpty()) {
			_announceUpdateCheck = false;
			_window->showToast(text);
		}
	}
#ifdef ELEZA_DEBUG_TOOLS
	// --update-now: the whole way without a click (the end-to-end test).
	if (state == Updater::State::Ready
		&& _launcher->commandLine().updateNow
		&& !_launcher->commandLine().ephemeral()) {
		QTimer::singleShot(0, this, [this] { restartToUpdateNow(); });
	}
#endif // ELEZA_DEBUG_TOOLS
}

void Application::checkForUpdates() {
	if (!_updater) {
		return;
	}
	if (_window) {
		_window->activateAndShow();
	}
	const auto controller = _window ? _window->sessionController() : nullptr;
	if (controller && !_window->upgradeRequired()) {
		controller->openRoute(
			Window::Routes::Settings(QStringLiteral("update")));
	} else {
		_announceUpdateCheck = true;
	}
	_updater->checkNow();
}

void Application::openDownloadPage() {
	auto url = QUrl(_account
		? _account->clientConfig().updateUrl()
		: QString());
	// Only a web page may come from the configuration.
	if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
		url = QUrl(QString::fromLatin1(Updater::kDownloadPage));
	}
	QDesktopServices::openUrl(url);
}

void Application::restartToUpdate() {
	if (!_updater
		|| !_window
		|| _updater->state() != Updater::State::Ready) {
		return;
	}
	// Not in the middle of something that a restart would cut off (a call).
	for (const auto &blocker : _restartBlockers) {
		if (const auto why = blocker(); !why.isEmpty()) {
			_window->showToast(why);
			return;
		}
	}
	// Messages that are still being sent or uploaded would wait for the next
	// start: that is the user's call.
	const auto session = this->session();
	const auto pending = session
		? session->data().outbox().pendingCount()
		: 0;
	if (pending <= 0) {
		restartToUpdateNow();
		return;
	}
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_update_button(tr::now);
	args.text = tr::lng_update_restart_pending(tr::now, lt_count, pending);
	args.confirmText = tr::lng_update_restart_confirm(tr::now);
	args.confirmed = [this](std::function<void()> close) {
		close();
		restartToUpdateNow();
	};
	_window->show(Ui::MakeConfirmBox(std::move(args)));
}

void Application::restartToUpdateNow() {
	if (!_updater
		|| !_installTarget
		|| _updater->state() != Updater::State::Ready) {
		return;
	}
	for (const auto &blocker : _restartBlockers) {
		if (!blocker().isEmpty()) {
			return; // restartToUpdate() said why
		}
	}
	auto request = Updater::InstallRequest();
	request.version = _updater->info().version;
	request.stagedDir = _updater->readyDir();
	request.updatesDir = QDir(workDir()).filePath(QStringLiteral("updates"));
	request.arguments = QCoreApplication::arguments().mid(1);
	if (!Updater::PrepareInstall(*_installTarget, request)) {
		_window->showToast(tr::lng_update_failed_start(tr::now));
		return;
	}
	// main() swaps the files and starts the new version once everything of
	// this one is closed.
	quit();
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
	if (_quitting) {
		return;
	}
	_quitting = true;
	if (_settings) {
		_settings->flush();
	}
	if (_window && !_exitTasks.empty()) {
		// Nothing to look at while the last requests go out.
		_window->hide();
	}
	finishExitTasks([] { QCoreApplication::quit(); });
}

void Application::registerExitTask(ExitTask task) {
	if (task) {
		_exitTasks.push_back(std::move(task));
	}
}

void Application::finishExitTasks(std::function<void()> then) {
	if (_exitTasks.empty()) {
		then();
		return;
	}
	struct State {
		int left = 0;
		bool finished = false;
		std::function<void()> then;
	};
	const auto state = std::make_shared<State>();
	state->left = int(_exitTasks.size());
	state->then = std::move(then);
	const auto finish = [state] {
		if (!std::exchange(state->finished, true)) {
			const auto then = std::move(state->then);
			then();
		}
	};
	// A server that does not answer must not keep the app from closing.
	QTimer::singleShot(kExitTasksTimeout, this, finish);
	const auto tasks = _exitTasks;
	for (const auto &task : tasks) {
		task([state, finish] {
			if (--state->left <= 0) {
				finish();
			}
		});
	}
}

void Application::registerRestartBlocker(std::function<QString()> blocker) {
	if (blocker) {
		_restartBlockers.push_back(std::move(blocker));
	}
}

void Application::registerCloseGuard(std::function<bool()> guard) {
	if (guard) {
		_closeGuards.push_back(std::move(guard));
	}
}

bool Application::closeGuarded() const {
	for (const auto &guard : _closeGuards) {
		if (guard()) {
			return true;
		}
	}
	return false;
}

// ---- debug hooks
#ifdef ELEZA_DEBUG_TOOLS

void Application::applyDebugUpdateState(const QString &name) {
	using State = Updater::State;
	using Failure = Updater::Failure;
	auto info = Updater::Info();
	info.version = QStringLiteral("0.2.0");
	info.size = qint64(36812544);
	info.notesFa = QStringLiteral(
		"• الزا حالا خودش به‌روز می‌شود: نسخهٔ تازه در پس‌زمینه دریافت و با یک کلیک نصب می‌شود.\n"
		"• نوار عنوان تازه، هماهنگ با پوستهٔ روشن و تیره.\n"
		"• رفع چند اشکال در ارسال پیام صوتی.");
	info.notesEn = QStringLiteral(
		"• Eleza now updates itself: a new version is downloaded in the background and installed with one click.\n"
		"• A new title bar that follows the light and the dark theme.\n"
		"• Fixes for sending voice messages.");
	auto state = State::Idle;
	auto failure = Failure::None;
	auto received = qint64(0);
	if (name == QStringLiteral("checking")) {
		state = State::Checking;
	} else if (name == QStringLiteral("uptodate")) {
		state = State::UpToDate;
		info = Updater::Info();
	} else if (name == QStringLiteral("available")
		|| name == QStringLiteral("announced")) {
		state = State::Available;
	} else if (name == QStringLiteral("downloading")) {
		state = State::Downloading;
		received = info.size * 42 / 100;
	} else if (name == QStringLiteral("preparing")) {
		state = State::Preparing;
		received = info.size;
	} else if (name == QStringLiteral("ready")) {
		state = State::Ready;
	} else if (name == QStringLiteral("failed")) {
		state = State::Failed;
		failure = Failure::Network;
		info = Updater::Info();
	} else if (name == QStringLiteral("failed-install")) {
		state = State::Failed;
		failure = Failure::Install;
	} else if (name == QStringLiteral("required")) {
		state = State::Downloading;
		received = info.size * 42 / 100;
		info.mandatory = true;
	} else if (name == QStringLiteral("required-ready")) {
		state = State::Ready;
		info.mandatory = true;
	}
	_updater->debugSetState(state, failure, info, received);
}

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
