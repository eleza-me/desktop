// Ui::Text: entity offsets in UTF-16 units (surrogate pairs), parsing, layout, hit tests.
#include "ui/style/style_core.h"
#include "ui/text/text.h"
#include "ui/text/text_entity.h"
#include "styles/style_basic.h"

#include <QtCore/QElapsedTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest/QtTest>

using namespace Ui::Text;

class TestUiText : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

private Q_SLOTS:
	void initTestCase() {
		style::Start(style::Scheme::Light);
		style::SetRightToLeft(false);
	}

	void isolatedLinks() {
		using namespace Ui::Text;
		const auto lri = QChar(0x2066);
		const auto fsi = QChar(0x2068);
		const auto pdi = QChar(0x2069);
		const auto source = QStringLiteral("see @sara and #tag now");
		auto parsed = ParseEntities(source, ParseAll);
		// A formatting range over the mention and a bit around it.
		parsed.entities.push_back({ EntityType::Bold, 2, 9, QString() });
		const auto shown = IsolateLinks(parsed);
		QCOMPARE(
			shown.text,
			QStringLiteral("see ") + lri + QStringLiteral("@sara") + pdi
				+ QStringLiteral(" and ") + fsi + QStringLiteral("#tag") + pdi
				+ QStringLiteral(" now"));
		QCOMPARE(StripIsolates(shown.text), source);
		auto links = 0;
		for (const auto &entity : shown.entities) {
			const auto part = shown.text.mid(entity.offset, entity.length);
			if (entity.type == EntityType::Mention) {
				QCOMPARE(part, QStringLiteral("@sara"));
				++links;
			} else if (entity.type == EntityType::Hashtag) {
				QCOMPARE(part, QStringLiteral("#tag"));
				++links;
			} else if (entity.type == EntityType::Bold) {
				// "e @sara an" with the marks of the mention inside.
				QCOMPARE(StripIsolates(part), source.mid(2, 9));
			}
		}
		QCOMPARE(links, 2);

		// Selections map both ways ("sara" is 5..9 in the source).
		const auto inShown = IsolatedSelection(shown.text, { 5, 9 });
		QCOMPARE(shown.text.mid(inShown.from, inShown.to - inShown.from), QStringLiteral("sara"));
		QCOMPARE(SourceSelection(shown.text, inShown), (Selection{ 5, 9 }));
		const auto whole = SourceSelection(shown.text, { 0, int(shown.text.size()) });
		QCOMPARE(whole, (Selection{ 0, int(source.size()) }));
		QCOMPARE(IsolatedSelection(shown.text, { 0, int(source.size()) }).to, int(shown.text.size()));

		// Nothing to isolate: untouched.
		const auto plain = ParseEntities(QStringLiteral("plain text"), ParseAll);
		QCOMPARE(IsolateLinks(plain).text, QStringLiteral("plain text"));
	}

	void normalizeSurrogates() {
		// "a😀b": the emoji takes two UTF-16 units (offsets 1 and 2).
		const auto text = U("a😀b");
		QCOMPARE(text.size(), 4);
		auto entities = NormalizeEntities(text, {
			Entity{ EntityType::Bold, 1, 2, QString() }, // exactly the emoji
			Entity{ EntityType::Italic, 2, 2, QString() }, // starts inside the pair
			Entity{ EntityType::Underline, 0, 2, QString() }, // ends inside the pair
			Entity{ EntityType::Code, 3, 50, QString() }, // runs past the end
			Entity{ EntityType::Spoiler, 9, 2, QString() }, // completely outside
			Entity{ EntityType::Invalid, 0, 1, QString() },
			Entity{ EntityType::StrikeOut, -3, 4, QString() }, // negative offset
		});
		// Sorted by offset, longer first.
		QCOMPARE(entities.size(), 5);
		QCOMPARE(entities[0].type, EntityType::Underline);
		QCOMPARE(entities[0].offset, 0);
		QCOMPARE(entities[0].length, 3); // extended to keep the pair whole
		QCOMPARE(entities[1].type, EntityType::StrikeOut);
		QCOMPARE(entities[1].offset, 0);
		QCOMPARE(entities[1].length, 1);
		QCOMPARE(entities[2].type, EntityType::Italic);
		QCOMPARE(entities[2].offset, 1); // moved back to the pair's start
		QCOMPARE(entities[2].length, 3);
		QCOMPARE(entities[3].type, EntityType::Bold);
		QCOMPARE(entities[3].offset, 1);
		QCOMPARE(entities[3].length, 2);
		QCOMPARE(entities[4].type, EntityType::Code);
		QCOMPARE(entities[4].offset, 3);
		QCOMPARE(entities[4].length, 1); // clamped
	}

	void linkOffsetsAfterEmoji() {
		// Offsets as a server counting UTF-16 units would send them.
		const auto text = U("😀😀 سلام @sara 👩‍👩‍👧 #تست");
		const auto mention = int(text.indexOf(QStringLiteral("@sara")));
		const auto hashtag = int(text.indexOf(QLatin1Char('#')));
		QCOMPARE(mention, 10); // 2 emoji (4 units) + space + 4 letters + space
		auto string = String(st::defaultTextStyle, TextWithEntities{ text, {
			Entity{ EntityType::Mention, mention, 5, QString() },
			Entity{ EntityType::Hashtag, hashtag, int(text.size()) - hashtag, QString() },
			Entity{ EntityType::CustomUrl, 0, 4, QStringLiteral("eleza.me/x") },
		} });
		QCOMPARE(string.linksCount(), 3);
		QCOMPARE(string.link(0).text, U("😀😀"));
		QCOMPARE(string.link(0).data, QStringLiteral("eleza.me/x"));
		QCOMPARE(LinkUrl(string.link(0)), QStringLiteral("https://eleza.me/x"));
		QCOMPARE(string.link(1).text, QStringLiteral("@sara"));
		QCOMPARE(string.link(1).data, QStringLiteral("@sara"));
		QCOMPARE(string.link(2).text, U("#تست"));
		QCOMPARE(string.toString(), text);
		QCOMPARE(string.toTextWithEntities().entities.size(), 3);
	}

	void parseEntities() {
		const auto text = U(
			"ببین eleza.me و https://example.com/a?b=1, ایمیل a.b@mail.org "
			"یا @sara.m. و #کتاب_خوب 📚 #tag2!");
		const auto parsed = ParseEntities(text);
		const auto find = [&](EntityType type, int skip = 0) {
			for (const auto &entity : parsed.entities) {
				if (entity.type == type && !skip--) {
					return text.mid(entity.offset, entity.length);
				}
			}
			return QString();
		};
		QCOMPARE(find(EntityType::Url), QStringLiteral("eleza.me"));
		QCOMPARE(find(EntityType::Url, 1), QStringLiteral("https://example.com/a?b=1"));
		QCOMPARE(find(EntityType::Email), QStringLiteral("a.b@mail.org"));
		QCOMPARE(find(EntityType::Mention), QStringLiteral("@sara.m"));
		QCOMPARE(find(EntityType::Hashtag), U("#کتاب_خوب"));
		QCOMPARE(find(EntityType::Hashtag, 1), QStringLiteral("#tag2"));

		// Existing links are kept and not doubled.
		const auto kept = ParseEntities(
			QStringLiteral("open eleza.me now"),
			ParseAll,
			{ Entity{ EntityType::CustomUrl, 0, 13, QStringLiteral("x.y") } });
		QCOMPARE(kept.entities.size(), 1);
		QCOMPARE(kept.entities[0].type, EntityType::CustomUrl);

		QCOMPARE(EntityTypeFromName(QStringLiteral("text_link")), EntityType::CustomUrl);
		QCOMPARE(EntityTypeFromName(QStringLiteral("spoiler")), EntityType::Spoiler);
		QCOMPARE(EntityTypeFromName(QStringLiteral("nope")), EntityType::Invalid);
	}

	void parseBotCommands() {
		const auto text = U(
			"/start یا /help@weather_bot و and/or 1405/07/11 1/2 "
			"https://eleza.me/x/y @sara/x /Settings_2!");
		const auto commands = [&](uint flags) {
			auto result = QStringList();
			for (const auto &entity : ParseEntities(text, flags).entities) {
				if (entity.type == EntityType::BotCommand) {
					result.push_back(text.mid(entity.offset, entity.length));
				}
			}
			return result;
		};
		// Only where a bot reads the chat.
		QVERIFY(commands(ParseAll).isEmpty());
		QCOMPARE(
			commands(ParseAll | ParseBotCommands),
			(QStringList{
				QStringLiteral("/start"),
				QStringLiteral("/help@weather_bot"),
				QStringLiteral("/Settings_2"),
			}));
		// The command is a link of its own, isolated like an address.
		const auto shown = IsolateLinks(ParseEntities(
			QStringLiteral("/start"),
			ParseBotCommands));
		QCOMPARE(shown.text, U("\u2066/start\u2069"));
	}

		void layout() {
		auto string = String(st::defaultTextStyle, QStringLiteral(
			"one two three four five six seven eight nine ten eleven twelve"));
		const auto line = string.minHeight();
		QVERIFY(line > 0);
		QVERIFY(string.maxWidth() > 100);
		QCOMPARE(string.countHeight(string.maxWidth() + 10), line);
		const auto narrow = string.maxWidth() / 3;
		const auto lines = string.countLines(narrow);
		QVERIFY(lines >= 3);
		QCOMPARE(string.countHeight(narrow), lines * line);
		QCOMPARE(string.countHeight(narrow, 2), 2 * line);
		QVERIFY(string.countMaxLineWidth(narrow) <= narrow);

		// Paragraph breaks and empty paragraphs take lines.
		auto paragraphs = String(st::defaultTextStyle, QStringLiteral("a\n\nb"));
		QCOMPARE(paragraphs.countLines(500), 3);

		auto empty = String();
		QVERIFY(empty.isEmpty());
		QCOMPARE(empty.countHeight(100), 0);
	}

	void paragraphDirection() {
		// The first strong character decides; an isolated name does not.
		const auto rtl = [](const QString &text) {
			return String(st::defaultTextStyle, text).countLastLine(1000).rtl;
		};
		QVERIFY(!rtl(QStringLiteral("hello")));
		QVERIFY(rtl(U("سلام hello")));
		QVERIFY(!rtl(U("⁨سارا⁩ left the group")));
		QVERIFY(rtl(U("⁨John⁩ گروه را ترک کرد")));
		// Nested and unbalanced isolates.
		QVERIFY(!rtl(U("⁦⁧سارا⁩x⁩ ok")));
		QVERIFY(!rtl(U("⁩ ok")));
	}

	void hitTest() {
		const auto text = QStringLiteral("hello link world");
		auto string = String(st::defaultTextStyle, TextWithEntities{ text, {
			Entity{ EntityType::Url, 6, 4, QStringLiteral("eleza.me") },
		} });
		const auto geometry = PaintContext{
			.position = QPoint(10, 20),
			.availableWidth = 400,
		};
		const auto &font = st::defaultTextStyle.font;
		const auto y = 20 + string.minHeight() / 2;
		const auto before = 10 + font->width(QStringLiteral("hel"));
		const auto inside = 10 + font->width(QStringLiteral("hello li"));
		const auto after = 10 + font->width(text) + 30;

		auto state = string.getState(QPoint(before, y), geometry);
		QVERIFY(state.uponSymbol);
		QCOMPARE(state.link, -1);
		QVERIFY(state.symbol == 2 || state.symbol == 3);

		state = string.getState(QPoint(inside, y), geometry);
		QVERIFY(state.uponSymbol);
		QCOMPARE(state.link, 0);

		state = string.getState(QPoint(after, y), geometry);
		QVERIFY(!state.uponSymbol);
		QCOMPARE(state.link, -1);
		QCOMPARE(state.cursor, int(text.size()));

		state = string.getState(QPoint(inside, 5), geometry); // above the text
		QCOMPARE(state.cursor, 0);
		state = string.getState(QPoint(inside, 500), geometry); // below
		QCOMPARE(state.cursor, int(text.size()));
	}

	void spoilers() {
		const auto text = QStringLiteral("the killer is the gardener, see eleza.me");
		auto string = String(st::defaultTextStyle, TextWithEntities{ text, {
			Entity{ EntityType::Spoiler, 14, 26, QString() },
			Entity{ EntityType::Url, 32, 8, QString() },
		} });
		QVERIFY(string.hasSpoilers());
		QVERIFY(!string.spoilersRevealed());
		const auto geometry = PaintContext{
			.position = QPoint(0, 0),
			.availableWidth = 600,
		};
		const auto &font = st::defaultTextStyle.font;
		const auto point = QPoint(
			font->width(text.left(35)),
			string.minHeight() / 2);
		auto state = string.getState(point, geometry);
		QVERIFY(state.spoiler);
		QCOMPARE(state.link, -1); // links under a hidden spoiler are not clickable
		string.setSpoilersRevealed(true);
		state = string.getState(point, geometry);
		QVERIFY(!state.spoiler);
		QCOMPARE(state.link, 0);
	}

	void selection() {
		const auto text = U("سلام دنیای خوب\nsecond line here");
		auto string = String(st::defaultTextStyle, text);
		const auto word = string.adjustSelection(Selection{ 6, 7 }, SelectType::Words);
		QCOMPARE(string.toString(word), U("دنیای"));
		const auto paragraph = string.adjustSelection(
			Selection{ 20, 21 },
			SelectType::Paragraphs);
		QCOMPARE(string.toString(paragraph), QStringLiteral("second line here"));
		// A zero width non-joiner is a part of the word.
		auto joined = String(st::defaultTextStyle, U("کتاب‌ها خوبند"));
		QCOMPARE(
			joined.toString(joined.adjustSelection(Selection{ 1, 2 }, SelectType::Words)),
			U("کتاب‌ها"));
		// Selection edges never split a surrogate pair.
		auto emoji = String(st::defaultTextStyle, U("a😀b"));
		QCOMPARE(
			emoji.adjustSelection(Selection{ 2, 2 }, SelectType::Letters),
			(Selection{ 1, 3 }));
	}

	void paints() {
		// Drawing mixed-direction, formatted, elided and selected text must
		// put something on the canvas and must not crash.
		const auto text = U(
			"متن فارسی with English و 😀 emoji، پیوند eleza.me و ادامهٔ طولانی "
			"برای چند خط شدن و کوتاه‌سازی در انتهای خط دوم. One more sentence.");
		auto string = String(st::defaultTextStyle, ParseEntities(text, ParseAll, {
			Entity{ EntityType::Bold, 0, 3, QString() },
			Entity{ EntityType::Spoiler, 4, 5, QString() },
			Entity{ EntityType::Code, 15, 7, QString() },
		}));
		for (const auto rtl : { false, true }) {
			style::SetRightToLeft(rtl);
			auto image = QImage(240, 120, QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::transparent);
			{
				auto p = QPainter(&image);
				p.setPen(Qt::black);
				string.draw(p, PaintContext{
					.position = QPoint(8, 8),
					.availableWidth = 220,
					.elisionLines = 2,
					.autoDirection = true,
					.selection = Selection{ 2, 30 },
					.hoveredLink = 0,
				});
			}
			auto painted = 0;
			auto belowLimit = 0;
			const auto limit = 8 + 2 * string.minHeight() + 2;
			for (auto y = 0; y != image.height(); ++y) {
				for (auto x = 0; x != image.width(); ++x) {
					if (qAlpha(image.pixel(x, y)) > 0) {
						++painted;
						if (y > limit) {
							++belowLimit;
						}
					}
				}
			}
			QVERIFY(painted > 500);
			QCOMPARE(belowLimit, 0); // nothing below the two allowed lines
		}
		style::SetRightToLeft(false);
	}

	// A peer (or a broken server) controls the text and its entities: none
	// of this may take long on the GUI thread, whatever the length.
	void hostileTextsAreFast() {
		const auto check = [](const char *name, const QString &text, Entities entities = {}) {
			auto timer = QElapsedTimer();
			timer.start();
			const auto parsed = ParseEntities(text, ParseAll, std::move(entities));
			const auto shown = IsolateLinks(parsed);
			const auto took = timer.elapsed();
			for (const auto &entity : shown.entities) {
				QVERIFY2(entity.offset >= 0 && entity.length >= 0
					&& entity.offset + entity.length <= shown.text.size(), name);
			}
			qInfo("%s: %lld chars, %lld entities, %lld ms",
				name, qlonglong(text.size()), qlonglong(shown.entities.size()), qlonglong(took));
			QVERIFY2(took < 3000, name);
		};
		check("plain", QString(100000, QLatin1Char('x')));
		check("dashes", QStringLiteral("-a").repeated(50000));
		check("dots", QStringLiteral("a.").repeated(50000));
		check("links", QStringLiteral("a.io ").repeated(20000));
		check("mixed", QStringLiteral("@sara #tag x@y.co https://a.b/c ").repeated(3000));
		check("at signs", QStringLiteral("a@").repeated(50000));
		check("hashes", QStringLiteral("#a").repeated(50000));
		check("percent", QStringLiteral("a%b+c-").repeated(16000) + QStringLiteral("@"));
		auto many = Entities();
		for (auto i = 0; i != 20000; ++i) {
			many.push_back({ (i % 2) ? EntityType::CustomUrl : EntityType::Bold, i % 977, 1 + i % 13, QStringLiteral("https://x.example") });
		}
		check("many entities", QStringLiteral("a.io b ").repeated(2000), many);
	}

};

QTEST_MAIN(TestUiText)
#include "test_ui_text.moc"
