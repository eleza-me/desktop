// Core::Application — the application object: owns settings, applies language and
// theme, creates the main window and decides what it shows.
//
//   Core::App().settings()                  preferences (core_settings.h)
//   Core::App().setLanguage(...)            switches the language at runtime
//   Core::App().setThemeMode(...)           System / Light / Dark
//   Core::App().window()                    the main window
//   Core::App().debugRoute()                --debug-route=… (empty normally)
//   Core::App().apiUrl(), workDir()         for the network / storage layers
//
// What the window shows:
//   showGallery()        the UI gallery (--gallery)
//   showIntro()          the registered intro (sign in) widget, if a module registered one
//   showSession(session) the session layout: nav rail | side column | main column
//
// Without the core library the app starts straight into the session layout with
// placeholder tabs; the session pointer is null then.
#pragma once

#include "core/core_launcher.h"
#include "core/core_settings.h"

#include <QtCore/QObject>

#include <memory>

namespace Main {
class Session;
} // namespace Main

namespace Window {
class MainWindow;
} // namespace Window

namespace Core {

class Application final : public QObject {
	Q_OBJECT

public:
	explicit Application(Launcher *launcher);
	~Application();

	[[nodiscard]] static Application &Instance();
	[[nodiscard]] static bool Exists();

	// Applies settings, creates the window. Called once by the launcher.
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
	[[nodiscard]] QString workDir() const {
		return _launcher->workDir();
	}

	// Stored in settings and applied right away; the window content is rebuilt
	// for a language switch (the direction changes).
	void setLanguage(Lang::Language language);
	void setThemeMode(style::ThemeMode mode);
	// Light <-> dark, as a manual override.
	void toggleTheme();

	void showGallery(const QString &page = QString());
	void showIntro();
	void showSession(Main::Session *session);

	// A later launch of the app asked us to come to the front.
	void handleActivation(const QString &message);
	void quit();

Q_SIGNALS:
	void languageChanged();
	void themeChanged();
	// --debug-route or a route passed by a second launch.
	void routeRequested(QString route);

private:
	enum class Content {
		None,
		Gallery,
		Intro,
		Session,
	};

	void applyLanguage(Lang::Language language);
	void refreshPhrases();
	void rebuildContent();
	void scheduleDebugGrab();

	Launcher *_launcher = nullptr;
	std::unique_ptr<Settings> _settings;
	std::unique_ptr<Window::MainWindow> _window;
	QString _debugRoute;
	Content _content = Content::None;
	QString _galleryPage;
	Main::Session *_session = nullptr;

};

[[nodiscard]] inline Application &App() {
	return Application::Instance();
}

} // namespace Core
