#include "window/window_main_window.h"

#include "core/core_application.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/layers/box_content.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "window/window_main_layout.h"
#include "window/window_section_widget.h"
#include "window/window_shortcuts.h"
#include "window/window_upgrade_blocker.h"
#include "window/window_session_controller.h"
#include "window/window_title_bar.h"
#include "window/window_title_frame.h"
#include "styles/style_window.h"

#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QShortcut>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSystemTrayIcon>

namespace Window {
namespace {

constexpr auto kGeometrySaveDelay = 500;

[[nodiscard]] QIcon BaseIcon() {
	auto result = QIcon();
	for (const auto size : { 16, 24, 32, 48, 64, 256 }) {
		result.addFile(
			QStringLiteral(":/gui/art/icon%1.png").arg(size),
			QSize(size, size));
	}
	return result;
}

} // namespace

MainWindow::MainWindow()
: RpWidget(nullptr)
, _body(new Ui::RpWidget(this))
, _layers(new Ui::LayerStackWidget(_body))
, _baseIcon(BaseIcon()) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setMinimumSize(st::windowMinWidth, st::windowMinHeight);
	resize(st::windowDefaultWidth, st::windowDefaultHeight);
	setWindowIcon(_baseIcon);
	refreshTitle();

	_geometryTimer.setSingleShot(true);
	_geometryTimer.setInterval(kGeometrySaveDelay);
	QObject::connect(&_geometryTimer, &QTimer::timeout, this, [this] {
		saveGeometryNow();
	});

	QObject::connect(_body, &Ui::RpWidget::paintRequest, _body, [this] {
		auto p = QPainter(_body);
		p.fillRect(_body->rect(), st::bgCanvas);
	});

	// The keyboard goes back to the content when the last box closes.
	QObject::connect(
		_layers,
		&Ui::LayerStackWidget::layersShownChanged,
		this,
		[this](bool shown) {
			if (shown || !_content || _upgrade) {
				return;
			} else if (const auto layout = dynamic_cast<MainLayout*>(
					_content.data())) {
				layout->setInnerFocus();
			} else {
				_content->setFocus();
			}
		});

	setupShortcuts();
	if (!Core::App().commandLine().ephemeral()) {
		setupTray();
	}
	setupTitleBar();
}

MainWindow::~MainWindow() {
	saveGeometryNow();
	// Overlays die with the body, after everything here: they must not call
	// back by then.
	for (const auto &overlay : _darkOverlays) {
		if (overlay) {
			overlay->removeEventFilter(this);
			QObject::disconnect(overlay, nullptr, this, nullptr);
		}
	}
	_darkOverlays.clear();
	if (_titleJoin) {
		_titleJoin->removeEventFilter(this);
	}
	_titleFrame = nullptr;
	// Sections refer to the controller: destroy them first.
	_content.destroy();
	_controller = nullptr;
}

void MainWindow::setContent(Ui::object_ptr<Ui::RpWidget> content) {
	_layers->hideLayers(anim::type::instant);
	if (const auto was = _content.release()) {
		was->hide();
		was->deleteLater();
	}
	_content = std::move(content);
	if (_content) {
		_content->setParent(_body);
		_content->setGeometry(_body->rect());
		_content->show();
		_content->lower();
		_content->setFocus();
	}
	_layers->raise();
	if (_upgrade) {
		_upgrade->raise();
	}
	updateTitleJoin();
}

void MainWindow::showUpgradeRequired(const QString &updateUrl) {
	if (_upgrade) {
		return;
	}
	_layers->hideLayers(anim::type::instant);
	_upgrade = new UpgradeBlocker(_body, updateUrl);
	_upgrade->setGeometry(_body->rect());
	_upgrade->show();
	_upgrade->raise();
	_upgrade->setFocus();
	updateTitleJoin();
}

void MainWindow::showSession(Main::Session *session) {
	_layers->hideLayers(anim::type::instant);
	_content.destroy();
	_controller = std::make_unique<SessionController>(this, session);
	const auto layout = new MainLayout(_body, _controller.get());
	setContent(Ui::Owned<Ui::RpWidget>(layout));
	layout->setInnerFocus();
	if (_title) {
		// The rail is of the bar's surface: they join without a line.
		_titleJoin = layout->navRail();
		_titleJoin->installEventFilter(this);
		updateTitleJoin();
	}
}

void MainWindow::destroySession() {
	if (_controller) {
		_layers->hideLayers(anim::type::instant);
		_content.destroy();
		_controller = nullptr;
	}
}

