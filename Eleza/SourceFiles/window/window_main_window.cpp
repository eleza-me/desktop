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
}

MainWindow::~MainWindow() {
	saveGeometryNow();
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
}

void MainWindow::showSession(Main::Session *session) {
	_layers->hideLayers(anim::type::instant);
	_content.destroy();
	_controller = std::make_unique<SessionController>(this, session);
	const auto layout = new MainLayout(_body, _controller.get());
	setContent(Ui::Owned<Ui::RpWidget>(layout));
	layout->setInnerFocus();
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
	resize(size);
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
	_body->setGeometry(rect());
	if (_content) {
		_content->setGeometry(_body->rect());
	}
	if (_upgrade) {
		_upgrade->setGeometry(_body->rect());
	}
	scheduleGeometrySave();
}

void MainWindow::showEvent(QShowEvent *e) {
	RpWidget::showEvent(e);
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
		emit activeChanged();
	} else if (e->type() == QEvent::ActivationChange) {
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

} // namespace Window
