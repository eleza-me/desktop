#include "data/data_feed.h"

#include "base/unixtime.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_transport.h"
#include "data/data_users.h"

namespace Data {
namespace {

constexpr auto kViewsFlushMs = 5000;
constexpr auto kViewsBatch = 100;
constexpr auto kCountsDebounceMs = 400;
constexpr auto kCountsBatch = 50;
constexpr auto kCountsMaxAgeMs = 60 * 1000;
constexpr auto kCreateAttempts = 60;

[[nodiscard]] QString ReactionName(Feed::Reaction reaction) {
	switch (reaction) {
	case Feed::Reaction::Like: return u"like"_s;
	case Feed::Reaction::Dislike: return u"dislike"_s;
	case Feed::Reaction::None: break;
	}
	return u"none"_s;
}

} // namespace

QJsonObject CreatePost::toJson() const {
	auto result = QJsonObject{ { u"type"_s, type } };
	Json::Put(result, "text"_L1, text);
	Json::Put(result, "reply_to"_L1, replyTo);
	Json::Put(result, "quote_of"_L1, quoteOf);
	Json::Put(result, "reply_policy"_L1, replyPolicy);
	if (!media.isEmpty()) {
		auto list = QJsonArray();
		for (const auto &item : media) {
			auto object = QJsonObject{ { u"id"_s, item.id } };
			Json::Put(object, "alt"_L1, item.alt);
			list.push_back(object);
		}
		result.insert("media"_L1, list);
	}
	if (!pollOptions.isEmpty()) {
		result.insert("poll"_L1, QJsonObject{
			{ u"question"_s, pollQuestion },
			{ u"options"_s, Json::Array(pollOptions) },
			{ u"duration"_s, double(pollDuration) },
		});
	}
	Json::Put(result, "conversation"_L1, conversation);
	Json::Put(result, "thread_parts"_L1, Json::Array(threadParts));
	return result;
}

Feed::Feed(Session *session)
: QObject(session)
, _session(session)
, _viewTimer([this] { flushViews(); })
, _countsTimer([this] { sendCounts(); }) {
	connect(&session->transport(), &Transport::frameReceived, this, [this](const Rt::Frame &frame) {
		if (frame.type == Rt::Frame::Type::FeedNew) {
			Q_EMIT newPosts(frame.count, frame.since);
		}
	});
}

// ---- the post store

const Post *Feed::post(const QString &id) const {
	const auto i = _posts.constFind(id);
	return (i != _posts.cend()) ? &*i : nullptr;
}

void Feed::store(const Post &post, qint64 now) {
	if (!post.valid()) {
		return;
	}
	if (post.quote) {
		store(*post.quote, now);
	}
	for (const auto &part : post.threadParts) {
		store(part, now);
	}
	const auto i = _posts.find(post.id);
	if (i == _posts.end()) {
		_posts.insert(post.id, post);
		_statsAt.insert(post.id, now);
	} else if (_localUntil.value(post.id) > now) {
		// A local (optimistic) change is fresher than this copy's stats.
		const auto counts = i->counts;
		const auto my = i->my;
		*i = post;
		i->counts = counts;
		i->my = my;
	} else {
		*i = post;
		_statsAt.insert(post.id, now);
	}
	Q_EMIT postChanged(post.id);
}

void Feed::merge(const QVector<Post> &posts) {
	const auto now = base::unixtime::nowMs();
	for (const auto &post : posts) {
		store(post, now);
	}
}

void Feed::noteReplyAdded(const QString &id) {
	patch(id, [](Post &post) {
		++post.counts.replies;
	});
}

QStringList Feed::mergeJson(const QJsonValue &list) {
	auto ids = QStringList();
	const auto now = base::unixtime::nowMs();
	for (const auto &item : list.toArray()) {
		const auto post = Post::FromJson(item.toObject());
		if (post.valid()) {
			store(post, now);
			ids.push_back(post.id);
		}
	}
	return ids;
}

void Feed::patch(const QString &id, const Fn<void(Post &)> &change) {
	const auto i = _posts.find(id);
	if (i == _posts.end()) {
		return;
	}
	_localUntil.insert(id, base::unixtime::nowMs() + kLocalHoldMs);
	change(*i);
	Q_EMIT postChanged(id);
}

void Feed::markDeleted(const QString &id) {
	const auto i = _posts.find(id);
	if (i == _posts.end()) {
		return;
	}
	i->deleted = true;
	i->text = QString();
	i->media.clear();
	i->poll = std::nullopt;
	i->quote = nullptr;
	Q_EMIT postChanged(id);
}

void Feed::applyCounts(const QJsonObject &map) {
	const auto now = base::unixtime::nowMs();
	for (auto i = map.begin(); i != map.end(); ++i) {
		const auto post = _posts.find(i.key());
		if (post == _posts.end() || _localUntil.value(i.key()) > now) {
			continue;
		}
		const auto value = i.value().toObject();
		if (value.value("counts"_L1).isObject()) {
			post->counts = PostCounts::FromJson(value.value("counts"_L1).toObject());
		}
		if (value.value("my"_L1).isObject()) {
			post->my = PostMy::FromJson(value.value("my"_L1).toObject());
		}
		_statsAt.insert(i.key(), now);
		Q_EMIT postChanged(i.key());
	}
}

// ---- parsing

FeedPage Feed::parseFeed(const QJsonObject &json, bool videoOnly) {
	auto page = FeedPage();
	_session->users().seedJson(json.value("authors"_L1));
	// The author cards carry the story ring hints (has_story, story_unseen).
	_session->stories().applyAuthorHints(json.value("authors"_L1));
	const auto now = base::unixtime::nowMs();
	auto actors = QStringList();
	for (const auto &value : json.value("items"_L1).toArray()) {
		const auto object = value.toObject();
		const auto post = Post::FromJson(object.value("post"_L1).toObject());
		if (!post.valid()) {
			continue;
		}
		if (videoOnly) {
			const auto hasVideo = std::any_of(post.media.cbegin(), post.media.cend(), [](const PostMedia &media) {
				return media.kind == u"video";
			});
			if (!hasVideo) {
				continue;
			}
		}
		store(post, now);
		auto item = FeedItem();
		item.postId = post.id;
		item.reason = Json::String(object, "reason"_L1);
		item.actor = Json::String(object, "actor"_L1);
		if (!item.actor.isEmpty()) {
			actors.push_back(item.actor);
		}
		_session->users().request(post.author);
		page.items.push_back(item);
	}
	_session->users().request(actors);
	page.nextCursor = Json::String(json, "next_cursor"_L1);
	return page;
}

PostsPage Feed::parsePosts(const QJsonObject &json) {
	auto page = PostsPage();
	_session->users().seedJson(json.value("authors"_L1));
	page.posts = mergeJson(json.value("posts"_L1));
	for (const auto &id : page.posts) {
		if (const auto found = post(id)) {
			_session->users().request(found->author);
		}
	}
	page.nextBefore = Json::String(json, "next_before"_L1);
	page.nextCursor = Json::String(json, "next_cursor"_L1);
	return page;
}

void Feed::simple(
		Api::Method method,
		const QString &path,
		const QJsonObject &body,
		const QString &action,
		const QString &subject,
		Fn<void()> onDone,
		Fn<void()> onFail) {
	auto builder = _session->api().request(method, path);
	if (method != Api::Get && method != Api::Delete) {
		builder.body(body);
	}
	builder
		.guard(this)
		.done([=] {
			if (onDone) {
				onDone();
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (onFail) {
				onFail();
			}
			_session->reportFailure(action, subject, error);
		})
		.send();
}

// ---- feeds

void Feed::loadFeed(
		Tab tab,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const FeedPage &)> done,
		FailCallback fail) {
	_session->api().request(
		Api::Get,
		(tab == Tab::ForYou) ? u"/v1/feed/for-you"_s : u"/v1/feed/following"_s)
		.query(u"cursor"_s, cursor)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = parseFeed(result, false);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::loadVideoFeed(
		const QString &cursor,
		base::Guard guard,
		Fn<void(const FeedPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/feed/video"_s)
		.query(u"cursor"_s, cursor)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = parseFeed(result, true);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::newAuthors(qint64 since, base::Guard guard, Fn<void(const QStringList &)> done) {
	// Reads the first page once; it is not applied to any list.
	_session->api().request(Api::Get, u"/v1/feed/following"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			_session->users().seedJson(result.value("authors"_L1));
			auto posts = QVector<Post>();
			for (const auto &value : result.value("items"_L1).toArray()) {
				const auto post = Post::FromJson(value.toObject().value("post"_L1).toObject());
				if (post.valid()) {
					posts.push_back(post);
				}
			}
			std::sort(posts.begin(), posts.end(), [](const Post &a, const Post &b) {
				return a.createdAt > b.createdAt;
			});
			const auto me = _session->userId();
			auto authors = QStringList();
			for (const auto &post : posts) {
				if (post.createdAt <= since || post.author == me || authors.contains(post.author)) {
					continue;
				}
				authors.push_back(post.author);
				if (authors.size() == 3) {
					break;
				}
			}
			if (done && guard.alive()) {
				done(authors);
			}
		})
		.fail([=](const Api::Error &) {
			if (done && guard.alive()) {
				done({});
			}
		})
		.send();
}

void Feed::newCount(qint64 since, base::Guard guard, Fn<void(int)> done) {
	_session->api().request(Api::Get, u"/v1/feed/following/new"_s)
		.query(u"since"_s, since)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::Int(result, "count"_L1));
			}
		})
		.fail([=](const Api::Error &) {
			if (done) {
				done(0);
			}
		})
		.send();
}

