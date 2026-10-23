// Notes: the wire shapes and the text rules (length in code points, one line,
// the reply message and both of its formats, its entities), and Data::Notes
// on the test HTTP server: the list is read once and seeds the cards, mine
// is set and removed, a read that crosses a change of mine keeps the change,
// notes leave at their expiry, a 404 or `feature.notes` false turn notes off,
// and a reply goes to the 1:1 chat as the plain text message.
#include "api/api_client_config.h"
#include "base/unixtime.h"
#include "data/data_note_types.h"
#include "data/data_notes.h"
#include "data/data_users.h"
#include "test_harness.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

using namespace Data;
using Test::Harness;
using Reply = Test::FakeTransport::Reply;
using Call = Test::FakeTransport::Call;
using Request = Test::HttpServer::Request;
using Response = Test::HttpServer::Response;

namespace {

const auto kArrow = u"↩︎"_s;

[[nodiscard]] qint64 Now() {
	return base::unixtime::nowMs() / 1000;
}

[[nodiscard]] QJsonObject NoteJson(
		const QString &id,
		const QString &text,
		qint64 createdAt,
		qint64 expiresAt,
		const QString &audience = u"mutuals"_s) {
	return QJsonObject{
		{ u"id"_s, id },
		{ u"text"_s, text },
		{ u"audience"_s, audience },
		{ u"created_at"_s, double(createdAt) },
		{ u"expires_at"_s, double(expiresAt) },
	};
}

[[nodiscard]] QJsonObject Card(const QString &id, const QString &name, bool deleted = false) {
	auto result = QJsonObject{
		{ u"id"_s, id },
		{ u"handle"_s, id },
		{ u"display_name"_s, name },
	};
	if (deleted) {
		result.insert(u"deleted"_s, true);
	}
	return result;
}

[[nodiscard]] QJsonObject ItemJson(const QJsonObject &card, const QJsonObject &note) {
	return QJsonObject{ { u"user"_s, card }, { u"note"_s, note } };
}

[[nodiscard]] QJsonObject FeedJson(qint64 now) {
	return QJsonObject{
		{ u"me"_s, NoteJson(u"nt_me"_s, u"قهوه؟ ☕"_s, now - 60, now + 3600, u"close_friends"_s) },
		{ u"items"_s, QJsonArray{
			ItemJson(Card(u"u_sara"_s, u"سارا"_s), NoteJson(u"nt1"_s, u"کسی کوه میاد؟"_s, now - 600, now + 7200)),
			ItemJson(Card(u"u_ali"_s, u"علی"_s), NoteJson(u"nt2"_s, u"امتحان دارم"_s, now - 300, now + 120)),
		} },
	};
}

[[nodiscard]] int Count(const Test::HttpServer &http, const QByteArray &method, const QByteArray &path) {
	auto result = 0;
	for (const auto &request : http.requests) {
		if (request.method == method && request.path == path) {
			++result;
		}
	}
	return result;
}

} // namespace

class TestNotes : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void wireShapes();
	void textRules();
	void replyRoundTrip();
	void otherTextsAreNotReplies_data();
	void otherTextsAreNotReplies();
	void replyEntities();
	void listIsReadOnceAndSeedsCards();
	void setAndRemoveMine();
	void readAcrossAChangeKeepsMine();
	void notesLeaveAtTheirExpiry();
	void oldServerTurnsNotesOff();
	void flagTurnsNotesOff();
	void replyGoesToTheChat();
};

