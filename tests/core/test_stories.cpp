// Data::Stories on a fake transport: the wire shapes, the tray order and the
// rings, seen batching, the `story` hint, optimistic reactions and mute; the
// story reference of story_reply / story_reaction messages.
#include "data/data_json.h"
#include "data/data_stories.h"
#include "data/data_story_types.h"
#include "data/data_types.h"
#include "test_harness.h"

#include <QtTest/QSignalSpy>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;

namespace {

[[nodiscard]] QJsonObject Card(const QString &id, const QString &name) {
	return QJsonObject{
		{ u"id"_s, id },
		{ u"handle"_s, id },
		{ u"display_name"_s, name },
	};
}

[[nodiscard]] QJsonObject TrayItem(
		const QString &id,
		int count,
		int unseen,
		qint64 latest,
		bool muted = false,
		bool close = false) {
	return QJsonObject{
		{ u"user"_s, Card(id, id.toUpper()) },
		{ u"count"_s, count },
		{ u"unseen"_s, unseen },
		{ u"latest_at"_s, double(latest) },
		{ u"muted"_s, muted },
		{ u"close_friends"_s, close },
	};
}

[[nodiscard]] QJsonObject StoryJson(
		const QString &id,
		qint64 createdAt,
		bool seen,
		const QString &kind = u"image"_s) {
	auto result = QJsonObject{
		{ u"id"_s, id },
		{ u"kind"_s, kind },
		{ u"caption"_s, QJsonValue::Null },
		{ u"created_at"_s, double(createdAt) },
		{ u"expires_at"_s, double(QDateTime::currentSecsSinceEpoch() + 3600) },
		{ u"seen"_s, seen },
		{ u"my_reaction"_s, QJsonValue::Null },
		{ u"audience"_s, u"everyone"_s },
		{ u"can_reply"_s, true },
	};
	if (kind == u"post") {
		result.insert(u"media"_s, QJsonValue::Null);
		result.insert(u"post"_s, QJsonObject{
			{ u"id"_s, u"p1"_s },
			{ u"author"_s, QJsonValue::Null },
			{ u"text_excerpt"_s, u""_s },
			{ u"thumb_url"_s, QJsonValue::Null },
			{ u"available"_s, false },
		});
	} else {
		result.insert(u"media"_s, QJsonObject{
			{ u"url"_s, u"https://cdn/x.jpg"_s },
			{ u"thumb_url"_s, u"https://cdn/x_t.jpg"_s },
			{ u"width"_s, 1080 },
			{ u"height"_s, 1920 },
			{ u"duration_ms"_s, (kind == u"video") ? 12000 : 5000 },
		});
	}
	return result;
}

} // namespace

class TestStories final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void parsesStory();
	void parsesTrayAndSorts();
	void ringStates();
	void authorHints();
	void userStoriesOrderAndFirstUnseen();
	void seenIsBatched();
	void seenSplitsIntoHundreds();
	void storySignalRefreshesTray();
	void reactRollsBack();
	void muteMovesToTheEnd();
	void createStoryBody();
	void messageStoryRef();
};

void TestStories::parsesStory() {
	auto json = StoryJson(u"st1"_s, 100, false, u"video"_s);
	json.insert(u"caption"_s, u"سلام"_s);
	json.insert(u"viewer_count"_s, 12);
	json.insert(u"audience"_s, u"close_friends"_s);
	json.insert(u"my_reaction"_s, u"🔥"_s);
	const auto story = Story::FromJson(json);
	QCOMPARE(story.id, u"st1"_s);
	QCOMPARE(story.kind, StoryKind::Video);
	QVERIFY(story.media.has_value());
	QCOMPARE(story.media->width, 1080);
	QCOMPARE(story.showMs(), qint64(12000));
	QCOMPARE(story.caption, u"سلام"_s);
	QCOMPARE(story.viewerCount, qint64(12));
	QVERIFY(story.closeFriends);
	QVERIFY(story.canReply);
	QCOMPARE(story.myReaction, u"🔥"_s);

	const auto post = Story::FromJson(StoryJson(u"st2"_s, 100, true, u"post"_s));
	QCOMPARE(post.kind, StoryKind::Post);
	QVERIFY(!post.media);
	QVERIFY(post.post.has_value());
	QVERIFY(!post.post->available);
	QVERIFY(!post.post->author);
	QCOMPARE(post.viewerCount, qint64(-1));
	QCOMPARE(post.showMs(), qint64(5000));

	auto expired = StoryJson(u"st3"_s, 100, true);
	expired.insert(u"expires_at"_s, 200);
	QVERIFY(Story::FromJson(expired).expired(200));
	QVERIFY(!Story::FromJson(expired).expired(199));
}

