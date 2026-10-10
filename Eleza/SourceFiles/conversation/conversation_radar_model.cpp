#include "conversation/conversation_radar_model.h"

#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <limits>

namespace Conversation {
namespace {

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] bool IsEndingEvent(const QString &event) {
	return (event == u"ended")
		|| (event == u"expired")
		|| (event == u"continued")
		|| (event == u"closed")
		|| (event == u"purged");
}

} // namespace

bool IsEndedState(const QString &state) {
	return (state == u"ended")
		|| (state == u"expired")
		|| (state == u"closed")
		|| (state == u"purged")
		|| (state == u"readonly")
		|| (state == u"continued");
}

RadarPhase PhaseOf(const Data::Conversation &conversation, qint64 nowMs) {
	if (IsEndedState(conversation.state)) {
		return RadarPhase::Closed;
	} else if (conversation.listener) {
		return RadarPhase::Matched;
	} else if (conversation.expiresAt > 0
		&& conversation.expiresAt * 1000 <= nowMs) {
		return RadarPhase::Closed;
	}
	return RadarPhase::Waiting;
}

RadarModel::RadarModel(
	Data::Session *data,
	const QString &conversationId,
	QObject *parent)
: QObject(parent)
, _data(data)
, _id(conversationId)
, _poll([this] { reload(); })
, _expiry([this] {
	if (_phase == RadarPhase::Waiting
		&& _conversation
		&& PhaseOf(*_conversation, NowMs()) == RadarPhase::Closed) {
		setPhase(RadarPhase::Closed, u"expired"_s);
	} else {
		scheduleExpiry();
	}
}) {
}

void RadarModel::setInitial(const Data::Conversation &conversation) {
	if (conversation.id == _id) {
		apply(conversation);
	}
}

void RadarModel::start() {
	if (_started) {
		return;
	}
	_started = true;
	connect(
		&_data->conversations(),
		&Data::Conversations::stateChanged,
		this,
		&RadarModel::stateChanged);
	connect(
		&_data->transport(),
		&Data::Transport::connectedChanged,
		this,
		&RadarModel::connectedChanged);
	reload();
	updatePolling();
}

void RadarModel::setActive(bool active) {
	if (_active == active) {
		return;
	}
	_active = active;
	if (_active && std::exchange(_missedWhileInactive, false)) {
		reload();
	}
	updatePolling();
}

void RadarModel::setOfflinePollInterval(int ms) {
	_pollInterval = std::max(ms, 1);
	if (_poll.isActive()) {
		_poll.callEach(_pollInterval);
	}
}

void RadarModel::reload() {
	if (!_started || finished()) {
		return;
	} else if (!_active) {
		_missedWhileInactive = true;
		return;
	}
	++_requests;
	_data->conversations().get(_id, this, [this](const Data::Conversation &conversation) {
		--_requests;
		if (finished()) {
			return;
		} else if (conversation.id.isEmpty()) {
			// An answer without a view: treat it like a failed read.
			if (!_conversation) {
				_error = Api::Error();
				setPhase(RadarPhase::Failed);
			}
			return;
		}
		apply(conversation);
	}, [this](const Api::Error &error) {
		--_requests;
		if (finished()) {
			return;
		} else if (error.notFound()) {
			// Removed on the server: there is nothing to wait for.
			setPhase(RadarPhase::Closed, u"ended"_s);
		} else if (!_conversation) {
			_error = error;
			setPhase(RadarPhase::Failed);
		} else {
			Q_EMIT changed();
		}
	});
}

void RadarModel::apply(const Data::Conversation &conversation) {
	_conversation = conversation;
	_error = std::nullopt;
	auto users = QStringList();
	if (!conversation.speaker.userId.isEmpty()) {
		users.push_back(conversation.speaker.userId);
	}
	if (conversation.listener && !conversation.listener->userId.isEmpty()) {
		users.push_back(conversation.listener->userId);
	}
	if (!users.isEmpty()) {
		_data->users().request(users);
	}
	const auto phase = PhaseOf(conversation, NowMs());
	setPhase(
		phase,
		(phase != RadarPhase::Closed)
			? QString()
			: IsEndedState(conversation.state)
			? conversation.state
			: u"expired"_s);
}

void RadarModel::setPhase(RadarPhase phase, const QString &closedState) {
	const auto was = _phase;
	_phase = phase;
	if (phase == RadarPhase::Closed) {
		_closedState = closedState;
		_poll.cancel();
		_expiry.cancel();
	} else {
		scheduleExpiry();
		updatePolling();
	}
	Q_EMIT changed();
	if (was != phase) {
		if (phase == RadarPhase::Matched) {
			Q_EMIT matched();
		} else if (phase == RadarPhase::Closed) {
			Q_EMIT closed(_closedState);
		}
	}
}

void RadarModel::stateChanged(
		const QString &chat,
		const QString &state,
		const QString &event) {
	if (finished()) {
		return;
	}
	const auto own = _data->chats().chatOfConversation(_id);
	if (own.isEmpty()) {
		// Our chat row is not projected yet. A chat we know belongs to
		// something else; an unknown one may be ours.
		if (_data->chats().chat(chat) != nullptr) {
			return;
		}
	} else if (own != chat) {
		return;
	}
	const auto certain = !own.isEmpty();
	if (certain && (IsEndedState(state) || IsEndingEvent(event))) {
		const auto closed = IsEndedState(state) ? state : event;
		if (_conversation) {
			_conversation->state = closed;
		}
		setPhase(RadarPhase::Closed, closed);
		return;
	}
	// "joined", "opened", "continue_requested", or not surely ours: read it.
	reload();
}

void RadarModel::connectedChanged(bool connected) {
	if (connected) {
		// Whatever happened while the connection was down.
		reload();
	}
	updatePolling();
}

void RadarModel::updatePolling() {
	const auto needed = _started
		&& _active
		&& !finished()
		&& (_phase != RadarPhase::Failed)
		&& !_data->transport().connected();
	if (!needed) {
		_poll.cancel();
	} else if (!_poll.isActive()) {
		_poll.callEach(_pollInterval);
	}
}

void RadarModel::scheduleExpiry() {
	if (_phase != RadarPhase::Waiting
		|| !_conversation
		|| _conversation->expiresAt <= 0) {
		_expiry.cancel();
		return;
	}
	const auto left = _conversation->expiresAt * 1000 - NowMs();
	_expiry.callOnce(int(std::clamp<qint64>(
		left + 250,
		0,
		std::numeric_limits<int>::max() / 2)));
}

} // namespace Conversation
