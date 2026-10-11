// "debug/calls/<what>": a call in a given state without a server and without
// audio, for screenshots and for looking at the screens while working on
// them. The call is a real Calls::Call driven by a scripted server and a
// media that connects when told to, so what is on screen is what the state
// machine would show.
//
//   calling ringing connecting connected muted reconnecting      (the bar)
//   ended declined no-answer busy failed lost not-allowed        (how it ended)
//   incoming                                                     (the window)
//   missed                                    (the desktop notification)
//   …-expanded                                (the same in the expanded view)
//
// The other side is the peer of the first direct chat, or "?user=<id>".
#ifdef ELEZA_DEBUG_TOOLS

#include "calls/calls_service.h"

#include "calls/calls_text.h"
#include "data/data_chats.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/notifications.h"
#include "support/session_services.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"

#include "core/core_application.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>

namespace Calls {
namespace {

const auto kDemoCall = u"demo-call"_s;

[[nodiscard]] QString DebugStateName(State state) {
	switch (state) {
	case State::Calling: return u"calling"_s;
	case State::Ringing: return u"ringing"_s;
	case State::Incoming: return u"incoming"_s;
	case State::Connecting: return u"connecting"_s;
	case State::InCall: return u"in-call"_s;
	case State::Reconnecting: return u"reconnecting"_s;
	case State::Ended: return u"ended"_s;
	}
	return QString();
}

[[nodiscard]] QString DebugReasonName(EndReason reason) {
	switch (reason) {
	case EndReason::None: return QString();
	case EndReason::Hangup: return u"hangup"_s;
	case EndReason::Declined: return u"declined"_s;
	case EndReason::NoAnswer: return u"no-answer"_s;
	case EndReason::Busy: return u"busy"_s;
	case EndReason::NotAllowed: return u"not-allowed"_s;
	case EndReason::Failed: return u"failed"_s;
	case EndReason::Lost: return u"lost"_s;
	case EndReason::DeclinedByMe: return u"declined-by-me"_s;
	case EndReason::Missed: return u"missed"_s;
	case EndReason::AnsweredElsewhere: return u"answered-elsewhere"_s;
	}
	return QString();
}

class DemoMedia final : public Media {
public:
	using Media::Media;

	void join(const MediaJoin &) override {
		set(State::Connecting);
	}
	void leave() override {
		set(State::Idle);
	}
	State state() const override {
		return _state;
	}
	void setMuted(bool) override {
	}
	void setInputDevice(const QByteArray &) override {
	}
	void setOutputDevice(const QByteArray &) override {
	}
	void setProcessing(bool, bool, bool) override {
	}
	void requestStats() override {
	}
	void set(State state) {
		if (_state != state) {
			_state = state;
			Q_EMIT stateChanged(state);
		}
	}

private:
	State _state = State::Idle;

};

// Answers what the test told it to; everything else never gets an answer.
class DemoWorld final : public QObject, public Data::CallsApi {
public:
	using QObject::QObject;

	std::optional<Api::Error> inviteError;
	QPointer<DemoMedia> media;

