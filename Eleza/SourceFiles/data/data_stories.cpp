#include "data/data_stories.h"

#include "api/api_client_config.h"
#include "base/unixtime.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"

#include <QtCore/QUuid>

namespace Data {
namespace {

constexpr auto kCreateAttempts = 30;
constexpr auto kCreateRetryMs = 1000;

[[nodiscard]] QString NewKey() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] qint64 NowSeconds() {
	return base::unixtime::nowMs() / 1000;
}

} // namespace

QJsonObject CreateStory::toJson() const {
	auto result = QJsonObject();
	if (!postId.isEmpty()) {
		result.insert(u"post_id"_s, postId);
	} else {
		result.insert(u"media_id"_s, mediaId);
	}
	Json::Put(result, "caption"_L1, caption.trimmed());
	result.insert(
		u"audience"_s,
		closeFriends ? u"close_friends"_s : u"everyone"_s);
	return result;
}

Stories::Stories(Session *session)
: QObject(session)
, _session(session)
, _seenTimer([this] { flushSeen(); })
, _signalTimer([this] {
	const auto users = std::exchange(_signalUsers, {});
	for (const auto &id : users) {
		if (const auto i = _users.find(id); i != _users.end()) {
			i->second.fresh = false;
		}
	}
	refreshTray();
}) {
	_wasEnabled = enabled();
	if (const auto config = _session->clientConfig()) {
		connect(config, &Api::ClientConfig::changed, this, [this] {
			const auto now = enabled();
			if (_wasEnabled != now) {
				_wasEnabled = now;
				Q_EMIT enabledChanged();
			}
		});
	}
	connect(&session->transport(), &Transport::frameReceived, this, [this](
			const Rt::Frame &frame) {
		if (frame.type == Rt::Frame::Type::Signal && frame.kind == u"story") {
			// { user?, action: new | deleted }: a hint, never data.
			handleSignal(frame.data.value("user"_L1).toString());
		}
	});
}

Stories::~Stories() = default;

bool Stories::enabled() const {
	const auto config = _session->clientConfig();
	return !config || config->boolean(u"feature.stories"_s, true);
}

// ---- the tray

void Stories::refreshTray() {
	if (!enabled()) {
		return;
	}
	const auto request = ++_trayRequest;
	_trayLoading = true;
	_session->transport().rpc(u"stories.tray"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (request != _trayRequest) {
				return;
			}
			_trayLoading = false;
			applyTray(StoryTrayPage::FromJson(result), true);
		})
		.fail([=, this](const Api::Error &) {
			if (request == _trayRequest) {
				_trayLoading = false;
			}
		})
		.send();
}

void Stories::loadMoreTray() {
	if (_trayLoading || _trayCursor.isEmpty()) {
		return;
	}
	const auto request = ++_trayRequest;
	_trayLoading = true;
	_session->transport().rpc(u"stories.tray"_s)
		.body({ { u"cursor"_s, _trayCursor } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (request != _trayRequest) {
				return;
			}
			_trayLoading = false;
			applyTray(StoryTrayPage::FromJson(result), false);
		})
		.fail([=, this](const Api::Error &) {
			if (request == _trayRequest) {
				_trayLoading = false;
			}
		})
		.send();
}

void Stories::applyTray(const StoryTrayPage &page, bool first) {
	auto changed = QStringList();
	if (first) {
		for (const auto &item : std::as_const(_tray)) {
			changed.push_back(item.user.id);
		}
		_tray.clear();
	}
	if (page.me.user.valid() || first) {
		_me = page.me;
		if (_me.user.id.isEmpty()) {
			_me.user.id = _session->userId();
		}
		changed.push_back(_me.user.id);
	}
	auto known = QSet<QString>();
	for (const auto &item : std::as_const(_tray)) {
		known.insert(item.user.id);
	}
	auto cards = QVector<User>();
	for (const auto &item : page.items) {
		if (known.contains(item.user.id)) {
			continue;
		}
		known.insert(item.user.id);
		_tray.push_back(item);
		cards.push_back(item.user);
		changed.push_back(item.user.id);
	}
	if (_me.user.valid() && !_me.user.displayName.isEmpty()) {
		cards.push_back(_me.user);
	}
	SortTray(_tray);
	_trayCursor = page.nextCursor;
	_trayLoaded = true;
	if (!cards.isEmpty()) {
		_session->users().seed(cards);
	}
	Q_EMIT trayChanged();
	emitRings(changed);
}