void MainWindow::show(
		Ui::object_ptr<Ui::BoxContent> box,
		Ui::LayerOption option) {
	_layers->showBox(std::move(box), option);
}

void MainWindow::showSheet(
		Ui::object_ptr<Ui::BoxContent> content,
		Ui::LayerOption option) {
	_layers->showSheet(std::move(content), option);
}

void MainWindow::showToast(const QString &text) {
	Ui::Toast::Show(_body, text);
}

void MainWindow::refreshTitle() {
	const auto name = tr::lng_app_name(tr::now);
	setWindowTitle((_unreadCount > 0)
		? (QStringLiteral("(%1) ").arg(Lang::Num(_unreadCount)) + name)
		: name);
	if (_title) {
		_title->setTitle(windowTitle());
	}
	refreshTrayMenu();
}

void MainWindow::setUnreadCount(int count, bool muted) {
	count = std::max(count, 0);
	if (_unreadCount == count && _unreadMuted == muted) {
		return;
	}
	_unreadCount = count;
	_unreadMuted = muted;
	refreshTitle();
	refreshIcons();
}

QIcon MainWindow::iconWithBadge() const {
	if (_unreadCount <= 0) {
		return _baseIcon;
	}
	auto result = QIcon();
	const auto text = (_unreadCount > 99)
		? QStringLiteral("99+")
		: QString::number(_unreadCount);
	for (const auto size : { 16, 24, 32, 48, 64 }) {
		auto pixmap = _baseIcon.pixmap(QSize(size, size));
		pixmap.setDevicePixelRatio(1.);
		auto p = QPainter(&pixmap);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::TextAntialiasing);
		// A badge on the bottom right quarter; small sizes get just a dot.
		const auto diameter = (size <= 24) ? (size * 0.5) : (size * 0.56);
		const auto badge = QRectF(
			pixmap.width() - diameter,
			pixmap.height() - diameter,
			diameter,
			diameter);
		p.setPen(Qt::NoPen);
		p.setBrush(_unreadMuted ? QColor(0x85, 0x89, 0x7F) : QColor(0xD6, 0x4F, 0x45));
		p.drawEllipse(badge);
		if (size > 24) {
			auto font = QFont(style::TextFontFamily());
			font.setPixelSize(int(diameter * (text.size() > 2 ? 0.42 : 0.6)));
			font.setWeight(QFont::Bold);
			p.setFont(font);
			p.setPen(Qt::white);
			p.drawText(badge, Qt::AlignCenter, text);
		}
		p.end();
		result.addPixmap(pixmap);
	}
	return result;
}

void MainWindow::refreshIcons() {
	const auto icon = iconWithBadge();
	setWindowIcon(icon);
	if (_tray) {
		_tray->setIcon(icon);
		_tray->setToolTip((_unreadCount > 0)
			? tr::lng_unread_messages(tr::now, lt_count, _unreadCount)
			: tr::lng_app_name(tr::now));
	}
}

void MainWindow::setupShortcuts() {
	// The one table of the app's shortcuts (window_shortcuts.h).
	for (const auto &entry : Shortcuts()) {
		const auto shortcut = new QShortcut(entry.keys, this);
		shortcut->setContext(Qt::WindowShortcut);
		const auto command = entry.command;
		const auto run = [this, command] { runCommand(command); };
		QObject::connect(shortcut, &QShortcut::activated, this, run);
		QObject::connect(shortcut, &QShortcut::activatedAmbiguously, this, run);
	}
}

void MainWindow::runCommand(Command command) {
	switch (command) {
	case Command::Quit:
		Core::App().quit();
		return;
	case Command::ToggleTheme:
		Core::App().toggleTheme();
		return;
	case Command::Back:
		handleBack();
		return;
	case Command::Close:
		if (_upgrade) {
			close();
		} else if (_layers->hasLayers()) {
			_layers->hideTopLayer();
		} else if (!_controller
			|| !_controller->mainSection()
			|| !_controller->showBack()) {
			close();
		}
		return;
	default: break;
	}
	if (!_controller || _upgrade || _layers->hasLayers()) {
		// The intro and the boxes have their own keys.
		return;
	}
	const auto tab = [&]() -> std::optional<Tab> {
		switch (command) {
		case Command::ShowTabEli: return Tab::Eli;
		case Command::ShowTabChats: return Tab::Chats;
		case Command::ShowTabSearch: return Tab::Search;
		case Command::ShowTabMe: return Tab::Me;
		case Command::ShowTabSettings: return Tab::Settings;
		default: return std::nullopt;
		}
	}();
	if (tab) {
		_controller->showTab(*tab);
		return;
	}
	// The section with the keyboard focus first, then the other visible one.
	const auto side = _controller->isSingleColumn() && _controller->mainSection()
		? nullptr
		: _controller->sideSection(_controller->currentTab());
	const auto main = _controller->mainSection();
	const auto focused = QApplication::focusWidget();
	const auto sideFocused = side
		&& focused
		&& (focused == side || side->isAncestorOf(focused));
	const auto first = sideFocused ? side : main;
	const auto second = sideFocused ? main : side;
	if ((first && first->handleCommand(command))
		|| (second && second->handleCommand(command))) {
		return;
	}
	[[maybe_unused]] const auto handled = RunCommandHandlers(
		command,
		_controller.get());
}

