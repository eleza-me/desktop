// History:: bot rules: inline queries in the composer, the command menu, the
// «شروع» bar and its text, what a button of an inline keyboard does, the
// reply keyboard's shown / hidden state.
#include "history/history_bot_logic.h"

#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

namespace {

[[nodiscard]] Data::MarkupButton Button(
		const QString &kind,
		const QString &url = QString(),
		std::optional<QString> query = std::nullopt) {
	auto result = Data::MarkupButton();
	result.text = u"b"_s;
	result.kind = kind;
	result.url = url;
	result.query = query;
	return result;
}

[[nodiscard]] Data::BotCommand Command(const QString &name) {
	auto result = Data::BotCommand();
	result.command = name;
	result.description = name + u" it"_s;
	return result;
}

} // namespace

class TestHistoryBots : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void inlineQueryNeedsHandleAndSpace() {
		using History::ParseInlineQuery;
		const auto weather = ParseInlineQuery(u"@Weather_Bot tehran"_s);
		QVERIFY(weather.has_value());
		QCOMPARE(weather->handle, u"weather_bot"_s);
		QCOMPARE(weather->query, u"tehran"_s);

		// The space right after the handle: an empty query is a query.
		const auto empty = ParseInlineQuery(u"@gif_bot "_s);
		QVERIFY(empty.has_value());
		QCOMPARE(empty->query, QString());

		// Everything after the first whitespace, new lines included.
		const auto lines = ParseInlineQuery(u"@gif_bot  cats\nand dogs"_s);
		QVERIFY(lines.has_value());
		QCOMPARE(lines->query, u" cats\nand dogs"_s);

		const auto persian = ParseInlineQuery(u"@wiki_bot تهران"_s);
		QVERIFY(persian.has_value());
		QCOMPARE(persian->query, u"تهران"_s);

