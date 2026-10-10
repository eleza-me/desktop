// The .strings format: tags, plural forms, cross-file validation, generated API.
#include "lang/lang_generator.h"
#include "lang/lang_parser.h"

#include <QtTest/QtTest>

using namespace codegen::lang;

class TestLangCodegen : public QObject {
	Q_OBJECT

private:
	bool parse(const QString &content, File &file, Error &error) {
		return ParseContent(QStringLiteral("lang.strings"), content, file, error);
	}

private Q_SLOTS:
	void tags() {
		QCOMPARE(
			ExtractTags(QStringLiteral("Sent to {phone} at {time}, {phone}")),
			QStringList({ QStringLiteral("phone"), QStringLiteral("time") }));
		QCOMPARE(ExtractTags(QStringLiteral("no tags { here } {Bad} {}")), QStringList());
	}

	void simpleAndEscapes() {
		auto file = File();
		auto error = Error();
		QVERIFY2(parse(QString::fromUtf8(
			"// comment\n"
			"\"lng_hello\" = \"سلام {name}\";\n"
			"/* block\n comment */\n"
			"\"lng_quote\" = \"a \\\"b\\\" \\\\ c\\nd\";\n"), file, error),
			qPrintable(error.format()));
		QCOMPARE(file.entries.size(), 2);
		QCOMPARE(file.entries[0].key, QStringLiteral("lng_hello"));
		QCOMPARE(file.entries[0].value, QString::fromUtf8("سلام {name}"));
		QCOMPARE(file.entries[0].tags, QStringList({ QStringLiteral("name") }));
		QVERIFY(!file.entries[0].plural);
		QCOMPARE(file.entries[0].line, 2);
		QCOMPARE(file.entries[1].value, QStringLiteral("a \"b\" \\ c\nd"));
		QCOMPARE(file.entries[1].line, 5);
	}

	void plurals() {
		auto file = File();
		auto error = Error();
		QVERIFY2(parse(QStringLiteral(
			"\"lng_posts#one\" = \"{count} new post by {name}\";\n"
			"\"lng_posts#other\" = \"{count} new posts by {name}\";\n"
			"\"lng_only_other#other\" = \"{count} items\";\n"), file, error),
			qPrintable(error.format()));
		QCOMPARE(file.entries.size(), 2);
		QVERIFY(file.entries[0].plural);
		QVERIFY(file.entries[0].hasOne);
		QCOMPARE(file.entries[0].one, QStringLiteral("{count} new post by {name}"));
		QCOMPARE(file.entries[0].value, QStringLiteral("{count} new posts by {name}"));
		QVERIFY(file.entries[1].plural);
		QVERIFY(!file.entries[1].hasOne);
	}

	void parseErrors() {
		auto file = File();
		auto error = Error();
		QVERIFY(!parse(QStringLiteral("\"hello\" = \"x\";"), file, error));
		QVERIFY(error.text.contains(QStringLiteral("lng_")));

		QVERIFY(!parse(QStringLiteral(
			"\"lng_a\" = \"x\";\n\"lng_a\" = \"y\";"), file, error));
		QCOMPARE(error.line, 2);
		QVERIFY(error.text.contains(QStringLiteral("twice")));

		QVERIFY(!parse(QStringLiteral("\"lng_a\" = \"x\""), file, error));
		QVERIFY(error.text.contains(QStringLiteral("';'")));

		QVERIFY(!parse(QStringLiteral("\"lng_a#few\" = \"x\";"), file, error));
		QVERIFY(error.text.contains(QStringLiteral("plural form")));

		QVERIFY(!parse(QStringLiteral("\"lng_a#one\" = \"{count} x\";"), file, error));
		QVERIFY(error.text.contains(QStringLiteral("#other")));

		QVERIFY(!parse(QStringLiteral("\"lng_a#other\" = \"many\";"), file, error));
		QVERIFY(error.text.contains(QStringLiteral("{count}")));

		QVERIFY(!parse(QStringLiteral("\"lng_a\" = \"line\nbreak\";"), file, error));
		QVERIFY(error.text.contains(QStringLiteral("unterminated")));

		QVERIFY(!parse(QStringLiteral(
			"\"lng_a\" = \"x\";\n\"lng_a#other\" = \"{count}\";"), file, error));
		QVERIFY(error.text.contains(QStringLiteral("with and without")));
	}

