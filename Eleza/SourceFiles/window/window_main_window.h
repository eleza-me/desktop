// Window::MainWindow — the application window.
//
//  * Title «الزا» / "Eleza", minimal size, remembers its geometry.
//  * A tray icon with a menu; closing the window keeps the app in the tray when the
//    "close to tray" setting is on.
//  * setUnreadCount() paints a badge on the taskbar / tray icon.
//  * bodyWidget() hosts the content (intro, session layout or gallery); layers()
//    is the modal layer stack over it; toasts are shown over it.
//  * With the custom title bar (window_title_bar.h, window_title_frame.h) the
//    bar lies above the body and is not a part of it: everything that covers
//    "the window" covers the body, the bar stays reachable. The minimum size,
//    the saved geometry and the default size are those of the whole window;
//    --debug-size is the size of the body, so grabs made with and without the
//    bar show the same content.
//
// Feature code reaches it through Window::SessionController::window().
#pragma once

#include "ui/layers/layer_widget.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QIcon>

#include <memory>
#include <optional>
#include <vector>

class QMenu;
class QSystemTrayIcon;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class BoxContent;
} // namespace Ui

namespace Window {

class MainLayout;
class SessionController;
class TitleBar;
class TitleFrame;
enum class Command;

class MainWindow final : public Ui::RpWidget {
	Q_OBJECT

public:
	MainWindow();
	~MainWindow();

	[[nodiscard]] Ui::RpWidget *bodyWidget() const {
		return _body;
	}
	[[nodiscard]] Ui::LayerStackWidget *layers() const {
		return _layers;
	}

	// Replaces what the body shows (the previous content is destroyed).
	void setContent(Ui::object_ptr<Ui::RpWidget> content);
	// Creates the session layout for the signed-in session.
	void showSession(Main::Session *session);
	void destroySession();
	[[nodiscard]] SessionController *sessionController() const {
		return _controller.get();
	}

	void show(
		Ui::object_ptr<Ui::BoxContent> box,
		Ui::LayerOption option = Ui::LayerOption::KeepOther);
	void showSheet(
		Ui::object_ptr<Ui::BoxContent> content,
		Ui::LayerOption option = Ui::LayerOption::KeepOther);
	void showToast(const QString &text);

	// First show: restores the saved geometry.
	void showFirstTime();
	// Brings the window to the front (tray click, second launch, notification).
	void activateAndShow();
	void refreshTitle();

	// Unread counter for the taskbar / tray icon and the title; 0 clears it.
	void setUnreadCount(int count, bool muted = false);
	[[nodiscard]] int unreadCount() const {
		return _unreadCount;
	}

	// The server refuses this build: a blocking screen with the update link
	// covers everything until the app is restarted.
	void showUpgradeRequired(const QString &updateUrl);
	[[nodiscard]] bool upgradeRequired() const {
		return _upgrade != nullptr;
	}

	// A dark overlay over the whole body (a media viewer): the custom title
	// bar is dark while the widget is shown.
	void registerDarkOverlay(QWidget *overlay);

	// A strip between the title bar and the body (the bar of a call): the
	// window gives it its own height at the full width and the body what is
	// left below; showing, hiding or resizing the strip moves the body.
	// Like the title bar it is not a part of the body: boxes and viewers
	// never cover it. The window does not own it; null removes it.
	void setTopStrip(QWidget *strip);

#ifdef ELEZA_DEBUG_TOOLS
	// Screenshot helpers (--debug-size / --debug-grab).
	void setFixedDebugSize(QSize size);
	[[nodiscard]] QImage grabForDebug();
	// "inactive,maximized,dark,hover-close,press-max": what the custom title
	// bar shows in a grab (--debug-title-bar=…, the route debug/title/…).
	void setDebugTitleState(const QString &states);
#endif // ELEZA_DEBUG_TOOLS

Q_SIGNALS:
	// The window was activated / deactivated, minimized, hidden or shown.
	void activeChanged();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void moveEvent(QMoveEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void closeEvent(QCloseEvent *e) override;
	void changeEvent(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	bool event(QEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;
	bool nativeEvent(
		const QByteArray &eventType,
		void *message,
		qintptr *result) override;

private:
	void setupTitleBar();
	void updateTitleState();
	void updateTitleJoin();
	void updateTitleDark();
	void updateMinimumSize();
	void updateBodyGeometry();
	[[nodiscard]] int titleHeight() const;
	void setupShortcuts();
	void setupTray();
	void refreshTrayMenu();
	void refreshIcons();
	void scheduleGeometrySave();
	void saveGeometryNow();
	void restoreGeometry();
	void handleBack();
	void runCommand(Command command);
	[[nodiscard]] QIcon iconWithBadge() const;

	Ui::RpWidget *_body = nullptr;
	Ui::LayerStackWidget *_layers = nullptr;
	Ui::object_ptr<Ui::RpWidget> _content;
	Ui::RpWidget *_upgrade = nullptr;
	std::unique_ptr<SessionController> _controller;
	QSystemTrayIcon *_tray = nullptr;
	std::unique_ptr<QMenu> _trayMenu;
	QTimer _geometryTimer;
	QIcon _baseIcon;
	int _unreadCount = 0;
	bool _unreadMuted = false;
	bool _debugSize = false;
	bool _positioned = false;

	TitleBar *_title = nullptr;
	std::unique_ptr<TitleFrame> _titleFrame;
	QPointer<QWidget> _titleJoin;
	QPointer<QWidget> _topStrip;
	bool _inBodyGeometry = false;
	std::vector<QPointer<QWidget>> _darkOverlays;
	bool _titleLost = false;
#ifdef ELEZA_DEBUG_TOOLS
	std::optional<bool> _debugTitleActive;
	std::optional<bool> _debugTitleMaximized;
	bool _debugTitleDark = false;
#endif // ELEZA_DEBUG_TOOLS

};

} // namespace Window
