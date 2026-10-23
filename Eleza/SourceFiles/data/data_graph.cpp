#include "data/data_graph.h"

#include "api/api_client.h"
#include "data/data_feed.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "storage/storage_database.h"

namespace Data {
namespace {

const auto kDismissedKey = u"suggestions.dismissed"_s;
constexpr auto kUpdateAttempts = 40;

} // namespace

QJsonObject ProfilePatch::toJson() const {
	auto result = QJsonObject();
	if (displayName) {
		result.insert("display_name"_L1, *displayName);
	}
	if (bio) {
		result.insert("bio"_L1, *bio);
	}
	if (link) {
		result.insert("link"_L1, *link);
	}
	if (metaLine) {
		result.insert("meta_line"_L1, *metaLine);
	}
	Json::Put(result, "avatar_id"_L1, avatarId);
	Json::Put(result, "clear_avatar"_L1, clearAvatar);
	return result;
}

Graph::Graph(Session *session)
: QObject(session)
, _session(session) {
	qRegisterMetaType<Data::GraphState>();
}

void Graph::simple(
		Api::Method method,
		const QString &path,
		const QJsonObject &body,
		const QString &action,
		const QString &subject,
		base::Guard guard,
		Fn<void()> done) {
	auto builder = _session->api().request(method, path);
	if (method != Api::Get && method != Api::Delete) {
		builder.body(body);
	}
	builder
		.guard(this)
		.done([=] {
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=, this](const Api::Error &error) {
			_session->reportFailure(action, subject, error);
		})
		.send();
}

// ---- profiles

void Graph::profileByHandle(
		const QString &handle,
		base::Guard guard,
		Fn<void(const Profile &)> done,
		FailCallback fail) {
	auto clean = handle;
	if (clean.startsWith(u'@')) {
		clean.remove(0, 1);
	}
	_session->api().request(Api::Get, u"/v1/users/by-handle/"_s + PathSegment(clean))
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto profile = Profile::FromJson(result);
			if (!profile.deleted) {
				_session->users().seed(profile.card());
			}
			_session->users().seed(profile.mutuals);
			if (done && guard.alive()) {
				done(profile);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::profile(
		const QString &userId,
		base::Guard guard,
		Fn<void(const Profile &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/users/"_s + PathSegment(userId))
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto profile = Profile::FromJson(result);
			if (!profile.deleted) {
				_session->users().seed(profile.card());
			}
			_session->users().seed(profile.mutuals);
			if (done && guard.alive()) {
				done(profile);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::relationship(
		const QString &userId,
		base::Guard guard,
		Fn<void(const GraphState &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/users/%1/relationship"_s.arg(PathSegment(userId)))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(GraphState::FromJson(result));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::relation(
		Api::Method method,
		const QString &path,
		const QString &userId,
		const QString &action,
		base::Guard guard,
		Fn<void(const GraphState &)> done,
		FailCallback fail) {
	auto builder = _session->api().request(method, path);
	if (method == Api::Post || method == Api::Put) {
		builder.body(QJsonObject());
	}
	builder
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto state = GraphState::FromJson(result);
			Q_EMIT relationshipChanged(userId, state);
			if (action == u"graph.block") {
				_session->feed().noteAuthorBlocked(userId);
			}
			if (done && guard.alive()) {
				done(state);
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			} else {
				_session->reportFailure(action, userId, error);
			}
		})
		.send();
}

void Graph::follow(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Post, u"/v1/users/%1/follow"_s.arg(PathSegment(userId)), userId, u"graph.follow"_s, guard, done, fail);
}

void Graph::unfollow(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Delete, u"/v1/users/%1/follow"_s.arg(PathSegment(userId)), userId, u"graph.unfollow"_s, guard, done, fail);
}

void Graph::removeFollower(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Delete, u"/v1/me/followers/"_s + PathSegment(userId), userId, u"graph.remove_follower"_s, guard, done, fail);
}

void Graph::block(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Post, u"/v1/users/%1/block"_s.arg(PathSegment(userId)), userId, u"graph.block"_s, guard, done, fail);
}

void Graph::unblock(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Delete, u"/v1/users/%1/block"_s.arg(PathSegment(userId)), userId, u"graph.unblock"_s, guard, done, fail);
}

