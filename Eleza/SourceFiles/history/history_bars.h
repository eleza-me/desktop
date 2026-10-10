// The bars around the message list: the pinned message under the top bar, the
// anonymous conversation's banner, search in the chat, and the notice that
// replaces the composer when one cannot write (a message request, a blocked
// user, a finished conversation).
#pragma once

#include "data/data_types.h"
#include "ui/image/image_source.h"
#include "ui/rp_widget.h"

#include <QtCore/QBasicTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class IconButton;
class RoundButton;
class SearchField;
} // namespace Ui

namespace style {
struct RoundButton;
} // namespace style

namespace Window {
class SessionController;
} // namespace Window

namespace History {

// ---- pinned messages

class PinnedBar final : public Ui::RpWidget {
	Q_OBJECT

public:
	PinnedBar(QWidget *parent, Main::Session *session, const QString &chatId);

	[[nodiscard]] bool hasPins() const {
		return !_pins.isEmpty();
	}

Q_SIGNALS:
	void jumpRequested(qint64 seq);
	void unpinRequested(const Data::Message &message);
	// Appeared / disappeared: the section lays itself out again.
	void pinsChanged();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refresh();
	[[nodiscard]] const Data::PinnedMessage *current() const;
	void refreshThumb();

	Main::Session *_session = nullptr;
	QString _chatId;
	QVector<Data::PinnedMessage> _pins; // newest first
	int _index = 0;
	bool _pressed = false;
	Ui::IconButton *_unpin = nullptr;
	QTimer _reloadTimer; // the list is read again after a pin
	qint64 _requestedAt = 0;
	std::shared_ptr<Ui::ImageSource> _thumb;
	QMetaObject::Connection _thumbConnection;
	qint64 _thumbSeq = 0;
	QImage _thumbPrepared;
	qint64 _thumbPreparedKey = 0;

};

// ---- the banner of an anonymous conversation

class ConversationBanner final : public Ui::RpWidget {
	Q_OBJECT

public:
	ConversationBanner(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId);

	// Nothing to show (a direct chat, an unknown state).
	[[nodiscard]] bool empty() const {
		return _state == State::None;
	}

Q_SIGNALS:
	void stateChanged();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void timerEvent(QTimerEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	enum class State {
		None,
		Waiting,
		Active,
		Readonly,
		Continued,
		Ended,
	};

	void refresh();
	void requestContinue();

	Window::SessionController *_controller = nullptr;
	Main::Session *_session = nullptr;
	QString _chatId;
	State _state = State::None;
	QString _text;
	QString _extra; // the countdown / «request sent»
	QString _conversationId;
	bool _canContinue = false;
	bool _requested = false;
	Ui::RoundButton *_continue = nullptr;
	QBasicTimer _timer;

};

// ---- search in the chat

class SearchBar final : public Ui::RpWidget {
	Q_OBJECT

public:
	SearchBar(QWidget *parent, Main::Session *session, const QString &chatId);

	void activate();
	[[nodiscard]] QString query() const;

Q_SIGNALS:
	void jumpRequested(qint64 seq);
	void closeRequested();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void search();
	void step(int delta);
	void showCurrent();
	[[nodiscard]] QString countText() const;

	Main::Session *_session = nullptr;
	QString _chatId;
	Ui::SearchField *_field = nullptr;
	Ui::IconButton *_up = nullptr;
	Ui::IconButton *_down = nullptr;
	Ui::IconButton *_close = nullptr;
	QTimer _timer;
	QVector<qint64> _results; // newest first
	int _current = -1;
	bool _searched = false;

};

// ---- what is shown instead of the composer

struct NoticeButton {
	QString text;
	const style::RoundButton *st = nullptr;
	Fn<void()> callback;
};

class BottomNotice final : public Ui::RpWidget {
public:
	explicit BottomNotice(QWidget *parent);

	void setContent(
		const QString &title,
		const QString &text,
		std::vector<NoticeButton> buttons);
	// Buttons show a spinner and ignore clicks.
	void setLoading(bool loading);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void layout();

	QString _title;
	QString _text;
	std::vector<Ui::RoundButton*> _buttons;
	bool _stacked = false; // the buttons went under the text (narrow window)

};

} // namespace History