void MainWindow::setupTray() {
	if (!QSystemTrayIcon::isSystemTrayAvailable()) {
		return;
	}
	_tray = new QSystemTrayIcon(_baseIcon, this);
	_trayMenu = std::make_unique<QMenu>();
	_tray->setContextMenu(_trayMenu.get());
	refreshTrayMenu();
	QObject::connect(
		_tray,
		&QSystemTrayIcon::activated,
		this,
		[this](QSystemTrayIcon::ActivationReason reason) {
			if (reason == QSystemTrayIcon::Context) {
				return;
			} else if (isVisible() && isActiveWindow()) {
				hide();
			} else {
				activateAndShow();
			}
		});
	_tray->setToolTip(tr::lng_app_name(tr::now));
	_tray->show();
}

void MainWindow::refreshTrayMenu() {
	if (!_trayMenu) {
		return;
	}
	_trayMenu->clear();
	_trayMenu->setLayoutDirection(Ui::LayoutDirection());
	_trayMenu->addAction(tr::lng_tray_open(tr::now), this, [this] {
		activateAndShow();
	});
	_trayMenu->addSeparator();
	_trayMenu->addAction(tr::lng_tray_quit(tr::now), this, [] {
		Core::App().quit();
	});
}

void MainWindow::showFirstTime() {
	if (!_debugSize) {
		restoreGeometry();
	}
	const auto geometry = Core::App().settings().windowGeometry();
	if (!_debugSize && geometry.maximized) {
		showMaximized();
	} else {
		QWidget::show();
	}
}

void MainWindow::activateAndShow() {
	if (isMinimized()) {
		setWindowState(windowState() & ~Qt::WindowMinimized);
	}
	QWidget::show();
	raise();
	activateWindow();
	if (const auto handle = windowHandle()) {
		handle->requestActivate();
	}
}

void MainWindow::restoreGeometry() {
	const auto saved = Core::App().settings().windowGeometry();
	auto geometry = saved.rect;
	const auto screens = QGuiApplication::screens();
	auto screen = static_cast<QScreen*>(nullptr);
	if (!geometry.isNull()) {
		for (const auto candidate : screens) {
			if (candidate->availableGeometry().intersects(geometry)) {
				screen = candidate;
				break;
			}
		}
	}
	if (!screen) {
		screen = QGuiApplication::primaryScreen();
		geometry = QRect();
	}
	if (!screen) {
		return;
	}
	const auto available = screen->availableGeometry();
	if (geometry.isNull()) {
		const auto size = QSize(
			std::min(int(st::windowDefaultWidth), available.width()),
			std::min(int(st::windowDefaultHeight), available.height()));
		geometry = QRect(
			available.x() + (available.width() - size.width()) / 2,
			available.y() + (available.height() - size.height()) / 2,
			size.width(),
			size.height());
	} else {
		geometry.setWidth(std::clamp(
			geometry.width(),
			int(st::windowMinWidth),
			std::max(available.width(), int(st::windowMinWidth))));
		geometry.setHeight(std::clamp(
			geometry.height(),
			int(st::windowMinHeight),
			std::max(available.height(), int(st::windowMinHeight))));
	}
	setGeometry(geometry);
	_positioned = true;
}

void MainWindow::scheduleGeometrySave() {
	if (_positioned && !_debugSize && isVisible()) {
		_geometryTimer.start();
	}
}

void MainWindow::saveGeometryNow() {
	_geometryTimer.stop();
	if (!_positioned || _debugSize || !Core::Application::Exists()) {
		return;
	}
	auto geometry = Core::App().settings().windowGeometry();
	geometry.maximized = isMaximized();
	if (!isMaximized() && !isMinimized() && !isFullScreen()) {
		geometry.rect = this->geometry();
	}
	Core::App().settings().setWindowGeometry(geometry);
}