void Graph::mute(const QString &userId) {
	simple(Api::Post, u"/v1/users/%1/mute"_s.arg(PathSegment(userId)), QJsonObject(), u"graph.mute"_s, userId, base::Guard(), nullptr);
}

void Graph::unmute(const QString &userId) {
	simple(Api::Delete, u"/v1/users/%1/mute"_s.arg(PathSegment(userId)), QJsonObject(), u"graph.unmute"_s, userId, base::Guard(), nullptr);
}

void Graph::hide(const QString &userId) {
	simple(Api::Post, u"/v1/users/%1/hide"_s.arg(PathSegment(userId)), QJsonObject(), u"graph.hide"_s, userId, base::Guard(), nullptr);
}

void Graph::unhide(const QString &userId) {
	simple(Api::Delete, u"/v1/users/%1/hide"_s.arg(PathSegment(userId)), QJsonObject(), u"graph.unhide"_s, userId, base::Guard(), nullptr);
}

void Graph::postNotifications(const QString &userId, base::Guard guard, Fn<void(bool)> done) {
	_session->api().request(Api::Get, u"/v1/users/%1/post-notifications"_s.arg(PathSegment(userId)))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::Bool(result, "enabled"_L1));
			}
		})
		.fail([=](const Api::Error &) {
			if (done) {
				done(false);
			}
		})
		.send();
}

void Graph::setPostNotifications(const QString &userId, bool on, base::Guard guard, Fn<void(bool)> done) {
	auto builder = _session->api().request(
		on ? Api::Put : Api::Delete,
		u"/v1/users/%1/post-notifications"_s.arg(PathSegment(userId)));
	if (on) {
		builder.body(QJsonObject());
	}
	builder
		.guard(this)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(Json::Bool(result, "enabled"_L1, on));
			}
		})
		.fail([=, this](const Api::Error &error) {
			_session->reportFailure(u"graph.post_notifications"_s, userId, error);
			if (done && guard.alive()) {
				done(!on);
			}
		})
		.send();
}

void Graph::followRequests(base::Guard guard, Fn<void(const QVector<Profile> &)> done, FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/me/follow-requests"_s)
		.query(u"limit"_s, 50)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto list = Json::List<Profile>(result.value("users"_L1).isArray()
				? result.value("users"_L1)
				: result.value("requests"_L1));
			_session->users().seed(list);
			if (done && guard.alive()) {
				done(list);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::incomingFollowRequests(const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail) {
	list(u"/v1/me/follow-requests"_s, cursor, guard, done, fail);
}

void Graph::acceptFollowRequest(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Post, u"/v1/me/follow-requests/%1/accept"_s.arg(PathSegment(userId)), userId, u"graph.accept_request"_s, guard, done, fail);
}

void Graph::declineFollowRequest(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail) {
	relation(Api::Post, u"/v1/me/follow-requests/%1/decline"_s.arg(PathSegment(userId)), userId, u"graph.decline_request"_s, guard, done, fail);
}

void Graph::sentFollowRequests(const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail) {
	list(u"/v1/me/follow-requests/sent"_s, cursor, guard, done, fail);
}

// ---- lists

void Graph::list(
		const QString &path,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const GraphList &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, path)
		.query(u"cursor"_s, cursor)
		.query(u"limit"_s, 50)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto list = GraphList();
			list.users = Json::List<Profile>(result.value("users"_L1));
			list.nextCursor = Json::String(result, "next_cursor"_L1);
			_session->users().seed(list.users);
			if (done && guard.alive()) {
				done(list);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::followers(const QString &userId, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail) {
	list(u"/v1/users/%1/followers"_s.arg(PathSegment(userId)), cursor, guard, done, fail);
}

void Graph::following(const QString &userId, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail) {
	list(u"/v1/users/%1/following"_s.arg(PathSegment(userId)), cursor, guard, done, fail);
}

void Graph::blocks(const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail) {
	list(u"/v1/me/blocks"_s, cursor, guard, done, fail);
}

void Graph::filters(base::Guard guard, Fn<void(const Filters &)> done, FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/me/filters"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Filters::FromJson(result));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::openConversationsOf(
		const QString &userId,
		base::Guard guard,
		Fn<void(const QVector<OpenConversation> &)> done) {
	_session->api().request(Api::Get, u"/v1/users/%1/conversations/open"_s.arg(PathSegment(userId)))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::List<OpenConversation>(result.value("conversations"_L1)));
			}
		})
		.fail([=](const Api::Error &) {
			if (done) {
				done({});
			}
		})
		.send();
}

