// Api::Client (headers, retries, errors, downloads, uploads) against an
// in-process HTTP server, and Main::Account's sign-in / token refresh flows.
#include "test_fakes.h"

#include "api/api_auth.h"
#include "api/api_client.h"
#include "api/api_client_config.h"
#include "base/bytes.h"
#include "base/platform_file.h"
#include "base/unixtime.h"
#include "crypto/crypto_device_key.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"

#include <QtCore/QTemporaryDir>

using Test::HttpServer;

namespace {

class Tokens final : public Api::TokenProvider {
public:
	QString token = QStringLiteral("token-1");
	int refreshes = 0;
	bool refreshOk = true;
	QStringList failed;

	void accessToken(Fn<void(QString)> done) override {
		done(token);
	}
	void refreshAfterUnauthorized(const QString &failedToken, Fn<void(bool)> done) override {
		failed.push_back(failedToken);
		++refreshes;
		if (refreshOk) {
			token = QStringLiteral("token-%1").arg(refreshes + 1);
		}
		done(refreshOk);
	}
};

struct Outcome {
	bool done = false;
	bool failed = false;
	QJsonObject json;
	Api::Error error;

	[[nodiscard]] bool finished() const {
		return done || failed;
	}
};

[[nodiscard]] std::shared_ptr<Outcome> Send(Api::RequestBuilder builder) {
	const auto outcome = std::make_shared<Outcome>();
	builder
		.done([=](const QJsonObject &json) {
			outcome->done = true;
			outcome->json = json;
		})
		.fail([=](const Api::Error &error) {
			outcome->failed = true;
			outcome->error = error;
		})
		.send();
	return outcome;
}

[[nodiscard]] QByteArray AccessToken(const QString &user, const QString &sid) {
	const auto claims = QJsonDocument(QJsonObject{ { u"sub"_s, user }, { u"sid"_s, sid } }).toJson(QJsonDocument::Compact);
	return "h." + claims.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) + ".s";
}

[[nodiscard]] QJsonObject TokensDocument(int generation, int expiresInSeconds, const QString &user = u"u_me"_s) {
	return QJsonObject{
		{ u"access_token"_s, QString::fromLatin1(AccessToken(user, u"s1"_s)) + QString::number(generation) },
		{ u"access_expires_at"_s, QDateTime::currentDateTimeUtc().addSecs(expiresInSeconds).toString(Qt::ISODateWithMs) },
		{ u"refresh_token"_s, u"refresh-%1"_s.arg(generation) },
		{ u"device_nonce"_s, base::bytes::toBase64(QByteArray("nonce-") + QByteArray::number(generation)) },
		{ u"user_id"_s, user },
	};
}

} // namespace

class TestApi final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void headersAndQuery();
	void idempotencyKeys();
	void problemJson();
	void logLinesCarryNoIds();
	void overloadRetriesReuseIds();
	void upgradeRequired();
	void unauthorizedRefreshesOnce();
	void offlineAndDecoding();
	void guardAndCancel();
	void downloadFollowsRedirectWithoutToken();
	void uploadSendsSlice();
	void clientConfig();
	void signInBindsDeviceKey();
	void twoStepAndProfile();
	void refreshProvesDeviceKeyAndIsShared();
	void refreshReissuesNonceOnce();
	void refreshFailuresKeepOrEndTheSession();
	void logoutWipes();
	void onlyWebAddressesAreReached();
	void oversizedJsonIsRefused();
	void abortAuthenticatedStopsRetries();
	void errorBodiesAreBounded();

private:
	[[nodiscard]] Main::Account::Config accountConfig(const HttpServer &server, const QString &name) {
		auto config = Main::Account::Config();
		config.workdir = _dir.path() + u'/' + name;
		config.apiBase = server.base();
		config.version = u"9.9.9"_s;
		config.realtime = false;
		return config;
	}

	QTemporaryDir _dir;

};

void TestApi::headersAndQuery() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &) {
		return HttpServer::Response::Ok(Test::Json(R"({"ok":true})"));
	};
	auto client = Api::Client(server.base() + u"/"_s, u"1.2.3"_s);
	client.setLanguage(u"en"_s);
	QCOMPARE(client.base(), server.base());
	QVERIFY(client.clientHeader().startsWith(u"desktop/1.2.3 ("));
	QCOMPARE(client.webSocketUrl().toString(), u"ws://127.0.0.1:%1/v2/ws"_s.arg(QUrl(server.base()).port()));
	QCOMPARE(Api::Client(u"https://api.eleza.me"_s, u"1"_s).webSocketUrl().toString(), u"wss://api.eleza.me/v2/ws"_s);

	const auto outcome = Send(std::move(client.request(Api::Get, u"/v1/search"_s)
		.query(u"q"_s, u"سلام & co"_s)
		.query(u"empty"_s, QString())
		.query(u"limit"_s, 20)));
	QVERIFY(Test::WaitFor([&] { return outcome->finished(); }));
	QVERIFY(outcome->done);
	QCOMPARE(outcome->json.value(u"ok"_s).toBool(), true);
	QCOMPARE(server.requests.size(), 1);
	const auto &request = server.requests[0];
	QCOMPARE(request.method, QByteArray("GET"));
	QCOMPARE(request.path, QByteArray("/v1/search"));
	QCOMPARE(QUrl::fromEncoded(request.target).query(QUrl::FullyDecoded), u"q=سلام & co&limit=20"_s);
	QVERIFY(request.target.contains("%26")); // the ampersand of the value is escaped
	QCOMPARE(request.header("X-Eleza-Client"), client.clientHeader().toUtf8());
	QCOMPARE(request.header("Accept-Language"), QByteArray("en"));
	QCOMPARE(request.header("Accept"), QByteArray("application/json"));
	QCOMPARE(request.header("X-Request-Id").size(), 36);
	QVERIFY(!request.headers.contains("idempotency-key")); // GET
	QVERIFY(!request.headers.contains("authorization"));   // no token provider
	QVERIFY(!request.headers.contains("content-type"));
}

void TestApi::idempotencyKeys() {
	auto server = HttpServer();
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);
	const auto first = Send(std::move(client.request(Api::Post, u"/v1/a"_s).body(Test::Json(R"({"x":1})"))));
	const auto second = Send(std::move(client.request(Api::Put, u"/v1/b"_s).idempotencyKey(u"my-key"_s)));
	const auto third = Send(std::move(client.request(Api::Delete, u"/v1/c"_s).noIdempotencyKey().requestId(u"my-request"_s)));
	const auto fourth = Send(std::move(client.request(Api::Post, u"/v1/d"_s).unauthenticated()));
	QVERIFY(Test::WaitFor([&] { return first->finished() && second->finished() && third->finished() && fourth->finished(); }));
	const auto a = server.to("/v1/a").value(0);
	QCOMPARE(a.method, QByteArray("POST"));
	QCOMPARE(a.header("Idempotency-Key").size(), 36);
	QCOMPARE(a.header("Content-Type"), QByteArray("application/json"));
	QCOMPARE(a.json(), Test::Json(R"({"x":1})"));
	QCOMPARE(a.header("Authorization"), QByteArray("Bearer token-1"));
	QCOMPARE(server.to("/v1/b").value(0).header("Idempotency-Key"), QByteArray("my-key"));
	QCOMPARE(server.to("/v1/b").value(0).method, QByteArray("PUT"));
	const auto c = server.to("/v1/c").value(0);
	QVERIFY(!c.headers.contains("idempotency-key"));
	QCOMPARE(c.header("X-Request-Id"), QByteArray("my-request"));
	QCOMPARE(c.method, QByteArray("DELETE"));
	QVERIFY(!server.to("/v1/d").value(0).headers.contains("authorization"));
	// Different calls never share keys.
	QVERIFY(a.header("Idempotency-Key") != server.to("/v1/d").value(0).header("Idempotency-Key"));
}

