// Keeps the client in step with the server's per-user update log (`pts`).
//
// The connection carries everything: the auth request says which pts we
// hold, the server replays what we missed right after Hello (Replayed
// frames, then Resumed), live frames apply when they are the next update,
// and `sync.difference` is only the fallback for a gap (a skipped pts, a
// Resync signal, a log that is too long) or for a connection that stays
// down - then, and only then, a timer reads the difference every
// `realtime.fallback_sync_interval`. A client that starts without a stored
// pts while the connection is down reads `sync.state` at once, so the
// fallback has a point to read the difference from.
//
// Applied update kinds: message, edited, deleted, live_stopped, reaction,
// read, played, draft, cleared, member, state, message_pinned,
// message_unpinned, notif, group.
//
// Groups are a second track with a counter per group (Data::Groups): this
// store hands it the group frames of the connection, the user updates of
// kind `group` (my own place in a group) and the moments to catch up (every
// Hello, every fallback tick).
#pragma once

#include "base/timer.h"
#include "base/weak_guard.h"
#include "data/data_types.h"
#include "rt/rt_client.h"

namespace Data {

class Session;

class Sync final : public QObject {
	Q_OBJECT

public:
	explicit Sync(Session *session);

	// Reads the stored pts, then starts listening to the transport.
	void start(base::Guard guard = base::Guard(), Fn<void()> ready = nullptr);
	void stop();

	// The last update applied; nullopt before the first state was read.
	[[nodiscard]] std::optional<qint64> pts() const {
		return _pts;
	}
	// A replay or a difference is running («updating…»).
	[[nodiscard]] bool updating() const {
		return _replaying || _syncing;
	}

	// Reads the difference now (from `from`, or from the stored pts).
	void catchUp(std::optional<qint64> from = std::nullopt);

	// Frames from the transport (also the tests' entry point).
	void handle(const Rt::Frame &frame);

	static constexpr auto kAckEvery = 16;
	static constexpr auto kMaxDifferencePages = 50;
	static constexpr auto kInboxReloadDelayMs = 800;

Q_SIGNALS:
	void updatingChanged(bool updating);
	void ptsChanged(qint64 pts);
	// A live message from somebody else arrived (for notifications / sounds).
	void messageReceived(const Data::Message &message);
	// The inbox must be read again (rows the updates referred to are unknown).
	void inboxReloadRequested();

private:
	void setPts(qint64 pts);
	void updateUpdating();
	void flushAck(bool force);
	void applyItem(const QJsonObject &item, bool live);
	void applyLive(const QString &kind, const QJsonObject &payload, qint64 framePts, bool live);
	void applyMessage(Message message, bool live);
	void applyCleared(const QString &chat, qint64 clearedTo, bool all);
	void rememberInfo(const ChatInfo &info);
	void reloadInboxSoon();
	void scheduleFallback();
	void syncLoop(std::optional<qint64> from);
	void syncOnce(std::optional<qint64> from, Fn<void()> done);
	void differenceStep(qint64 pts, int page, Fn<void()> done);
	void readState(Fn<void(std::optional<qint64>)> done);

	Session *_session = nullptr;
	std::optional<qint64> _pts;
	base::Timer _fallbackTimer;
	base::Timer _inboxReload;
	int _unacked = 0;
	bool _started = false;
	bool _syncing = false;
	bool _syncAgain = false;
	bool _replaying = false;
	bool _wasUpdating = false;

};

} // namespace Data
