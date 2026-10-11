// The window of a call that rings here: a small window of its own above
// every other window, at the top trailing corner of the screen (the corner
// the message toasts do not use), so that it is there also when the main
// window is minimized or waits in the tray.
//
//   userpic · name · «تماس صوتی ورودی»            [ رد کردن ] [ پاسخ ]
//
// It never takes the keyboard by itself (somebody typing in another program
// must not answer a call with Enter); once it has the focus - a click on it,
// or Alt+Tab - Enter accepts and Esc declines. It can be dragged.
//
// Created and destroyed by Calls::Service as the call starts and stops
// ringing.
#pragma once

#include "ui/effects/animations.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>

#include <functional>

namespace Main {
class Session;
} // namespace Main

namespace Support {
class UserpicButton;
} // namespace Support

namespace Ui {
class RoundButton;
} // namespace Ui

namespace Calls {

class IncomingWindow final : public Ui::RpWidget {
public:
	IncomingWindow(
		Main::Session *session,
		const QString &callId,
		const QString &peerId,
		bool video = false);
	~IncomingWindow();

	[[nodiscard]] QString callId() const {
		return _callId;
	}
	// `withCamera`: «تصویری» of a video call; «پاسخ» / «فقط صدا» and the
	// Enter key answer with the microphone only.
	void setAcceptCallback(std::function<void(bool withCamera)> callback);
	// The intent became known after the window was shown (the signal that
	// rang did not say; the first poll did).
	void setVideo(bool video);
	[[nodiscard]] bool video() const {
		return _video;
	}
	void setDeclineCallback(std::function<void()> callback);

	void showAtCorner();

protected:
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] QRect bodyRect() const;
	void relayout();
	void refreshName();

	QPointer<Main::Session> _session;
	QString _callId;
	QString _peerId;
	QString _name;
	Support::UserpicButton *_userpic = nullptr;
	Ui::RoundButton *_accept = nullptr;
	Ui::RoundButton *_decline = nullptr;
	Ui::RoundButton *_acceptVideo = nullptr; // a video call: «تصویری»
	bool _video = false;
	std::function<void(bool withCamera)> _acceptCallback;
	std::function<void()> _declineCallback;
	Ui::Animations::Basic _pulse;
	QPoint _dragStart;
	QPoint _dragWindowStart;
	bool _dragging = false;

};

} // namespace Calls