void TestApi::problemJson() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		if (request.path == "/v1/missing") {
			return HttpServer::Response::Problem(404, "chat.not_found", "پیدا نشد");
		} else if (request.path == "/v1/limited") {
			auto response = HttpServer::Response::Problem(429, "rate.limited");
			response.headers.push_back({ "Retry-After", "7" });
			return response;
		} else if (request.path == "/v1/nested") {
			auto response = HttpServer::Response();
			response.status = 400;
			response.body = R"({"error":{"code":"nested.code","message":"nested message"}})";
			return response;
		}
		return HttpServer::Response::Bytes("<html>bad gateway</html>", "text/html");
	};
	server.handler = [inner = server.handler](const HttpServer::Request &request) {
		auto response = inner(request);
		if (request.path == "/v1/html") {
			response.status = 502;
		}
		return response;
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto failures = 0;
	connect(&client, &Api::Client::requestFailed, this, [&] { ++failures; });
	const auto missing = Send(client.request(Api::Get, u"/v1/missing"_s));
	const auto limited = Send(client.request(Api::Get, u"/v1/limited"_s));
	const auto nested = Send(client.request(Api::Get, u"/v1/nested"_s));
	const auto html = Send(std::move(client.request(Api::Get, u"/v1/html"_s).noOverloadRetry()));
	QVERIFY(Test::WaitFor([&] { return missing->finished() && limited->finished() && nested->finished() && html->finished(); }));
	QVERIFY(missing->failed);
	QCOMPARE(missing->error.status, 404);
	QCOMPARE(missing->error.code, u"chat.not_found"_s);
	QCOMPARE(missing->error.message, u"پیدا نشد"_s);
	QVERIFY(missing->error.notFound() && !missing->error.retryable && !missing->error.transient());
	QCOMPARE(missing->error.requestId.size(), 36);
	QCOMPARE(missing->error.body.toObject().value(u"code"_s).toString(), u"chat.not_found"_s);
	QVERIFY(missing->error.describe().contains(u"404 chat.not_found"));

	QCOMPARE(limited->error.status, 429);
	QCOMPARE(limited->error.retryAfter.value_or(0.), 7.);
	QVERIFY(limited->error.retryable && limited->error.rateLimited() && limited->error.transient());

	QCOMPARE(nested->error.code, u"nested.code"_s);
	QCOMPARE(nested->error.message, u"nested message"_s);

	QCOMPARE(html->error.status, 502);
	QVERIFY(html->error.code.isEmpty());
	QVERIFY(html->error.retryable && html->error.serverSide());
	QCOMPARE(failures, 4);
}

void TestApi::logLinesCarryNoIds() {
	using Api::RouteTemplate;
	QCOMPARE(RouteTemplate(u"/v1/me"_s), u"/v1/me"_s);
	QCOMPARE(RouteTemplate(u"/v1/me/note"_s), u"/v1/me/note"_s);
	QCOMPARE(RouteTemplate(u"/v1/notes"_s), u"/v1/notes"_s);
	QCOMPARE(RouteTemplate(u"/v1/auth/otp/verify"_s), u"/v1/auth/otp/verify"_s);
	QCOMPARE(RouteTemplate(u"/v1/chats/c1/messages?before=40&limit=50"_s), u"/v1/chats/:id/messages"_s);
	QCOMPARE(
		RouteTemplate(u"/v1/chats/01HZX3K8/messages/42/attachment?variant=thumb"_s),
		u"/v1/chats/:id/messages/:id/attachment"_s);
	QCOMPARE(RouteTemplate(u"/v1/users/u_sara/follow"_s), u"/v1/users/:id/follow"_s);
	QCOMPARE(RouteTemplate(u"/v1/posts/p1/media/m7?variant=full"_s), u"/v1/posts/:id/media/:id"_s);
	QCOMPARE(RouteTemplate(u"/v1/me/follow-requests/u9/accept"_s), u"/v1/me/follow-requests/:id/accept"_s);
	// A handle or a slug is never kept, even when it spells a word of the API.
	QCOMPARE(RouteTemplate(u"/v1/users/by-handle/sara"_s), u"/v1/users/by-handle/:id"_s);
	QCOMPARE(RouteTemplate(u"/v1/users/by-handle/settings"_s), u"/v1/users/by-handle/:id"_s);
	QCOMPARE(RouteTemplate(u"/v1/handles/me/availability"_s), u"/v1/handles/:id/availability"_s);
	QCOMPARE(RouteTemplate(u"/v1/posts/by-slug/search"_s), u"/v1/posts/by-slug/:id"_s);
	// Unknown words, encoded text, other hosts, fragments.
	QCOMPARE(RouteTemplate(u"/v1/something-new/x"_s), u"/v1/:id/:id"_s);
	QCOMPARE(RouteTemplate(u"/v1/users/%D8%B3%D8%A7%D8%B1%D8%A7"_s), u"/v1/users/:id"_s);
	QCOMPARE(RouteTemplate(u"https://storage.example/bucket/a1.png?sig=abc"_s), u"<url>"_s);
	QCOMPARE(RouteTemplate(u"/v1/search?q=sara#frag"_s), u"/v1/search"_s);
	QCOMPARE(RouteTemplate(u"/"_s), u"/"_s);
	QCOMPARE(RouteTemplate(QString()), QString());

	auto error = Api::Error::Local(404, u"chat.not_found"_s, u"no chat with Sara"_s);
	error.requestId = u"req-1"_s;
	QVERIFY(error.describe().contains(u"Sara"));
#ifdef ELEZA_DEBUG_TOOLS
	QCOMPARE(error.forLog(), error.describe());
	QCOMPARE(Api::RouteForLog(u"/v1/chats/c1/read"_s), u"/v1/chats/c1/read"_s);
#else // ELEZA_DEBUG_TOOLS
	QCOMPARE(error.forLog(), u"404 chat.not_found (request req-1)"_s);
	QCOMPARE(Api::RouteForLog(u"/v1/chats/c1/read"_s), u"/v1/chats/:id/read"_s);
#endif // ELEZA_DEBUG_TOOLS
}

void TestApi::overloadRetriesReuseIds() {
	auto server = HttpServer();
	auto failuresLeft = 2;
	server.handler = [&](const HttpServer::Request &request) {
		if (request.path == "/v1/flaky" && failuresLeft-- > 0) {
			auto response = HttpServer::Response::Problem(503, "server.overloaded");
			response.headers.push_back({ "Retry-After", "0.05" });
			return response;
		} else if (request.path == "/v1/down") {
			return HttpServer::Response::Problem(503, "server.overloaded");
		} else if (request.path == "/v1/slow") {
			auto response = HttpServer::Response::Problem(503, "server.overloaded");
			response.headers.push_back({ "Retry-After", "60" });
			return response;
		}
		return HttpServer::Response::Ok(Test::Json(R"({"seq":8})"));
	};
	auto client = Api::Client(server.base(), u"1"_s);
	const auto flaky = Send(std::move(client.request(Api::Post, u"/v1/flaky"_s).body(QJsonObject())));
	QVERIFY(Test::WaitFor([&] { return flaky->finished(); }));
	QVERIFY(flaky->done);
	QCOMPARE(flaky->json.value(u"seq"_s).toInt(), 8);
	const auto attempts = server.to("/v1/flaky");
	QCOMPARE(attempts.size(), 3);
	// The same Idempotency-Key and X-Request-Id on every attempt.
	QVERIFY(!attempts[0].header("Idempotency-Key").isEmpty());
	QCOMPARE(attempts[1].header("Idempotency-Key"), attempts[0].header("Idempotency-Key"));
	QCOMPARE(attempts[2].header("Idempotency-Key"), attempts[0].header("Idempotency-Key"));
	QCOMPARE(attempts[2].header("X-Request-Id"), attempts[0].header("X-Request-Id"));

	// Two retries (0.4 s, 0.8 s), then the error.
	const auto down = Send(client.request(Api::Get, u"/v1/down"_s));
	QVERIFY(Test::WaitFor([&] { return down->finished(); }, 8000));
	QVERIFY(down->failed);
	QCOMPARE(down->error.status, 503);
	QCOMPARE(server.count("/v1/down"), 3);

	// A long Retry-After is the caller's to wait for; noOverloadRetry never retries.
	const auto slow = Send(client.request(Api::Get, u"/v1/slow"_s));
	const auto once = Send(std::move(client.request(Api::Get, u"/v1/down"_s).noOverloadRetry()));
	QVERIFY(Test::WaitFor([&] { return slow->finished() && once->finished(); }));
	QCOMPARE(server.count("/v1/slow"), 1);
	QCOMPARE(slow->error.retryAfter.value_or(0.), 60.);
	QCOMPARE(server.count("/v1/down"), 4);
}

void TestApi::upgradeRequired() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &) {
		return HttpServer::Response::Problem(426, "client.upgrade_required");
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto upgrades = 0;
	connect(&client, &Api::Client::upgradeRequired, this, [&] { ++upgrades; });
	const auto outcome = Send(client.request(Api::Get, u"/v1/me"_s));
	QVERIFY(Test::WaitFor([&] { return outcome->finished(); }));
	QCOMPARE(outcome->error.status, 426);
	QCOMPARE(upgrades, 1);
}