QStringList Stories::trayUserIds() const {
	auto result = QStringList();
	result.reserve(_tray.size());
	for (const auto &item : _tray) {
		result.push_back(item.user.id);
	}
	return result;
}

StoryTrayItem *Stories::trayItem(const QString &userId) {
	for (auto &item : _tray) {
		if (item.user.id == userId) {
			return &item;
		}
	}
	return nullptr;
}

void Stories::handleSignal(const QString &userId) {
	if (!userId.isEmpty()) {
		_signalUsers.insert(userId);
	}
	_signalTimer.callOnce(kSignalDebounceMs);
}

// ---- rings

StoryRingState Stories::ringState(const QString &userId) const {
	if (userId.isEmpty() || !enabled()) {
		return {};
	}
	if (userId == _session->userId() && _trayLoaded) {
		return RingStateOf(_me);
	}
	for (const auto &item : _tray) {
		if (item.user.id == userId) {
			return RingStateOf(item);
		}
	}
	if (const auto i = _users.find(userId); i != _users.end() && i->second.fresh) {
		const auto now = NowSeconds();
		auto result = StoryRingState();
		for (const auto &story : i->second.data.stories) {
			if (story.expired(now)) {
				continue;
			}
			if (!result.hasStory || (!result.unseen && !story.seen)) {
				// The first one I have not seen decides the colour.
				result.closeFriends = story.closeFriends;
			}
			result.hasStory = true;
			result.unseen = result.unseen || !story.seen;
		}
		return result;
	}
	return _hints.value(userId);
}

void Stories::applyAuthorHints(const QJsonValue &authors) {
	auto changed = QStringList();
	const auto apply = [&](const QString &key, const QJsonObject &card) {
		const auto id = card.value("id"_L1).toString(key);
		if (id.isEmpty() || !card.contains("has_story"_L1)) {
			return;
		}
		auto state = StoryRingState();
		state.hasStory = Json::Bool(card, "has_story"_L1);
		state.unseen = state.hasStory && Json::Bool(card, "story_unseen"_L1);
		state.closeFriends = state.hasStory
			&& Json::Bool(card, "story_close_friends"_L1);
		if (_hints.value(id) != state) {
			_hints.insert(id, state);
			changed.push_back(id);
		}
	};
	if (authors.isArray()) {
		for (const auto &value : authors.toArray()) {
			apply(QString(), value.toObject());
		}
	} else {
		const auto map = authors.toObject();
		for (auto i = map.begin(); i != map.end(); ++i) {
			apply(i.key(), i.value().toObject());
		}
	}
	emitRings(changed);
}

void Stories::emitRings(const QStringList &ids) {
	auto changed = QStringList();
	for (const auto &id : ids) {
		if (id.isEmpty() || changed.contains(id)) {
			continue;
		}
		const auto now = ringState(id);
		const auto i = _rings.find(id);
		if (i == _rings.end() || *i != now) {
			_rings.insert(id, now);
			changed.push_back(id);
		}
	}
	if (!changed.isEmpty()) {
		Q_EMIT ringsChanged(changed);
	}
}

void Stories::refreshRingOf(const QString &userId) {
	emitRings({ userId });
}

// ---- the stories of one person

const UserStories *Stories::cached(const QString &userId) const {
	const auto i = _users.find(userId);
	return (i != _users.end()) ? &i->second.data : nullptr;
}

const Story *Stories::story(const QString &storyId) const {
	for (const auto &[id, entry] : _users) {
		for (const auto &story : entry.data.stories) {
			if (story.id == storyId) {
				return &story;
			}
		}
	}
	return nullptr;
}

