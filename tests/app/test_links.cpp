// Support::ParseLink / DisplayUrl: what a link in somebody's text may do.
#include "support/links.h"

#include <QtTest/QtTest>

using Support::ParsedLink;
using Support::ParseLink;

class TestLinks : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

private Q_SLOTS:
	void onlyWebMailAndPhoneLeaveTheApp() {
		const auto refused = {
			"file:///etc/passwd",
			"FILE:///C:/Windows/system32/calc.exe",
			"javascript:alert(1)",
			"data:text/html,<script>1</script>",
			"vbscript:msgbox(1)",
			"smb://evil.example/share",
			"ftp://evil.example/x",
			"ms-msdt:/id PCWDiagnostic",
			"search-ms:query=x",
			"tg://resolve?domain=x",
			"qrc:/gui/art/icon256.png",
			"about:blank",
			"https://",
			"http:///path",
			"eleza://debug/session-expired",
			"eleza://debug/logout",
			"eleza://debug/viewer?file=/etc/passwd",
			"eleza://intro/phone",
			"eleza://nothing/here",
		};
		for (const auto link : refused) {
			QVERIFY2(
				ParseLink(QString::fromLatin1(link)).type == ParsedLink::Type::Invalid,
				link);
		}
	}

	void lookAlikeHostsAreForeign() {
		const auto foreign = {
			"https://eleza.me.evil.example/u/sara",
			"https://evil-eleza.me/u/sara",
			"https://xeleza.me/",
			"https://eleza.me@evil.example/u/sara",
			"https://eleza.me:pass@evil.example/",
			"https://evil.example/eleza.me/u/sara",
			"https://evil.example#@eleza.me/",
			"https://eleza.me.",
			"https://eleza.me%2eevil.example/",
			"https://eleza.me%40evil.example/",
			// Other names under eleza.me are not ours to vouch for.
			"https://app.eleza.me/u/sara",
			"https://api.eleza.me/v1/me",
			"https://cdn.eleza.me/x.png",
			"https://status.eleza.me/",
			"https://evil.web.eleza.me/",
			"https://www.web.eleza.me/",
			"https://web.eleza.me.evil.example/",
			"https://xweb.eleza.me/",
			"https://wwww.eleza.me/",
			"https://.eleza.me/",
		};
		for (const auto link : foreign) {
			const auto parsed = ParseLink(QString::fromLatin1(link));
			QVERIFY2(
				parsed.type == ParsedLink::Type::External
					|| parsed.type == ParsedLink::Type::Invalid,
				link);
			if (parsed.type == ParsedLink::Type::External) {
				QVERIFY2(!Support::IsElezaHost(parsed.url.host()), link);
			}
		}
		// Cyrillic "е" in the name: not our host, and shown in its ASCII form.
		const auto homograph = ParseLink(U("https://\xd0\xb5leza.me/u/sara"));
		QCOMPARE(homograph.type, ParsedLink::Type::External);
		QVERIFY(Support::DisplayUrl(homograph.url).contains(QStringLiteral("xn--")));

		// Qt reads the part before "\@" as user info (a browser would read a
		// path): what is opened is the address as Qt read it, without the user
		// info, so both agree on our own host.
		const auto slash = ParseLink(QStringLiteral("https://evil.example\\@eleza.me/terms"));
		if (slash.type != ParsedLink::Type::Invalid) {
			QVERIFY(slash.url.userInfo().isEmpty());
			QCOMPARE(
				Support::IsElezaHost(slash.url.host()),
				slash.type != ParsedLink::Type::External);
			if (Support::IsElezaHost(slash.url.host())) {
				QVERIFY(!slash.url.toString(QUrl::FullyEncoded).contains(QStringLiteral("evil")));
			}
		}

		QVERIFY(Support::IsElezaHost(QStringLiteral("eleza.me")));
		QVERIFY(Support::IsElezaHost(QStringLiteral("www.eleza.me")));
		QVERIFY(Support::IsElezaHost(QStringLiteral("web.eleza.me")));
		QVERIFY(Support::IsElezaHost(QStringLiteral("Web.Eleza.Me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("app.eleza.me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("api.eleza.me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("a.b.eleza.me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("evil.www.eleza.me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral(".eleza.me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("eleza.me.evil.example")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("evil-eleza.me")));
		QVERIFY(!Support::IsElezaHost(QStringLiteral("eleza.me.")));
		QVERIFY(!Support::IsElezaHost(QString()));
	}

	void userInfoIsNeitherShownNorOpened() {
		const auto parsed = ParseLink(QStringLiteral("https://eleza.me@evil.example/u/sara"));
		QCOMPARE(parsed.type, ParsedLink::Type::External);
		QCOMPARE(parsed.url.host(), QStringLiteral("evil.example"));
		QVERIFY(parsed.url.userInfo().isEmpty());
		const auto shown = Support::DisplayUrl(parsed.url);
		QVERIFY2(!shown.contains(QLatin1Char('@')), qPrintable(shown));
		QVERIFY(shown.startsWith(QStringLiteral("https://evil.example/")));
	}

	void shownAddressHasNoDirectionTricks() {
		const auto parsed = ParseLink(U("https://evil.example/\xe2\x80\xaegnp.exe"));
		QCOMPARE(parsed.type, ParsedLink::Type::External);
		const auto shown = Support::DisplayUrl(parsed.url);
		for (const auto ch : shown) {
			QVERIFY(ch.category() != QChar::Other_Format);
			QVERIFY(ch.category() != QChar::Other_Control);
		}
		const auto longLink = QStringLiteral("https://evil.example/")
			+ QString(5000, QLatin1Char('a'));
		QVERIFY(Support::DisplayUrl(ParseLink(longLink).url).size() <= 301);
	}

	void ownLinksBecomeRoutes() {
		const auto profile = ParseLink(QStringLiteral("https://eleza.me/u/Sara"));
		QCOMPARE(profile.type, ParsedLink::Type::Route);
		QCOMPARE(profile.route.toString(), QStringLiteral("u/sara"));
		QCOMPARE(
			ParseLink(QStringLiteral("eleza://chat/c1?seq=4")).route.toString(),
			QStringLiteral("chat/c1?seq=4"));
		QCOMPARE(ParseLink(QStringLiteral("@sara")).type, ParsedLink::Type::Route);
		// A page of ours without a screen here: the browser.
		QCOMPARE(
			ParseLink(QStringLiteral("https://eleza.me/terms")).type,
			ParsedLink::Type::OwnWeb);
		// The three hosts that are ours, and only they, skip the question.
		for (const auto host : { "eleza.me", "www.eleza.me", "web.eleza.me", "WEB.Eleza.me" }) {
			const auto base = QStringLiteral("https://") + QString::fromLatin1(host);
			QVERIFY2(ParseLink(base + QStringLiteral("/terms")).type == ParsedLink::Type::OwnWeb, host);
			const auto user = ParseLink(base + QStringLiteral("/u/sara"));
			QVERIFY2(user.type == ParsedLink::Type::Route, host);
			QCOMPARE(user.route.toString(), QStringLiteral("u/sara"));
		}
		for (const auto host : { "app.eleza.me", "help.eleza.me", "x.web.eleza.me" }) {
			const auto base = QStringLiteral("https://") + QString::fromLatin1(host);
			QVERIFY2(ParseLink(base + QStringLiteral("/terms")).type == ParsedLink::Type::External, host);
			QVERIFY2(ParseLink(base + QStringLiteral("/u/sara")).type == ParsedLink::Type::External, host);
		}
		// A bot's deep link, in the web form and the app's own scheme.
		const auto bot = ParseLink(QStringLiteral("https://eleza.me/weather_bot?start=promo42"));
		QCOMPARE(bot.type, ParsedLink::Type::Route);
		QCOMPARE(bot.route.toString(), QStringLiteral("u/weather_bot?start=promo42"));
		QCOMPARE(
			ParseLink(QStringLiteral("eleza://u/weather_bot?start=promo42")).route.botStart(),
			std::optional<QString>(QStringLiteral("promo42")));
		QCOMPARE(
			ParseLink(QStringLiteral("https://www.eleza.me/@weather_bot?start=x")).route.toString(),
			QStringLiteral("u/weather_bot?start=x"));
				// Plain http to our own name is still ours, a foreign one is not.
		QCOMPARE(
			ParseLink(QStringLiteral("example.com/x")).type,
			ParsedLink::Type::External);
	}

	void systemLinksCarryOnlyAnAddress() {
		const auto mail = ParseLink(QStringLiteral(
			"mailto:sara@example.com?subject=Hi&attach=/home/u/.ssh/id_rsa"
			"&attachment=C:%5Csecret.txt&cc=x@evil.example&body=text&bcc=y@evil.example"));
		QCOMPARE(mail.type, ParsedLink::Type::System);
		const auto text = mail.url.toString(QUrl::FullyEncoded);
		QVERIFY2(text.startsWith(QStringLiteral("mailto:sara@example.com?")), qPrintable(text));
		QVERIFY2(!text.contains(QStringLiteral("attach"), Qt::CaseInsensitive), qPrintable(text));
		QVERIFY2(!text.contains(QStringLiteral("cc="), Qt::CaseInsensitive), qPrintable(text));
		QVERIFY(text.contains(QStringLiteral("subject=Hi")));
		QVERIFY(text.contains(QStringLiteral("body=text")));

		QCOMPARE(
			ParseLink(QStringLiteral("mailto:a@example.com,b@evil.example")).type,
			ParsedLink::Type::Invalid);
		QCOMPARE(
			ParseLink(QStringLiteral("mailto:?attach=/etc/passwd")).type,
			ParsedLink::Type::Invalid);

		const auto phone = ParseLink(QStringLiteral("tel:+98 (912) 123-4567"));
		QCOMPARE(phone.type, ParsedLink::Type::System);
		QCOMPARE(phone.url.toString(), QStringLiteral("tel:+989121234567"));
		QCOMPARE(
			ParseLink(QStringLiteral("tel:+989121234567,,*21*5550100%23")).type,
			ParsedLink::Type::Invalid);
		QCOMPARE(
			ParseLink(QStringLiteral("tel:;phone-context=evil")).type,
			ParsedLink::Type::Invalid);
	}
};

QTEST_GUILESS_MAIN(TestLinks)
#include "test_links.moc"