void TestApi::unauthorizedRefreshesOnce() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		if (request.path == "/v1/auth/thing") {
			return HttpServer::Response::Problem(401, "auth.invalid");
		} else if (request.path == "/v1/always") {
			return HttpServer::Response::Problem(401, "auth.unauthorized");
		} else if (request.header("Authorization") == "Bearer token-1") {
			return HttpServer::Response::Problem(401, "auth.token_expired");
		}
		return HttpServer::Response::Ok(Test::Json(R"({"id":"u_me"})"));
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);

	const auto me = Send(std::move(client.request(Api::Post, u"/v1/me"_s).body(QJsonObject())));
	QVERIFY(Test::WaitFor([&] { return me->finished(); }));
	QVERIFY(me->done);
	QCOMPARE(tokens.refreshes, 1);
	QCOMPARE(tokens.failed, QStringList{ u"token-1"_s });
	const auto attempts = server.to("/v1/me");
	QCOMPARE(attempts.size(), 2);
	QCOMPARE(attempts[1].header("Authorization"), QByteArray("Bearer token-2"));
	QCOMPARE(attempts[1].header("X-Request-Id"), attempts[0].header("X-Request-Id"));
	QCOMPARE(attempts[1].header("Idempotency-Key"), attempts[0].header("Idempotency-Key"));

	// A second 401 after the refresh is final: no loop.
	const auto always = Send(client.request(Api::Get, u"/v1/always"_s));
	QVERIFY(Test::WaitFor([&] { return always->finished(); }));
	QVERIFY(always->failed && always->error.unauthorized());
	QCOMPARE(server.count("/v1/always"), 2);
	QCOMPARE(tokens.refreshes, 2);

	// Auth endpoints answer 401 for their own reasons: never refreshed.
	const auto auth = Send(std::move(client.request(Api::Post, u"/v1/auth/thing"_s).body(QJsonObject())));
	QVERIFY(Test::WaitFor([&] { return auth->finished(); }));
	QCOMPARE(server.count("/v1/auth/thing"), 1);
	QCOMPARE(tokens.refreshes, 2);

	// The refresh itself fails: the original error surfaces.
	tokens.token = u"token-1"_s;
	tokens.refreshOk = false;
	const auto failed = Send(client.request(Api::Get, u"/v1/me"_s));
	QVERIFY(Test::WaitFor([&] { return failed->finished(); }));
	QVERIFY(failed->failed);
	QCOMPARE(failed->error.code, u"auth.token_expired"_s);
}

void TestApi::offlineAndDecoding() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		if (request.path == "/v1/empty") {
			auto response = HttpServer::Response();
			response.status = 204;
			return response;
		} else if (request.path == "/v1/list") {
			return HttpServer::Response::Bytes("[1,2,3]", "application/json");
		}
		return HttpServer::Response::Bytes("{not json", "application/json");
	};
	auto client = Api::Client(server.base(), u"1"_s);
	const auto bad = Send(client.request(Api::Get, u"/v1/bad"_s));
	const auto empty = Send(client.request(Api::Get, u"/v1/empty"_s));
	auto list = QJsonValue();
	client.request(Api::Get, u"/v1/list"_s).doneValue([&](const QJsonValue &value) { list = value; }).send();
	QVERIFY(Test::WaitFor([&] { return bad->finished() && empty->finished() && list.isArray(); }));
	QVERIFY(bad->failed);
	QCOMPARE(bad->error.code, u"decoding"_s);
	QVERIFY(empty->done);
	QVERIFY(empty->json.isEmpty());
	QCOMPARE(list.toArray().size(), 3);

	// Nothing listens there: no HTTP answer at all.
	auto closed = QTcpServer();
	QVERIFY(closed.listen(QHostAddress::LocalHost, 0));
	const auto port = closed.serverPort();
	closed.close();
	auto nowhere = Api::Client(u"http://127.0.0.1:%1"_s.arg(port), u"1"_s);
	const auto offline = Send(nowhere.request(Api::Get, u"/v1/me"_s));
	QVERIFY(Test::WaitFor([&] { return offline->finished(); }));
	QVERIFY(offline->failed);
	QVERIFY(offline->error.offline());
	QCOMPARE(offline->error.code, u"offline"_s);
	QVERIFY(offline->error.retryable && offline->error.transient());
}

void TestApi::guardAndCancel() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		auto response = HttpServer::Response::Ok();
		response.hang = (request.path == "/v1/hang");
		return response;
	};
	auto client = std::make_unique<Api::Client>(server.base(), u"1"_s);
	auto calls = 0;
	auto owner = std::make_unique<QObject>();
	client->request(Api::Get, u"/v1/guarded"_s)
		.guard(owner.get())
		.done([&] { ++calls; })
		.fail([&](const Api::Error &) { ++calls; })
		.send();
	owner = nullptr; // the screen went away before the answer
	const auto id = client->request(Api::Get, u"/v1/hang"_s)
		.done([&] { ++calls; })
		.fail([&](const Api::Error &) { ++calls; })
		.send();
	auto other = QObject();
	client->request(Api::Get, u"/v1/hang"_s).guard(&other).fail([&](const Api::Error &) { ++calls; }).send();
	QVERIFY(Test::WaitFor([&] { return server.count("/v1/hang") == 2 && server.count("/v1/guarded") == 1; }));
	client->cancel(id);
	client->cancelAll(&other);
	Test::Settle(50);
	QCOMPARE(calls, 0);
	// A short timeout turns a hanging request into an offline error.
	const auto timed = Send(std::move(client->request(Api::Get, u"/v1/hang"_s).timeout(100)));
	QVERIFY(Test::WaitFor([&] { return timed->finished(); }));
	QVERIFY(timed->error.offline());
	// Destroying the client with a request in flight is quiet.
	client->request(Api::Get, u"/v1/hang"_s).fail([&](const Api::Error &) { ++calls; }).send();
	QVERIFY(Test::WaitFor([&] { return server.count("/v1/hang") == 4; }));
	client = nullptr;
	Test::Settle(50);
	QCOMPARE(calls, 0);
}

void TestApi::downloadFollowsRedirectWithoutToken() {
	auto storage = HttpServer();
	const auto payload = QByteArray(300000, 'x') + "end";
	storage.handler = [&](const HttpServer::Request &request) {
		return (request.path == "/bucket/object")
			? HttpServer::Response::Bytes(payload, "image/png")
			: HttpServer::Response::Problem(404, "storage.missing");
	};
	auto api = HttpServer();
	api.handler = [&](const HttpServer::Request &request) {
		if (request.header("Authorization") == "Bearer token-1" && request.path != "/v1/public") {
			return HttpServer::Response::Problem(401, "auth.token_expired");
		} else if (request.path == "/v1/media") {
			return HttpServer::Response::Redirect((storage.base() + u"/bucket/object?sig=abc"_s).toLatin1());
		} else if (request.path == "/v1/relative") {
			return HttpServer::Response::Redirect("/v1/media");
		} else if (request.path == "/v1/loop") {
			return HttpServer::Response::Redirect("/v1/loop");
		} else if (request.path == "/v1/gone") {
			return HttpServer::Response::Problem(404, "media.not_found");
		}
		return HttpServer::Response::Bytes("public", "text/plain");
	};
	auto client = Api::Client(api.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);

	auto result = std::optional<Api::DownloadResult>();
	auto progress = qint64(0);
	client.download(u"/v1/relative?variant=full"_s)
		.progress([&](qint64 received, qint64) { progress = received; })
		.done([&](const Api::DownloadResult &value) { result = value; })
		.send();
	QVERIFY(Test::WaitFor([&] { return result.has_value(); }));
	QCOMPARE(result->bytes, payload);
	QCOMPARE(result->size, payload.size());
	QCOMPARE(result->contentType, u"image/png"_s);
	QCOMPARE(progress, payload.size());
	// The API saw the (refreshed) token on every hop it served...
	QCOMPARE(tokens.refreshes, 1);
	QCOMPARE(api.to("/v1/media").value(0).header("Authorization"), QByteArray("Bearer token-2"));
	QCOMPARE(api.to("/v1/media").value(0).header("X-Eleza-Client"), client.clientHeader().toUtf8());
	// ...and the storage host saw neither the token nor our client header.
	QCOMPARE(storage.requests.size(), 1);
	QVERIFY(!storage.requests[0].headers.contains("authorization"));
	QVERIFY(!storage.requests[0].headers.contains("x-eleza-client"));
	QCOMPARE(storage.requests[0].target, QByteArray("/bucket/object?sig=abc"));

	// To a file: written as .part and renamed.
	const auto path = _dir.path() + u"/downloads/a/photo.bin"_s;
	result = std::nullopt;
	client.download(u"/v1/media"_s).toFile(path).done([&](const Api::DownloadResult &value) { result = value; }).send();
	QVERIFY(Test::WaitFor([&] { return result.has_value(); }));
	QCOMPARE(result->filePath, path);
	QVERIFY(result->bytes.isEmpty());
	QCOMPARE(QFileInfo(path).size(), payload.size());
	QVERIFY(!QFileInfo::exists(path + u".part"_s));

	// An absolute URL is fetched without the token.
	result = std::nullopt;
	client.download(storage.base() + u"/bucket/object"_s).done([&](const Api::DownloadResult &value) { result = value; }).send();
	QVERIFY(Test::WaitFor([&] { return result.has_value(); }));
	QVERIFY(!storage.requests.back().headers.contains("authorization"));

	auto errors = QVector<Api::Error>();
	const auto fail = [&](const Api::Error &error) { errors.push_back(error); };
	client.download(u"/v1/gone"_s).fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 1; }));
	QCOMPARE(errors[0].status, 404);
	QCOMPARE(errors[0].code, u"media.not_found"_s);
	client.download(u"/v1/loop"_s).fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 2; }));
	QCOMPARE(errors[1].code, u"redirect"_s);
	const auto limited = _dir.path() + u"/downloads/limited.bin"_s;
	client.download(u"/v1/media"_s).toFile(limited).maxBytes(1000).fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 3; }));
	QCOMPARE(errors[2].code, u"too_large"_s);
	QVERIFY(!QFileInfo::exists(limited) && !QFileInfo::exists(limited + u".part"_s));
}