void TestStories::parsesTrayAndSorts() {
	const auto page = StoryTrayPage::FromJson(QJsonObject{
		{ u"me"_s, QJsonObject{
			{ u"user"_s, Card(u"u_me"_s, u"من"_s) },
			{ u"count"_s, 2 },
			{ u"has_unseen_by_me"_s, false },
			{ u"latest_at"_s, 50 },
			{ u"close_friends"_s, false },
		} },
		{ u"items"_s, QJsonArray{
			TrayItem(u"a"_s, 2, 0, 900),
			TrayItem(u"b"_s, 1, 1, 100),
			TrayItem(u"c"_s, 3, 2, 500, true),
			TrayItem(u"d"_s, 1, 1, 700),
			TrayItem(u"e"_s, 0, 0, 999), // nothing to see: dropped
		} },
		{ u"next_cursor"_s, u"next"_s },
	});
	QCOMPARE(page.me.count, 2);
	QCOMPARE(page.items.size(), 4);
	QCOMPARE(page.nextCursor, u"next"_s);

	auto items = page.items;
	SortTray(items);
	auto order = QStringList();
	for (const auto &item : items) {
		order.push_back(item.user.id);
	}
	// Unseen by newest, then seen, muted last whatever it holds.
	QCOMPARE(order, (QStringList{ u"d"_s, u"b"_s, u"a"_s, u"c"_s }));
}

void TestStories::ringStates() {
	auto item = StoryTrayItem();
	QCOMPARE(RingStateOf(item).ring(), StoryRing::None);
	item.count = 2;
	QCOMPARE(RingStateOf(item).ring(), StoryRing::Seen);
	item.unseen = 1;
	QCOMPARE(RingStateOf(item).ring(), StoryRing::Unseen);
	item.closeFriends = true;
	QCOMPARE(RingStateOf(item).ring(), StoryRing::CloseFriends);
	item.muted = true;
	QCOMPARE(RingStateOf(item).ring(), StoryRing::Seen);
}

void TestStories::authorHints() {
	Harness harness;
	auto &stories = harness.session->stories();
	QSignalSpy rings(&stories, &Stories::ringsChanged);
	stories.applyAuthorHints(QJsonObject{
		{ u"u1"_s, QJsonObject{
			{ u"id"_s, u"u1"_s },
			{ u"has_story"_s, true },
			{ u"story_unseen"_s, true },
			{ u"story_close_friends"_s, false },
		} },
		{ u"u2"_s, QJsonObject{
			{ u"id"_s, u"u2"_s },
			{ u"has_story"_s, true },
			{ u"story_unseen"_s, false },
		} },
		{ u"u3"_s, QJsonObject{ { u"id"_s, u"u3"_s } } }, // no hint at all
	});
	QCOMPARE(stories.ring(u"u1"_s), StoryRing::Unseen);
	QCOMPARE(stories.ring(u"u2"_s), StoryRing::Seen);
	QCOMPARE(stories.ring(u"u3"_s), StoryRing::None);
	QCOMPARE(rings.count(), 1);
	const auto ids = rings.takeFirst().at(0).toStringList();
	QVERIFY(ids.contains(u"u1"_s) && ids.contains(u"u2"_s) && !ids.contains(u"u3"_s));

	// The same again changes nothing.
	stories.applyAuthorHints(QJsonArray{ QJsonObject{
		{ u"id"_s, u"u1"_s },
		{ u"has_story"_s, true },
		{ u"story_unseen"_s, true },
	} });
	QCOMPARE(rings.count(), 0);

	// The tray wins over a hint.
	auto page = StoryTrayPage();
	page.me.user.id = u"u_me"_s;
	page.items.push_back(StoryTrayItem::FromJson(TrayItem(u"u1"_s, 1, 0, 10)));
	stories.applyTray(page, true);
	QCOMPARE(stories.ring(u"u1"_s), StoryRing::Seen);
}

