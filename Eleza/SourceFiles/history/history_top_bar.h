// The header of a chat: back, userpic, name (+ the verified tick), the status
// line (typing / presence / connection), search, mute and «…». With messages
// selected it turns into the selection bar: count, copy, forward, delete.
#pragma once

#include "ui/effects/animations.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <QtCore/QPointer>

namespace Main {
class Session;
} // namespace Main

namespace Support {
class ConnectionState;
class UserpicButton;
} // namespace Support

namespace Ui {
class IconButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace History {

class TopBar final : public Ui::RpWidget {
	Q_OBJECT

public:
	TopBar(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId);
	~TopBar();

	void setBackShown(bool shown);
	// count > 0 switches to the selection bar.
	void setSelection(int count, bool canForward);
	[[nodiscard]] Ui::IconButton *moreButton() const {
		return _more;
	}
	// The peer's user id when the title leads to a profile ("" otherwise).
	[[nodiscard]] QString profileUserId() const;

Q_SIGNALS:
	void backRequested();
	void profileRequested();
	void searchRequested();
	void moreRequested();
	void selectionCopy();
	void selectionForward();
	void selectionDelete();
	void selectionCancel();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refresh();
	void refreshStatus();
	void updateControls();
	void watchPeer(const QString &userId);
	[[nodiscard]] QRect titleRect() const; // logical (leading-side) coordinates
	[[nodiscard]] bool titleClickable() const;

	Window::SessionController *_controller = nullptr;
	Main::Session *_session = nullptr;
	QString _chatId;
	QPointer<Support::ConnectionState> _connection;

	Ui::IconButton *_back = nullptr;
	Support::UserpicButton *_userpic = nullptr;
	Ui::IconButton *_search = nullptr;
	Ui::IconButton *_mute = nullptr;
	Ui::IconButton *_more = nullptr;
	Ui::IconButton *_selectionCancel = nullptr;
	Ui::IconButton *_selectionCopy = nullptr;
	Ui::IconButton *_selectionForward = nullptr;
	Ui::IconButton *_selectionDelete = nullptr;

	Ui::Text::String _name;
	QString _nameText;
	QString _status;
	bool _statusActive = false;
	bool _typing = false;
	bool _verified = false;
	bool _topic = false;
	bool _muted = false;
	bool _backShown = false;
	bool _titleOver = false;
	bool _titlePressed = false;
	int _selected = 0;
	bool _selectedForward = true;
	QString _watched;
	QString _userpicFor;
	Ui::Animations::Basic _dots;

};

} // namespace History