void TestApi::uploadSendsSlice() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		if (request.path == "/expired") {
			return HttpServer::Response::Bytes("<Error>expired</Error>", "application/xml");
		}
		return HttpServer::Response();
	};
	server.handler = [inner = server.handler](const HttpServer::Request &request) {
		auto response = inner(request);
		if (request.path == "/expired") {
			response.status = 403;
		}
		return response;
	};
	const auto path = _dir.path() + u"/upload.bin"_s;
	auto content = QByteArray();
	for (auto i = 0; i != 100000; ++i) {
		content.append(char('a' + i % 26));
	}
	{
		auto file = QFile(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(content);
	}
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);
	auto done = 0;
	auto sent = qint64(0);
	auto total = qint64(0);
	client.upload(QUrl(server.base() + u"/part1?sig=1"_s))
		.contentType(u"video/mp4"_s)
		.file(path, 1000, 50000)
		.progress([&](qint64 s, qint64 t) { sent = s; total = t; })
		.done([&] { ++done; })
		.send();
	client.upload(QUrl(server.base() + u"/tail"_s)).file(path, 99990).done([&] { ++done; }).send();
	client.upload(QUrl(server.base() + u"/bytes"_s)).method(Api::Post).header("x-amz-meta", "1").data("hello").done([&] { ++done; }).send();
	auto error = std::optional<Api::Error>();
	client.upload(QUrl(server.base() + u"/expired"_s)).data("x").fail([&](const Api::Error &e) { error = e; }).send();
	auto missing = std::optional<Api::Error>();
	client.upload(QUrl(server.base() + u"/missing"_s)).file(path + u".nope"_s).fail([&](const Api::Error &e) { missing = e; }).send();
	QVERIFY(Test::WaitFor([&] { return done == 3 && error && missing; }));
	const auto part = server.to("/part1").value(0);
	QCOMPARE(part.method, QByteArray("PUT"));
	QCOMPARE(part.body, content.mid(1000, 50000));
	QCOMPARE(part.header("Content-Type"), QByteArray("video/mp4"));
	QVERIFY(!part.headers.contains("authorization")); // presigned: no bearer token
	QCOMPARE(sent, 50000);
	QCOMPARE(total, 50000);
	QCOMPARE(server.to("/tail").value(0).body, content.right(10));
	QCOMPARE(server.to("/bytes").value(0).method, QByteArray("POST"));
	QCOMPARE(server.to("/bytes").value(0).body, QByteArray("hello"));
	QCOMPARE(server.to("/bytes").value(0).header("x-amz-meta"), QByteArray("1"));
	QCOMPARE(error->status, 403);
	QCOMPARE(error->code, u"upload.part"_s);
	QVERIFY(error->retryable); // an expired presigned URL: ask for a new one
	QCOMPARE(missing->code, u"file"_s);
	QCOMPARE(server.count("/missing"), 0);
}

void TestApi::clientConfig() {
	QCOMPARE(Api::ClientConfig::ParseDuration(u"24h"_s), 86400);
	QCOMPARE(Api::ClientConfig::ParseDuration(u"1h30m"_s), 5400);
	QCOMPARE(Api::ClientConfig::ParseDuration(u"90s"_s), 90);
	QCOMPARE(Api::ClientConfig::ParseDuration(u"2d"_s), 172800);
	QCOMPARE(Api::ClientConfig::ParseDuration(u"1500ms"_s), 1);
	QCOMPARE(Api::ClientConfig::ParseDuration(u"soon"_s), -1);
	QCOMPARE(Api::ClientConfig::ParseDuration(QString()), -1);
	QVERIFY(Api::ClientConfig::CompareVersions(u"1.2.3"_s, u"1.10"_s) < 0);
	QCOMPARE(Api::ClientConfig::CompareVersions(u"1.2"_s, u"1.2.0"_s), 0);
	QVERIFY(Api::ClientConfig::CompareVersions(u"2.0"_s, u"1.99.99"_s) > 0);

	auto server = HttpServer();
	auto document = Test::Fixture("client_config.json");
	document.insert(u"api"_s, Test::Json(R"({"min_client_version":{"web":"0.0.0","desktop":"1.5.0"}})"));
	document.insert(u"feature.realtime_v2"_s, false);
	document.insert(u"messaging.edit_window"_s, u"48h"_s);
	document.insert(u"realtime.ping_interval"_s, 25e9); // a Go duration as a number
	document.insert(u"messaging"_s, Test::Json(R"({"max_message_length":2000})"));
	server.handler = [&](const HttpServer::Request &) {
		return HttpServer::Response::Ok(document);
	};
	const auto cachePath = _dir.path() + u"/config/settings.json"_s;
	{
		auto cache = Storage::KeyValue(cachePath);
		auto client = Api::Client(server.base(), u"1"_s);
		auto config = Api::ClientConfig(&client, &cache);
		// Defaults before anything is known.
		QVERIFY(!config.loaded());
		QCOMPARE(config.conversationTopicMaxChars(), 120);
		QCOMPARE(config.conversationCategories().size(), 10);
		QCOMPARE(config.maxMessageLength(), 4096);
		QVERIFY(config.realtimeV2Enabled());
		QCOMPARE(config.minClientVersion(), u"0.0.0"_s);
		auto changes = 0;
		connect(&config, &Api::ClientConfig::changed, this, [&] { ++changes; });
		auto answers = QVector<bool>();
		config.load(this, [&](bool fresh) { answers.push_back(fresh); });
		config.load(this, [&](bool fresh) { answers.push_back(fresh); });
		QVERIFY(Test::WaitFor([&] { return answers.size() == 2; }));
		QCOMPARE(answers, (QVector<bool>{ true, true }));
		QCOMPARE(server.requests.size(), 1); // shared
		QVERIFY(!server.requests[0].headers.contains("authorization"));
		QCOMPARE(changes, 1);
		QCOMPARE(config.conversationDurations(), (QStringList{ u"1h"_s, u"6h"_s, u"24h"_s }));
		QCOMPARE(config.conversationCategories().at(1).slug, u"cinema"_s);
		QCOMPARE(config.conversationCategories().at(1).title, u"سینما"_s);
		QCOMPARE(config.conversationDefaultDuration(), u"24h"_s);
		QCOMPARE(config.minClientVersion(), u"1.5.0"_s);       // nested object
		QCOMPARE(config.minClientVersion(u"web"_s), u"0.0.0"_s);
		QVERIFY(config.updateRequired(u"1.4.9"_s));
		QVERIFY(!config.updateRequired(u"1.5.0"_s));
		QVERIFY(!config.realtimeV2Enabled());                    // dotted key
		QCOMPARE(config.editWindow(), 48 * 3600);
		QCOMPARE(config.realtimePingInterval(), 25);
		QCOMPARE(config.maxMessageLength(), 2000);               // nested path
		QCOMPARE(config.integer(u"nope.nope"_s, 7), 7);
	}
	// The next start has the previous answer before the first response.
	auto cache = Storage::KeyValue(cachePath);
	auto offline = Api::Client(u"http://127.0.0.1:1"_s, u"1"_s);
	auto config = Api::ClientConfig(&offline, &cache);
	QCOMPARE(config.minClientVersion(), u"1.5.0"_s);
	auto answer = std::optional<bool>();
	config.load(this, [&](bool fresh) { answer = fresh; });
	QVERIFY(Test::WaitFor([&] { return answer.has_value(); }));
	QCOMPARE(*answer, false);
	QCOMPARE(config.editWindow(), 48 * 3600);
}