void Stories::loadUser(
		const QString &userId,
		base::Guard guard,
		Fn<void(const UserStories &)> done,
		FailCallback fail) {
	_session->transport().rpc(u"stories.user"_s)
		.body({ { u"user"_s, userId } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			applyUserStories(UserStories::FromJson(result, userId));
			if (done && guard.alive()) {
				done(_users[userId].data);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Stories::applyUserStories(const UserStories &list) {
	const auto userId = list.user.id;
	if (userId.isEmpty()) {
		return;
	}
	auto &entry = _users[userId];
	entry.data = list;
	entry.fresh = true;
	if (list.user.valid() && !list.user.displayName.isEmpty()) {
		_session->users().seed(list.user);
	}
	// What the tray and the hints say follows what was just read.
	const auto now = NowSeconds();
	auto showing = 0;
	auto unseen = 0;
	for (const auto &story : list.stories) {
		if (!story.expired(now)) {
			++showing;
			unseen += story.seen ? 0 : 1;
		}
	}
	if (userId == _session->userId()) {
		_me.count = showing;
		_me.hasUnseenByMe = (unseen > 0);
		if (!list.stories.isEmpty()) {
			_me.latestAt = list.stories.back().createdAt;
			_me.closeFriends = list.stories.back().closeFriends;
		}
		Q_EMIT trayChanged();
	} else if (const auto item = trayItem(userId)) {
		if (!showing) {
			_tray.erase(_tray.begin() + (item - _tray.data()));
		} else {
			item->count = showing;
			item->unseen = unseen;
		}
		SortTray(_tray);
		Q_EMIT trayChanged();
	}
	if (const auto i = _hints.find(userId); i != _hints.end()) {
		i->hasStory = (showing > 0);
		i->unseen = (unseen > 0);
	}
	Q_EMIT userStoriesChanged(userId);
	refreshRingOf(userId);
}

void Stories::patchStory(const QString &storyId, const Fn<void(Story &)> &change) {
	for (auto &[userId, entry] : _users) {
		for (auto &story : entry.data.stories) {
			if (story.id == storyId) {
				change(story);
				const auto owner = userId;
				Q_EMIT userStoriesChanged(owner);
				return;
			}
		}
	}
}

// ---- seen

void Stories::markSeen(const QString &storyId) {
	if (storyId.isEmpty()) {
		return;
	}
	auto owner = QString();
	auto wasUnseen = false;
	auto stillUnseen = 0;
	auto firstUnseenClose = false;
	for (auto &[userId, entry] : _users) {
		for (auto &story : entry.data.stories) {
			if (story.id == storyId) {
				owner = userId;
				wasUnseen = !story.seen;
				story.seen = true;
			}
		}
		if (!owner.isEmpty()) {
			for (const auto &story : entry.data.stories) {
				if (!story.seen) {
					if (!stillUnseen) {
						firstUnseenClose = story.closeFriends;
					}
					++stillUnseen;
				}
			}
			break;
		}
	}
	if (!_seenSent.contains(storyId) && !_seenQueue.contains(storyId)) {
		_seenQueue.push_back(storyId);
		if (!_seenTimer.isActive()) {
			_seenTimer.callOnce(kSeenFlushMs);
		}
	}
	if (owner.isEmpty() || !wasUnseen) {
		return;
	}
	if (owner == _session->userId()) {
		_me.hasUnseenByMe = (stillUnseen > 0);
		Q_EMIT trayChanged();
	} else if (const auto item = trayItem(owner)) {
		item->unseen = std::min(std::max(item->unseen - 1, 0), stillUnseen);
		if (item->unseen > 0) {
			item->closeFriends = firstUnseenClose;
		}
		SortTray(_tray);
		Q_EMIT trayChanged();
	}
	if (const auto i = _hints.find(owner); i != _hints.end()) {
		i->unseen = (stillUnseen > 0);
	}
	Q_EMIT userStoriesChanged(owner);
	refreshRingOf(owner);
}

void Stories::flushSeen() {
	_seenTimer.cancel();
	while (!_seenQueue.isEmpty()) {
		const auto batch = _seenQueue.mid(0, kSeenBatch);
		_seenQueue = _seenQueue.mid(batch.size());
		sendSeen(batch);
	}
}

void Stories::sendSeen(const QStringList &ids) {
	for (const auto &id : ids) {
		_seenSent.insert(id);
	}
	_session->transport().rpc(u"stories.seen"_s)
		.body({ { u"ids"_s, Json::Array(ids) } })
		.guard(this)
		.done([] {})
		.fail([=, this](const Api::Error &error) {
			if (!error.retryable && !error.offline()) {
				return;
			}
			// Sent again with the next batch (the call is idempotent).
			for (const auto &id : ids) {
				_seenSent.remove(id);
				if (!_seenQueue.contains(id)) {
					_seenQueue.push_back(id);
				}
			}
			if (!_seenTimer.isActive()) {
				_seenTimer.callOnce(kSeenFlushMs * 4);
			}
		})
		.send();
}

// ---- reactions and replies

void Stories::simple(
		const QString &method,
		const QJsonObject &body,
		const QString &action,
		const QString &subject,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->transport().rpc(method)
		.body(body)
		.guard(this)
		.done([=] {
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			} else if (!fail) {
				_session->reportFailure(action, subject, error);
			}
		})
		.send();
}

void Stories::react(
		const QString &storyId,
		const QString &emoji,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	auto was = QString();
	patchStory(storyId, [&](Story &story) {
		was = story.myReaction;
		story.myReaction = emoji;
		story.seen = true;
	});
	simple(
		u"stories.react"_s,
		{ { u"story"_s, storyId }, { u"emoji"_s, emoji } },
		u"story.react"_s,
		storyId,
		guard,
		done,
		[=, this](const Api::Error &error) {
			patchStory(storyId, [&](Story &story) {
				story.myReaction = was;
			});
			if (fail) {
				fail(error);
			} else {
				_session->reportFailure(u"story.react"_s, storyId, error);
			}
		});
}

void Stories::unreact(
		const QString &storyId,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	auto was = QString();
	patchStory(storyId, [&](Story &story) {
		was = story.myReaction;
		story.myReaction = QString();
	});
	simple(
		u"stories.unreact"_s,
		{ { u"story"_s, storyId } },
		u"story.unreact"_s,
		storyId,
		guard,
		done,
		[=, this](const Api::Error &error) {
			patchStory(storyId, [&](Story &story) {
				story.myReaction = was;
			});
			if (fail) {
				fail(error);
			} else {
				_session->reportFailure(u"story.react"_s, storyId, error);
			}
		});
}

void Stories::reply(
		const QString &storyId,
		const QString &text,
		base::Guard guard,
		Fn<void(const QString &chat, qint64 seq)> done,
		FailCallback fail) {
	_session->transport().rpc(u"stories.reply"_s)
		.body({ { u"story"_s, storyId }, { u"text"_s, text } })
		.guard(this)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(
					Json::String(result, "chat"_L1),
					Json::Int64(result, "seq"_L1));
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

// ---- my stories

void Stories::viewers(
		const QString &storyId,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const StoryViewersPage &)> done,
		FailCallback fail) {
	auto body = QJsonObject{ { u"story"_s, storyId } };
	Json::Put(body, "cursor"_L1, cursor);
	_session->transport().rpc(u"stories.viewers"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = StoryViewersPage::FromJson(result);
			auto cards = QVector<User>();
			for (const auto &viewer : page.viewers) {
				cards.push_back(viewer.user);
			}
			_session->users().seed(cards);
			if (cursor.isEmpty()) {
				patchStory(storyId, [&](Story &story) {
					story.viewerCount = page.total;
				});
			}
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Stories::deleteStory(
		const QString &storyId,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	const auto finished = [=, this] {
		const auto me = _session->userId();
		if (const auto i = _users.find(me); i != _users.end()) {
			auto &list = i->second.data.stories;
			list.erase(std::remove_if(list.begin(), list.end(), [&](const Story &story) {
				return story.id == storyId;
			}), list.end());
			applyUserStories(i->second.data);
		} else if (_me.count > 0) {
			--_me.count;
			Q_EMIT trayChanged();
			refreshRingOf(me);
		}
		if (done && guard.alive()) {
			done();
		}
	};
	simple(
		u"stories.delete"_s,
		{ { u"story"_s, storyId } },
		u"story.delete"_s,
		storyId,
		guard,
		finished,
		[=](const Api::Error &error) {
			if (error.notFound()) {
				// Gone already: the same outcome.
				finished();
			} else if (fail && guard.alive()) {
				fail(error);
			}
		});
}

void Stories::create(
		const CreateStory &request,
		base::Guard guard,
		Fn<void(const Story &)> done,
		FailCallback fail) {
	createAttempt(request.toJson(), NewKey(), 0, guard, done, fail);
}

void Stories::createAttempt(
		const QJsonObject &body,
		const QString &key,
		int attempt,
		base::Guard guard,
		Fn<void(const Story &)> done,
		FailCallback fail) {
	_session->transport().rpc(u"stories.create"_s)
		.body(body)
		.idempotencyKey(key)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto story = Story::FromJson(result);
			story.owner = _session->userId();
			addMine(story);
			if (done && guard.alive()) {
				done(story);
			}
		})
		.fail([=, this](const Api::Error &error) {
			// The uploaded media is still being processed: the same request again.
			if (error.code == u"media.not_ready"
				&& attempt < kCreateAttempts
				&& guard.alive()) {
				base::call_delayed(kCreateRetryMs, this, [=, this] {
					createAttempt(body, key, attempt + 1, guard, done, fail);
				});
				return;
			}
			if (fail && guard.alive()) {
				fail(error);
			}
		})
		.send();
}

void Stories::addMine(const Story &story) {
	if (!story.valid()) {
		return;
	}
	const auto me = _session->userId();
	if (const auto i = _users.find(me); i != _users.end()) {
		auto list = i->second.data;
		list.stories.push_back(story);
		applyUserStories(list);
	} else {
		++_me.count;
		_me.latestAt = story.createdAt;
		_me.closeFriends = story.closeFriends;
		Q_EMIT trayChanged();
		refreshRingOf(me);
	}
}

// ---- posting from a file

bool Stories::post(const PostStoryRequest &request) {
	if (_posting.active || request.filePath.isEmpty()) {
		return false;
	}
	_postingRequest = request;
	_postingMediaId = QString();
	_postingKey = NewKey();
	startPosting();
	return true;
}

void Stories::retryPosting() {
	if (_posting.active || _postingRequest.filePath.isEmpty()) {
		return;
	}
	startPosting();
}

void Stories::cancelPosting() {
	++_postingGeneration;
	if (_postingUpload) {
		_session->upload().cancel(std::exchange(_postingUpload, 0));
	}
	_posting = StoryPosting();
	_postingRequest = PostStoryRequest();
	_postingMediaId = QString();
	Q_EMIT postingChanged();
}

void Stories::startPosting() {
	const auto generation = ++_postingGeneration;
	_posting = StoryPosting();
	_posting.active = true;
	_posting.filePath = _postingRequest.filePath;
	_posting.video = _postingRequest.video;
	Q_EMIT postingChanged();

	const auto failed = [=, this](const Api::Error &error) {
		if (generation != _postingGeneration) {
			return;
		}
		_postingUpload = 0;
		_posting.active = false;
		_posting.failed = true;
		_posting.error = error;
		Q_EMIT postingChanged();
	};
	const auto createWith = [=, this](const QString &mediaId) {
		_postingMediaId = mediaId;
		_posting.progress = 0.95;
		Q_EMIT postingChanged();
		auto request = CreateStory();
		request.mediaId = mediaId;
		request.caption = _postingRequest.caption;
		request.closeFriends = _postingRequest.closeFriends;
		createAttempt(request.toJson(), _postingKey, 0, this, [=, this](
				const Story &story) {
			if (generation != _postingGeneration) {
				return;
			}
			_posting = StoryPosting();
			_postingRequest = PostStoryRequest();
			_postingMediaId = QString();
			Q_EMIT postingChanged();
			Q_EMIT posted(story);
			refreshTray();
		}, failed);
	};
	if (!_postingMediaId.isEmpty()) {
		// The upload went through before: only the story is missing.
		createWith(_postingMediaId);
		return;
	}
	auto upload = UploadRequest();
	upload.filePath = _postingRequest.filePath;
	upload.kind = _postingRequest.video ? UploadKind::Video : UploadKind::Image;
	upload.purpose = UploadPurpose::Story;
	_postingUpload = _session->upload().start(upload, this, {
		.onUploadId = nullptr,
		.progress = [=, this](qint64 sent, qint64 total) {
			if (generation != _postingGeneration || total <= 0) {
				return;
			}
			_posting.progress = 0.9 * double(sent) / double(total);
			Q_EMIT postingChanged();
		},
		.done = [=, this](const UploadResult &result) {
			if (generation != _postingGeneration) {
				return;
			}
			_postingUpload = 0;
			createWith(result.mediaId);
		},
		.fail = failed,
	});
}

// ---- mute, hide-from, close friends, settings

void Stories::setMuted(
		const QString &userId,
		bool muted,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	const auto apply = [=, this](bool value) {
		if (const auto item = trayItem(userId)) {
			item->muted = value;
			SortTray(_tray);
			Q_EMIT trayChanged();
		}
		if (const auto i = _hints.find(userId); i != _hints.end()) {
			i->muted = value;
		}
		refreshRingOf(userId);
	};
	apply(muted);
	simple(
		muted ? u"stories.mute"_s : u"stories.unmute"_s,
		{ { u"user"_s, userId } },
		u"story.mute"_s,
		userId,
		guard,
		done,
		[=, this](const Api::Error &error) {
			apply(!muted);
			if (fail) {
				fail(error);
			} else {
				_session->reportFailure(u"story.mute"_s, userId, error);
			}
		});
}

void Stories::usersPage(
		const QString &method,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const StoryUsersPage &)> done,
		FailCallback fail) {
	auto body = QJsonObject();
	Json::Put(body, "cursor"_L1, cursor);
	_session->transport().rpc(method)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = StoryUsersPage::FromJson(result);
			_session->users().seed(page.users);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Stories::hidden(
		const QString &cursor,
		base::Guard guard,
		Fn<void(const StoryUsersPage &)> done,
		FailCallback fail) {
	usersPage(u"stories.hidden"_s, cursor, guard, done, fail);
}

void Stories::setHidden(
		const QString &userId,
		bool hidden,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	simple(
		hidden ? u"stories.hide"_s : u"stories.unhide"_s,
		{ { u"user"_s, userId } },
		u"story.hide"_s,
		userId,
		guard,
		[=, this] {
			if (_settings) {
				_settings->hiddenCount = std::max(
					_settings->hiddenCount + (hidden ? 1 : -1),
					0);
			}
			if (done) {
				done();
			}
		},
		fail);
}

void Stories::closeFriends(
		const QString &cursor,
		base::Guard guard,
		Fn<void(const StoryUsersPage &)> done,
		FailCallback fail) {
	usersPage(u"stories.close_friends"_s, cursor, guard, done, fail);
}

void Stories::setCloseFriend(
		const QString &userId,
		bool on,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	simple(
		on ? u"stories.close_friend_add"_s : u"stories.close_friend_remove"_s,
		{ { u"user"_s, userId } },
		u"story.close_friend"_s,
		userId,
		guard,
		[=, this] {
			if (_settings) {
				_settings->closeFriendsCount = std::max(
					_settings->closeFriendsCount + (on ? 1 : -1),
					0);
			}
			if (done) {
				done();
			}
		},
		fail);
}

void Stories::updateCloseFriends(
		const QStringList &add,
		const QStringList &remove,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	if (add.isEmpty() && remove.isEmpty()) {
		if (done && guard.alive()) {
			done();
		}
		return;
	}
	simple(
		u"stories.close_friends_bulk"_s,
		{ { u"add"_s, Json::Array(add) }, { u"remove"_s, Json::Array(remove) } },
		u"story.close_friends"_s,
		QString(),
		guard,
		[=, this] {
			if (_settings) {
				_settings->closeFriendsCount = std::max(
					_settings->closeFriendsCount
						+ int(add.size())
						- int(remove.size()),
					0);
			}
			if (done) {
				done();
			}
		},
		fail);
}

void Stories::settings(
		base::Guard guard,
		Fn<void(const StorySettings &)> done,
		FailCallback fail) {
	_session->transport().rpc(u"stories.settings"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			_settings = StorySettings::FromJson(result);
			if (done && guard.alive()) {
				done(*_settings);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Stories::updateSettings(
		const QString &replies,
		base::Guard guard,
		Fn<void(const StorySettings &)> done,
		FailCallback fail) {
	_session->transport().rpc(u"stories.settings_update"_s)
		.body({ { u"replies"_s, replies } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			_settings = StorySettings::FromJson(result);
			if (done && guard.alive()) {
				done(*_settings);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

} // namespace Data