void TestStories::userStoriesOrderAndFirstUnseen() {
	const auto list = UserStories::FromJson(QJsonObject{
		{ u"user"_s, Card(u"u1"_s, u"سارا"_s) },
		{ u"stories"_s, QJsonArray{
			StoryJson(u"st3"_s, 300, false),
			StoryJson(u"st1"_s, 100, true),
			StoryJson(u"st2"_s, 200, false),
		} },
	}, u"u1"_s);
	QCOMPARE(list.stories.size(), 3);
	QCOMPARE(list.stories[0].id, u"st1"_s);
	QCOMPARE(list.stories[2].id, u"st3"_s);
	QCOMPARE(list.stories[1].owner, u"u1"_s);
	QCOMPARE(FirstUnseenIndex(list.stories), 1);
	auto seen = list.stories;
	for (auto &story : seen) {
		story.seen = true;
	}
	QCOMPARE(FirstUnseenIndex(seen), 0);
}

void TestStories::seenIsBatched() {
	Harness harness;
	auto &stories = harness.session->stories();
	auto page = StoryTrayPage();
	page.items.push_back(StoryTrayItem::FromJson(TrayItem(u"u1"_s, 2, 2, 10)));
	stories.applyTray(page, true);
	stories.applyUserStories(UserStories::FromJson(QJsonObject{
		{ u"user"_s, Card(u"u1"_s, u"سارا"_s) },
		{ u"stories"_s, QJsonArray{
			StoryJson(u"st1"_s, 100, false),
			StoryJson(u"st2"_s, 200, false),
		} },
	}, u"u1"_s));
	QCOMPARE(stories.ring(u"u1"_s), StoryRing::Unseen);

	stories.markSeen(u"st1"_s);
	stories.markSeen(u"st1"_s); // twice: queued once
	QCOMPARE(stories.tray().front().unseen, 1);
	QVERIFY(stories.cached(u"u1"_s)->stories[0].seen);
	Test::Settle(50);
	QCOMPARE(harness.transport.count(u"stories.seen"_s), 0);

	stories.markSeen(u"st2"_s);
	QCOMPARE(stories.ring(u"u1"_s), StoryRing::Seen);
	QCOMPARE(stories.queuedSeen(), (QStringList{ u"st1"_s, u"st2"_s }));

	// The timer sends them together.
	QVERIFY(Test::WaitFor([&] {
		return harness.transport.count(u"stories.seen"_s) == 1;
	}, Stories::kSeenFlushMs + 2000));
	const auto body = harness.bodies(u"stories.seen"_s).front();
	QCOMPARE(Json::Strings(body.value("ids"_L1)), (QStringList{ u"st1"_s, u"st2"_s }));
	QVERIFY(stories.queuedSeen().isEmpty());

	// Sent once: seeing it again sends nothing.
	stories.markSeen(u"st1"_s);
	stories.flushSeen();
	Test::Settle(20);
	QCOMPARE(harness.transport.count(u"stories.seen"_s), 1);
}

