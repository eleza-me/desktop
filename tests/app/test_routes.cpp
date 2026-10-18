// The canonical routes (window/window_routes.h): parsing, building, web links.
#include "window/window_routes.h"

#include <QtTest/QtTest>

using namespace Window;

class TestRoutes : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

private Q_SLOTS:
	void tabs() {
		QCOMPARE(ParseRoute("chats").type, RouteType::Chats);
		QCOMPARE(ParseRoute("/eli/").type, RouteType::Eli);
		QCOMPARE(ParseRoute("search").type, RouteType::Search);
		QCOMPARE(ParseRoute("me").type, RouteType::Me);
		QCOMPARE(ParseRoute("settings").type, RouteType::Settings);
		QVERIFY(ParseRoute("settings").id.isEmpty());
		QCOMPARE(ParseRoute("settings/privacy").id, QStringLiteral("privacy"));
	}

	void chat() {
		const auto plain = ParseRoute("chat/c1");
		QCOMPARE(plain.type, RouteType::Chat);
		QCOMPARE(plain.id, QStringLiteral("c1"));
		QCOMPARE(plain.seq(), qint64(0));
		const auto at = ParseRoute("chat/c1?seq=42");
		QCOMPARE(at.seq(), qint64(42));
		QCOMPARE(at.toString(), QStringLiteral("chat/c1?seq=42"));
		QCOMPARE(Routes::Chat("c1", 42), QStringLiteral("chat/c1?seq=42"));
		QCOMPARE(Routes::Chat("c1"), QStringLiteral("chat/c1"));
		QCOMPARE(ParseRoute("chat/c1?seq=-3").seq(), qint64(0));
		QVERIFY(!ParseRoute("chat").valid());
		QVERIFY(!ParseRoute("chat/").valid());
		QVERIFY(!ParseRoute("chat/a/b").valid());
	}

	void people() {
		QCOMPARE(ParseRoute("user/u_sara").type, RouteType::User);
		QCOMPARE(ParseRoute("user/u_sara").id, QStringLiteral("u_sara"));
		const auto handle = ParseRoute("u/@Sara");
		QCOMPARE(handle.type, RouteType::Handle);
		QCOMPARE(handle.id, QStringLiteral("sara"));
		QCOMPARE(Routes::Handle("@Ali.R"), QStringLiteral("u/ali.r"));
		QCOMPARE(Routes::User("u1"), QStringLiteral("user/u1"));
	}

	void eli() {
		QCOMPARE(ParseRoute("eli/post/p1").type, RouteType::Post);
		QCOMPARE(ParseRoute("eli/post/p1").id, QStringLiteral("p1"));
		QCOMPARE(ParseRoute("eli/notifications").type, RouteType::EliNotifications);
		const auto topic = ParseRoute(Routes::Topic(U("#تهران")));
		QCOMPARE(topic.type, RouteType::Topic);
		QCOMPARE(topic.id, U("تهران"));
		// Non-ASCII ids survive a round trip through the string form.
		QCOMPARE(ParseRoute(topic.toString()), topic);
		QVERIFY(!ParseRoute("eli/post").valid());
		QVERIFY(!ParseRoute("eli/unknown/x").valid());
	}

	void searchAndConversations() {
		QCOMPARE(Routes::Search(), QStringLiteral("search"));
		const auto search = ParseRoute(Routes::Search(U("سلام دنیا")));
		QCOMPARE(search.type, RouteType::Search);
		QCOMPARE(search.queryValue("q"), U("سلام دنیا"));
		QCOMPARE(ParseRoute("search?q=a+b").queryValue("q"), QStringLiteral("a b"));
		QCOMPARE(ParseRoute("conversation/topics").type, RouteType::ConversationTopics);
		const auto conversation = ParseRoute("conversation/conv1");
		QCOMPARE(conversation.type, RouteType::Conversation);
		QCOMPARE(conversation.id, QStringLiteral("conv1"));
	}

	void unknown() {
		QVERIFY(!ParseRoute("").valid());
		QVERIFY(!ParseRoute("debug/main").valid());
		QVERIFY(!ParseRoute("nothing/here").valid());
		QVERIFY(Route().toString().isEmpty());
	}

	void webPaths() {
		QCOMPARE(RouteFromWebPath("/chats/c1")->toString(), QStringLiteral("chat/c1"));
		QCOMPARE(RouteFromWebPath("/chats")->type, RouteType::Chats);
		QCOMPARE(RouteFromWebPath("/chats/archived")->type, RouteType::Chats);
		QCOMPARE(RouteFromWebPath("/u/Sara")->toString(), QStringLiteral("u/sara"));
		QCOMPARE(RouteFromWebPath("/u/sara/followers")->toString(), QStringLiteral("u/sara"));
		QCOMPARE(RouteFromWebPath("/@Sara")->toString(), QStringLiteral("u/sara"));
		QCOMPARE(RouteFromWebPath("/@ali.r/")->toString(), QStringLiteral("u/ali.r"));
		QVERIFY(!RouteFromWebPath("/@").has_value());
		QCOMPARE(RouteFromWebPath("/p/p1")->toString(), QStringLiteral("eli/post/p1"));
		QCOMPARE(RouteFromWebPath("/s/abcp1")->toString(), QStringLiteral("eli/share/abcp1"));
		QCOMPARE(ParseRoute("eli/share/abcp1").type, RouteType::SharedPost);
		QCOMPARE(Routes::SharedPost("x1"), QStringLiteral("eli/share/x1"));
		QVERIFY(!ParseRoute("eli/share").valid());

		// Groups: the info screen, «new group» and invite links. The chat
		// of a group is an ordinary chat route.
		QCOMPARE(ParseRoute("group/j1team").type, RouteType::Group);
		QCOMPARE(ParseRoute("group/j1team").id, QStringLiteral("j1team"));
		QCOMPARE(ParseRoute("group/new").type, RouteType::NewGroup);
		QCOMPARE(Routes::NewGroup(), QStringLiteral("group/new"));
		QCOMPARE(Routes::Group("j1team"), QStringLiteral("group/j1team"));
		QVERIFY(!ParseRoute("group").valid());
		const auto token = QStringLiteral("j1team.AAAAAAAAAAAAAAAAAAAAAA");
		QCOMPARE(RouteFromWebPath("/g/" + token)->type, RouteType::GroupInvite);
		QCOMPARE(RouteFromWebPath("/g/" + token)->id, token);
		QCOMPARE(RouteFromWebPath("/g/" + token + "/")->id, token);
		QCOMPARE(Routes::GroupInvite(token), QStringLiteral("g/") + token);
		QCOMPARE(ParseRoute(Routes::GroupInvite(token)).id, token);
		QVERIFY(!RouteFromWebPath("/g").has_value());
		QVERIFY(!ParseRoute("g").valid());
		QCOMPARE(RouteFromWebPath("/eli/post/p1")->toString(), QStringLiteral("eli/post/p1"));
		QCOMPARE(RouteFromWebPath("/eli/thread/p1")->toString(), QStringLiteral("eli/post/p1"));
		QCOMPARE(
			RouteFromWebPath("/eli/notifications/requests")->type,
			RouteType::EliNotifications);
		QCOMPARE(RouteFromWebPath("/settings/privacy")->id, QStringLiteral("privacy"));
		QCOMPARE(RouteFromWebPath("/search", "q=x")->queryValue("q"), QStringLiteral("x"));
		QCOMPARE(RouteFromWebPath("/")->type, RouteType::Eli);
		// Pages without a screen in the desktop app.
		QVERIFY(!RouteFromWebPath("/terms").has_value());
		QVERIFY(!RouteFromWebPath("/s").has_value());
		QVERIFY(!RouteFromWebPath("/p").has_value());
		QVERIFY(!RouteFromWebPath("/eli/sound/x").has_value());
	}

	void botDeepLinks() {
		// eleza.me/<bot>?start=<payload>: the handle route keeps the payload.
		const auto link = RouteFromWebPath("/Weather_Bot", "start=promo42");
		QVERIFY(link.has_value());
		QCOMPARE(link->type, RouteType::Handle);
		QCOMPARE(link->id, QStringLiteral("weather_bot"));
		QCOMPARE(link->botStart(), std::optional<QString>(QStringLiteral("promo42")));
		QCOMPARE(link->toString(), QStringLiteral("u/weather_bot?start=promo42"));
		// An empty payload is still a start.
		QCOMPARE(
			RouteFromWebPath("/weather_bot", "start=")->botStart(),
			std::optional<QString>(QString()));
		QCOMPARE(
			RouteFromWebPath("/u/weather_bot", "start=x")->botStart(),
			std::optional<QString>(QStringLiteral("x")));
		QCOMPARE(
			RouteFromWebPath("/@weather_bot", "start=x")->botStart(),
			std::optional<QString>(QStringLiteral("x")));
		// Other parameters are not kept; a bare handle is a profile.
		QCOMPARE(RouteFromWebPath("/sara", "utm=1")->toString(), QStringLiteral("u/sara"));
		QVERIFY(!RouteFromWebPath("/sara")->botStart().has_value());
		// Pages of the site are never handles.
		QVERIFY(!RouteFromWebPath("/terms", "start=x").has_value());
		QVERIFY(!RouteFromWebPath("/FAQ").has_value());
		QVERIFY(!RouteFromWebPath("/a").has_value());
		QVERIFY(!RouteFromWebPath("/not a handle").has_value());

		// The builders.
		QCOMPARE(
			Routes::HandleStart(QStringLiteral("@Weather_Bot"), QStringLiteral("p-1")),
			QStringLiteral("u/weather_bot?start=p-1"));
		QCOMPARE(
			Routes::BotStart(QStringLiteral("c1"), QStringLiteral("p_1")),
			QStringLiteral("chat/c1?start=p_1"));
		const auto chat = ParseRoute(Routes::BotStart(QStringLiteral("c1"), QString()));
		QCOMPARE(chat.type, RouteType::Chat);
		QCOMPARE(chat.botStart(), std::optional<QString>(QString()));
		QVERIFY(!ParseRoute("chat/c1").botStart().has_value());
		// Only chats and handles start a bot.
		QVERIFY(!ParseRoute("eli/post/p1?start=x").botStart().has_value());
	}
};

QTEST_GUILESS_MAIN(TestRoutes)
#include "test_routes.moc"