void TestNotes::wireShapes() {
	const auto now = Now();
	auto json = FeedJson(now);
	auto items = json.value("items"_L1).toArray();
	// Nobody to answer, an empty note, a second note of the same person.
	items.push_back(ItemJson(Card(u"u_gone"_s, u""_s, true), NoteJson(u"nt3"_s, u"x"_s, now, now + 60)));
	items.push_back(ItemJson(Card(u"u_empty"_s, u"E"_s), NoteJson(u"nt4"_s, u"   "_s, now, now + 60)));
	items.push_back(ItemJson(Card(u"u_sara"_s, u"سارا"_s), NoteJson(u"nt5"_s, u"again"_s, now, now + 60)));
	items.push_back(QJsonObject{ { u"note"_s, NoteJson(u"nt6"_s, u"no card"_s, now, now + 60) } });
	json.insert(u"items"_s, items);
	const auto feed = NotesFeed::FromJson(json);
	QVERIFY(feed.me.has_value());
	QCOMPARE(feed.me->id, u"nt_me"_s);
	QCOMPARE(feed.me->audience, NoteAudience::CloseFriends);
	QCOMPARE(feed.me->expiresAt, now + 3600);
	QCOMPARE(feed.items.size(), 2);
	QCOMPARE(feed.items[0].user.id, u"u_sara"_s);
	QCOMPARE(feed.items[0].note.text, u"کسی کوه میاد؟"_s);
	QCOMPARE(feed.items[0].note.audience, NoteAudience::Mutuals);
	QCOMPARE(feed.items[1].user.id, u"u_ali"_s);
	QVERIFY(feed.item(u"u_ali"_s));
	QVERIFY(!feed.item(u"u_gone"_s));

	// No note of mine; an unknown audience reads as mutuals.
	const auto none = NotesFeed::FromJson(QJsonObject{
		{ u"me"_s, QJsonValue::Null },
		{ u"items"_s, QJsonArray{
			ItemJson(Card(u"u1"_s, u"A"_s), NoteJson(u"n"_s, u"line\nbreak"_s, now, now + 9, u"everyone"_s)),
		} },
	});
	QVERIFY(!none.me);
	QCOMPARE(none.items[0].note.audience, NoteAudience::Mutuals);
	QCOMPARE(none.items[0].note.text, u"line break"_s);
	QCOMPARE(NoteAudienceWire(NoteAudience::CloseFriends), u"close_friends"_s);
	QCOMPARE(NoteAudienceWire(NoteAudience::Mutuals), u"mutuals"_s);

	// The limit of items.
	QCOMPARE(NotesFeed::FromJson(FeedJson(now), 1).items.size(), 1);

	// Expiry: the order stays, the earliest one is next.
	QCOMPARE(feed.nextExpiry(now), now + 120);
	const auto later = feed.fresh(now + 200);
	QCOMPARE(later.items.size(), 1);
	QCOMPARE(later.items[0].user.id, u"u_sara"_s);
	QVERIFY(later.me.has_value());
	QCOMPARE(later.nextExpiry(now + 200), now + 3600);
	const auto gone = feed.fresh(now + 100000);
	QVERIFY(gone.empty());
	QCOMPARE(gone.nextExpiry(now + 100000), qint64(0));
}

void TestNotes::textRules() {
	// Code points: an emoji outside the BMP is one, a flag (two code points) two.
	QCOMPARE(NoteText::Length(u"سلام"_s), 4);
	QCOMPARE(NoteText::Length(u"\U0001F600"_s), 1);
	QCOMPARE(NoteText::Length(u"\U0001F1EE\U0001F1F7"_s), 2);
	QCOMPARE(NoteText::Length(QString()), 0);
	// One line, white space collapsed.
	QCOMPARE(NoteText::Cleaned(u"  یک\n\nدو\t سه  "_s), u"یک دو سه"_s);
	QVERIFY(!NoteText::IsValid(u" \n "_s, 60));
	QVERIFY(NoteText::IsValid(QString(60, u'a'), 60));
	QVERIFY(!NoteText::IsValid(QString(61, u'a'), 60));
	// 60 emoji are 120 UTF-16 units and still a valid note.
	auto emoji = QString();
	for (auto i = 0; i != 60; ++i) {
		emoji += u"\U0001F600"_s;
	}
	QCOMPARE(emoji.size(), 120);
	QVERIFY(NoteText::IsValid(emoji, 60));
	QVERIFY(!NoteText::IsValid(emoji + u"!"_s, 60));
	QCOMPARE(NoteText::Left(u"abc  "_s, 10), 7);
	QCOMPARE(NoteText::Left(emoji + u"!!"_s, 60), -2);
	// Cut between characters: a flag is not split, nor a letter from its mark.
	QCOMPARE(NoteText::Limited(u"abc"_s, 60), u"abc"_s);
	QCOMPARE(NoteText::Limited(u"ab\U0001F1EE\U0001F1F7"_s, 3), u"ab"_s);
	QCOMPARE(NoteText::Limited(u"ab\U0001F1EE\U0001F1F7"_s, 4), u"ab\U0001F1EE\U0001F1F7"_s);
	QCOMPARE(NoteText::Limited(u"ae\u0301z"_s, 2), u"a"_s);
	QCOMPARE(NoteText::Limited(emoji + u"!"_s, 60), emoji);
}

