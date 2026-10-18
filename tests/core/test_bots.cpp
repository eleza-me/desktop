// Data::Bots on the test HTTP server: a bot's information is fetched once
// (callers that wait together share one request), handles are looked up
// once (a person and a missing account are remembered, a failure is not),
// inline queries post {query, offset} and parse the page.
#include "data/data_bots.h"
#include "test_harness.h"

#include <QtTest/QtTest>

namespace {

using Request = Test::HttpServer::Request;
using Response = Test::HttpServer::Response;

[[nodiscard]] QJsonObject WeatherBot() {
	return QJsonObject{
		{ u"id"_s, u"ubot1"_s },
		{ u"handle"_s, u"Weather_Bot"_s },
		{ u"name"_s, u"Weather"_s },
		{ u"description"_s, u"Forecasts for any city."_s },
		{ u"commands"_s, QJsonArray{
			QJsonObject{ { u"command"_s, u"today"_s }, { u"description"_s, u"Today"_s } },
		} },
		{ u"inline"_s, QJsonObject{
			{ u"enabled"_s, true },
			{ u"placeholder"_s, u"City"_s },
		} },
	};
}

[[nodiscard]] int Count(const Test::HttpServer &http, const QByteArray &path) {
	auto result = 0;
	for (const auto &request : http.requests) {
		if (request.path == path) {
			++result;
		}
	}
	return result;
}

} // namespace