#ifdef ELEZA_DEBUG_TOOLS
void MainWindow::setFixedDebugSize(QSize size) {
	_debugSize = true;
	setMinimumSize(1, 1);
	// The size of the body: the same content with and without the bar.
	resize(size.width(), size.height() + titleHeight());
}

QImage MainWindow::grabForDebug() {
	Ui::SendPendingMoveResizeEvents(this);
	return Ui::GrabWidgetToImage(this, QRect(), st::bgCanvas->c);
}
#endif // ELEZA_DEBUG_TOOLS

void MainWindow::handleBack() {
	if (_upgrade) {
		return;
	} else if (_layers->hasLayers()) {
		_layers->hideTopLayer();
	} else if (_controller) {
		_controller->showBack();
	}
}

void MainWindow::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateBodyGeometry();
	scheduleGeometrySave();
}

void MainWindow::showEvent(QShowEvent *e) {
	RpWidget::showEvent(e);
	updateTitleState();
	emit activeChanged();
}

void MainWindow::hideEvent(QHideEvent *e) {
	RpWidget::hideEvent(e);
	emit activeChanged();
}

void MainWindow::moveEvent(QMoveEvent *e) {
	Q_UNUSED(e);
	scheduleGeometrySave();
}

void MainWindow::closeEvent(QCloseEvent *e) {
	saveGeometryNow();
	if (_tray && Core::App().settings().closeToTray()) {
		// Stay alive in the tray.
		e->ignore();
		hide();
		return;
	}
	e->accept();
	Core::App().quit();
}

void MainWindow::changeEvent(QEvent *e) {
	if (e->type() == QEvent::WindowStateChange) {
		scheduleGeometrySave();
		updateTitleState();
		emit activeChanged();
	} else if (e->type() == QEvent::ActivationChange) {
		updateTitleState();
		emit activeChanged();
	}
	RpWidget::changeEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) {
		handleBack();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void MainWindow::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::BackButton) {
		handleBack();
	} else {
		RpWidget::mousePressEvent(e);
	}
}

// ---- the custom title bar

void MainWindow::setupTitleBar() {
	if (!UseCustomTitleBar()) {
		return;
	}
	_title = new TitleBar(this);
	_titleFrame = CreateTitleFrame(this, _title, [this] {
		// The platform frame gave the window back to the system.
		_titleLost = true;
		updateTitleState();
	});
#ifdef ELEZA_DEBUG_TOOLS
	// A grab of the bar is possible without a window system behind it.
	const auto keep = Core::App().commandLine().debugTitleBar;
#else // ELEZA_DEBUG_TOOLS
	constexpr auto keep = false;
#endif // ELEZA_DEBUG_TOOLS
	if (!_titleFrame && !keep) {
		delete std::exchange(_title, nullptr);
		return;
	}
	_title->setTitle(windowTitle());
	QObject::connect(_title, &TitleBar::minimizeRequested, this, [this] {
		showMinimized();
	});
	QObject::connect(_title, &TitleBar::maximizeToggleRequested, this, [this] {
		if (isMaximized()) {
			showNormal();
		} else {
			showMaximized();
		}
	});
	// The same way out as the close button of the system: closeEvent().
	QObject::connect(_title, &TitleBar::closeRequested, this, [this] {
		close();
	});
	style::OnPaletteChanged(this, [this] {
		if (_titleFrame) {
			_titleFrame->paletteChanged();
		}
	});
	updateMinimumSize();
	updateTitleState();
	updateBodyGeometry();
#ifdef ELEZA_DEBUG_TOOLS
	setDebugTitleState(Core::App().commandLine().debugTitleState);
#endif // ELEZA_DEBUG_TOOLS
}

int MainWindow::titleHeight() const {
	return (_title && !_title->isHidden()) ? int(st::titleBarHeight) : 0;
}

void MainWindow::updateMinimumSize() {
	if (!_debugSize) {
		setMinimumSize(
			st::windowMinWidth,
			st::windowMinHeight + titleHeight());
	}
}

void MainWindow::updateBodyGeometry() {
	const auto top = titleHeight();
	if (top > 0) {
		_title->setGeometry(0, 0, width(), top);
		_title->raise();
	}
	_body->setGeometry(0, top, width(), height() - top);
	if (_content) {
		_content->setGeometry(_body->rect());
	}
	if (_upgrade) {
		_upgrade->setGeometry(_body->rect());
	}
	if (_titleFrame) {
		_titleFrame->windowChanged();
	}
}