	void validation() {
		auto catalog = Catalog();
		auto error = Error();
		QVERIFY(parse(QStringLiteral(
			"\"lng_a\" = \"to {phone}\";\n"
			"\"lng_b#other\" = \"{count} x\";\n"), catalog.base, error));

		QVERIFY(ParseContent(QStringLiteral("lang_en.strings"), QStringLiteral(
			"\"lng_b#one\" = \"{count} y\";\n"
			"\"lng_b#other\" = \"{count} ys\";\n"
			"\"lng_a\" = \"{phone} is it\";\n"), catalog.other, error));
		QVERIFY2(Validate(catalog, error), qPrintable(error.format()));

		// A missing key.
		QVERIFY(ParseContent(QStringLiteral("lang_en.strings"), QStringLiteral(
			"\"lng_a\" = \"{phone}\";\n"), catalog.other, error));
		QVERIFY(!Validate(catalog, error));
		QVERIFY(error.text.contains(QStringLiteral("lng_b")));
		QVERIFY(error.text.contains(QStringLiteral("missing")));

		// Different tags.
		QVERIFY(ParseContent(QStringLiteral("lang_en.strings"), QStringLiteral(
			"\"lng_a\" = \"{number}\";\n"
			"\"lng_b#other\" = \"{count}\";\n"), catalog.other, error));
		QVERIFY(!Validate(catalog, error));
		QVERIFY(error.text.contains(QStringLiteral("{number}")));
		QVERIFY(error.file.endsWith(QStringLiteral("lang_en.strings")));

		// An extra key.
		QVERIFY(ParseContent(QStringLiteral("lang_en.strings"), QStringLiteral(
			"\"lng_a\" = \"{phone}\";\n"
			"\"lng_b#other\" = \"{count}\";\n"
			"\"lng_c\" = \"extra\";\n"), catalog.other, error));
		QVERIFY(!Validate(catalog, error));
		QVERIFY(error.text.contains(QStringLiteral("lng_c")));

		// Plural in one file only.
		QVERIFY(ParseContent(QStringLiteral("lang_en.strings"), QStringLiteral(
			"\"lng_a\" = \"{phone}\";\n"
			"\"lng_b\" = \"{count}\";\n"), catalog.other, error));
		QVERIFY(!Validate(catalog, error));
		QVERIFY(error.text.contains(QStringLiteral("plural")));
	}

	void generated() {
		auto catalog = Catalog();
		auto error = Error();
		QVERIFY(parse(QString::fromUtf8(
			"\"lng_plain\" = \"ساده\";\n"
			"\"lng_tagged\" = \"{user} به {chat}\";\n"
			"\"lng_count#other\" = \"{count} پیام از {user}\";\n"), catalog.base, error));
		QVERIFY(ParseContent(QStringLiteral("lang_en.strings"), QStringLiteral(
			"\"lng_plain\" = \"plain\";\n"
			"\"lng_tagged\" = \"{user} to {chat}\";\n"
			"\"lng_count#one\" = \"{count} message from {user}\";\n"
			"\"lng_count#other\" = \"{count} messages from {user}\";\n"),
			catalog.other, error));
		QVERIFY(Validate(catalog, error));
		const auto files = Generate(catalog);
		QCOMPARE(files.size(), 2);
		const auto &header = files[0].content;
		QVERIFY(header.contains("enum lngtag_count { lt_count };"));
		QVERIFY(header.contains("enum lngtag_user { lt_user };"));
		QVERIFY(header.contains("QString lng_plain(now_t);"));
		QVERIFY(header.contains(
			"QString lng_tagged(now_t, lngtag_user, const QString &user, "
			"lngtag_chat, const QString &chat);"));
		QVERIFY(header.contains(
			"QString lng_count(now_t, lngtag_count, double count, "
			"lngtag_user, const QString &user);"));
		QVERIFY(header.contains("kKeysCount = 3"));
		const auto &source = files[1].content;
		QVERIFY(source.contains("Lang::GetPlural(Lang::Key::lng_count, count)"));
		QVERIFY(source.contains("\"{count} message from {user}\""));
		// Persian is emitted as escaped UTF-8 bytes, safe for any compiler.
		QVERIFY(!source.contains(QByteArray("ساده")));
		QVERIFY(source.contains("\\330\\263"));
	}

};

QTEST_APPLESS_MAIN(TestLangCodegen)
#include "test_lang_codegen.moc"