void TestNotes::replyRoundTrip() {
	const auto message = NoteReply::Build(u"کسی کوه میاد؟"_s, u"  من هستم!  "_s);
	QCOMPARE(message, kArrow + u" «کسی کوه میاد؟»\nمن هستم!"_s);
	const auto parsed = NoteReply::Parse(message);
	QVERIFY(parsed.has_value());
	QCOMPARE(parsed->note, u"کسی کوه میاد؟"_s);
	QCOMPARE(parsed->reply, u"من هستم!"_s);
	QCOMPARE(message.mid(parsed->replyOffset), u"من هستم!"_s);
	// A note on several lines is sent as one.
	QCOMPARE(
		NoteReply::Build(u"یک\nدو"_s, u"x"_s),
		kArrow + u" «یک دو»\nx"_s);
	// The answer may have several lines; the note may hold « and ».
	const auto multi = NoteReply::Parse(NoteReply::Build(u"«سلام» دنیا"_s, u"من هستم!\nساعت ۶؟"_s));
	QVERIFY(multi.has_value());
	QCOMPARE(multi->note, u"«سلام» دنیا"_s);
	QCOMPARE(multi->reply, u"من هستم!\nساعت ۶؟"_s);
	// What the first app versions sent (iOS and Android).
	const auto old = NoteReply::Parse(kArrow + u" نوتت: «کسی کوه میاد؟»\nمن هستم!"_s);
	QVERIFY(old.has_value());
	QCOMPARE(old->note, u"کسی کوه میاد؟"_s);
	QCOMPARE(old->reply, u"من هستم!"_s);
	const auto english = NoteReply::Parse(kArrow + u" Your note: «Tea?»\nYes"_s);
	QVERIFY(english.has_value());
	QCOMPARE(english->note, u"Tea?"_s);
	// The arrow without its selector, Windows line ends.
	const auto bare = NoteReply::Parse(u"↩ «چای؟»\r\nآره"_s);
	QVERIFY(bare.has_value());
	QCOMPARE(bare->note, u"چای؟"_s);
	QCOMPARE(bare->reply, u"آره"_s);
	// The chat list shows the answer.
	QCOMPARE(NoteReply::Preview(message), kArrow + u" من هستم!"_s);
	QCOMPARE(NoteReply::Preview(u"سلام"_s), u"سلام"_s);
}

void TestNotes::otherTextsAreNotReplies_data() {
	QTest::addColumn<QString>("text");
	QTest::newRow("plain") << u"سلام"_s;
	QTest::newRow("no answer") << (kArrow + u" «نوت»"_s);
	QTest::newRow("blank answer") << (kArrow + u" «نوت»\n   "_s);
	QTest::newRow("empty note") << (kArrow + u" «»\nجواب"_s);
	QTest::newRow("no quote") << (kArrow + u" نوت\nجواب"_s);
	QTest::newRow("arrow later") << (u"جواب "_s + kArrow + u" «نوت»\nجواب"_s);
	QTest::newRow("text after quote") << (kArrow + u" «نوت» و بعد\nجواب"_s);
	QTest::newRow("long label") << (kArrow + u" یک برچسب خیلی خیلی خیلی خیلی بلند: «نوت»\nجواب"_s);
	QTest::newRow("empty") << QString();
}

void TestNotes::otherTextsAreNotReplies() {
	QFETCH(QString, text);
	QVERIFY(!NoteReply::Parse(text).has_value());
	QCOMPARE(NoteReply::Preview(text), text);
}