void TestStories::seenSplitsIntoHundreds() {
	Harness harness;
	auto &stories = harness.session->stories();
	for (auto i = 0; i != 230; ++i) {
		stories.markSeen(u"st"_s + QString::number(i));
	}
	stories.flushSeen();
	Test::Settle(20);
	const auto bodies = harness.bodies(u"stories.seen"_s);
	QCOMPARE(bodies.size(), 3);
	QCOMPARE(bodies[0].value("ids"_L1).toArray().size(), 100);
	QCOMPARE(bodies[1].value("ids"_L1).toArray().size(), 100);
	QCOMPARE(bodies[2].value("ids"_L1).toArray().size(), 30);
}

void TestStories::storySignalRefreshesTray() {
	Harness harness;
	harness.on(u"stories.tray"_s, [](const Call &) {
		return Reply::Ok(QJsonObject{
			{ u"me"_s, QJsonObject{ { u"user"_s, Card(u"u_me"_s, u"من"_s) }, { u"count"_s, 0 } } },
			{ u"items"_s, QJsonArray{ TrayItem(u"u1"_s, 1, 1, 10) } },
		});
	});
	auto &stories = harness.session->stories();
	auto frame = Rt::Frame();
	frame.type = Rt::Frame::Type::Signal;
	frame.kind = u"story"_s;
	frame.data = QJsonObject{ { u"user"_s, u"u1"_s }, { u"action"_s, u"new"_s } };
	// Three hints in a row are one refresh.
	harness.transport.emitFrame(frame);
	harness.transport.emitFrame(frame);
	harness.transport.emitFrame(frame);
	Test::Settle(50);
	QCOMPARE(harness.transport.count(u"stories.tray"_s), 0);
	QVERIFY(Test::WaitFor([&] {
		return stories.trayLoaded();
	}, Stories::kSignalDebounceMs + 2000));
	QCOMPARE(harness.transport.count(u"stories.tray"_s), 1);
	QCOMPARE(stories.tray().size(), 1);
	QCOMPARE(stories.ring(u"u1"_s), StoryRing::Unseen);

	// Another signal type does nothing.
	frame.kind = u"other"_s;
	harness.transport.emitFrame(frame);
	Test::Settle(Stories::kSignalDebounceMs + 300);
	QCOMPARE(harness.transport.count(u"stories.tray"_s), 1);
}

void TestStories::reactRollsBack() {
	Harness harness;
	harness.on(u"stories.react"_s, [](const Call &) {
		return Reply::Fail(403, u"story.replies_off"_s);
	});
	auto &stories = harness.session->stories();
	stories.applyUserStories(UserStories::FromJson(QJsonObject{
		{ u"user"_s, Card(u"u1"_s, u"سارا"_s) },
		{ u"stories"_s, QJsonArray{ StoryJson(u"st1"_s, 100, false) } },
	}, u"u1"_s));
	auto failed = QString();
	stories.react(u"st1"_s, u"🔥"_s, base::Guard(), nullptr, [&](const Api::Error &error) {
		failed = error.code;
	});
	QCOMPARE(stories.story(u"st1"_s)->myReaction, u"🔥"_s);
	QVERIFY(Test::WaitFor([&] { return !failed.isEmpty(); }));
	QCOMPARE(failed, u"story.replies_off"_s);
	QVERIFY(stories.story(u"st1"_s)->myReaction.isEmpty());
	const auto body = harness.bodies(u"stories.react"_s).front();
	QCOMPARE(body.value("story"_L1).toString(), u"st1"_s);
	QCOMPARE(body.value("emoji"_L1).toString(), u"🔥"_s);
}

void TestStories::muteMovesToTheEnd() {
	Harness harness;
	auto &stories = harness.session->stories();
	auto page = StoryTrayPage();
	page.items.push_back(StoryTrayItem::FromJson(TrayItem(u"a"_s, 1, 1, 30)));
	page.items.push_back(StoryTrayItem::FromJson(TrayItem(u"b"_s, 1, 1, 20)));
	page.items.push_back(StoryTrayItem::FromJson(TrayItem(u"c"_s, 1, 0, 10)));
	stories.applyTray(page, true);
	QCOMPARE(stories.trayUserIds(), (QStringList{ u"a"_s, u"b"_s, u"c"_s }));
	stories.setMuted(u"a"_s, true);
	QCOMPARE(stories.trayUserIds(), (QStringList{ u"b"_s, u"c"_s, u"a"_s }));
	QCOMPARE(stories.ring(u"a"_s), StoryRing::Seen);
	Test::Settle(20);
	QCOMPARE(harness.bodies(u"stories.mute"_s).front().value("user"_L1).toString(), u"a"_s);
}