void Feed::watchNew(qint64 since) {
	if (since > 0) {
		_session->transport().subscribeFeed(since);
	} else {
		_session->transport().unsubscribeFeed();
	}
}

// ---- posts

void Feed::fetchPost(
		const QString &id,
		base::Guard guard,
		Fn<void(const Post &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/posts/"_s + PathSegment(id))
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto post = Post::FromJson(result);
			merge({ post });
			_session->users().request(post.author);
			if (done && guard.alive()) {
				done(post);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::createPost(
		const CreatePost &body,
		const QString &key,
		base::Guard guard,
		Fn<void(const Post &)> done,
		FailCallback fail) {
	createAttempt(body.toJson(), key, 0, std::move(guard), std::move(done), std::move(fail));
}

void Feed::createAttempt(
		const QJsonObject &body,
		const QString &key,
		int attempt,
		base::Guard guard,
		Fn<void(const Post &)> done,
		FailCallback fail) {
	_session->api().request(Api::Post, u"/v1/posts"_s)
		.body(body)
		.idempotencyKey(key)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto post = Post::FromJson(result);
			merge({ post });
			Q_EMIT postPublished(post.id);
			if (done && guard.alive()) {
				done(post);
			}
		})
		.fail([=, this](const Api::Error &error) {
			// The uploaded media is still being processed: the same request again.
			if (error.code == u"media.not_ready" && attempt < kCreateAttempts && guard.alive()) {
				base::call_delayed(1000, this, [=, this] {
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

void Feed::editPost(
		const QString &id,
		const QString &text,
		base::Guard guard,
		Fn<void(const Post &)> done,
		FailCallback fail) {
	_session->api().request(Api::Patch, u"/v1/posts/"_s + PathSegment(id))
		.body(QJsonObject{ { u"text"_s, text } })
		.guard(this)
		.done([=, this] {
			fetchPost(id, guard, done, fail);
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::deletePost(
		const QString &id,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->api().request(Api::Delete, u"/v1/posts/"_s + PathSegment(id))
		.guard(this)
		.done([=, this] {
			markDeleted(id);
			Q_EMIT postRemoved(id);
			if (done && guard.alive()) {
				done();
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			} else {
				_session->reportFailure(u"post.delete"_s, id, error);
			}
		})
		.send();
}

// Optimistic reaction with rollback; like and dislike are one group.
void Feed::react(const QString &id, Reaction reaction) {
	const auto before = post(id);
	if (!before) {
		return;
	}
	const auto counts = before->counts;
	const auto my = before->my;
	const auto name = ReactionName(reaction);
	patch(id, [&](Post &p) {
		if (p.my.reaction == u"like") {
			p.counts.likes = std::max<qint64>(0, p.counts.likes - 1);
		} else if (p.my.reaction == u"dislike" && p.counts.hasDislikes) {
			p.counts.dislikes = std::max<qint64>(0, p.counts.dislikes - 1);
		}
		if (reaction == Reaction::Like) {
			++p.counts.likes;
		} else if (reaction == Reaction::Dislike) {
			++p.counts.dislikes;
			p.counts.hasDislikes = true;
		}
		p.my.reaction = (reaction == Reaction::None) ? QString() : name;
	});
	simple(Api::Put, u"/v1/posts/%1/reaction"_s.arg(PathSegment(id)), QJsonObject{
		{ u"kind"_s, name },
	}, u"post.react"_s, id, nullptr, [=, this] {
		patch(id, [&](Post &p) {
			p.counts = counts;
			p.my = my;
		});
	});
}

void Feed::repost(const QString &id, bool on) {
	const auto before = post(id);
	if (!before) {
		return;
	}
	const auto counts = before->counts;
	const auto my = before->my;
	patch(id, [&](Post &p) {
		p.counts.reposts = std::max<qint64>(0, p.counts.reposts + (on ? 1 : -1));
		p.my.reposted = on;
	});
	simple(
		on ? Api::Post : Api::Delete,
		u"/v1/posts/%1/repost"_s.arg(PathSegment(id)),
		QJsonObject(),
		u"post.repost"_s,
		id,
		nullptr,
		[=, this] {
			patch(id, [&](Post &p) {
				p.counts = counts;
				p.my = my;
			});
		});
}

void Feed::bookmark(const QString &id, bool on) {
	const auto before = post(id);
	if (!before) {
		return;
	}
	const auto my = before->my;
	patch(id, [&](Post &p) { p.my.bookmarked = on; });
	simple(
		on ? Api::Post : Api::Delete,
		u"/v1/posts/%1/bookmark"_s.arg(PathSegment(id)),
		QJsonObject(),
		u"post.bookmark"_s,
		id,
		nullptr,
		[=, this] {
			patch(id, [&](Post &p) { p.my = my; });
		});
}

void Feed::vote(
		const QString &id,
		int option,
		base::Guard guard,
		Fn<void(const Post &)> done,
		FailCallback fail) {
	auto builder = _session->api().request(
		(option < 0) ? Api::Delete : Api::Post,
		u"/v1/posts/%1/vote"_s.arg(PathSegment(id)));
	if (option >= 0) {
		builder.body(QJsonObject{ { u"option"_s, option } });
	}
	builder
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto post = Post::FromJson(result);
			merge({ post });
			if (done && guard.alive()) {
				done(post);
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			} else {
				_session->reportFailure(u"post.vote"_s, id, error);
			}
		})
		.send();
}

void Feed::notInterested(const QString &id) {
	simple(Api::Post, u"/v1/posts/%1/not-interested"_s.arg(PathSegment(id)), QJsonObject(), u"post.not_interested"_s, id, [=, this] {
		Q_EMIT postRemoved(id);
	});
}

void Feed::setReplyPolicy(const QString &id, const QString &policy) {
	simple(Api::Put, u"/v1/posts/%1/reply-policy"_s.arg(PathSegment(id)), QJsonObject{
		{ u"reply_policy"_s, policy },
	}, u"post.reply_policy"_s, id, [=, this] {
		patch(id, [&](Post &p) { p.replyPolicy = policy; });
	});
}

void Feed::setPinned(const QString &id, bool pinned) {
	const auto applied = [=, this] {
		patch(id, [&](Post &p) { p.pinned = pinned; });
	};
	if (pinned) {
		simple(Api::Post, u"/v1/me/pins"_s, QJsonObject{ { u"post"_s, id } }, u"post.pin"_s, id, applied);
	} else {
		simple(Api::Delete, u"/v1/me/pins/"_s + PathSegment(id), QJsonObject(), u"post.pin"_s, id, applied);
	}
}

void Feed::shareLink(
		const QString &id,
		base::Guard guard,
		Fn<void(const QString &)> done,
		FailCallback fail) {
	_session->api().request(Api::Post, u"/v1/posts/%1/share-link"_s.arg(PathSegment(id)))
		.body(QJsonObject())
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(u"https://eleza.me/s/"_s + Json::String(result, "slug"_L1));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Feed::revokeShareLinks(
		const QString &id,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail) {
	_session->api().request(Api::Delete, u"/v1/posts/%1/share-links"_s.arg(PathSegment(id)))
		.guard(guard)
		.done([=] {
			if (done) {
				done();
			}
		})
		.fail(std::move(fail))
		.send();
}

void Feed::sharedPost(
		const QString &slug,
		base::Guard guard,
		Fn<void(const Post &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/share/"_s + PathSegment(slug))
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto post = Post::FromJson(result);
			merge({ post });
			_session->users().request(post.author);
			if (done && guard.alive()) {
				done(post);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::replies(
		const QString &root,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const RepliesPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/posts/%1/replies"_s.arg(PathSegment(root)))
		.query(u"cursor"_s, cursor)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto page = RepliesPage();
			page.replies = mergeJson(result.value("replies"_L1));
			for (const auto &id : page.replies) {
				if (const auto found = post(id)) {
					_session->users().request(found->author);
				}
			}
			page.nextCursor = Json::String(result, "next_cursor"_L1);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::deleteReply(const QString &root, const QString &reply, bool mine) {
	const auto path = mine
		? (u"/v1/posts/"_s + PathSegment(reply))
		: u"/v1/posts/%1/replies/%2"_s.arg(PathSegment(root), PathSegment(reply));
	simple(Api::Delete, path, QJsonObject(), u"post.delete"_s, reply, [=, this] {
		markDeleted(reply);
		Q_EMIT postRemoved(reply);
	});
}

void Feed::reactors(
		const QString &id,
		Reaction kind,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const GraphList &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/posts/%1/reactions"_s.arg(PathSegment(id)))
		.query(u"kind"_s, ReactionName(kind == Reaction::Dislike ? kind : Reaction::Like))
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

void Feed::noteAuthorBlocked(const QString &userId) {
	Q_EMIT authorBlocked(userId);
}

// ---- view beacons and counts

void Feed::reportViews(const QStringList &ids) {
	const auto me = _session->userId();
	for (const auto &id : ids) {
		if (_viewed.contains(id)) {
			continue;
		}
		const auto found = post(id);
		if (found && !me.isEmpty() && found->author == me) {
			continue;
		}
		_viewed.insert(id);
		_viewQueue.push_back(id);
	}
	if (!_viewQueue.isEmpty() && !_viewTimer.isActive()) {
		_viewTimer.callOnce(kViewsFlushMs);
	}
}

void Feed::flushViews() {
	_viewTimer.cancel();
	if (_viewQueue.isEmpty()) {
		return;
	}
	const auto batch = _viewQueue.mid(0, kViewsBatch);
	_viewQueue = _viewQueue.mid(batch.size());
	_session->api().request(Api::Post, u"/v1/posts/views"_s)
		.body(QJsonObject{ { u"ids"_s, Json::Array(batch) } })
		.guard(this)
		.fail([](const Api::Error &error) {
			qInfo("Data: views beacon failed: %s", qPrintable(error.forLog()));
		})
		.send();
	if (!_viewQueue.isEmpty()) {
		_viewTimer.callOnce(kViewsFlushMs);
	}
}

void Feed::refreshCounts(const QStringList &ids) {
	const auto now = base::unixtime::nowMs();
	for (const auto &id : ids) {
		if (_posts.contains(id) && now - _statsAt.value(id) > kCountsMaxAgeMs) {
			_countsQueue.insert(id);
		}
	}
	if (!_countsQueue.isEmpty()) {
		_countsTimer.callOnce(kCountsDebounceMs);
	}
}

void Feed::sendCounts() {
	if (_countsQueue.isEmpty()) {
		return;
	}
	auto batch = QStringList();
	for (auto i = _countsQueue.begin(); i != _countsQueue.end() && batch.size() < kCountsBatch;) {
		batch.push_back(*i);
		i = _countsQueue.erase(i);
	}
	const auto now = base::unixtime::nowMs();
	for (const auto &id : batch) {
		_statsAt.insert(id, now);
	}
	_session->api().request(Api::Post, u"/v1/posts/counts"_s)
		.body(QJsonObject{ { u"ids"_s, Json::Array(batch) } })
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			applyCounts(result.value("posts"_L1).toObject());
		})
		.fail([](const Api::Error &error) {
			qInfo("Data: counts refresh failed: %s", qPrintable(error.forLog()));
		})
		.send();
	if (!_countsQueue.isEmpty()) {
		_countsTimer.callOnce(kCountsDebounceMs);
	}
}

// ---- topics

void Feed::topic(
		const QString &slug,
		base::Guard guard,
		Fn<void(const TopicInfo &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/topics/"_s + PathSegment(slug))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(TopicInfo::FromJson(result));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Feed::topicPosts(
		const QString &slug,
		const QString &tab,
		const QString &before,
		base::Guard guard,
		Fn<void(const PostsPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/topics/%1/posts"_s.arg(PathSegment(slug)))
		.query(u"tab"_s, tab)
		.query(u"before"_s, before)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = parsePosts(result);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::topicConversations(
		const QString &slug,
		base::Guard guard,
		Fn<void(const QStringList &)> done) {
	_session->api().request(Api::Get, u"/v1/topics/%1/conversations"_s.arg(PathSegment(slug)))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::Strings(result.value("conversations"_L1)));
			}
		})
		.fail([=](const Api::Error &) {
			if (done) {
				done({});
			}
		})
		.send();
}

void Feed::followTopic(const QString &slug, bool on) {
	simple(
		on ? Api::Post : Api::Delete,
		u"/v1/topics/%1/follow"_s.arg(PathSegment(slug)),
		QJsonObject(),
		u"topic.follow"_s,
		slug);
}

void Feed::hideTopic(const QString &id, bool hidden) {
	simple(
		hidden ? Api::Post : Api::Delete,
		u"/v1/me/hidden-topics/"_s + PathSegment(id),
		QJsonObject(),
		u"topic.hide"_s,
		id);
}

void Feed::trending(
		base::Guard guard,
		Fn<void(const QVector<Trend> &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/topics/trending"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(Json::List<Trend>(result.value("topics"_L1)));
			}
		})
		.fail(std::move(fail))
		.send();
}

// ---- lists

void Feed::bookmarks(
		const QString &cursor,
		base::Guard guard,
		Fn<void(const PostsPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/me/bookmarks"_s)
		.query(u"cursor"_s, cursor)
		.query(u"limit"_s, 25)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = parsePosts(result);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::userPosts(
		const QString &userId,
		const QString &tab,
		const QString &before,
		const QString &kind,
		base::Guard guard,
		Fn<void(const PostsPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/users/%1/posts"_s.arg(PathSegment(userId)))
		.query(u"tab"_s, tab)
		.query(u"before"_s, before)
		.query(u"kind"_s, kind)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = parsePosts(result);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::sound(
		const QString &id,
		base::Guard guard,
		Fn<void(const PostSound &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/sounds/"_s + PathSegment(id))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(PostSound::FromJson(result));
			}
		})
		.fail(std::move(fail))
		.send();
}

void Feed::soundPosts(
		const QString &id,
		const QString &before,
		base::Guard guard,
		Fn<void(const PostsPage &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/sounds/%1/posts"_s.arg(PathSegment(id)))
		.query(u"before"_s, before)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto page = parsePosts(result);
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Feed::updateSound(const QString &id, const QJsonObject &changes) {
	simple(Api::Patch, u"/v1/sounds/"_s + PathSegment(id), changes, u"sound.update"_s, id);
}

// ---- reports

void Feed::reportReasons(
		base::Guard guard,
		Fn<void(const ReportReasons &)> done,
		FailCallback fail) {
	_session->api().request(Api::Get, u"/v1/reports/reasons"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			auto reasons = ReportReasons();
			reasons.post = Json::Strings(result.value("post"_L1));
			reasons.reply = Json::Strings(result.value("reply"_L1));
			reasons.account = Json::Strings(result.value("account"_L1));
			reasons.conversation = Json::Strings(result.value("conversation"_L1));
			reasons.group = Json::Strings(result.value("group"_L1));
			reasons.groupMessage = Json::Strings(result.value("group_message"_L1));
			if (done) {
				done(reasons);
			}
		})
		.fail(std::move(fail))
		.send();
}

void Feed::sendReport(
		const QString &targetKind,
		const QString &target,
		const QString &reason,
		const QString &note,
		base::Guard guard,
		Fn<void()> done,
		FailCallback fail,
		qint64 messageSeq) {
	auto body = QJsonObject{
		{ u"target_kind"_s, targetKind },
		{ u"target"_s, target },
		{ u"reason"_s, reason },
	};
	Json::Put(body, "note"_L1, note);
	if (messageSeq > 0) {
		// A message of a group: the target is the group.
		body.insert(u"message_seq"_s, messageSeq);
	}
	_session->api().request(Api::Post, u"/v1/reports"_s)
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
			} else {
				_session->reportFailure(u"report.send"_s, target, error);
			}
		})
		.send();
}

void Feed::searchUsers(
		const QString &query,
		base::Guard guard,
		Fn<void(const QVector<Profile> &)> done) {
	_session->api().request(Api::Get, u"/v1/search"_s)
		.query(u"q"_s, query)
		.query(u"type"_s, u"users"_s)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto list = result.value("users"_L1).isArray()
				? result.value("users"_L1)
				: result.value("people"_L1);
			auto users = Json::List<Profile>(list);
			_session->users().seed(users);
			if (users.size() > 5) {
				users.resize(5);
			}
			if (done && guard.alive()) {
				done(users);
			}
		})
		.fail([=](const Api::Error &) {
			if (done && guard.alive()) {
				done({});
			}
		})
		.send();
}

} // namespace Data