void TestNotes::replyEntities() {
	const auto message = NoteReply::Build(u"نوت"_s, u"bold and plain"_s);
	const auto parsed = NoteReply::Parse(message);
	QVERIFY(parsed.has_value());
	const auto offset = parsed->replyOffset;
	auto inQuote = Entity();
	inQuote.type = u"italic"_s;
	inQuote.offset = 2;
	inQuote.length = 3;
	auto across = Entity();
	across.type = u"spoiler"_s;
	across.offset = offset - 2;
	across.length = 6;
	auto bold = Entity();
	bold.type = u"bold"_s;
	bold.offset = offset;
	bold.length = 4;
	auto past = Entity();
	past.type = u"bold"_s;
	past.offset = offset + 9;
	past.length = 100;
	const auto moved = parsed->replyEntities({ inQuote, across, bold, past });
	QCOMPARE(moved.size(), 3);
	QCOMPARE(moved[0].type, u"spoiler"_s);
	QCOMPARE(moved[0].offset, 0);
	QCOMPARE(moved[0].length, 4);
	QCOMPARE(moved[1].offset, 0);
	QCOMPARE(moved[1].length, 4);
	QCOMPARE(parsed->reply.mid(moved[1].offset, moved[1].length), u"bold"_s);
	QCOMPARE(moved[2].offset, 9);
	QCOMPARE(moved[2].length, 5);
}

void TestNotes::listIsReadOnceAndSeedsCards() {
	auto harness = Harness();
	const auto now = Now();
	harness.http.handler = [&](const Request &request) {
		if (request.path == "/v1/notes") {
			return Response::Ok(FeedJson(now));
		}
		return Response::Problem(404, "not_found");
	};
	auto &notes = harness.session->notes();
	QVERIFY(notes.enabled());
	QCOMPARE(notes.maxChars(), 60);
	QVERIFY(!notes.loaded());
	auto spy = QSignalSpy(&notes, &Notes::changed);
	notes.refresh();
	notes.refresh(); // on its way: not asked twice
	QVERIFY(notes.loading());
	QVERIFY(Test::WaitFor([&] { return notes.loaded(); }));
	QCOMPARE(spy.count(), 1);
	QCOMPARE(Count(harness.http, "GET", "/v1/notes"), 1);
	QCOMPARE(notes.feed().items.size(), 2);
	QVERIFY(notes.mine());
	QCOMPARE(notes.mine()->text, u"قهوه؟ ☕"_s);
	QCOMPARE(notes.defaultAudience(), NoteAudience::CloseFriends);
	QCOMPARE(harness.session->users().peek(u"u_sara"_s).displayName, u"سارا"_s);
	// Read a moment ago: not again, unless asked to.
	notes.refresh();
	Test::Settle(50);
	QCOMPARE(Count(harness.http, "GET", "/v1/notes"), 1);
	notes.reload();
	QVERIFY(Test::WaitFor([&] { return Count(harness.http, "GET", "/v1/notes") == 2 && !notes.loading(); }));
	// An answer that failed keeps what is shown.
	harness.http.handler = [](const Request &) {
		return Response::Problem(503, "server.unavailable");
	};
	notes.reload();
	QVERIFY(Test::WaitFor([&] { return !notes.loading(); }));
	QCOMPARE(notes.feed().items.size(), 2);
	QVERIFY(notes.enabled());
}