void TestApi::signInBindsDeviceKey() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		if (request.path == "/v1/auth/otp/request") {
			return HttpServer::Response::Ok(Test::Json(R"({"challenge_id":"ch1","code_length":5,"resend_after":0,"expires_in":120})"));
		} else if (request.path == "/v1/auth/otp/verify") {
			if (request.json().value(u"code"_s).toString() != u"12345") {
				return HttpServer::Response::Problem(400, "otp.invalid");
			}
			return HttpServer::Response::Ok(TokensDocument(1, 600));
		} else if (request.path == "/v1/me") {
			return HttpServer::Response::Ok(Test::Fixture("me.json"));
		}
		return HttpServer::Response::Ok();
	};
	auto deviceId = QString();
	auto publicKey = QString();
	{
		auto account = Main::Account(accountConfig(server, u"signin"_s));
		auto sessionChanges = 0;
		connect(&account, &Main::Account::sessionChanged, this, [&] { ++sessionChanges; });
		account.start();
		QVERIFY(!account.signedIn());
		QVERIFY(!account.session());
		deviceId = account.deviceId();
		QCOMPARE(deviceId.size(), 36);

		auto challenge = std::optional<Api::OtpChallenge>();
		account.auth().requestCode(u"+98۹۱۲۱۲۳۴۵۶۷"_s, this, [&](const Api::OtpChallenge &value) { challenge = value; }, nullptr);
		QVERIFY(Test::WaitFor([&] { return challenge.has_value(); }));
		QCOMPARE(challenge->challengeId, u"ch1"_s);
		QCOMPARE(challenge->codeLength, 5);
		QCOMPARE(challenge->resendAfter, 1); // never below a second
		QCOMPARE(challenge->expiresIn, 120);
		const auto requested = server.to("/v1/auth/otp/request").value(0).json();
		QCOMPARE(requested.value(u"phone"_s).toString(), u"+989121234567"_s); // digits latinized
		QCOMPARE(requested.value(u"platform"_s).toString(), u"desktop"_s);
		QCOMPARE(requested.value(u"device_id"_s).toString(), deviceId);
		QVERIFY(!server.to("/v1/auth/otp/request").value(0).headers.contains("authorization"));

		auto error = std::optional<Api::Error>();
		account.auth().verifyCode(u"ch1"_s, u"00000"_s, false, this, nullptr, [&](const Api::Error &value) { error = value; });
		QVERIFY(Test::WaitFor([&] { return error.has_value(); }));
		QCOMPARE(error->code, u"otp.invalid"_s);
		QVERIFY(!account.signedIn());

		auto outcome = std::optional<Api::VerifyOutcome>();
		account.auth().verifyCode(u"ch1"_s, u"۱۲۳۴۵"_s, false, this, [&](const Api::VerifyOutcome &value) { outcome = value; }, nullptr);
		QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
		QVERIFY(outcome->kind == Api::VerifyOutcome::Kind::SignedIn);
		QCOMPARE(outcome->userId, u"u_me"_s);
		QVERIFY(!outcome->needsProfile); // /v1/me has a handle
		const auto verify = server.to("/v1/auth/otp/verify").value(1).json();
		QCOMPARE(verify.value(u"code"_s).toString(), u"12345"_s);
		QCOMPARE(verify.value(u"platform"_s).toString(), u"desktop"_s);
		QCOMPARE(verify.value(u"app_version"_s).toString(), u"9.9.9"_s);
		QVERIFY(!verify.value(u"device_name"_s).toString().isEmpty());
		const auto key = verify.value(u"device_key"_s).toObject();
		QCOMPARE(key.value(u"type"_s).toString(), u"ed25519"_s);
		publicKey = key.value(u"key"_s).toString();
		QCOMPARE(base::bytes::fromBase64(publicKey).size(), 32);
		QCOMPARE(publicKey, account.deviceKey().publicKeyBase64());
		// The failed attempt offered the same key: one identity per install.
		QCOMPARE(server.to("/v1/auth/otp/verify").value(0).json().value(u"device_key"_s).toObject().value(u"key"_s).toString(), publicKey);

		QVERIFY(account.signedIn());
		QCOMPARE(account.userId(), u"u_me"_s);
		QCOMPARE(account.sessionId(), u"s1"_s); // from the token's claims
		QVERIFY(account.session());
		QCOMPARE(account.session()->userId(), u"u_me"_s);
		QCOMPARE(sessionChanges, 1);
		QVERIFY(Test::WaitFor([&] { return account.session()->isReady(); }));
		QVERIFY(QFileInfo(account.session()->userDir()).isDir());
		// Requests of the session carry the token.
		QVERIFY(Test::WaitFor([&] { return server.count("/v1/chats") >= 3; }));
		QVERIFY(server.to("/v1/chats").value(0).header("Authorization").startsWith("Bearer h."));
		// The key of a live session is not replaced.
		account.resetDeviceKey();
		QCOMPARE(account.deviceKey().publicKeyBase64(), publicKey);
	}
	// A restart finds the same identity and session.
	auto account = Main::Account(accountConfig(server, u"signin"_s));
	QVERIFY(account.signedIn());
	QVERIFY(!account.session()); // until start()
	account.start();
	QVERIFY(account.session());
	QCOMPARE(account.deviceId(), deviceId);
	QCOMPARE(account.deviceKey().publicKeyBase64(), publicKey);
	QCOMPARE(account.userId(), u"u_me"_s);
	QCOMPARE(account.sessionId(), u"s1"_s);
	QCOMPARE(Main::Account::JwtClaims(account.currentAccessToken()).value(u"sub"_s).toString(), u"u_me"_s);
	QVERIFY(Main::Account::JwtClaims(u"garbage"_s).isEmpty());
}

void TestApi::twoStepAndProfile() {
	auto server = HttpServer();
	auto handle = QString();
	server.handler = [&](const HttpServer::Request &request) {
		const auto json = request.json();
		if (request.path == "/v1/auth/otp/verify") {
			return HttpServer::Response::Ok(Test::Json(R"({"two_step_required":true,"ticket":"t1","hint":"cat","recovery_email":"h***@x.com","expires_in":60})"));
		} else if (request.path == "/v1/auth/two-step") {
			return (json.value(u"password"_s).toString() == u"secret")
				? HttpServer::Response::Ok(TokensDocument(1, 600))
				: HttpServer::Response::Problem(403, "two_step.password_invalid");
		} else if (request.path == "/v1/auth/two-step/recover") {
			return HttpServer::Response::Ok(Test::Json(R"({"code_length":6,"resend_after":30})"));
		} else if (request.path == "/v1/auth/two-step/recover/confirm") {
			auto tokens = TokensDocument(2, 600);
			tokens.insert(u"is_new_user"_s, true);
			return HttpServer::Response::Ok(tokens);
		} else if (request.path == "/v1/me") {
			return HttpServer::Response::Ok(QJsonObject{ { u"id"_s, u"u_me"_s }, { u"display_name"_s, u"x"_s }, { u"handle"_s, handle } });
		} else if (request.path == "/v1/me/handle") {
			handle = json.value(u"handle"_s).toString();
			return HttpServer::Response::Ok(QJsonObject{ { u"id"_s, u"u_me"_s }, { u"display_name"_s, u"Hadi"_s }, { u"handle"_s, handle } });
		} else if (request.path.startsWith("/v1/handles/")) {
			return HttpServer::Response::Ok(QJsonObject{ { u"available"_s, request.path.contains("free") } });
		}
		return HttpServer::Response::Ok();
	};
	auto account = Main::Account(accountConfig(server, u"twostep"_s));
	account.start();
	auto &auth = account.auth();
	auto outcome = std::optional<Api::VerifyOutcome>();
	const auto store = [&](const Api::VerifyOutcome &value) { outcome = value; };
	auth.verifyCode(u"ch"_s, u"222222"_s, false, this, store, nullptr);
	QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
	QVERIFY(outcome->kind == Api::VerifyOutcome::Kind::TwoStep);
	const auto form = outcome->form;
	QCOMPARE(form.ticket, u"t1"_s);
	QCOMPARE(form.hint, u"cat"_s);
	QCOMPARE(form.recoveryEmail, u"h***@x.com"_s);
	QVERIFY(form.expiresAt > base::unixtime::nowMs() + 50000);
	QVERIFY(!account.signedIn());

	auto error = std::optional<Api::Error>();
	auth.signInWithPassword(form, u"wrong"_s, this, store, [&](const Api::Error &value) { error = value; });
	QVERIFY(Test::WaitFor([&] { return error.has_value(); }));
	QCOMPARE(error->status, 403);
	QVERIFY(!account.signedIn());

	auto recovery = std::optional<Api::RecoveryChallenge>();
	auth.requestRecovery(form, this, [&](const Api::RecoveryChallenge &value) { recovery = value; }, nullptr);
	QVERIFY(Test::WaitFor([&] { return recovery.has_value(); }));
	QCOMPARE(recovery->email, u"h***@x.com"_s); // falls back to the form's address
	QCOMPARE(recovery->resendAfter, 30);

	outcome = std::nullopt;
	auth.signInWithPassword(form, u"secret"_s, this, store, nullptr);
	QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
	QVERIFY(outcome->kind == Api::VerifyOutcome::Kind::SignedIn);
	QVERIFY(outcome->needsProfile); // /v1/me has no handle yet
	QCOMPARE(server.to("/v1/auth/two-step").value(1).json().value(u"ticket"_s).toString(), u"t1"_s);
	QVERIFY(account.signedIn());

	QVERIFY(Api::Auth::IsValidHandle(u"hadi_1.x"_s));
	QVERIFY(!Api::Auth::IsValidHandle(u"Hadi"_s));
	QVERIFY(!Api::Auth::IsValidHandle(u"ab"_s));
	auto available = QVector<bool>();
	auth.isHandleAvailable(u"free"_s, this, [&](bool value) { available.push_back(value); }, nullptr);
	auth.isHandleAvailable(u"taken"_s, this, [&](bool value) { available.push_back(value); }, nullptr);
	QVERIFY(Test::WaitFor([&] { return available.size() == 2; }));
	QVERIFY(available.contains(true) && available.contains(false));

	auto profile = std::optional<Data::Profile>();
	auth.completeProfile(u"Hadi"_s, u"hadi"_s, QString(), this, [&](const Data::Profile &value) { profile = value; }, nullptr);
	QVERIFY(Test::WaitFor([&] { return profile.has_value(); }));
	QCOMPARE(profile->handle, u"hadi"_s);
	const auto patch = server.to("/v1/me/profile").value(0);
	QCOMPARE(patch.method, QByteArray("PATCH"));
	QCOMPARE(patch.json(), Test::Json(R"({"display_name":"Hadi"})"));
	QCOMPARE(server.to("/v1/me/handle").value(0).method, QByteArray("PUT"));

	// Recovery signs in a new account: the profile step follows without asking /v1/me.
	const auto before = server.count("/v1/me");
	outcome = std::nullopt;
	auth.confirmRecovery(form, u"۱۲۳۴۵۶"_s, this, store, nullptr);
	QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
	QVERIFY(outcome->needsProfile);
	QCOMPARE(server.to("/v1/auth/two-step/recover/confirm").value(0).json().value(u"code"_s).toString(), u"123456"_s);
	Test::Settle();
	Q_UNUSED(before);
}

