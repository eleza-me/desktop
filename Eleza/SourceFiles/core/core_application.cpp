#include "core/core_application.h"

#include "core/core_modules.h"
#include "gallery/gallery_widget.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/style/style_core.h"
#include "ui/ui_phrases.h"
#include "ui/ui_utility.h"
#include "window/window_main_window.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

namespace Core {
namespace {

Application *InstanceValue = nullptr;

[[nodiscard]] QString FormatDigits(const QString &latin) {
	return Lang::Num(latin);
}

} // namespace

Application::Application(Launcher *launcher)
: _launcher(launcher)
, _debugRoute(launcher->commandLine().debugRoute) {
	InstanceValue = this;
	const auto &arguments = launcher->commandLine();
	_settings = std::make_unique<Settings>(arguments.ephemeral()
		? QString()
		: QDir(launcher->workDir()).filePath(QStringLiteral("settings.json")));
}

Application::~Application() {
	_window = nullptr;
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
	}

	RegisterModules();

	_window = std::make_unique<Window::MainWindow>();
	if (arguments.debugSize.isValid()) {
		_window->setFixedDebugSize(arguments.debugSize);
	}
	if (arguments.gallery) {
		showGallery(arguments.galleryPage);
	} else if (Window::HasIntro() && !_session) {
		// The module that registered the intro decides when to switch to the
		// session (Core::App().showSession).
		showIntro();
	} else {
		showSession(nullptr);
	}
	_window->showFirstTime();

	if (!_debugRoute.isEmpty()) {
		const auto route = _debugRoute;
		QTimer::singleShot(0, this, [this, route] {
			emit routeRequested(route);
		});
	}
	if (!arguments.debugGrab.isEmpty()) {
		scheduleDebugGrab();
	}
}

void Application::scheduleDebugGrab() {
	const auto &arguments = _launcher->commandLine();
	const auto path = arguments.debugGrab;
	QTimer::singleShot(arguments.debugGrabDelay, this, [this, path] {
		auto image = _window->grabForDebug();
		if (!image.save(path)) {
			qWarning("Could not save the grab to '%s'.", qPrintable(path));
			QCoreApplication::exit(1);
			return;
		}
		QCoreApplication::exit(0);
	});
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
	// The layout direction changed: rebuild everything the window shows.
	rebuildContent();
	emit languageChanged();
}

void Application::setThemeMode(style::ThemeMode mode) {
	_settings->setThemeMode(mode);
	style::SetThemeMode(mode);
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
	case Content::Gallery: showGallery(_galleryPage); break;
	case Content::Intro: showIntro(); break;
	case Content::Session: showSession(_session); break;
	case Content::None: break;
	}
}

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

void Application::showIntro() {
	if (!Window::HasIntro()) {
		showSession(nullptr);
		return;
	}
	_content = Content::Intro;
	_window->destroySession();
	_window->setContent(Window::CreateIntro(_window->bodyWidget(), _window.get()));
}

void Application::showSession(Main::Session *session) {
	_content = Content::Session;
	_session = session;
	_window->showSession(session);
}

void Application::handleActivation(const QString &message) {
	if (_window) {
		_window->activateAndShow();
	}
	const auto prefix = QStringLiteral("route:");
	if (message.startsWith(prefix)) {
		emit routeRequested(message.mid(prefix.size()));
	}
}

void Application::quit() {
	if (_settings) {
		_settings->flush();
	}
	QCoreApplication::quit();
}

} // namespace Core