void TestNotes::setAndRemoveMine() {
	auto harness = Harness();
	const auto now = Now();
	auto deleted = 0;
	harness.http.handler = [&](const Request &request) {
		if (request.path == "/v1/me/note" && request.method == "PUT") {
			const auto body = request.json();
			if (body.value("text"_L1).toString().size() > 60) {
				return Response::Problem(400, "note.invalid");
			}
			return Response::Ok(NoteJson(
				u"nt_new"_s,
				body.value("text"_L1).toString(),
				now,
				now + 86400,
				body.value("audience"_L1).toString()));
		} else if (request.path == "/v1/me/note" && request.method == "DELETE") {
			// The second time: it ran out meanwhile.
			if (++deleted > 1) {
				return Response::Problem(404, "not_found");
			}
			auto response = Response();
			response.status = 204;
			response.body = QByteArray();
			return response;
		}
		return Response::Problem(404, "not_found");
	};
	auto &notes = harness.session->notes();
	auto set = std::optional<Note>();
	notes.set(u"  امروز\nکتاب‌خونه  "_s, NoteAudience::CloseFriends, base::Guard(), [&](const Note &note) {
		set = note;
	});
	QVERIFY(Test::WaitFor([&] { return set.has_value(); }));
	QCOMPARE(set->text, u"امروز کتاب‌خونه"_s);
	QCOMPARE(set->audience, NoteAudience::CloseFriends);
	const auto sent = harness.http.requests.back().json();
	QCOMPARE(sent.value("text"_L1).toString(), u"امروز کتاب‌خونه"_s);
	QCOMPARE(sent.value("audience"_L1).toString(), u"close_friends"_s);
	QVERIFY(notes.mine());
	QCOMPARE(notes.mine()->id, u"nt_new"_s);

	auto error = std::optional<Api::Error>();
	notes.set(QString(61, u'x'), NoteAudience::Mutuals, base::Guard(), nullptr, [&](const Api::Error &e) {
		error = e;
	});
	QVERIFY(Test::WaitFor([&] { return error.has_value(); }));
	QCOMPARE(error->code, u"note.invalid"_s);
	QCOMPARE(notes.mine()->id, u"nt_new"_s);
	// The composer starts with the last audience picked.
	QCOMPARE(notes.defaultAudience(), NoteAudience::CloseFriends);

	for (auto i = 0; i != 2; ++i) {
		auto removed = false;
		notes.remove(base::Guard(), [&] { removed = true; });
		QVERIFY(Test::WaitFor([&] { return removed; }));
		QVERIFY(!notes.mine());
	}
	QCOMPARE(notes.defaultAudience(), NoteAudience::Mutuals);
}

void TestNotes::readAcrossAChangeKeepsMine() {
	auto harness = Harness();
	const auto now = Now();
	auto slow = true;
	harness.http.handler = [&](const Request &request) {
		if (request.path == "/v1/notes") {
			// The list as it was before my new note.
			auto response = Response::Ok(FeedJson(now));
			response.delayMs = slow ? 400 : 0;
			return response;
		}
		return Response::Ok(NoteJson(u"nt_mine2"_s, u"تازه"_s, now, now + 86400));
	};
	auto &notes = harness.session->notes();
	notes.refresh();
	auto done = false;
	notes.set(u"تازه"_s, NoteAudience::Mutuals, base::Guard(), [&](const Note &) {
		done = true;
	});
	QVERIFY(Test::WaitFor([&] { return done; }));
	QVERIFY(notes.loading());
	QCOMPARE(notes.mine()->id, u"nt_mine2"_s);
	// The older list arrives: the friends' notes are taken, mine stays.
	QVERIFY(Test::WaitFor([&] { return notes.loaded(); }));
	QCOMPARE(notes.feed().items.size(), 2);
	QCOMPARE(notes.mine()->id, u"nt_mine2"_s);
	// A read that started after the change is the server's word.
	slow = false;
	notes.reload();
	QVERIFY(Test::WaitFor([&] { return !notes.loading(); }));
	QCOMPARE(notes.mine()->id, u"nt_me"_s);
}

void TestNotes::notesLeaveAtTheirExpiry() {
	auto harness = Harness();
	auto &notes = harness.session->notes();
	const auto now = Now();
	auto json = FeedJson(now);
	auto items = json.value("items"_L1).toArray();
	// A note that runs out in a second, and one that ran out already.
	items.push_back(ItemJson(Card(u"u_soon"_s, u"S"_s), NoteJson(u"nt7"_s, u"soon"_s, now - 10, now + 1)));
	items.push_back(ItemJson(Card(u"u_old"_s, u"O"_s), NoteJson(u"nt8"_s, u"old"_s, now - 90000, now - 5)));
	json.insert(u"items"_s, items);
	auto spy = QSignalSpy(&notes, &Notes::changed);
	notes.apply(NotesFeed::FromJson(json));
	QCOMPARE(notes.feed().items.size(), 3);
	QVERIFY(!notes.feed().item(u"u_old"_s));
	QVERIFY(notes.feed().item(u"u_soon"_s));
	QVERIFY(Test::WaitFor([&] { return !notes.feed().item(u"u_soon"_s); }, 4000));
	QCOMPARE(spy.count(), 2);
	QCOMPARE(notes.feed().items.size(), 2);
	QCOMPARE(Count(harness.http, "GET", "/v1/notes"), 0);
}

