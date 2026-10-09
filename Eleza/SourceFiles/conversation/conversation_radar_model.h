// The state behind the radar (the cover a speaker sees after opening a topic):
// waiting for a listener -> matched, or closed (ended / expired / continued).
//
// Reads the conversation once, then lives on Data::Conversations::stateChanged
// ("joined", "opened", "ended", "expired", "continued" ...). Only while the
// realtime connection is down it re-reads on a short timer, and once more when
// the connection returns - exactly the fallback the realtime contract allows.
// The expiry itself is a local timer.
//
// No widgets here (the unit tests drive it with the store fakes).
#pragma once

#include "base/timer.h"
#include "data/data_types.h"
#include "api/api_error.h"

#include <QtCore/QObject>

#include <optional>

namespace Data {
class Session;
} // namespace Data

namespace Conversation {

enum class RadarPhase {
	Loading,
	Waiting, // open, nobody listens yet
	Matched, // a listener joined
	Closed,  // ended / expired / continued / purged
	Failed,  // could not be read (and nothing to show)
};

// The states after which there is nothing to wait for.
[[nodiscard]] bool IsEndedState(const QString &state);
// The phase a conversation view is in at `nowMs`.
[[nodiscard]] RadarPhase PhaseOf(const Data::Conversation &conversation, qint64 nowMs);

class RadarModel final : public QObject {
	Q_OBJECT

public:
	RadarModel(
		Data::Session *data,
		const QString &conversationId,
		QObject *parent = nullptr);

	// A view the caller already has (the answer of `create`): shown at once.
	void setInitial(const Data::Conversation &conversation);
	// Reads the conversation and starts following it.
	void start();
	// The screen is hidden: nothing is read until it is shown again (then the
	// state is read once, it may have been missed).
	void setActive(bool active);

	[[nodiscard]] const QString &id() const {
		return _id;
	}
	[[nodiscard]] RadarPhase phase() const {
		return _phase;
	}
	[[nodiscard]] const std::optional<Data::Conversation> &conversation() const {
		return _conversation;
	}
	// Why it closed: the server's state ("ended", "expired", "continued" ...).
	[[nodiscard]] const QString &closedState() const {
		return _closedState;
	}
	[[nodiscard]] const std::optional<Api::Error> &error() const {
		return _error;
	}
	[[nodiscard]] bool loading() const {
		return _requests > 0;
	}
	[[nodiscard]] bool pollingOffline() const {
		return _poll.isActive();
	}

	void reload();
	// Tests shorten the fallback interval.
	void setOfflinePollInterval(int ms);

	static constexpr auto kOfflinePollMs = 3000;

Q_SIGNALS:
	void changed();
	// Once, when a listener appears.
	void matched();
	// Once, when there is nothing to wait for any more.
	void closed(const QString &state);

private:
	void apply(const Data::Conversation &conversation);
	void setPhase(RadarPhase phase, const QString &closedState = QString());
	void stateChanged(const QString &chat, const QString &state, const QString &event);
	void connectedChanged(bool connected);
	void updatePolling();
	void scheduleExpiry();
	[[nodiscard]] bool finished() const {
		return (_phase == RadarPhase::Closed);
	}

	Data::Session *_data = nullptr;
	QString _id;
	std::optional<Data::Conversation> _conversation;
	std::optional<Api::Error> _error;
	RadarPhase _phase = RadarPhase::Loading;
	QString _closedState;
	base::Timer _poll;
	base::Timer _expiry;
	int _pollInterval = kOfflinePollMs;
	int _requests = 0;
	bool _started = false;
	bool _active = true;
	bool _missedWhileInactive = false;

};

} // namespace Conversation