	void invite(const QString &userId, base::Guard guard, JoinDone done, Data::FailCallback fail) override {
		if (!guard.alive()) {
			return;
		} else if (inviteError) {
			fail(*inviteError);
		} else {
			done(Data::CallJoin::FromJson({
				{ u"call"_s, kDemoCall },
				{ u"state"_s, u"ringing"_s },
				{ u"sfu_url"_s, u"wss://demo.invalid"_s },
				{ u"token"_s, u"demo"_s },
				{ u"peer"_s, userId },
			}));
		}
	}
	void accept(const QString &, base::Guard, JoinDone, Data::FailCallback) override {
	}
	void rejoin(const QString &, base::Guard, JoinDone, Data::FailCallback) override {
	}
	void get(const QString &, base::Guard, Fn<void(const Data::CallInfo &)>, Data::FailCallback) override {
	}
	void incoming(base::Guard, Fn<void(const std::optional<Data::CallInfo> &)>, Data::FailCallback) override {
	}
	void active(base::Guard, Fn<void(const std::optional<Data::ActiveCall> &)>, Data::FailCallback) override {
	}
	void command(const QString &, const QString &, base::Guard, Done done, Data::FailCallback) override {
		if (done) {
			done();
		}
	}
	void qos(const QString &, const Data::CallQos &) override {
	}
	bool signalsConnected() const override {
		return true;
	}
};

[[nodiscard]] Data::CallSignal Signal(const char *type, const QString &reason = QString()) {
	return Data::CallSignal::FromJson(QString::fromLatin1(type), {
		{ u"call"_s, kDemoCall },
		{ u"reason"_s, reason },
	});
}

[[nodiscard]] QString DemoPeer(Main::Session *session, const QString &asked) {
	if (!asked.isEmpty()) {
		return asked;
	}
	for (const auto &chat : session->data().chats().list()) {
		if (Data::CallAvailabilityFor(chat) == Data::CallAvailability::Available) {
			return chat.peerUserId;
		}
	}
	return u"u_demo"_s;
}

} // namespace

bool Service::showDemo(const QString &what) {
	auto name = what;
	auto peerAsked = QString();
	if (const auto mark = name.indexOf(u'?'); mark >= 0) {
		peerAsked = QUrlQuery(name.mid(mark + 1)).queryItemValue(u"user"_s);
		name = name.left(mark);
	}
	const auto suffix = u"-expanded"_s;
	const auto expand = name.endsWith(suffix);
	if (expand) {
		name.chop(suffix.size());
	}
	const auto peer = DemoPeer(_session, peerAsked);
	const auto known = QStringList{
		u"calling"_s, u"ringing"_s, u"connecting"_s, u"connected"_s,
		u"muted"_s, u"reconnecting"_s, u"ended"_s, u"declined"_s,
		u"no-answer"_s, u"busy"_s, u"failed"_s, u"lost"_s,
		u"not-allowed"_s, u"incoming"_s, u"missed"_s,
	};
	if (!known.contains(name)) {
		return false;
	}
	if (name == u"missed") {
		// What the record of a missed call brings up while the window is
		// not looked at.
		if (const auto services = Support::SessionServices::For(_controller)) {
			auto data = Support::NotificationData();
			data.key = u"demo-missed"_s;
			data.title = PeerName(_session, peer);
			data.text = tr::lng_calls_record_missed(tr::now);
			data.userId = peer;
			services->notifications()->showCustom(data);
		}
		return true;
	}

	const auto world = new DemoWorld();
	auto args = Instance::Args();
	args.api = world;
	args.createMedia = [=]() -> std::unique_ptr<Media> {
		auto result = std::make_unique<DemoMedia>();
		world->media = result.get();
		return result;
	};
	args.checkMedia = [] { return MediaProblem::None; };
	args.hasMicrophone = [] { return true; };
	// Nothing times out or polls while it is being looked at.
	args.timings.reconnectBudgetMs = 3600 * 1000;
	args.timings.ringTimeoutMs = 3600 * 1000;
	args.timings.qosFirstMs = 0;
	if (name == u"busy") {
		world->inviteError = Api::Error::Local(409, u"call.busy"_s, u"busy"_s);
	} else if (name == u"not-allowed") {
		world->inviteError = Api::Error::Local(403, u"call.not_allowed"_s, u"no"_s);
	} else if (name == u"failed") {
		world->inviteError = Api::Error::Local(503, u"call.capacity"_s, u"full"_s);
	}
	auto instance = std::make_unique<Instance>(args);
	world->setParent(instance.get());
	const auto raw = instance.get();
	replaceInstance(std::move(instance));

	if (name == u"incoming") {
		raw->handleSignal(Data::CallSignal::FromJson(u"call.incoming"_s, {
			{ u"call"_s, kDemoCall },
			{ u"from"_s, peer },
		}));
		return true;
	}
	raw->start(peer);
	const auto connect = [&] {
		raw->handleSignal(Signal("call.accepted"));
		if (world->media) {
			world->media->set(Media::State::Connected);
		}
		if (const auto call = raw->call()) {
			// «۰۲:۱۵»
			call->debugSetConnectedAt(
				QDateTime::currentMSecsSinceEpoch() - 135 * 1000);
			if (world->media) {
				Q_EMIT world->media->levelsChanged(0.35f, 0.75f);
			}
		}
	};
	if (name == u"ringing") {
		raw->handleSignal(Signal("call.ringing"));
	} else if (name == u"connecting") {
		raw->handleSignal(Signal("call.accepted"));
	} else if (name == u"connected") {
		connect();
	} else if (name == u"muted") {
		connect();
		toggleMuted();
	} else if (name == u"reconnecting") {
		connect();
		if (world->media) {
			world->media->set(Media::State::Reconnecting);
		}
	} else if (name == u"ended") {
		connect();
		raw->handleSignal(Signal("call.ended", u"hangup"_s));
	} else if (name == u"lost") {
		connect();
		raw->handleSignal(Signal("call.ended", u"lost"_s));
	} else if (name == u"declined") {
		raw->handleSignal(Signal("call.ringing"));
		raw->handleSignal(Signal("call.ended", u"rejected"_s));
	} else if (name == u"no-answer") {
		raw->handleSignal(Signal("call.ringing"));
		raw->handleSignal(Signal("call.ended", u"timeout"_s));
	}
	Q_EMIT callUpdated();
	if (expand && call()) {
		// An ended call can be looked at in the expanded view as well.
		if (!_expanded) {
			_expanded = true;
			Q_EMIT expandedChanged();
		}
	}
	return true;
}

// ---- driven by a script
//
// <folder>/command   one line, taken (and removed) within 100 ms:
//                      start <user id> | answer | hangup | mute | dismiss
//                      | input <device id> | quit
// <folder>/state     the call now, rewritten on every change:
//                      { "state": "none|calling|ringing|incoming|connecting
//                                  |in-call|reconnecting|ended",
//                        "reason": "", "id": "", "muted": false, "peer": "" }
// <folder>/events    the same, one line per change with the time, and the
//                    notices the user was shown.
void Service::startDebugControl() {
	const auto folder = qEnvironmentVariable("ELEZA_DEBUG_CALL_CONTROL");
	if (folder.isEmpty() || !QDir().mkpath(folder)) {
		return;
	}
	const auto control = new QObject(this);
	const auto last = std::make_shared<QByteArray>();
	const auto record = [=](const QByteArray &line) {
		auto events = QFile(folder + u"/events"_s);
		if (events.open(QIODevice::WriteOnly | QIODevice::Append)) {
			events.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch())
				+ ' '
				+ line
				+ '\n');
		}
	};
	const auto write = [=, this] {
		const auto call = this->call();
		const auto text = QJsonDocument(QJsonObject{
			{ u"state"_s, call ? DebugStateName(call->state()) : u"none"_s },
			{ u"reason"_s, call ? DebugReasonName(call->endReason()) : QString() },
			{ u"id"_s, call ? call->id() : QString() },
			{ u"muted"_s, call && call->muted() },
			{ u"peer"_s, call ? call->peerId() : QString() },
		}).toJson(QJsonDocument::Compact);
		if (*last == text) {
			return;
		}
		*last = text;
		auto file = QSaveFile(folder + u"/state"_s);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(text);
			file.commit();
		}
		record(text);
	};
	connect(this, &Service::callChanged, control, write);
	connect(this, &Service::callUpdated, control, write);
	connect(this, &Service::noticeShown, control, [=](Notice notice) {
		record("notice " + QByteArray::number(int(notice))
			+ ' '
			+ NoticeText(notice).toUtf8());
	});
	const auto timer = new QTimer(control);
	timer->setInterval(100);
	connect(timer, &QTimer::timeout, control, [=, this] {
		write();
		auto file = QFile(folder + u"/command"_s);
		if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
			return;
		}
		const auto line = QString::fromUtf8(file.readAll()).trimmed();
		file.close();
		file.remove();
		const auto space = line.indexOf(QLatin1Char(' '));
		const auto verb = (space < 0) ? line : line.left(space);
		const auto value = (space < 0) ? QString() : line.mid(space + 1).trimmed();
		record("command " + line.toUtf8());
		if (verb == u"start") {
			start(value);
		} else if (verb == u"answer") {
			answer();
		} else if (verb == u"hangup") {
			hangup();
		} else if (verb == u"mute") {
			toggleMuted();
		} else if (verb == u"dismiss") {
			dismissEnded();
		} else if (verb == u"input") {
			setInputDevice(value.toUtf8());
		} else if (verb == u"quit") {
			Core::App().quit();
		}
	});
	timer->start();
	write();
}

} // namespace Calls

#endif // ELEZA_DEBUG_TOOLS