class TestBots : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void infoIsFetchedOnce() {
		auto harness = Test::Harness();
		harness.http.handler = [](const Request &request) {
			if (request.path == "/v1/bots/ubot1") {
				return Response::Ok(WeatherBot());
			}
			return Response::Problem(404, "user.not_found");
		};
		auto &bots = harness.session->bots();
		auto answers = 0;
		auto changed = 0;
		connect(&bots, &Data::Bots::changed, this, [&](const QString &id) {
			QCOMPARE(id, u"ubot1"_s);
			++changed;
		});
		// Two callers before the answer: one request.
		bots.load(u"ubot1"_s, base::Guard(), [&](const Data::BotInfo &info) {
			QCOMPARE(info.commands.size(), 1);
			++answers;
		});
		bots.load(u"ubot1"_s, base::Guard(), [&](const Data::BotInfo &info) {
			QCOMPARE(info.description, u"Forecasts for any city."_s);
			++answers;
		});
		QVERIFY(Test::WaitFor([&] { return answers == 2; }));
		QCOMPARE(Count(harness.http, "/v1/bots/ubot1"), 1);
		QCOMPARE(changed, 1);
		QVERIFY(bots.info(u"ubot1"_s));
		QVERIFY(bots.info(u"ubot1"_s)->inlineEnabled);
		// From memory now: answered at once.
		auto cached = false;
		bots.load(u"ubot1"_s, base::Guard(), [&](const Data::BotInfo &) {
			cached = true;
		});
		QVERIFY(cached);
		QCOMPARE(Count(harness.http, "/v1/bots/ubot1"), 1);
		// Its handle is known as a bot's, whatever its case.
		QCOMPARE(bots.handleState(u"weather_bot"_s), Data::Bots::HandleState::Bot);
		QCOMPARE(bots.byHandle(u"@WEATHER_BOT"_s)->id, u"ubot1"_s);
	}

	void failureReachesEveryCaller() {
		auto harness = Test::Harness();
		harness.http.handler = [](const Request &) {
			return Response::Problem(403, "bot.disabled");
		};
		auto &bots = harness.session->bots();
		auto failed = 0;
		for (auto i = 0; i != 2; ++i) {
			bots.load(u"ubot1"_s, base::Guard(), [](const Data::BotInfo &) {
				QFAIL("no info expected");
			}, [&](const Api::Error &error) {
				QCOMPARE(error.code, u"bot.disabled"_s);
				++failed;
			});
		}
		QVERIFY(Test::WaitFor([&] { return failed == 2; }));
		QVERIFY(!bots.info(u"ubot1"_s));
	}

	void handlesAreResolved() {
		auto harness = Test::Harness();
		auto broken = true;
		harness.http.handler = [&](const Request &request) {
			if (request.path == "/v1/users/by-handle/weather_bot") {
				return Response::Ok(QJsonObject{
					{ u"id"_s, u"ubot1"_s },
					{ u"handle"_s, u"weather_bot"_s },
					{ u"display_name"_s, u"Weather"_s },
					{ u"is_bot"_s, true },
				});
			} else if (request.path == "/v1/bots/ubot1") {
				return Response::Ok(WeatherBot());
			} else if (request.path == "/v1/users/by-handle/sara_bot") {
				// A person whose handle happens to end in «bot».
				return Response::Ok(QJsonObject{
					{ u"id"_s, u"usara"_s },
					{ u"handle"_s, u"sara_bot"_s },
				});
			} else if (request.path == "/v1/users/by-handle/flaky_bot" && broken) {
				return Response::Problem(503, "server.unavailable");
			}
			return Response::Problem(404, "user.not_found");
		};
		auto &bots = harness.session->bots();
		const auto resolve = [&](const QString &handle) {
			auto done = false;
			bots.resolve(handle, base::Guard(), [&] { done = true; });
			return Test::WaitFor([&] { return done; });
		};
		QCOMPARE(bots.handleState(u"weather_bot"_s), Data::Bots::HandleState::Unknown);
		QVERIFY(resolve(u"weather_bot"_s));
		QCOMPARE(bots.handleState(u"weather_bot"_s), Data::Bots::HandleState::Bot);
		QVERIFY(bots.byHandle(u"weather_bot"_s)->inlineEnabled);

		QVERIFY(resolve(u"sara_bot"_s));
		QCOMPARE(bots.handleState(u"sara_bot"_s), Data::Bots::HandleState::NotBot);
		QVERIFY(resolve(u"nobody_bot"_s));
		QCOMPARE(bots.handleState(u"nobody_bot"_s), Data::Bots::HandleState::NotBot);

		// Remembered: no second request.
		const auto before = harness.http.requests.size();
		QVERIFY(resolve(u"sara_bot"_s));
		QCOMPARE(harness.http.requests.size(), before);

		// A failure is not remembered: the next keystroke asks again.
		QVERIFY(resolve(u"flaky_bot"_s));
		QCOMPARE(bots.handleState(u"flaky_bot"_s), Data::Bots::HandleState::Unknown);
		broken = false;
		QVERIFY(resolve(u"flaky_bot"_s));
		QCOMPARE(bots.handleState(u"flaky_bot"_s), Data::Bots::HandleState::NotBot);
	}

	void inlineQueryPage() {
		auto harness = Test::Harness();
		harness.http.handler = [](const Request &request) {
			if (request.method == "POST" && request.path == "/v1/bots/ubot1/inline-query") {
				const auto body = request.json();
				const auto more = (body.value("offset"_L1).toString() == u"20");
				return Response::Ok(QJsonObject{
					{ u"query_id"_s, more ? u"q2"_s : u"q1"_s },
					{ u"answered"_s, true },
					{ u"next_offset"_s, more ? QString() : u"20"_s },
					{ u"results"_s, QJsonArray{ QJsonObject{
						{ u"type"_s, u"article"_s },
						{ u"id"_s, more ? u"syz"_s : u"thr"_s },
						{ u"title"_s, body.value("query"_L1).toString() },
					} } },
				});
			}
			return Response::Problem(404, "not_found");
		};
		auto &bots = harness.session->bots();
		auto first = std::optional<Data::InlineResults>();
		bots.inlineQuery(u"ubot1"_s, u"tehran"_s, QString(), base::Guard(), [&](
				const Data::InlineResults &page) {
			first = page;
		});
		QVERIFY(Test::WaitFor([&] { return first.has_value(); }));
		QCOMPARE(first->queryId, u"q1"_s);
		QCOMPARE(first->nextOffset, u"20"_s);
		QCOMPARE(first->results.size(), 1);
		QCOMPARE(first->results[0].title, u"tehran"_s);
		const auto sent = harness.http.requests.back().json();
		QCOMPARE(sent.value("query"_L1).toString(), u"tehran"_s);
		QVERIFY(!sent.contains("offset"_L1)); // the first page has none

		auto second = std::optional<Data::InlineResults>();
		bots.inlineQuery(u"ubot1"_s, u"tehran"_s, u"20"_s, base::Guard(), [&](
				const Data::InlineResults &page) {
			second = page;
		});
		QVERIFY(Test::WaitFor([&] { return second.has_value(); }));
		QCOMPARE(second->queryId, u"q2"_s);
		QVERIFY(second->nextOffset.isEmpty());
		QCOMPARE(second->results[0].id, u"syz"_s);
	}

	void inlineResultGoesThroughTheOutbox() {
		auto harness = Test::Harness();
		QVERIFY(harness.start());
		const auto id = harness.outbox().sendInlineResult(
			u"c1"_s,
			u"ubot1"_s,
			u"q1"_s,
			u"thr"_s,
			u"Tehran"_s);
		QVERIFY(!id.isEmpty());
		QVERIFY(Test::WaitFor([&] {
			return !harness.bodies(u"chat.send"_s).isEmpty();
		}));
		const auto body = harness.bodies(u"chat.send"_s).back();
		QCOMPARE(body.value("chat"_L1).toString(), u"c1"_s);
		QCOMPARE(body.value("client_message_id"_L1).toString(), id);
		// The content comes from the bot's answer: no kind, no text.
		QVERIFY(!body.contains("text"_L1));
		QVERIFY(!body.contains("kind"_L1));
		const auto result = body.value("inline_result"_L1).toObject();
		QCOMPARE(result.value("bot"_L1).toString(), u"ubot1"_s);
		QCOMPARE(result.value("query_id"_L1).toString(), u"q1"_s);
		QCOMPARE(result.value("result_id"_L1).toString(), u"thr"_s);
		// The local bubble shows the title until the message comes back.
		const auto &history = harness.messages().history(u"c1"_s);
		const auto index = history.indexOfKey(Data::Message::KeyForLocal(id));
		QVERIFY(index >= 0);
		QCOMPARE(history.items[index].text, u"Tehran"_s);
	}
};

QTEST_GUILESS_MAIN(TestBots)
#include "test_bots.moc"