// ---- search and suggestions

void Graph::search(
		const QString &query,
		const QString &type,
		int offset,
		base::Guard guard,
		Fn<void(const SearchResults &)> done,
		FailCallback fail) {
	auto builder = _session->api().request(Api::Get, u"/v1/search"_s);
	builder.query(u"q"_s, query).query(u"type"_s, type);
	if (offset > 0) {
		builder.query(u"offset"_s, offset);
	}
	builder
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto results = SearchResults();
			results.users = Json::List<Profile>(result.value("users"_L1).isArray()
				? result.value("users"_L1)
				: result.value("people"_L1));
			_session->users().seed(results.users);
			_session->users().seedJson(result.value("authors"_L1));
			results.posts = _session->feed().mergeJson(result.value("posts"_L1));
			results.topics = Json::Strings(result.value("topics"_L1));
			results.conversations = Json::List<OpenConversation>(result.value("conversations"_L1));
			results.nextOffset = Json::Int(result, "next_offset"_L1);
			if (done && guard.alive()) {
				done(results);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::searchPeople(
		const QString &query,
		base::Guard guard,
		Fn<void(const QVector<Profile> &)> done) {
	const auto trimmed = query.trimmed();
	if (trimmed.isEmpty()) {
		if (done && guard.alive()) {
			done({});
		}
		return;
	}
	_session->api().request(Api::Get, u"/v1/search"_s)
		.query(u"q"_s, trimmed)
		.query(u"type"_s, u"users"_s) // users | posts | topics | conversations
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			// A page of the server's search.max_page people.
			const auto list = Json::List<Profile>(result.value("users"_L1));
			_session->users().seed(list);
			if (done && guard.alive()) {
				done(list);
			}
		})
		.fail([=](const Api::Error &) {
			if (done && guard.alive()) {
				done({});
			}
		})
		.send();
}

