// Window::SectionWidget — the base of everything shown in the columns of the session
// layout: tab lists in the side column, chats / posts / profiles / settings pages in the
// main column.
//
//   class ChatSection : public Window::SectionWidget {
//   public:
//       ChatSection(QWidget *parent, Window::SessionController *controller, QString chatId)
//       : SectionWidget(parent, controller) {
//           _topBar = Ui::CreateChild<Ui::TopBar>(this);
//           setTopBar(_topBar);            // back button + Esc are wired for you
//           ...
//       }
//       QString sectionKey() const override { return u"chat/"_s + _chatId; }
//       void setInnerFocus() override { _composer->setFocusFast(); }
//       bool onBackRequest() override { return closeSearchIfOpen(); }
//   protected:
//       void resizeEvent(QResizeEvent *e) override { /* place top bar, scroll, composer */ }
//   };
//
//   controller->showMainSection([=](QWidget *parent, Window::SessionController *controller) {
//       return Ui::Owned<Window::SectionWidget>(new ChatSection(parent, controller, id));
//   });
//
// The layout resizes the section to its column; the section paints its own background
// (st::bgCanvas by default) and lays its children out in resizeEvent.
#pragma once

#include "ui/rp_widget.h"

#include <QtCore/QPointer>

namespace Ui {
class TopBar;
} // namespace Ui

namespace Window {

class SessionController;

class SectionWidget : public Ui::RpWidget {
	Q_OBJECT

public:
	SectionWidget(QWidget *parent, SessionController *controller);

	[[nodiscard]] SessionController *controller() const {
		return _controller;
	}

	// Identifies the content ("chat/42"). Showing a section whose key equals the
	// key of the current one does not push a duplicate. Empty = always push.
	[[nodiscard]] virtual QString sectionKey() const {
		return QString();
	}
	// The section became visible (the slide animation finished).
	virtual void showFinished() {
	}
	// Put the keyboard focus where typing should go.
	virtual void setInnerFocus() {
		setFocus();
	}
	// Esc / back button / mouse "back". Return true if the section handled it
	// itself (closed its search, left selection mode); false lets the layout
	// go back in the stack.
	[[nodiscard]] virtual bool onBackRequest() {
		return false;
	}
	// The layout tells whether the section must offer a way back (it is not
	// the first in its stack, or the window is in the single column mode).
	// With setTopBar() this shows / hides the bar's back button.
	virtual void setBackButtonShown(bool shown);
	[[nodiscard]] bool backButtonShown() const {
		return _backButtonShown;
	}
	// Background painted by the default paintEvent.
	void setBackground(style::color color);

protected:
	// Registers the section's top bar: its back button follows
	// setBackButtonShown() and calls controller()->showBack().
	void setTopBar(Ui::TopBar *bar);
	[[nodiscard]] Ui::TopBar *topBar() const;

	void paintEvent(QPaintEvent *e) override;

private:
	SessionController *_controller = nullptr;
	QPointer<Ui::TopBar> _topBar;
	bool _backButtonShown = false;
	style::color _bg;

};

} // namespace Window
