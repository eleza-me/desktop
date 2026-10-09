// The .style language: parsing, inheritance, references, error messages.
#include "style/style_generator.h"
#include "style/style_parser.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtTest/QtTest>

using namespace codegen::style;

class TestStyleCodegen : public QObject {
	Q_OBJECT

private:
	Palette palette() {
		auto result = Palette();
		auto error = Error();
		const auto ok = ParsePaletteContent(
			QStringLiteral("/virtual/colors.palette"),
			QStringLiteral(
				"windowBg: #ffffff | #000000; // comment\n"
				"windowFg: #112233 | #AABBCC80;\n"
				"accentFg: #018A54;\n"
				"linkFg: accentFg;\n"),
			result,
			error);
		if (!ok) {
			qFatal("palette: %s", qPrintable(error.format()));
		}
		return result;
	}

	std::shared_ptr<Module> parse(const QString &content, Error &error) {
		const auto colors = palette();
		return ParseModuleContent(
			QStringLiteral("/virtual/test.style"),
			content,
			Options(),
			colors,
			error);
	}

	QString failure(const QString &content) {
		auto error = Error();
		const auto module = parse(content, error);
		return module ? QString() : error.format();
	}

private Q_SLOTS:
	void paletteValues() {
		const auto colors = palette();
		QCOMPARE(colors.colors.size(), 4);
		QCOMPARE(colors.colors[0].light, 0xFFFFFFFFU);
		QCOMPARE(colors.colors[0].dark, 0x000000FFU);
		QCOMPARE(colors.colors[1].dark, 0xAABBCC80U);
		// One value applies to both schemes.
		QCOMPARE(colors.colors[2].light, 0x018A54FFU);
		QCOMPARE(colors.colors[2].dark, 0x018A54FFU);
		// A reference copies both values.
		QCOMPARE(colors.colors[3].light, 0x018A54FFU);
		QCOMPARE(colors.indexByName.value(QStringLiteral("windowFg")), 1);
	}

	void paletteErrors() {
		auto colors = Palette();
		auto error = Error();
		QVERIFY(!ParsePaletteContent(
			QStringLiteral("p"),
			QStringLiteral("a: #fff;"),
			colors,
			error));
		QVERIFY(error.text.contains(QStringLiteral("#rrggbb")));
		QVERIFY(!ParsePaletteContent(
			QStringLiteral("p"),
			QStringLiteral("a: #ffffff;\na: #000000;"),
			colors,
			error));
		QCOMPARE(error.where.line, 2);
		QVERIFY(error.text.contains(QStringLiteral("twice")));
	}

	void scalarValues() {
		auto error = Error();
		const auto module = parse(QStringLiteral(
			"rowHeight: 72px;\n"
			"count: 3;\n"
			"ratio: 1.5;\n"
			"flag: true;\n"
			"name: \"hello \\\"world\\\"\";\n"
			"fade: 150ms;\n"
			"bg: windowBg;\n"
			"literal: #11223344;\n"
			"pad: margins(1px, 2px, 3px, 4px);\n"
			"box: size(10px, 20px);\n"
			"shift: point(-3px, 0px);\n"
			"where: align(topright);\n"
			"titleFont: font(15px semibold);\n"
			"serif: font(26px bold \"Markazi Text\");\n"
			"sendIcon: icon {{ \"send\", windowFg }};\n"
			"smallIcon: icon {{ \"send:mirror-ltr\", literal, 16px }};\n"),
			error);
		QVERIFY2(module != nullptr, qPrintable(error.format()));
		const auto value = [&](const char *name) {
			const auto found = module->findVariable(QString::fromLatin1(name));
			Q_ASSERT(found != nullptr);
			return found->value;
		};
		QCOMPARE(value("rowHeight").type.kind, TypeKind::Pixels);
		QCOMPARE(value("rowHeight").intValue, 72);
		QCOMPARE(value("count").type.kind, TypeKind::Int);
		QCOMPARE(value("ratio").doubleValue, 1.5);
		QCOMPARE(value("flag").intValue, 1);
		QCOMPARE(value("name").stringValue, QStringLiteral("hello \"world\""));
		QCOMPARE(value("fade").type.kind, TypeKind::Duration);
		QCOMPARE(value("fade").intValue, 150);
		QVERIFY(value("bg").colorFromPalette);
		QCOMPARE(value("bg").colorName, QStringLiteral("windowBg"));
		QVERIFY(!value("literal").colorFromPalette);
		QCOMPARE(value("literal").colorRgba, 0x11223344U);
		QCOMPARE(value("pad").numbers[3], 4);
		QCOMPARE(value("box").numbers[1], 20);
		QCOMPARE(value("shift").numbers[0], -3);
		QCOMPARE(value("where").stringValue, QStringLiteral("topright"));
		QCOMPARE(value("titleFont").fontSize, 15);
		QCOMPARE(value("titleFont").fontFlags, int(FontSemibold));
		QCOMPARE(value("serif").fontFamily, QStringLiteral("Markazi Text"));
		QCOMPARE(value("serif").fontFlags, int(FontBold));
		QCOMPARE(value("sendIcon").stringValue, QStringLiteral("send"));
		QCOMPARE(value("sendIcon").iconSize, 24);
		QVERIFY(value("sendIcon").iconColor->colorFromPalette);
		QCOMPARE(value("smallIcon").iconSize, 16);
		QCOMPARE(value("smallIcon").iconColor->colorRgba, 0x11223344U);
	}