void TestStories::createStoryBody() {
	auto media = CreateStory();
	media.mediaId = u"f1"_s;
	media.caption = u"  سلام  "_s;
	QCOMPARE(media.toJson(), (QJsonObject{
		{ u"media_id"_s, u"f1"_s },
		{ u"caption"_s, u"سلام"_s },
		{ u"audience"_s, u"everyone"_s },
	}));
	auto post = CreateStory();
	post.postId = u"p1"_s;
	post.closeFriends = true;
	QCOMPARE(post.toJson(), (QJsonObject{
		{ u"post_id"_s, u"p1"_s },
		{ u"audience"_s, u"close_friends"_s },
	}));
}

void TestStories::messageStoryRef() {
	const auto reply = Message::FromJson(QJsonObject{
		{ u"chat"_s, u"c1"_s },
		{ u"seq"_s, 7 },
		{ u"sender"_s, u"u1"_s },
		{ u"kind"_s, u"story_reply"_s },
		{ u"text"_s, u"چه خوب!"_s },
		{ u"story"_s, QJsonObject{
			{ u"story_id"_s, u"st1"_s },
			{ u"owner"_s, u"u_me"_s },
			{ u"thumb_url"_s, u"/v1/stories/st1/media?variant=thumb"_s },
			{ u"kind"_s, u"image"_s },
			{ u"expires_at"_s, 1792165400 },
		} },
	});
	QCOMPARE(reply.kind, MessageKind::StoryReply);
	QVERIFY(reply.story.has_value());
	QCOMPARE(reply.story->storyId, u"st1"_s);
	QCOMPARE(reply.story->owner, u"u_me"_s);
	QCOMPARE(reply.story->expiresAt, qint64(1792165400));
	QVERIFY(reply.story->expired(1792165400));
	QVERIFY(!reply.story->expired(1792165399));

	// The local cache keeps the reference.
	auto stored = reply;
	stored.mine = true;
	const auto back = Message::FromStored(stored.toStored());
	QCOMPARE(back.kind, MessageKind::StoryReply);
	QVERIFY(back.story.has_value());
	QCOMPARE(back.story->thumbUrl, u"/v1/stories/st1/media?variant=thumb"_s);
	QCOMPARE(back.toJson().value("kind"_L1).toString(), u"story_reply"_s);

	const auto reaction = Message::FromJson(QJsonObject{
		{ u"chat"_s, u"c1"_s },
		{ u"seq"_s, 8 },
		{ u"kind"_s, u"story_reaction"_s },
		{ u"story"_s, QJsonObject{
			{ u"story_id"_s, u"st1"_s },
			{ u"owner"_s, u"u1"_s },
			{ u"kind"_s, u"video"_s },
			{ u"expires_at"_s, 1 },
			{ u"emoji"_s, u"🔥"_s },
		} },
	});
	QCOMPARE(reaction.kind, MessageKind::StoryReaction);
	QCOMPARE(reaction.story->emoji, u"🔥"_s);
	QCOMPARE(Message::FromJson(reaction.toJson()).story->emoji, u"🔥"_s);
	QCOMPARE(MessageKindToString(MessageKind::StoryReaction), u"story_reaction"_s);

	// No story id: no reference.
	const auto broken = Message::FromJson(QJsonObject{
		{ u"kind"_s, u"story_reply"_s },
		{ u"story"_s, QJsonObject{ { u"owner"_s, u"u1"_s } } },
	});
	QVERIFY(!broken.story);
}

QTEST_GUILESS_MAIN(TestStories)

#include "test_stories.moc"
