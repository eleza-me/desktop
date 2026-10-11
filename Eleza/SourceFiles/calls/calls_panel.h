// A call inside the main window.
//
//   Calls::Bar    the strip between the title bar and the content, on every
//                 tab, for as long as this device is in a call (and a moment
//                 after it, to say how it ended):
//
//                 userpic  name                         [ mute ] [ end ]
//                          در حال زنگ زدن… / ۰۲:۱۵
//
//                 A click on it opens the expanded view (calls_view.h).
//   Calls::Host   what a session layout gets from Calls::Service: it creates
//                 the bar and the view for that layout and removes them with
//                 it (a language switch rebuilds the layout during a call).
//
// A call that only rings here has a window of its own instead
// (calls_incoming_window.h).
#pragma once

#include "ui/effects/animations.h"
#include "ui/widgets/buttons.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace Support {
class UserpicButton;
} // namespace Support

namespace Window {
class MainWindow;
class SessionController;
} // namespace Window

namespace Calls {

class Button;
class Service;
class View;

class Bar final : public Ui::AbstractButton {
public:
	Bar(QWidget *parent, Service *service);

	void refresh();
	// Slides in / out; hidden it takes no height.
	void setShown(bool shown);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void updateControls();
	void updateHeight();

	Service *_service = nullptr;
	Support::UserpicButton *_userpic = nullptr;
	Button *_mute = nullptr;
	Button *_end = nullptr;
	Button *_close = nullptr;
	Ui::RoundButton *_again = nullptr;
	Ui::Animations::Simple _shownAnimation;
	QTimer _clock;
	QString _peerId;
	QString _name;
	bool _shown = false;

};

class Host final : public QObject {
public:
	Host(Service *service, Window::SessionController *controller);
	~Host();

private:
	void refresh();
	void updateViewGeometry();

	Service *_service = nullptr;
	// The window outlives the layout this object belongs to.
	QPointer<Window::MainWindow> _window;
	QPointer<Bar> _bar;
	QPointer<View> _view;

};

} // namespace Calls