void Graph::suggestions(
		const QString &cursor,
		int limit,
		base::Guard guard,
		Fn<void(const SuggestionsPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/suggestions/people"_s)
		.query(u"limit"_s, limit > 0 ? limit : 20)
		.query(u"cursor"_s, cursor)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto page = SuggestionsPage();
			page.people = Json::List<Profile>(result.value("people"_L1).isArray()
				? result.value("people"_L1)
				: result.value("users"_L1));
			page.nextCursor = Json::String(result, "next_cursor"_L1);
			_session->users().seed(page.people);
			for (const auto &person : page.people) {
				_session->users().seed(person.mutuals);
			}
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

QStringList Graph::pendingDismissals() const {
	return _pendingDismissals;
}

void Graph::savePendingDismissals(const QStringList &list) {
	_pendingDismissals = list.mid(std::max<qsizetype>(0, list.size() - 200));
	if (const auto db = _session->local()) {
		db->setValue(kDismissedKey, Json::Array(_pendingDismissals));
	}
}

void Graph::dismissSuggestion(const QString &userId) {
	Q_EMIT suggestionDismissed(userId);
	_session->api().request(Api::Post, u"/v1/suggestions/people/%1/dismiss"_s.arg(PathSegment(userId)))
		.body(QJsonObject())
		.guard(this)
		.fail([=, this](const Api::Error &error) {
			auto list = pendingDismissals();
			if (!list.contains(userId)) {
				list.push_back(userId);
			}
			savePendingDismissals(list);
			_session->reportFailure(u"suggestion.dismiss"_s, userId, error);
		})
		.send();
}

void Graph::retryDismissals() {
	const auto retry = [=, this] {
		for (const auto &userId : pendingDismissals()) {
			_session->api().request(Api::Post, u"/v1/suggestions/people/%1/dismiss"_s.arg(PathSegment(userId)))
				.body(QJsonObject())
				.guard(this)
				.done([=, this] {
					auto list = pendingDismissals();
					list.removeAll(userId);
					savePendingDismissals(list);
				})
				.send();
		}
	};
	const auto db = _session->local();
	if (_dismissalsLoaded || !db) {
		retry();
		return;
	}
	_dismissalsLoaded = true;
	db->value(kDismissedKey, this, [=, this](QJsonValue value) {
		for (const auto &id : Json::Strings(value)) {
			if (!_pendingDismissals.contains(id)) {
				_pendingDismissals.push_back(id);
			}
		}
		retry();
	});
}

// ---- my profile

void Graph::updateProfile(
		const ProfilePatch &patch,
		base::Guard guard,
		Fn<void(const Profile &)> done,
		FailCallback fail) {
	updateAttempt(patch.toJson(), 0, std::move(guard), std::move(done), std::move(fail));
}

void Graph::updateAttempt(
		const QJsonObject &body,
		int attempt,
		base::Guard guard,
		Fn<void(const Profile &)> done,
		FailCallback fail) {
	_session->api().request(Api::Patch, u"/v1/me/profile"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto profile = Profile::FromJson(result);
			_session->users().setMe(profile);
			if (done && guard.alive()) {
				done(profile);
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (error.code == u"media.not_ready" && attempt < kUpdateAttempts && guard.alive()) {
				base::call_delayed((attempt < 10) ? 1000 : 2000, this, [=, this] {
					updateAttempt(body, attempt + 1, guard, done, fail);
				});
				return;
			}
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Graph::setHandle(
		const QString &handle,
		base::Guard guard,
		Fn<void(const Profile &)> done,
		FailCallback fail) {
	_session->api().request(Api::Put, u"/v1/me/handle"_s)
		.body(QJsonObject{ { u"handle"_s, handle } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto profile = Profile::FromJson(result);
			_session->users().setMe(profile);
			if (done && guard.alive()) {
				done(profile);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::handleAvailable(
		const QString &handle,
		base::Guard guard,
		Fn<void(bool)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/handles/%1/availability"_s.arg(PathSegment(handle)))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::Bool(result, "available"_L1));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::setBirthday(
		const Birthday &birthday,
		base::Guard guard,
		Fn<void(const Profile &)> done,
		FailCallback fail) {
	auto body = birthday.toJson();
	body.remove("today"_L1);
	_session->api().request(Api::Put, u"/v1/me/birthday"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto profile = Profile::FromJson(result);
			_session->users().setMe(profile);
			if (done && guard.alive()) {
				done(profile);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::clearBirthday(base::Guard guard, Fn<void()> done, FailCallback fail) {
	_session->api().request(Api::Delete, u"/v1/me/birthday"_s)
		.guard(this)
		.done([=, this] {
			auto me = _session->users().me();
			me.birthday = std::nullopt;
			_session->users().setMe(me);
			if (done && guard.alive()) {
				done();
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

// ---- synced settings

QJsonValue Graph::DeepMerge(const QJsonValue &base, const QJsonValue &patch) {
	if (!patch.isObject()) {
		return patch;
	}
	auto result = base.toObject();
	const auto object = patch.toObject();
	for (auto i = object.begin(); i != object.end(); ++i) {
		result.insert(i.key(), DeepMerge(result.value(i.key()), i.value()));
	}
	return result;
}

QJsonObject Graph::SettingPatch(const QString &dotted, const QJsonValue &value) {
	const auto parts = dotted.split(u'.');
	auto current = value;
	for (auto i = parts.size(); i-- > 0;) {
		current = QJsonObject{ { parts[i], current } };
	}
	return current.toObject();
}

QJsonValue Graph::settingValue(const QString &dotted) const {
	auto current = QJsonValue(_settings);
	for (const auto &part : dotted.split(u'.')) {
		if (!current.isObject()) {
			return QJsonValue(QJsonValue::Undefined);
		}
		current = current.toObject().value(part);
	}
	return current;
}

void Graph::setSetting(const QString &dotted, const QJsonValue &value) {
	patchSettings(SettingPatch(dotted, value));
}

void Graph::settingsVersionChanged(qint64 version) {
	if (version > 0 && version == _settingsVersion) {
		return;
	}
	_settingsVersion = version;
	// Not read yet: whoever needs it reads the current one then.
	if (_settingsLoaded) {
		loadSettings(true, base::Guard(), nullptr);
	}
}

void Graph::loadSettings(
		bool force,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	if (_settingsLoaded && !force) {
		if (done && guard.alive()) {
			done(_settings);
		}
		return;
	}
	_session->api().request(Api::Get, u"/v1/me/settings"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			_settings = result;
			_settingsLoaded = true;
			Q_EMIT settingsChanged();
			if (done && guard.alive()) {
				done(_settings);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::patchSettings(
		const QJsonObject &partial,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	const auto previous = _settings;
	_settings = DeepMerge(_settings, partial).toObject();
	Q_EMIT settingsChanged();
	_session->api().request(Api::Patch, u"/v1/me/settings"_s)
		.body(partial)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (!result.isEmpty()) {
				_settings = DeepMerge(_settings, result).toObject();
				Q_EMIT settingsChanged();
			}
			if (done && guard.alive()) {
				done(_settings);
			}
		})
		.fail([=, this](const Api::Error &error) {
			_settings = previous;
			Q_EMIT settingsChanged();
			if (error.status == 412) {
				// The document moved on elsewhere: read it again.
				loadSettings(true, this, nullptr);
			}
			if (fail && guard.alive()) {
				fail(error);
			} else {
				_session->reportFailure(u"settings.patch"_s, QString(), error);
			}
		})
		.send();
}

// ---- muted words, hidden topics

void Graph::addMutedWord(const QString &word, base::Guard guard, Fn<void()> done) {
	simple(Api::Post, u"/v1/me/muted-words"_s, QJsonObject{ { u"word"_s, word } }, u"filters.mute_word"_s, word, guard, done);
}

void Graph::removeMutedWord(const QString &word, base::Guard guard, Fn<void()> done) {
	simple(Api::Post, u"/v1/me/muted-words/remove"_s, QJsonObject{ { u"word"_s, word } }, u"filters.unmute_word"_s, word, guard, done);
}

void Graph::unhideTopic(const QString &id, base::Guard guard, Fn<void()> done) {
	simple(Api::Delete, u"/v1/me/hidden-topics/"_s + PathSegment(id), QJsonObject(), u"topic.unhide"_s, id, guard, done);
}

// ---- account

void Graph::sessions(
		base::Guard guard,
		Fn<void(const QVector<DeviceSession> &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/sessions"_s)
		.guard(guard)
		.doneValue([=](const QJsonValue &result) {
			// A bare list or { sessions: [...] }.
			const auto list = result.isArray()
				? result
				: result.toObject().value("sessions"_L1);
			if (done) {
				done(Json::List<DeviceSession>(list));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::revokeSession(const QString &id, base::Guard guard, Fn<void()> done, FailCallback fail) {
	_session->api().request(Api::Delete, u"/v1/sessions/"_s + PathSegment(id))
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::revokeOtherSessions(base::Guard guard, Fn<void()> done, FailCallback fail) {
	_session->api().request(Api::Post, u"/v1/sessions/revoke-others"_s)
		.body(QJsonObject())
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

// ---- privacy

void Graph::lastSeenExceptions(
		const QString &kind,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const LastSeenExceptionsPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/me/privacy/last-seen/exceptions"_s)
		.query(u"kind"_s, kind)
		.query(u"cursor"_s, cursor)
		.query(u"limit"_s, 100)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = LastSeenExceptionsPage::FromJson(result);
			_session->users().seed(page.users);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Graph::setLastSeenException(
		const QString &userId,
		const QString &kind,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->api().request(
		Api::Put,
		u"/v1/me/privacy/last-seen/exceptions/"_s + PathSegment(userId)
	).body(QJsonObject{ { u"kind"_s, kind } })
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::removeLastSeenException(
		const QString &userId,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->api().request(
		Api::Delete,
		u"/v1/me/privacy/last-seen/exceptions/"_s + PathSegment(userId)
	).guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::contactBans(
		base::Guard guard,
		Fn<void(const QVector<ContactBan> &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/me/contact-bans"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::List<ContactBan>(result.value("bans"_L1)));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::removeContactBan(
		const QString &token,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->api().request(
		Api::Delete,
		u"/v1/me/contact-bans/"_s + PathSegment(token)
	).guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

// ---- account

void Graph::reportUnauthorizedSession(
		const QString &id,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->api().request(
		Api::Post,
		u"/v1/sessions/%1/report-unauthorized"_s.arg(PathSegment(id))
	).body(QJsonObject())
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::requestAccountCode(
		const QString &purpose,
		const QString &newPhone,
		const QString &deviceId,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	auto body = QJsonObject{
		{ u"purpose"_s, purpose },
		{ u"device_id"_s, deviceId },
	};
	Json::Put(body, "new_phone"_L1, newPhone);
	_session->api().request(Api::Post, u"/v1/account/otp"_s)
		.body(body)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(result);
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::changePhone(
		const QString &challengeId,
		const QString &code,
		const QString &password,
		const QString &deviceId,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	auto body = QJsonObject{
		{ u"challenge_id"_s, challengeId },
		{ u"code"_s, code },
		{ u"device_id"_s, deviceId },
	};
	Json::Put(body, "password"_L1, password);
	_session->api().request(Api::Post, u"/v1/account/phone/change"_s)
		.body(body)
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::deleteAccount(
		const QString &challengeId,
		const QString &code,
		const QString &password,
		const QString &deviceId,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	auto body = QJsonObject{
		{ u"challenge_id"_s, challengeId },
		{ u"code"_s, code },
		{ u"device_id"_s, deviceId },
	};
	Json::Put(body, "password"_L1, password);
	_session->api().request(Api::Post, u"/v1/account/deletion"_s)
		.body(body)
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::accountPhone(base::Guard guard, Fn<void(const QString &)> done) {
	_session->api().request(Api::Get, u"/v1/account/phone"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::String(result, "phone"_L1));
			}
		})
		.fail([=](const Api::Error &) {
			if (done) {
				done(QString());
			}
		})
		.send();
}

void Graph::latestExport(base::Guard guard, Fn<void(const QJsonObject &)> done, FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/me/exports/latest"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(result);
			}
		})
		.fail([=](const Api::Error &error) {
			if (error.notFound()) {
				if (done) {
					done(QJsonObject());
				}
			} else if (fail) {
				fail(error);
			}
		})
		.send();
}

void Graph::requestExportCode(
		const QString &deviceId,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	_session->api().request(Api::Post, u"/v1/account/otp"_s)
		.body(QJsonObject{
			{ u"purpose"_s, u"data_export"_s },
			{ u"device_id"_s, deviceId },
		})
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(result);
			}
		})
		.fail(std::move(fail))
		.send();
}

void Graph::requestExport(
		const QString &challengeId,
		const QString &code,
		const QString &deviceId,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	_session->api().request(Api::Post, u"/v1/me/exports"_s)
		.body(QJsonObject{
			{ u"challenge_id"_s, challengeId },
			{ u"code"_s, code },
			{ u"device_id"_s, deviceId },
		})
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(result);
			}
		})
		.fail(std::move(fail))
		.send();
}

} // namespace Data