void MainWindow::updateTitleState() {
	if (!_title) {
		return;
	}
	const auto shown = !_titleLost && !isFullScreen();
	if (_title->isHidden() == shown) {
		_title->setVisible(shown);
		updateMinimumSize();
		updateBodyGeometry();
	} else if (_titleFrame) {
		_titleFrame->windowChanged();
	}
	// Screenshot runs have no window manager to activate the window.
	auto active = isActiveWindow() || Core::App().commandLine().ephemeral();
	auto maximized = isMaximized();
#ifdef ELEZA_DEBUG_TOOLS
	active = _debugTitleActive.value_or(active);
	maximized = _debugTitleMaximized.value_or(maximized);
#endif // ELEZA_DEBUG_TOOLS
	_title->setActive(active);
	_title->setMaximized(maximized);
}

void MainWindow::updateTitleJoin() {
	if (!_title) {
		return;
	}
	const auto rail = _titleJoin.data();
	_title->setJoinedWidth((rail && !_upgrade && rail->isVisibleTo(_body))
		? rail->width()
		: 0);
}

void MainWindow::registerDarkOverlay(QWidget *overlay) {
	if (!_title || !overlay) {
		return;
	}
	_darkOverlays.push_back(overlay);
	overlay->installEventFilter(this);
	QObject::connect(overlay, &QObject::destroyed, this, [this] {
		updateTitleDark();
	});
	updateTitleDark();
}

void MainWindow::updateTitleDark() {
	if (!_title) {
		return;
	}
	auto dark = false;
	for (auto i = _darkOverlays.begin(); i != _darkOverlays.end();) {
		if (!*i) {
			i = _darkOverlays.erase(i);
		} else {
			dark = dark || !(*i)->isHidden();
			++i;
		}
	}
#ifdef ELEZA_DEBUG_TOOLS
	dark = dark || _debugTitleDark;
#endif // ELEZA_DEBUG_TOOLS
	_title->setDarkOverlay(dark);
	if (_titleFrame) {
		_titleFrame->paletteChanged();
	}
}

#ifdef ELEZA_DEBUG_TOOLS
void MainWindow::setDebugTitleState(const QString &states) {
	if (!_title) {
		return;
	}
	using Button = TitleBar::Button;
	const auto button = [](const QString &name) {
		return (name == QStringLiteral("min"))
			? Button::Minimize
			: (name == QStringLiteral("max"))
			? Button::Maximize
			: (name == QStringLiteral("close"))
			? Button::Close
			: Button::None;
	};
	_debugTitleActive = std::nullopt;
	_debugTitleMaximized = std::nullopt;
	_debugTitleDark = false;
	auto over = Button::None;
	auto down = Button::None;
	const auto hover = QStringLiteral("hover-");
	const auto press = QStringLiteral("press-");
	for (const auto &state : states.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		if (state == QStringLiteral("inactive")) {
			_debugTitleActive = false;
		} else if (state == QStringLiteral("active")) {
			_debugTitleActive = true;
		} else if (state == QStringLiteral("maximized")) {
			_debugTitleMaximized = true;
		} else if (state == QStringLiteral("dark")) {
			_debugTitleDark = true;
		} else if (state.startsWith(hover)) {
			over = button(state.mid(hover.size()));
		} else if (state.startsWith(press)) {
			over = down = button(state.mid(press.size()));
		}
	}
	updateTitleState();
	updateTitleDark();
	_title->setNativeOver(over, false);
	_title->setNativePressed(down);
}
#endif // ELEZA_DEBUG_TOOLS

bool MainWindow::event(QEvent *e) {
	if (e->type() == QEvent::WinIdChange && _titleFrame) {
		_titleFrame->nativeWindowChanged();
	}
	return RpWidget::event(e);
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
	switch (event->type()) {
	case QEvent::Show:
	case QEvent::Hide:
	case QEvent::Resize:
		if (object == _titleJoin.data()) {
			updateTitleJoin();
		} else if (event->type() != QEvent::Resize) {
			updateTitleDark();
		}
		break;
	default: break;
	}
	return RpWidget::eventFilter(object, event);
}

bool MainWindow::nativeEvent(
		const QByteArray &eventType,
		void *message,
		qintptr *result) {
	if (_titleFrame && _titleFrame->nativeEvent(message, result)) {
		return true;
	}
	return RpWidget::nativeEvent(eventType, message, result);
}

} // namespace Window