void TestApi::refreshProvesDeviceKeyAndIsShared() {
	auto server = HttpServer();
	auto generation = 1;
	server.handler = [&](const HttpServer::Request &request) {
		if (request.path == "/v1/auth/otp/verify") {
			return HttpServer::Response::Ok(TokensDocument(1, -5)); // already expired
		} else if (request.path == "/v1/auth/token/refresh") {
			return HttpServer::Response::Ok(TokensDocument(++generation, 600));
		}
		return HttpServer::Response::Ok();
	};
	auto account = Main::Account(accountConfig(server, u"refresh"_s));
	account.start();
	auto outcome = std::optional<Api::VerifyOutcome>();
	account.auth().verifyCode(u"ch"_s, u"1"_s, false, this, [&](const Api::VerifyOutcome &value) { outcome = value; }, nullptr);
	QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
	QVERIFY(Test::WaitFor([&] { return account.session() && account.session()->isReady(); }));
	QVERIFY(Test::WaitFor([&] { return server.count("/v1/notifications/unread") >= 1; }));

	// Everything the session asked at start shared one refresh.
	const auto refreshes = server.to("/v1/auth/token/refresh");
	QCOMPARE(refreshes.size(), 1);
	const auto body = refreshes[0].json();
	QCOMPARE(body.value(u"refresh_token"_s).toString(), u"refresh-1"_s);
	const auto nonce = body.value(u"device_nonce"_s).toString();
	QCOMPARE(base::bytes::fromBase64(nonce), QByteArray("nonce-1"));
	// The signature is the device key's over "eleza/device-proof/v1\0refresh\0<nonce>".
	QVERIFY(Crypto::DeviceKey::Verify(
		account.deviceKey().publicKey(),
		u"refresh"_s,
		QByteArray("nonce-1"),
		base::bytes::fromBase64(body.value(u"signature"_s).toString())));
	QVERIFY(!refreshes[0].headers.contains("authorization"));
	QVERIFY(!refreshes[0].headers.contains("idempotency-key"));
	QCOMPARE(refreshes[0].header("X-Request-Id").size(), 36);
	for (const auto &request : server.to("/v1/chats")) {
		QVERIFY(request.header("Authorization").endsWith(".s2")); // the refreshed token
	}

	// Fresh enough: no further refresh; many callers, one token.
	auto tokens = QStringList();
	for (auto i = 0; i != 5; ++i) {
		account.accessToken([&](QString token) { tokens.push_back(token); });
	}
	QCOMPARE(tokens.size(), 5);
	QCOMPARE(server.count("/v1/auth/token/refresh"), 1);

	// A request rejected with the current token forces one, shared by concurrent callers.
	auto answers = QVector<bool>();
	const auto current = account.currentAccessToken();
	for (auto i = 0; i != 4; ++i) {
		account.refreshAfterUnauthorized(current, [&](bool ok) { answers.push_back(ok); });
	}
	QVERIFY(Test::WaitFor([&] { return answers.size() == 4; }));
	QCOMPARE(answers.count(true), 4);
	QCOMPARE(server.count("/v1/auth/token/refresh"), 2);
	QCOMPARE(server.to("/v1/auth/token/refresh").value(1).json().value(u"refresh_token"_s).toString(), u"refresh-2"_s);
	QVERIFY(account.currentAccessToken() != current);
	// A stale failure (the token already moved on) costs nothing.
	account.refreshAfterUnauthorized(current, [&](bool ok) { answers.push_back(ok); });
	QCOMPARE(answers.size(), 5);
	QCOMPARE(server.count("/v1/auth/token/refresh"), 2);
}

void TestApi::refreshReissuesNonceOnce() {
	auto server = HttpServer();
	auto refreshCalls = 0;
	auto nonceCalls = 0;
	server.handler = [&](const HttpServer::Request &request) {
		if (request.path == "/v1/auth/otp/verify") {
			return HttpServer::Response::Ok(TokensDocument(1, 600));
		} else if (request.path == "/v1/auth/token/refresh") {
			++refreshCalls;
			if (base::bytes::fromBase64(request.json().value(u"device_nonce"_s).toString()) != "reissued") {
				return HttpServer::Response::Problem(401, "auth.device_nonce_invalid");
			}
			return HttpServer::Response::Ok(TokensDocument(5, 600));
		} else if (request.path == "/v1/auth/nonce") {
			++nonceCalls;
			return HttpServer::Response::Ok(QJsonObject{ { u"device_nonce"_s, base::bytes::toBase64(QByteArray("reissued")) } });
		}
		return HttpServer::Response::Ok();
	};
	auto account = Main::Account(accountConfig(server, u"nonce"_s));
	account.start();
	auto outcome = std::optional<Api::VerifyOutcome>();
	account.auth().verifyCode(u"ch"_s, u"1"_s, false, this, [&](const Api::VerifyOutcome &value) { outcome = value; }, nullptr);
	QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
	auto answer = std::optional<bool>();
	account.refreshAfterUnauthorized(account.currentAccessToken(), [&](bool ok) { answer = ok; });
	QVERIFY(Test::WaitFor([&] { return answer.has_value(); }));
	QVERIFY(*answer);
	QCOMPARE(refreshCalls, 2);
	QCOMPARE(nonceCalls, 1);
	const auto nonce = server.to("/v1/auth/nonce").value(0);
	QCOMPARE(nonce.json().value(u"refresh_token"_s).toString(), u"refresh-1"_s);
	QCOMPARE(nonce.header("Idempotency-Key").size(), 36);
	// The second attempt signed the re-issued nonce.
	const auto second = server.to("/v1/auth/token/refresh").value(1).json();
	QVERIFY(Crypto::DeviceKey::Verify(
		account.deviceKey().publicKey(),
		u"refresh"_s,
		QByteArray("reissued"),
		base::bytes::fromBase64(second.value(u"signature"_s).toString())));
	QVERIFY(account.signedIn());
	QVERIFY(account.currentAccessToken().endsWith(u".s5"));
}

