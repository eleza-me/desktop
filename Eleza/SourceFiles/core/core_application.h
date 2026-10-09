// Core::Application — the application object: owns the settings, the account and
// the main window, applies language and theme, and decides what the window shows.
//
//   Core::App().settings()                  preferences (core_settings.h)
//   Core::App().account()                   Main::Account: device, tokens, API client
//   Core::App().session()                   Main::Session of the signed-in user, or null
//   Core::App().setLanguage(...)            switches the language at runtime
//   Core::App().setThemeMode(...)           System / Light / Dark
//   Core::App().window()                    the main window
//   Core::App().openRoute(route)            now, or as soon as the session is shown
//
// What the window shows:
//   the UI gallery        --gallery (no account is created)
//   the intro             nobody is signed in, or the profile step is not done
//   the session layout    nav rail | side column | main column
//
// Sign-in creates the session inside the account while the intro is still on
// screen (it may need the profile step); the intro calls introFinished() when it
// is done. The end of a session (sign out, expired, revoked) always leads back to
// the intro, with a toast explaining why when it was not the user's choice.
#pragma once

#include "core/core_launcher.h"
#include "core/core_settings.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <memory>

class QTemporaryDir;

namespace Main {
class Account;
class Session;
enum class SessionEndReason;
} // namespace Main

namespace Window {
class MainWindow;
} // namespace Window

namespace Core {

class DebugActions;

class Application final : public QObject {
	Q_OBJECT

public:
	explicit Application(Launcher *launcher);
	~Application();

	[[nodiscard]] static Application &Instance();
	[[nodiscard]] static bool Exists();

	// Applies settings, creates the account and the window. Called once by the
	// launcher.
	void start();

	[[nodiscard]] Settings &settings() {
		return *_settings;
	}
	[[nodiscard]] Window::MainWindow *window() const {
		return _window.get();
	}
	[[nodiscard]] const CommandLine &commandLine() const {
		return _launcher->commandLine();
	}
	[[nodiscard]] QString debugRoute() const {
		return _debugRoute;
	}
	[[nodiscard]] QString apiUrl() const {
		return _launcher->apiUrl();
	}
	// The data directory (a temporary one for screenshot runs without --workdir).
	[[nodiscard]] QString workDir() const;

	// Exists always except in the gallery (--gallery).
	[[nodiscard]] bool hasAccount() const {
		return _account != nullptr;
	}
	[[nodiscard]] Main::Account &account() const;
	[[nodiscard]] Main::Session *session() const;

	// Stored in settings and applied right away; the window content is rebuilt
	// for a language switch (the direction changes).
	void setLanguage(Lang::Language language);
	void setThemeMode(style::ThemeMode mode);
	// Light <-> dark, as a manual override.
	void toggleTheme();

	void showGallery(const QString &page = QString());
	void showIntro();

	// ---- for the intro
	// A new account still has to pass the profile step (kept across restarts).
	[[nodiscard]] bool introNeedsProfile() const;
	void setIntroNeedsProfile(bool needs);
	// Signed in and the profile is complete: show the session.
	void introFinished();

	// Opens a route in the session layout: at once when it is shown, otherwise
	// when it appears (a link that arrived while signed out).
	void openRoute(const QString &route);

	// Whether the main window is the active one (notifications, presence).
	[[nodiscard]] bool windowActive() const;

	// A later launch of the app asked us to come to the front.
	void handleActivation(const QString &message);
	void quit();

Q_SIGNALS:
	void languageChanged();
	void themeChanged();
	// The session layout appeared (session != null) or went away.
	void sessionShownChanged();

private:
	enum class Content {
		None,
		Gallery,
		Intro,
		Session,
	};

	void createAccount();
	void applyLanguage(Lang::Language language);
	void refreshPhrases();
	void rebuildContent();
	void showSession();
	void sessionChanged();
	void sessionEnded(Main::SessionEndReason reason);
	void checkUpgradeRequired();
	void windowActiveChanged();
	void applyBackground();

	void startDebugLogin();
	void debugLoginFailed(const QString &why);
	void runDebugScenario();
	void finishDebugScenario();

	Launcher *_launcher = nullptr;
	std::unique_ptr<QTemporaryDir> _temporaryDir;
	std::unique_ptr<Settings> _settings;
	std::unique_ptr<Main::Account> _account;
	std::unique_ptr<Window::MainWindow> _window;
	std::unique_ptr<DebugActions> _debugActions;
	QString _debugRoute;
	QString _pendingRoute;
	Content _content = Content::None;
	QString _galleryPage;
	QTimer _backgroundTimer;
	bool _debugLoginRunning = false;
	bool _debugScenarioStarted = false;
	bool _quitting = false;

};

[[nodiscard]] inline Application &App() {
	return Application::Instance();
}

} // namespace Core
