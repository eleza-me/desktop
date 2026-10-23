// Notes (backend docs/notes.md): one short line a person shares for a day
// with their mutual followers or their close friends, shown in a row above
// the chat list. My own note and the notes of the people I follow that I may
// see come from `GET /v1/notes`; mine is set with `PUT /v1/me/note` and
// removed with `DELETE /v1/me/note`. HTTP only: the realtime connection has no
// notes and the server sends no signal when a friend's note changes.
//
//   auto &notes = session.data().notes();
//   notes.refresh();                      // at most once a minute; reload() for F5
//   connect(&notes, &Data::Notes::changed, this, [=] { ... notes.feed() ... });
//   notes.set(text, Data::NoteAudience::Mutuals, this, done, fail);
//   notes.reply(userId, note.text, answer, this, [=](const QString &chat) { ... });
//
// No polling: the list is read when the chat list shows it (and on demand),
// and lives on what this app changes. A note whose time ran out leaves the
// feed here at its expiry, without asking the server.
//
// Old servers: `feature.notes` false in the client config, or a 404 for the
// GET (a server from before notes) turn the whole feature off.
#pragma once

#include "base/timer.h"
#include "data/data_note_types.h"
#include "data/data_social_types.h"

namespace Data {

class Session;

class Notes final : public QObject {
	Q_OBJECT

public:
	explicit Notes(Session *session);
	~Notes();

	// client-config `feature.notes` (absent: on), and the server did not
	// answer 404 for the list.
	[[nodiscard]] bool enabled() const;
	// client-config `notes.max_chars` (60 when absent).
	[[nodiscard]] int maxChars() const;
	// client-config `notes.expiry`: how long a note shows (a day when absent).
	[[nodiscard]] qint64 expirySeconds() const;

	// What shows now: notes whose time ran out are left out.
	[[nodiscard]] const NotesFeed &feed() const {
		return _feed;
	}
	[[nodiscard]] const Note *mine() const {
		return _feed.me ? &*_feed.me : nullptr;
	}
	[[nodiscard]] bool loaded() const {
		return _loaded;
	}
	[[nodiscard]] bool loading() const {
		return _requestId != 0;
	}
	// The audience the composer starts with: my note's, else the last one
	// I picked in this run.
	[[nodiscard]] NoteAudience defaultAudience() const;

	// Reads the list unless it was read a moment ago or is on its way.
	void refresh();
	// Reads it again now (F5, a pull in the list).
	void reload();
	// Applies an answer of the list (also what the tests feed).
	void apply(const NotesFeed &feed);

	void set(
		const QString &text,
		NoteAudience audience,
		base::Guard guard,
		Fn<void(const Note &)> done,
		FailCallback fail = nullptr);
	void remove(
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail = nullptr);
	// Sends `answer` to the 1:1 chat with `userId` as a reply to their note
	// (NoteReply::Build); `done` gets the chat once the message is queued.
	void reply(
		const QString &userId,
		const QString &noteText,
		const QString &answer,
		base::Guard guard,
		Fn<void(const QString &chat)> done,
		FailCallback fail = nullptr);

	// A list read within this time is not read again by refresh().
	static constexpr auto kFreshMs = 60 * 1000;
	// The longest the expiry check sleeps (a laptop that slept meanwhile).
	static constexpr auto kExpiryCheckMaxMs = 10 * 60 * 1000;

Q_SIGNALS:
	void changed();
	void enabledChanged();

private:
	void load();
	void setFeed(NotesFeed feed);
	void setMine(std::optional<Note> note);
	void prune();
	void scheduleExpiry();
	void checkEnabled();

	Session *_session = nullptr;
	NotesFeed _feed;
	bool _loaded = false;
	bool _serverMissing = false; // 404 for the list: a server without notes
	bool _wasEnabled = true;
	int _requestId = 0;
	int _requestCounter = 0;
	qint64 _loadedAtMs = 0;
	// Bumped by every set / remove of mine: a read that started before one
	// keeps the newer «mine».
	int _mineVersion = 0;
	std::optional<NoteAudience> _lastAudience;
	base::Timer _expiryTimer;

};

} // namespace Data