void TestApi::refreshFailuresKeepOrEndTheSession() {
	auto server = HttpServer();
	auto mode = QByteArray("500");
	server.handler = [&](const HttpServer::Request &request) {
		if (request.path == "/v1/auth/otp/verify") {
			return HttpServer::Response::Ok(TokensDocument(1, 600));
		} else if (request.path == "/v1/auth/token/refresh") {
			if (mode == "500") {
				return HttpServer::Response::Problem(500, "internal");
			} else if (mode == "401-bare") {
				auto response = HttpServer::Response();
				response.status = 401; // a proxy's 401 without the API's problem body
				return response;
			} else if (mode == "proof") {
				return HttpServer::Response::Problem(401, "auth.device_proof_invalid");
			}
			return HttpServer::Response::Problem(401, mode);
		} else if (request.path == "/v1/auth/nonce") {
			return HttpServer::Response::Problem(401, "auth.refresh_invalid");
		}
		return HttpServer::Response::Ok();
	};
	const auto signIn = [&](Main::Account &account) {
		auto outcome = std::optional<Api::VerifyOutcome>();
		account.auth().verifyCode(u"ch"_s, u"1"_s, false, this, [&](const Api::VerifyOutcome &value) { outcome = value; }, nullptr);
		return Test::WaitFor([&] { return outcome.has_value(); }) && account.signedIn();
	};
	const auto refresh = [&](Main::Account &account) {
		auto answer = std::optional<bool>();
		account.refreshAfterUnauthorized(account.currentAccessToken(), [&](bool ok) { answer = ok; });
		const auto finished = Test::WaitFor([&] { return answer.has_value(); });
		Q_ASSERT(finished);
		Q_UNUSED(finished);
		return *answer;
	};
	auto account = Main::Account(accountConfig(server, u"failures"_s));
	auto ended = QVector<Main::SessionEndReason>();
	connect(&account, &Main::Account::sessionEnded, this, [&](Main::SessionEndReason reason) { ended.push_back(reason); });
	account.start();
	QVERIFY(signIn(account));
	const auto userDir = account.session()->userDir();
	QVERIFY(Test::WaitFor([&] { return account.session()->isReady(); }));

	// A server error keeps the session and backs off: the next attempt does
	// not even reach the server until the delay passed.
	QVERIFY(!refresh(account));
	QVERIFY(account.signedIn());
	QCOMPARE(server.count("/v1/auth/token/refresh"), 1);
	QVERIFY(!refresh(account));
	QCOMPARE(server.count("/v1/auth/token/refresh"), 1);
	base::unixtime::setTestShiftMs(40 * 1000);
	// A 401 that is not the API's own (no problem code) keeps it too.
	mode = "401-bare";
	QVERIFY(!refresh(account));
	QVERIFY(account.signedIn());
	QCOMPARE(server.count("/v1/auth/token/refresh"), 2);
	QVERIFY(ended.isEmpty());

	// The refresh token is no longer accepted: the session ends, the cache stays.
	base::unixtime::setTestShiftMs(120 * 1000);
	mode = "auth.refresh_invalid";
	QVERIFY(!refresh(account));
	QVERIFY(!account.signedIn());
	QVERIFY(!account.session());
	QCOMPARE(ended, (QVector<Main::SessionEndReason>{ Main::SessionEndReason::Expired }));
	QVERIFY(QFileInfo(userDir).isDir());
	QVERIFY(account.currentAccessToken().isEmpty());

	// Ended on purpose elsewhere: the local data is wiped.
	base::unixtime::setTestShiftMs(0);
	// What an earlier, different user left behind (an expired session keeps
	// its cache) does not outlive the next sign-in of somebody else.
	const auto stranger = QFileInfo(userDir).absolutePath() + u"/user-someone_else"_s;
	QVERIFY(QDir().mkpath(stranger + u"/media"_s));
	QVERIFY(base::platform::WriteFileAtomic(stranger + u"/cache.db"_s, "their messages", true));
	account.settings().set(u"eli.draft.someone_else"_s, QJsonObject{ { u"text"_s, u"theirs"_s } });
	QVERIFY(signIn(account));
	QVERIFY(Test::WaitFor([&] { return account.session()->isReady(); }));
	QVERIFY(!QFileInfo::exists(stranger));
	QVERIFY(!account.settings().contains(u"eli.draft.someone_else"_s));
	QVERIFY(QFileInfo(userDir).isDir());
	mode = "auth.session_revoked";
	QVERIFY(!refresh(account));
	QCOMPARE(ended.value(1), Main::SessionEndReason::Revoked);
	QVERIFY(!QFileInfo::exists(userDir));

	// A proof problem whose nonce cannot be re-issued (401 with a code) ends it as well.
	QVERIFY(signIn(account));
	mode = "proof";
	QVERIFY(!refresh(account));
	QCOMPARE(ended.size(), 3);
	QCOMPARE(ended.value(2), Main::SessionEndReason::Expired);
	QCOMPARE(server.count("/v1/auth/nonce"), 1);
	// Nothing is left on disk to sign in with.
	auto reopened = Main::Account(accountConfig(server, u"failures"_s));
	QVERIFY(!reopened.signedIn());
}

void TestApi::logoutWipes() {
	auto server = HttpServer();
	server.handler = [](const HttpServer::Request &request) {
		if (request.path == "/v1/auth/otp/verify") {
			return HttpServer::Response::Ok(TokensDocument(1, 600));
		} else if (request.path == "/v1/auth/logout") {
			return HttpServer::Response::Problem(500, "internal"); // best effort
		}
		return HttpServer::Response::Ok();
	};
	auto account = Main::Account(accountConfig(server, u"logout"_s));
	account.start();
	auto outcome = std::optional<Api::VerifyOutcome>();
	account.auth().verifyCode(u"ch"_s, u"1"_s, false, this, [&](const Api::VerifyOutcome &value) { outcome = value; }, nullptr);
	QVERIFY(Test::WaitFor([&] { return outcome.has_value(); }));
	const auto userDir = account.session()->userDir();
	QVERIFY(Test::WaitFor([&] { return account.session()->isReady(); }));
	const auto publicKey = account.deviceKey().publicKeyBase64();
	// What the screens keep per user next to the account goes with the user.
	const auto userId = account.session()->userId();
	account.settings().set(u"eli.draft."_s + userId, QJsonObject{ { u"text"_s, u"an unpublished post"_s } });
	account.settings().set(u"dialogs.pinned_order."_s + userId, QJsonArray{ u"c1"_s });
	account.settings().set(u"eli.draft.someone_else"_s, QJsonObject{ { u"text"_s, u"kept"_s } });
	account.settings().flush();
	const auto settingsPath = QFileInfo(userDir).absolutePath() + u"/settings.json"_s;
	QVERIFY(base::platform::ReadFile(settingsPath).value_or(QByteArray()).contains("an unpublished post"));
	auto ended = QVector<Main::SessionEndReason>();
	auto order = QStringList();
	connect(&account, &Main::Account::sessionChanged, this, [&] { order.push_back(account.session() ? u"session"_s : u"no session"_s); });
	connect(&account, &Main::Account::sessionEnded, this, [&](Main::SessionEndReason reason) {
		ended.push_back(reason);
		order.push_back(u"ended"_s);
	});
	auto done = false;
	account.logout(this, [&] { done = true; });
	QVERIFY(Test::WaitFor([&] { return done; }));
	QCOMPARE(server.count("/v1/auth/logout"), 1);
	QVERIFY(server.to("/v1/auth/logout").value(0).header("Authorization").startsWith("Bearer "));
	QCOMPARE(ended, (QVector<Main::SessionEndReason>{ Main::SessionEndReason::SignedOut }));
	QCOMPARE(order, (QStringList{ u"no session"_s, u"ended"_s }));
	QVERIFY(!account.signedIn());
	QVERIFY(!QFileInfo::exists(userDir));
	QVERIFY(!account.settings().contains(u"eli.draft."_s + userId));
	QVERIFY(!account.settings().contains(u"dialogs.pinned_order."_s + userId));
	QVERIFY(account.settings().contains(u"eli.draft.someone_else"_s));
	// Already on disk: not after the next turn of the event loop.
	const auto stored = base::platform::ReadFile(settingsPath).value_or(QByteArray());
	QVERIFY(!stored.contains("an unpublished post"));
	QVERIFY(!stored.contains(userId.toUtf8()));
	// The device identity stays; a fresh sign-in may ask for a new key.
	QCOMPARE(account.deviceKey().publicKeyBase64(), publicKey);
	account.resetDeviceKey();
	QVERIFY(account.deviceKey().publicKeyBase64() != publicKey);
	// Signing out twice is harmless.
	done = false;
	account.logout(this, [&] { done = true; });
	QVERIFY(Test::WaitFor([&] { return done; }));
	QCOMPARE(server.count("/v1/auth/logout"), 1);
}