	void structsAndInheritance() {
		auto error = Error();
		const auto module = parse(QStringLiteral(
			"Ripple { color: color; duration: duration; }\n"
			"Button {\n"
			"	height: pixels;\n"
			"	font: font;\n"
			"	textFg: color;\n"
			"	ripple: Ripple;\n"
			"	scale: double;\n"
			"}\n"
			"baseFont: font(14px);\n"
			"defaultRipple: Ripple { color: windowFg; duration: 200ms; }\n"
			"defaultButton: Button {\n"
			"	height: 36px;\n"
			"	font: baseFont;\n"
			"	textFg: accentFg;\n"
			"	ripple: defaultRipple;\n"
			"	scale: 1;\n" // int where double is expected
			"}\n"
			"bigButton: Button(defaultButton) {\n"
			"	height: 44px;\n"
			"	ripple: Ripple(defaultRipple) { duration: 0; }\n"
			"}\n"
			"copyButton: bigButton;\n"),
			error);
		QVERIFY2(module != nullptr, qPrintable(error.format()));
		QCOMPARE(module->structs.size(), 2);
		const auto big = module->findVariable(QStringLiteral("bigButton"));
		QVERIFY(big != nullptr);
		QCOMPARE(big->value.type.structName, QStringLiteral("Button"));
		QCOMPARE(big->value.fields.size(), size_t(5));
		QCOMPARE(big->value.fields[0].intValue, 44); // overridden
		QCOMPARE(big->value.fields[1].fontSize, 14); // inherited
		QCOMPARE(big->value.fields[2].colorName, QStringLiteral("accentFg"));
		QCOMPARE(big->value.fields[3].fields[1].intValue, 0); // nested override
		QCOMPARE(
			big->value.fields[3].fields[0].colorName,
			QStringLiteral("windowFg"));
		QCOMPARE(big->value.fields[4].type.kind, TypeKind::Double);
		const auto base = module->findVariable(QStringLiteral("defaultButton"));
		QCOMPARE(base->value.fields[0].intValue, 36); // the parent is untouched
		QCOMPARE(base->value.fields[3].fields[1].intValue, 200);
		const auto copy = module->findVariable(QStringLiteral("copyButton"));
		QCOMPARE(copy->value.fields[0].intValue, 44);
	}

