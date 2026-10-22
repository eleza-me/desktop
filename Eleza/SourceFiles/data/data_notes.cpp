#include "data/data_notes.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "base/unixtime.h"
#include "data/data_chats.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_users.h"

#include <QtCore/QDeadlineTimer>

#include <algorithm>

namespace Data {
namespace {

const auto kFlag = u"feature.notes"_s;
const auto kMaxCharsKey = u"notes.max_chars"_s;

[[nodiscard]] qint64 NowSeconds() {
	return base::unixtime::nowMs() / 1000;
}

[[nodiscard]] qint64 Monotonic() {
	return QDeadlineTimer::current().deadline();
}

} // namespace

Notes::Notes(Session *session)
: QObject(session)
, _session(session)
, _expiryTimer([this] { prune(); }) {
	_wasEnabled = enabled();
	if (const auto config = _session->clientConfig()) {
		connect(config, &Api::ClientConfig::changed, this, [this] {
			// A server that turned the feature on after a 404 has the list now.
			const auto config = _session->clientConfig();
			if (_serverMissing
				&& config->value(kFlag).isBool()
				&& config->boolean(kFlag)) {
				_serverMissing = false;
			}
			checkEnabled();
		});
	}
}

Notes::~Notes() = default;

bool Notes::enabled() const {
	if (_serverMissing) {
		return false;
	}
	const auto config = _session->clientConfig();
	return !config || config->boolean(kFlag, true);
}

int Notes::maxChars() const {
	const auto config = _session->clientConfig();
	const auto value = config
		? config->integer(kMaxCharsKey, NoteText::kDefaultMaxChars)
		: qint64(NoteText::kDefaultMaxChars);
	// The server's own range for the key.
	return int(std::clamp(value, qint64(10), qint64(200)));
}

qint64 Notes::expirySeconds() const {
	constexpr auto kDay = qint64(24 * 3600);
	const auto config = _session->clientConfig();
	const auto value = config ? config->seconds(u"notes.expiry"_s, kDay) : kDay;
	return (value > 0) ? value : kDay;
}

NoteAudience Notes::defaultAudience() const {
	if (const auto note = mine()) {
		return note->audience;
	}
	return _lastAudience.value_or(NoteAudience::Mutuals);
}

void Notes::checkEnabled() {
	const auto now = enabled();
	if (_wasEnabled != now) {
		_wasEnabled = now;
		Q_EMIT enabledChanged();
		if (now && !_loaded) {
			refresh();
		}
	}
}

void Notes::refresh() {
	if (!enabled() || loading()) {
		return;
	} else if (_loaded && Monotonic() - _loadedAtMs < kFreshMs) {
		prune();
		return;
	}
	load();
}

void Notes::reload() {
	if (!enabled() || loading()) {
		return;
	}
	load();
}

void Notes::load() {
	const auto request = ++_requestCounter;
	const auto version = _mineVersion;
	_requestId = request;
	_session->api().request(Api::Get, u"/v1/notes"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (request != _requestId) {
				return;
			}
			_requestId = 0;
			_loadedAtMs = Monotonic();
			auto feed = NotesFeed::FromJson(result);
			if (version != _mineVersion) {
				// Mine changed here while the list was on its way.
				feed.me = _feed.me;
			}
			apply(feed);
		})
		.fail([=, this](const Api::Error &error) {
			if (request != _requestId) {
				return;
			}
			_requestId = 0;
			if (error.notFound()) {
				// A server from before notes: nothing of them shows.
				_serverMissing = true;
				_feed = NotesFeed();
				_loaded = false;
				_expiryTimer.cancel();
				checkEnabled();
				Q_EMIT changed();
			}
			// Anything else (offline, 5xx): what is shown stays.
		})
		.send();
}

void Notes::apply(const NotesFeed &feed) {
	auto cards = QVector<User>();
	cards.reserve(feed.items.size());
	for (const auto &item : feed.items) {
		cards.push_back(item.user);
	}
	if (!cards.isEmpty()) {
		_session->users().seed(cards);
	}
	_loaded = true;
	setFeed(feed);
}

void Notes::setFeed(NotesFeed feed) {
	_feed = feed.fresh(NowSeconds());
	scheduleExpiry();
	Q_EMIT changed();
}

void Notes::setMine(std::optional<Note> note) {
	++_mineVersion;
	auto feed = _feed;
	feed.me = std::move(note);
	setFeed(std::move(feed));
}

void Notes::prune() {
	const auto fresh = _feed.fresh(NowSeconds());
	if (fresh == _feed) {
		scheduleExpiry();
		return;
	}
	setFeed(fresh);
}

void Notes::scheduleExpiry() {
	const auto now = NowSeconds();
	const auto next = _feed.nextExpiry(now);
	if (!next) {
		_expiryTimer.cancel();
		return;
	}
	// A second late, so that the note is over when the check runs.
	const auto ms = (next - now) * 1000 + 1000;
	_expiryTimer.callOnce(int(std::clamp(
		ms,
		qint64(1000),
		qint64(kExpiryCheckMaxMs))));
}

void Notes::set(
		const QString &text,
		NoteAudience audience,
		base::Guard guard,
		Fn<void(const Note &)> done,
		FailCallback fail) {
	const auto clean = NoteText::Cleaned(text);
	_lastAudience = audience;
	_session->api().request(Api::Put, u"/v1/me/note"_s)
		.body(QJsonObject{
			{ u"text"_s, clean },
			{ u"audience"_s, NoteAudienceWire(audience) },
		})
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto note = Note::FromJson(result);
			if (!note) {
				// An answer without the note: what was sent, from now.
				note = Note();
				note->text = clean;
				note->audience = audience;
				note->createdAt = NowSeconds();
			}
			if (!note->expiresAt) {
				note->expiresAt = note->createdAt + expirySeconds();
			}
			setMine(note);
			if (done && guard.alive()) {
				done(*note);
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Notes::remove(
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	const auto finished = [=, this] {
		setMine(std::nullopt);
		if (done && guard.alive()) {
			done();
		}
	};
	_session->api().request(Api::Delete, u"/v1/me/note"_s)
		.guard(this)
		.done(finished)
		.fail([=](const Api::Error &error) {
			if (error.notFound()) {
				// Already gone (its time ran out meanwhile).
				finished();
			} else if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Notes::reply(
		const QString &userId,
		const QString &noteText,
		const QString &answer,
		base::Guard guard,
		Fn<void(const QString &chat)> done,
		FailCallback fail) {
	const auto text = NoteReply::Build(noteText, answer);
	_session->chats().openDirect(userId, this, [=, this](const Chat &chat) {
		const auto id = chat.id;
		_session->outbox().sendText(id, text);
		if (done && guard.alive()) {
			done(id);
		}
	}, [=](const Api::Error &error) {
		if (fail && guard.alive()) {
			fail(error);
		}
	});
}

} // namespace Data
