// The report / block bar under a chat's header (Telegram's action bar): in a
// direct chat a stranger started, «مسدود کردن» and «گزارش هرزنامه»; in a
// group a stranger added me to, «X تو را به این گروه اضافه کرد» over «ترک
// گروه» and «گزارش هرزنامه»; and a × that hides it on all my devices. The
// rules and the order of the calls are Data::ActionBar's.
//
//   _actionBar = Ui::CreateChild<ActionBar>(this, controller, chatId);
//   connect(_actionBar, &ActionBar::shownChanged, this, [=] { updateLayout(); });
#pragma once

#include "data/data_action_bar.h"
#include "ui/rp_widget.h"

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class IconButton;
class RoundButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace History {

class ActionBar final : public Ui::RpWidget {
	Q_OBJECT

public:
	ActionBar(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId);

	// There is a bar to show.
	[[nodiscard]] bool shown() const {
		return _kind != Data::ActionBar::Kind::None;
	}

Q_SIGNALS:
	void shownChanged();
	// A choice that leaves the group is on its way (true): the screen
	// closes when it went through, and the bar says how it went. False: the
	// leave did not go through.
	void leaving(bool leaving);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refresh();
	void chooseBlock();
	void chooseReport();
	void chooseLeave();
	void chooseClose();
	void run(QVector<Data::ActionBar::Step> plan);
	[[nodiscard]] QString otherName() const;

	Window::SessionController *_controller = nullptr;
	Main::Session *_session = nullptr;
	QString _chatId;
	Data::ActionBar::Kind _kind = Data::ActionBar::Kind::None;
	QString _userId; // the peer, or who added me
	QString _line;   // a group: «X added you»
	bool _acting = false; // a choice runs: the bar stays away meanwhile

	Ui::RoundButton *_first = nullptr;  // «مسدود کردن» / «ترک گروه»
	Ui::RoundButton *_second = nullptr; // «گزارش هرزنامه»
	Ui::IconButton *_close = nullptr;

};

} // namespace History