		QVERIFY(!ParseInlineQuery(u"@weather_bot"_s));      // still typing the handle
		QVERIFY(!ParseInlineQuery(u"@sara hello"_s));       // not a bot's handle
		QVERIFY(!ParseInlineQuery(u"@bot hello"_s));        // nothing before «bot»
		QVERIFY(!ParseInlineQuery(u"hi @weather_bot x"_s)); // only at the start
		QVERIFY(!ParseInlineQuery(u" @weather_bot x"_s));
		QVERIFY(!ParseInlineQuery(u"@weather-bot x"_s));
		QVERIFY(!ParseInlineQuery(QString()));
	}

	void inlinePrefillAndLimit() {
		QCOMPARE(History::InlineQueryPrefill(u"gif_bot"_s, u"cats"_s), u"@gif_bot cats"_s);
		QCOMPARE(History::InlineQueryPrefill(u"@gif_bot"_s, QString()), u"@gif_bot "_s);
		const auto round = History::ParseInlineQuery(
			History::InlineQueryPrefill(u"gif_bot"_s, u"x y"_s));
		QVERIFY(round.has_value());
		QCOMPARE(round->query, u"x y"_s);

		QCOMPARE(History::LimitInlineQuery(u"abcdef"_s, 4), u"abcd"_s);
		QCOMPARE(History::LimitInlineQuery(u"abc"_s, 4), u"abc"_s);
		QCOMPARE(History::LimitInlineQuery(u"abc"_s, 0), u"abc"_s);
		// An emoji is not cut in half.
		const auto emoji = u"ab"_s + QString::fromUtf8("\xF0\x9F\x98\x80");
		QCOMPARE(History::LimitInlineQuery(emoji, 3), u"ab"_s);
	}

	void commandPrefix() {
		using History::CommandPrefix;
		QCOMPARE(CommandPrefix(u"/"_s), std::optional<QString>(QString()));
		QCOMPARE(CommandPrefix(u"/He"_s), std::optional<QString>(u"he"_s));
		QCOMPARE(CommandPrefix(u"/start_now"_s), std::optional<QString>(u"start_now"_s));
		QVERIFY(!CommandPrefix(u"/start now"_s));
		QVERIFY(!CommandPrefix(u"/start\n"_s));
		QVERIFY(!CommandPrefix(u"/st-art"_s));
		QVERIFY(!CommandPrefix(u"hello"_s));
		QVERIFY(!CommandPrefix(QString()));
		QVERIFY(!CommandPrefix(u"/"_s + QString(40, u'a')));
	}

	void filterCommands() {
		const auto commands = QVector<Data::BotCommand>{
			Command(u"start"_s),
			Command(u"help"_s),
			Command(u"settings"_s),
			Command(u"Stats"_s),
		};
		const auto all = History::FilterCommands(commands, QString());
		QCOMPARE(all.size(), 4);
		const auto st = History::FilterCommands(commands, u"st"_s);
		QCOMPARE(st.size(), 2);
		QCOMPARE(st[0].command, u"start"_s);
		QCOMPARE(st[1].command, u"Stats"_s);
		QCOMPARE(History::FilterCommands(commands, u"SE"_s).size(), 1);
		QVERIFY(History::FilterCommands(commands, u"x"_s).isEmpty());
	}

	void startText() {
		QCOMPARE(History::StartCommandText(QString()), u"/start"_s);
		QCOMPARE(History::StartCommandText(u"promo42"_s), u"/start promo42"_s);
		QCOMPARE(History::StartCommandText(u"a-b_C9"_s), u"/start a-b_C9"_s);
		// What the server would not take is not sent.
		QCOMPARE(History::StartCommandText(u"bad payload"_s), u"/start"_s);
		QCOMPARE(History::StartCommandText(u"x/y"_s), u"/start"_s);
		QCOMPARE(History::StartCommandText(QString(65, u'a')), u"/start"_s);
		QVERIFY(History::ValidStartPayload(QString(64, u'a')));
		QVERIFY(!History::ValidStartPayload(u"سلام"_s));
	}

	void startBar() {
		auto state = History::BotStartState();
		state.isBot = true;
		state.writable = true;
		state.listLoaded = true;
		state.listEmpty = true;
		QVERIFY(History::ShowBotStart(state));

		// Not for people, not where one cannot write, once pressed.
		auto person = state;
		person.isBot = false;
		QVERIFY(!History::ShowBotStart(person));
		auto blocked = state;
		blocked.writable = false;
		QVERIFY(!History::ShowBotStart(blocked));
		auto sent = state;
		sent.sent = true;
		QVERIFY(!History::ShowBotStart(sent));

		// A chat with messages: only from a deep link.
		auto talked = state;
		talked.listEmpty = false;
		talked.lastSeq = 7;
		QVERIFY(!History::ShowBotStart(talked));
		talked.pending = true;
		QVERIFY(History::ShowBotStart(talked));

		// A first message waiting in the outbox.
		auto queued = state;
		queued.outboxPending = 1;
		QVERIFY(!History::ShowBotStart(queued));

		// While the history is read the row decides.
		auto loading = state;
		loading.listLoaded = false;
		loading.listEmpty = true;
		QVERIFY(History::ShowBotStart(loading));
		loading.lastSeq = 3;
		QVERIFY(!History::ShowBotStart(loading));
	}

	void buttonRouting() {
		using History::ButtonAction;
		using History::ButtonActionFor;
		QCOMPARE(ButtonActionFor(Button(u"callback"_s)), ButtonAction::Callback);
		QCOMPARE(
			ButtonActionFor(Button(u"url"_s, u"https://example.com"_s)),
			ButtonAction::Url);
		QCOMPARE(ButtonActionFor(Button(u"url"_s)), ButtonAction::None);
		// The switch kinds never ask the bot.
		QCOMPARE(
			ButtonActionFor(Button(u"switch_inline_current"_s, QString(), u"q"_s)),
			ButtonAction::SwitchInlineCurrent);
		QCOMPARE(
			ButtonActionFor(Button(u"switch_inline"_s, QString(), u"q"_s)),
			ButtonAction::SwitchInlinePick);
		QCOMPARE(
			ButtonActionFor(Button(u"switch_inline_current"_s)),
			ButtonAction::SwitchInlineCurrent);
		QCOMPARE(ButtonActionFor(Button(u"web_app"_s)), ButtonAction::None);
		// Cached by an older build, without a kind.
		QCOMPARE(
			ButtonActionFor(Button(QString(), u"https://example.com"_s)),
			ButtonAction::Url);
		QCOMPARE(ButtonActionFor(Button(QString())), ButtonAction::Callback);
	}

	void buttonBotHandle() {
		auto message = Data::Message();
		QCOMPARE(History::ButtonBotHandle(message, u"chat_bot"_s), u"chat_bot"_s);
		auto via = Data::ViaBot();
		via.handle = u"gif_bot"_s;
		message.viaBot = via;
		QCOMPARE(History::ButtonBotHandle(message, u"chat_bot"_s), u"gif_bot"_s);
		message.viaBot->handle = QString();
		QCOMPARE(History::ButtonBotHandle(message, QString()), QString());
	}

	void replyKeyboardView() {
		auto view = History::ReplyKeyboardView{ 5, false };
		view.hidden = true;
		// The same keyboard stays put away; a new one shows itself.
		QVERIFY(History::ReplyKeyboardAfterChange(view, 5).hidden);
		const auto fresh = History::ReplyKeyboardAfterChange(view, 9);
		QCOMPARE(fresh.seq, qint64(9));
		QVERIFY(!fresh.hidden);
		// After a message of mine a persistent keyboard comes back.
		QVERIFY(History::ReplyKeyboardAfterSend(view, false).hidden);
		QVERIFY(!History::ReplyKeyboardAfterSend(view, true).hidden);
	}

	void markupParsing() {
		const auto markup = Data::ReplyMarkup::FromJson(QJsonDocument::fromJson(QByteArray("{ \"inline_keyboard\": [[ {\"text\":\"Share\",\"kind\":\"switch_inline\",\"query\":\"tehran\"}, {\"text\":\"Here\",\"kind\":\"switch_inline_current\",\"query\":\"\"} ]], \"keyboard\": [[{\"text\":\"Today\"},{\"text\":\"Week\"}]], \"resize\": true, \"persistent\": true, \"one_time\": true, \"placeholder\": \"Pick one\" }")).object());
		QCOMPARE(markup.inlineKeyboard.size(), 1);
		QCOMPARE(
			History::ButtonActionFor(markup.inlineKeyboard[0][0]),
			History::ButtonAction::SwitchInlinePick);
		QCOMPARE(markup.inlineKeyboard[0][0].query, std::optional<QString>(u"tehran"_s));
		QCOMPARE(
			History::ButtonActionFor(markup.inlineKeyboard[0][1]),
			History::ButtonAction::SwitchInlineCurrent);
		QVERIFY(markup.resize);
		QVERIFY(markup.persistent);
		QVERIFY(markup.oneTime);
		QCOMPARE(markup.placeholder, u"Pick one"_s);
	}

	void botInfoParsing() {
		const auto info = Data::BotInfo::FromJson(QJsonDocument::fromJson(QByteArray("{ \"id\":\"u1\",\"handle\":\"weather_bot\",\"name\":\"Weather\", \"about\":\"Forecasts\",\"description\":\"I tell the weather.\", \"verified\":true, \"commands\":[{\"command\":\"today\",\"description\":\"Today\"},{\"command\":\"\",\"description\":\"x\"}, {\"command\":\"/week\",\"description\":\"Week\"}], \"inline\":{\"enabled\":true,\"placeholder\":\"City…\"} }")).object());
		QCOMPARE(info.id, u"u1"_s);
		QCOMPARE(info.handle, u"weather_bot"_s);
		QCOMPARE(info.description, u"I tell the weather."_s);
		QVERIFY(info.verified);
		QCOMPARE(info.commands.size(), 2);
		QCOMPARE(info.commands[1].command, u"week"_s);
		QVERIFY(info.inlineEnabled);
		QCOMPARE(info.inlinePlaceholder, u"City…"_s);

		const auto page = Data::InlineResults::FromJson(QJsonDocument::fromJson(QByteArray("{ \"query_id\":\"q1\",\"next_offset\":\"20\",\"answered\":true, \"results\":[{\"type\":\"article\",\"id\":\"thr\",\"title\":\"Tehran\",\"thumb_url\":\"https://x/t.png\"}, {\"type\":\"article\",\"id\":\"\",\"title\":\"broken\"}] }")).object());
		QCOMPARE(page.queryId, u"q1"_s);
		QCOMPARE(page.results.size(), 1);
		QCOMPARE(page.results[0].thumbUrl, u"https://x/t.png"_s);
		QCOMPARE(page.nextOffset, u"20"_s);
		QVERIFY(page.answered);
		QVERIFY(!Data::InlineResults::FromJson(QJsonObject()).answered);
	}
};

QTEST_GUILESS_MAIN(TestHistoryBots)
#include "test_history_bots.moc"