	void usingFiles() {
		auto dir = QTemporaryDir();
		QVERIFY(dir.isValid());
		const auto write = [&](const QString &name, const QByteArray &content) {
			QDir(dir.path()).mkpath(QFileInfo(dir.filePath(name)).path());
			auto file = QFile(dir.filePath(name));
			QVERIFY(file.open(QIODevice::WriteOnly));
			file.write(content);
		};
		write(QStringLiteral("ui/basic.style"),
			"Ripple { color: color; }\n"
			"normalFont: font(14px);\n"
			"defaultRipple: Ripple { color: windowFg; }\n");
		write(QStringLiteral("app/window.style"),
			"using \"ui/basic.style\";\n"
			"titleFont: font(normalFont bold);\n"
			"railRipple: Ripple(defaultRipple) { color: accentFg; }\n");
		auto options = Options();
		options.includePaths.push_back(dir.path());
		auto error = Error();
		const auto colors = palette();
		const auto module = ParseModule(
			dir.filePath(QStringLiteral("app/window.style")),
			options,
			colors,
			error);
		QVERIFY2(module != nullptr, qPrintable(error.format()));
		QCOMPARE(module->name, QStringLiteral("window"));
		QCOMPARE(module->includes.size(), 1);
		QCOMPARE(module->includes[0]->name, QStringLiteral("basic"));
		QCOMPARE(module->variables.size(), 2); // only its own
		QCOMPARE(module->variables[0].value.fontSize, 14);
		QCOMPARE(module->variables[0].value.fontFlags, int(FontBold));

		const auto files = GenerateModule(*module);
		QCOMPARE(files.size(), 2);
		QCOMPARE(files[0].name, QStringLiteral("style_window.h"));
		QVERIFY(files[0].content.contains("#include \"styles/style_basic.h\""));
		QVERIFY(files[0].content.contains(
			"extern const style::font &titleFont;"));
		QVERIFY(files[0].content.contains(
			"extern const style::Ripple &railRipple;"));
		QVERIFY(files[1].content.contains("init_style_basic();"));
		QVERIFY(files[1].content.contains("st::accentFg"));

		// A missing file is reported at the `using` line.
		write(QStringLiteral("app/broken.style"),
			"\n\nusing \"ui/missing.style\";\n");
		QVERIFY(!ParseModule(
			dir.filePath(QStringLiteral("app/broken.style")),
			options,
			colors,
			error));
		QCOMPARE(error.where.line, 3);
		QVERIFY(error.text.contains(QStringLiteral("missing.style")));
	}

	void generatedPalette() {
		const auto files = GeneratePalette(palette());
		QCOMPARE(files.size(), 2);
		QVERIFY(files[0].content.contains("extern const style::color windowBg;"));
		QVERIFY(files[0].content.contains("kPaletteColorsCount = 4"));
		QVERIFY(files[1].content.contains(
			"{ \"windowFg\", 0x112233FFU, 0xAABBCC80U }"));
	}

	void errors() {
		// Each message carries file:line and says what is wrong.
		auto message = failure(QStringLiteral("a: 12px;\nb: unknownThing;\n"));
		QVERIFY2(message.contains(QStringLiteral("test.style:2: error:")),
			qPrintable(message));
		QVERIFY(message.contains(QStringLiteral("unknownThing")));

		message = failure(QStringLiteral("a: 12px\nb: 1px;"));
		QVERIFY2(message.contains(QStringLiteral("expected ';'")),
			qPrintable(message));

		message = failure(QStringLiteral("a: 1px;\na: 2px;"));
		QVERIFY(message.contains(QStringLiteral(":2: error:")));
		QVERIFY(message.contains(QStringLiteral("already defined")));

		message = failure(QStringLiteral("windowBg: 2px;"));
		QVERIFY(message.contains(QStringLiteral("palette color")));

		message = failure(QStringLiteral(
			"B { height: pixels; fg: color; }\n"
			"b: B { height: 10px; }\n"));
		QVERIFY2(message.contains(QStringLiteral("missing field(s) of 'B': fg")),
			qPrintable(message));

		message = failure(QStringLiteral(
			"B { height: pixels; }\n"
			"b: B { height: windowBg; }\n"));
		QVERIFY2(message.contains(QStringLiteral("expected pixels, got color")),
			qPrintable(message));

		message = failure(QStringLiteral(
			"B { height: pixels; }\n"
			"b: B { width: 1px; }\n"));
		QVERIFY(message.contains(QStringLiteral("no field 'width'")));

		message = failure(QStringLiteral(
			"A { x: pixels; }\nB { y: pixels; }\n"
			"a: A { x: 1px; }\n"
			"b: B(a) { }\n"));
		QVERIFY2(message.contains(QStringLiteral("parent 'a' is A, expected B")),
			qPrintable(message));

		message = failure(QStringLiteral("f: font(12px wide);"));
		QVERIFY(message.contains(QStringLiteral("unknown font flag 'wide'")));

		message = failure(QStringLiteral("x: 5em;"));
		QVERIFY(message.contains(QStringLiteral("unknown unit 'em'")));

		message = failure(QStringLiteral("x: #12;"));
		QVERIFY(message.contains(QStringLiteral("bad color")));

		message = failure(QStringLiteral("x: 1px; /* never closed"));
		QVERIFY(message.contains(QStringLiteral("unterminated comment")));

		message = failure(QStringLiteral("i: icon {{ \"send\" }};"));
		QVERIFY2(message.contains(QStringLiteral("icon color")),
			qPrintable(message));
	}

};

QTEST_APPLESS_MAIN(TestStyleCodegen)
#include "test_style_codegen.moc"