void TestNotes::oldServerTurnsNotesOff() {
	auto harness = Harness();
	harness.http.handler = [](const Request &) {
		return Response::Problem(404, "not_found");
	};
	auto &notes = harness.session->notes();
	auto spy = QSignalSpy(&notes, &Notes::enabledChanged);
	notes.refresh();
	QVERIFY(Test::WaitFor([&] { return spy.count() == 1; }));
	QVERIFY(!notes.enabled());
	QVERIFY(!notes.loaded());
	// Nothing more is asked of it.
	notes.reload();
	notes.refresh();
	Test::Settle(50);
	QCOMPARE(Count(harness.http, "GET", "/v1/notes"), 1);
}

void TestNotes::flagTurnsNotesOff() {
	auto values = QJsonObject{
		{ u"feature.notes"_s, false },
		{ u"notes.max_chars"_s, 80 },
	};
	auto configServer = Test::HttpServer();
	configServer.handler = [&](const Request &) {
		return Response::Ok(QJsonObject{ { u"version"_s, 1 }, { u"values"_s, values } });
	};
	auto configApi = Api::Client(configServer.base(), u"1.0.0"_s);
	auto config = Api::ClientConfig(&configApi, nullptr);
	auto loaded = false;
	config.load(nullptr, [&](bool) { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded; }));

	auto options = Harness::Options();
	options.clientConfig = &config;
	auto harness = Harness(options);
	auto &notes = harness.session->notes();
	QVERIFY(!notes.enabled());
	QCOMPARE(notes.maxChars(), 80);
	notes.refresh();
	Test::Settle(50);
	QCOMPARE(Count(harness.http, "GET", "/v1/notes"), 0);

	// Turned on: the list is read.
	harness.http.handler = [&](const Request &) {
		return Response::Ok(FeedJson(Now()));
	};
	auto spy = QSignalSpy(&notes, &Notes::enabledChanged);
	values.insert(u"feature.notes"_s, true);
	loaded = false;
	config.load(nullptr, [&](bool) { loaded = true; });
	QVERIFY(Test::WaitFor([&] { return loaded && notes.loaded(); }));
	QCOMPARE(spy.count(), 1);
	QVERIFY(notes.enabled());
}

void TestNotes::replyGoesToTheChat() {
	auto harness = Harness();
	QVERIFY(harness.start());
	harness.on(u"chat.thread"_s, [](const Call &call) {
		const auto peer = call.body.value("user_id"_L1).toString();
		return Reply::Ok(QJsonObject{
			{ u"chat"_s, u"c9"_s },
			{ u"kind"_s, u"direct"_s },
			{ u"state"_s, u"active"_s },
			{ u"participants"_s, QJsonArray{
				QJsonObject{ { u"ref"_s, u"u_me"_s }, { u"user_id"_s, u"u_me"_s }, { u"me"_s, true } },
				QJsonObject{ { u"ref"_s, peer }, { u"user_id"_s, peer } },
			} },
			{ u"last_seq"_s, 0 },
			{ u"can_send"_s, true },
		});
	});
	auto chat = QString();
	harness.session->notes().reply(
		u"u_sara"_s,
		u"کسی کوه میاد؟"_s,
		u" من هستم! "_s,
		base::Guard(),
		[&](const QString &id) { chat = id; });
	QVERIFY(Test::WaitFor([&] { return !chat.isEmpty(); }));
	QCOMPARE(chat, u"c9"_s);
	QCOMPARE(harness.bodies(u"chat.thread"_s).front().value("user_id"_L1).toString(), u"u_sara"_s);
	QVERIFY(Test::WaitFor([&] { return harness.transport.count(u"chat.send"_s) > 0; }));
	const auto body = harness.bodies(u"chat.send"_s).front();
	QCOMPARE(body.value("chat"_L1).toString(), u"c9"_s);
	QCOMPARE(body.value("text"_L1).toString(), kArrow + u" «کسی کوه میاد؟»\nمن هستم!"_s);
}

QTEST_GUILESS_MAIN(TestNotes)
#include "test_notes.moc"
