// The connection state as the user should see it, and the title
// substitute: «در حال اتصال…» / «در حال به‌روزرسانی…» / «در انتظار شبکه…».
//
//   // a section's top bar: the title is replaced while the app is not online
//   _connection = new Support::ConnectionTitle(_bar, &controller->session(), tr::lng_messages(tr::now));
//   _connection->setTitle(newTitle);                 // later, when the normal title changes
//
//   // a chat's top bar: the subtitle («آنلاین», «در حال نوشتن…») is replaced instead
//   _connection = new Support::ConnectionTitle(_bar, &session, name, Support::ConnectionTitle::Mode::Subtitle);
//   _connection->setSubtitle(tr::lng_status_online(tr::now), true);
//
//   // anywhere else: a small label with a spinner that hides itself when online
//   const auto label = Ui::CreateChild<Support::ConnectionLabel>(this, &session);
//
//   // or just the state
//   const auto state = Support::ConnectionState::For(&session);
//   connect(state, &Support::ConnectionState::changed, this, [=] { ... state->text() ... });
//
// The state combines the realtime connection (Main::Session::connectionStateChanged),
// the update replay (Data::Sync::updatingChanged) and whether HTTP requests get
// answers. When the socket cannot be opened but HTTP works (a proxy without
// websockets), the app runs on the HTTP fallback and is shown as online.
#pragma once

#include "rt/rt_client.h"
#include "ui/effects/radial_animation.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class TopBar;
} // namespace Ui

namespace Support {

enum class ConnectionStatus {
	Online,
	Connecting,
	Updating,
	WaitingForNetwork,
};

// The wording for a status; empty for Online.
[[nodiscard]] QString ConnectionText(ConnectionStatus status);

// Pure rule, exposed for the tests.
[[nodiscard]] ConnectionStatus ComputeConnectionStatus(
	Rt::State state,
	bool updating,
	bool httpReachable,
	bool fallbackWorks);

class ConnectionState final : public QObject {
	Q_OBJECT

public:
	// One shared object per session (owned by the session).
	[[nodiscard]] static ConnectionState *For(Main::Session *session);

	[[nodiscard]] ConnectionStatus status() const {
		return _status;
	}
	[[nodiscard]] bool online() const {
		return _status == ConnectionStatus::Online;
	}
	[[nodiscard]] QString text() const {
		return ConnectionText(_status);
	}

Q_SIGNALS:
	void changed();

private:
	explicit ConnectionState(Main::Session *session);

	void refresh();

	Main::Session *_session = nullptr;
	ConnectionStatus _status = ConnectionStatus::Online;
	QTimer _fallbackTimer;
	bool _fallbackWorks = false;

};

class ConnectionTitle final : public QObject {
public:
	enum class Mode {
		Title,
		Subtitle,
	};

	ConnectionTitle(
		Ui::TopBar *bar,
		Main::Session *session,
		const QString &title,
		Mode mode = Mode::Title);

	void setTitle(const QString &title);
	void setSubtitle(const QString &subtitle, bool accent = false);

private:
	void apply();

	QPointer<Ui::TopBar> _bar;
	QPointer<ConnectionState> _state;
	Mode _mode = Mode::Title;
	QString _title;
	QString _subtitle;
	bool _subtitleAccent = false;

};

class ConnectionLabel final : public Ui::RpWidget {
public:
	ConnectionLabel(QWidget *parent, Main::Session *session);

	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refresh();

	QPointer<ConnectionState> _state;
	QString _text;
	Ui::InfiniteRadialAnimation _spinner;

};

} // namespace Support