void TestApi::onlyWebAddressesAreReached() {
	// Where the tokens may go at all.
	QVERIFY(Api::Client::IsAcceptableBase(u"https://api.eleza.me"_s));
	QVERIFY(Api::Client::IsAcceptableBase(u"http://127.0.0.1:8000"_s));
	QVERIFY(Api::Client::IsAcceptableBase(u"http://localhost:8000/"_s));
	QVERIFY(Api::Client::IsAcceptableBase(u"http://[::1]:8000"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"http://api.eleza.me"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"http://192.168.1.10:8000"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"http://127.0.0.1.evil.example"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"http://localhost.evil.example"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"https://user:pass@api.eleza.me"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"file:///tmp/api"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"ftp://api.eleza.me"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(u"api.eleza.me"_s));
	QVERIFY(!Api::Client::IsAcceptableBase(QString()));
	QVERIFY(Api::Client::IsAcceptableBase(u"http://192.168.1.10:8000"_s, true));
	QVERIFY(!Api::Client::IsAcceptableBase(u"file:///tmp/api"_s, true));

	// A local file the network manager would happily "download".
	const auto secret = _dir.path() + u"/secret.txt"_s;
	{
		auto file = QFile(secret);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("top secret");
	}
	const auto secretUrl = QUrl::fromLocalFile(secret).toString();
	auto server = HttpServer();
	server.handler = [&](const HttpServer::Request &request) {
		if (request.path == "/v1/to-file") {
			return HttpServer::Response::Redirect(secretUrl.toLatin1());
		} else if (request.path == "/v1/to-qrc") {
			return HttpServer::Response::Redirect("qrc:/anything");
		} else if (request.path == "/v1/to-ftp") {
			return HttpServer::Response::Redirect("ftp://127.0.0.1/x");
		} else if (request.path == "/v1/to-data") {
			return HttpServer::Response::Redirect("data:text/plain,hello");
		}
		return HttpServer::Response::Ok();
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);

	auto errors = QVector<Api::Error>();
	auto results = 0;
	const auto fail = [&](const Api::Error &error) { errors.push_back(error); };
	const auto done = [&](const Api::DownloadResult &) { ++results; };
	const auto targets = QStringList{
		secretUrl,
		u"qrc:/anything"_s,
		u"data:text/plain,hello"_s,
		u"ftp://127.0.0.1/x"_s,
		u"javascript:alert(1)"_s,
		u"https:///no-host"_s,
		u"nonsense"_s,
	};
	for (const auto &target : targets) {
		client.download(target).unauthenticated().done(done).fail(fail).send();
	}
	QVERIFY(Test::WaitFor([&] { return errors.size() == targets.size(); }));
	for (const auto &error : errors) {
		QCOMPARE(error.code, u"url"_s);
	}
	errors.clear();
	const auto redirects = QStringList{ u"/v1/to-file"_s, u"/v1/to-qrc"_s, u"/v1/to-ftp"_s, u"/v1/to-data"_s };
	for (const auto &target : redirects) {
		client.download(target).done(done).fail(fail).send();
	}
	QVERIFY(Test::WaitFor([&] { return errors.size() == redirects.size(); }));
	for (const auto &error : errors) {
		QCOMPARE(error.code, u"redirect"_s);
	}
	QCOMPARE(results, 0);

	// A "presigned" address that would overwrite a local file.
	const auto victim = _dir.path() + u"/victim.txt"_s;
	errors.clear();
	auto uploaded = false;
	client.upload(QUrl::fromLocalFile(victim))
		.data("overwritten")
		.done([&] { uploaded = true; })
		.fail(fail)
		.send();
	client.upload(QUrl(u"ftp://127.0.0.1/x"_s)).data("x").done([&] { uploaded = true; }).fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 2; }));
	QCOMPARE(errors[0].code, u"url"_s);
	QCOMPARE(errors[1].code, u"url"_s);
	QVERIFY(!uploaded);
	QVERIFY(!QFileInfo::exists(victim));
}

void TestApi::oversizedJsonIsRefused() {
	auto server = HttpServer();
	// 33 MiB of valid JSON: more than any answer of ours, cut off on the way.
	const auto huge = "{\"x\":\"" + QByteArray(33 << 20, 'a') + "\"}";
	server.handler = [&](const HttpServer::Request &request) {
		return (request.path == "/v1/huge")
			? HttpServer::Response::Bytes(huge, "application/json")
			: HttpServer::Response::Ok(QJsonObject{ { u"fine"_s, true } });
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);
	const auto outcome = Send(client.request(Api::Get, u"/v1/huge"_s));
	QVERIFY(Test::WaitFor([&] { return outcome->finished(); }, 30000));
	QVERIFY(outcome->failed);
	QCOMPARE(outcome->error.code, u"decoding"_s);
	// The client goes on working.
	const auto next = Send(client.request(Api::Get, u"/v1/fine"_s));
	QVERIFY(Test::WaitFor([&] { return next->finished(); }));
	QVERIFY(next->done);
	QVERIFY(next->json.value(u"fine"_s).toBool());
}

void TestApi::abortAuthenticatedStopsRetries() {
	// A request of the user who signs out that waits for a retry (or for a
	// refreshed token) must not go out again with the next user's token.
	auto server = HttpServer();
	server.handler = [&](const HttpServer::Request &request) {
		if (request.path == "/v1/busy") {
			auto response = HttpServer::Response::Problem(503, "server.overloaded");
			response.headers.push_back({ "Retry-After", "1" });
			return response;
		} else if (request.path == "/v1/hang") {
			auto response = HttpServer::Response();
			response.hang = true;
			return response;
		}
		return HttpServer::Response::Ok(QJsonObject{ { u"ok"_s, true } });
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	tokens.token = u"token-A"_s;
	client.setTokenProvider(&tokens);

	const auto busy = Send(client.request(Api::Post, u"/v1/busy"_s).body(QJsonObject{ { u"text"_s, u"from A"_s } }));
	const auto hanging = Send(client.request(Api::Post, u"/v1/hang"_s));
	auto downloaded = false;
	client.download(u"/v1/hang"_s).done([&](const Api::DownloadResult &) { downloaded = true; }).send();
	const auto open = Send(client.request(Api::Get, u"/v1/public"_s).unauthenticated());
	QVERIFY(Test::WaitFor([&] { return server.count("/v1/busy") == 1 && server.count("/v1/hang") == 2; }));

	client.abortAuthenticated();
	tokens.token = u"token-B"_s; // the next user
	QTest::qWait(1600); // past Retry-After
	QCOMPARE(server.count("/v1/busy"), 1);
	QVERIFY(!busy->finished());
	QVERIFY(!hanging->finished());
	QVERIFY(!downloaded);
	for (const auto &request : server.requests) {
		QVERIFY(request.header("Authorization") != "Bearer token-B");
	}
	// Requests that carry no token are not touched, and new ones use the new token.
	QVERIFY(Test::WaitFor([&] { return open->finished(); }));
	QVERIFY(open->done);
	const auto fresh = Send(client.request(Api::Get, u"/v1/me"_s));
	QVERIFY(Test::WaitFor([&] { return fresh->finished(); }));
	QCOMPARE(server.to("/v1/me").value(0).header("Authorization"), QByteArray("Bearer token-B"));
}

void TestApi::errorBodiesAreBounded() {
	// The body of an error answer to a download is not ours to buffer without
	// a limit: the problem document at its start is used, the rest dropped.
	auto server = HttpServer();
	server.handler = [&](const HttpServer::Request &request) {
		auto response = HttpServer::Response();
		if (request.path == "/v1/problem") {
			response = HttpServer::Response::Problem(404, "media.not_found");
		} else if (request.path == "/v1/flood") {
			response.status = 500;
			response.contentType = "text/html";
			response.body = QByteArray(6 << 20, 'x');
		} else if (request.path == "/put-flood") {
			response.status = 403;
			response.contentType = "application/xml";
			response.body = QByteArray(6 << 20, 'y');
		}
		return response;
	};
	auto client = Api::Client(server.base(), u"1"_s);
	auto tokens = Tokens();
	client.setTokenProvider(&tokens);
	auto errors = QVector<Api::Error>();
	const auto fail = [&](const Api::Error &error) { errors.push_back(error); };
	client.download(u"/v1/problem"_s).fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 1; }));
	QCOMPARE(errors[0].status, 404);
	QCOMPARE(errors[0].code, u"media.not_found"_s);
	client.download(u"/v1/flood"_s).fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 2; }, 20000));
	QCOMPARE(errors[1].status, 500);
	client.upload(QUrl(server.base() + u"/put-flood"_s)).data("part").fail(fail).send();
	QVERIFY(Test::WaitFor([&] { return errors.size() == 3; }, 20000));
	QCOMPARE(errors[2].status, 403);
	QCOMPARE(errors[2].code, u"upload.part"_s);
}

QTEST_GUILESS_MAIN(TestApi)

#include "test_api.moc"
