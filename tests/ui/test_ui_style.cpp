// The style runtime and a few widgets: palette switching, fonts, icons, animations.
#include "ui/effects/animations.h"
#include "ui/emoji/emoji_pack.h"
#include "ui/image/image_prepare.h"
#include "ui/image/userpic.h"
#include "ui/style/style_core.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtGui/QTextLayout>
#include <QtWidgets/QTextEdit>
#include <QtTest/QtTest>

class TestUiStyle : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void initTestCase() {
		style::Start(style::Scheme::Light);
		style::SetRightToLeft(false);
	}

	void paletteSwitch() {
		QVERIFY(style::Started());
		const auto color = st::bgCanvas; // a handle, stays valid
		QCOMPARE(color->c, QColor(0xFA, 0xF8, 0xF3));
		QCOMPARE(st::overlayScrim->c.alpha(), 102); // 40%
		auto notified = 0;
		auto context = QObject();
		style::OnPaletteChanged(&context, [&] { ++notified; });
		const auto version = style::PaletteVersion();
		style::SetScheme(style::Scheme::Dark);
		QCOMPARE(notified, 1);
		QVERIFY(style::IsDark());
		QVERIFY(style::PaletteVersion() > version);
		QCOMPARE(color->c, QColor(0x0C, 0x0F, 0x0D));
		// Values copied into structs follow the palette too.
		QCOMPARE(st::defaultRoundButton.textBg->c, st::brandPrimary->c);
		QCOMPARE(st::brandPrimary->c, QColor(0x3F, 0xC5, 0x7F));
		style::SetScheme(style::Scheme::Light);
		QCOMPARE(st::brandPrimary->c, QColor(0x01, 0x8A, 0x54));
		QVERIFY(style::FindPaletteColor(QStringLiteral("textPrimary")).has_value());
		QVERIFY(!style::FindPaletteColor(QStringLiteral("nope")).has_value());
	}

	void fonts() {
		QCOMPARE(style::TextFontFamily(), QStringLiteral("Vazirmatn"));
		QCOMPARE(style::DisplayFontFamily(), QStringLiteral("Markazi Text"));
		QCOMPARE(st::normalFont->f.pixelSize(), 14);
		QCOMPARE(st::semiboldFont->f.weight(), QFont::DemiBold);
		QCOMPARE(st::topicFontL->f.families().front(), QStringLiteral("Markazi Text"));
		QVERIFY(st::normalFont->height > 14);
		QCOMPARE(
			st::normalFont->height,
			st::normalFont->ascent + st::normalFont->descent);
		// Shared instances.
		QVERIFY(st::normalFont->semibold() == st::semiboldFont);
		QVERIFY(st::semiboldFont->regular() == st::normalFont);
		QVERIFY(style::font(14, 0, QString()) == st::normalFont);
		const auto text = QString::fromUtf8("سلام دنیا، این یک متن بلند است");
		const auto elided = st::normalFont->elided(text, 60);
		QVERIFY(elided.size() < text.size());
		QVERIFY(st::normalFont->width(elided) <= 60);
	}

	void icons() {
		// Every bundled icon rasterises to something visible, at 1x and 2x.
		const auto names = QDir(QStringLiteral(":/icons")).entryList(
			{ QStringLiteral("*.svg") });
		QVERIFY(names.size() > 100);
		for (const auto &file : names) {
			const auto name = file.left(file.size() - 4);
			for (const auto ratio : { 1., 2. }) {
				const auto pixmap = style::IconPixmap(name, Qt::black, 24, ratio);
				QCOMPARE(pixmap.size(), QSize(24, 24) * ratio);
				const auto image = pixmap.toImage();
				auto opaque = 0;
				for (auto y = 0; y != image.height(); ++y) {
					for (auto x = 0; x != image.width(); ++x) {
						if (qAlpha(image.pixel(x, y)) > 128) {
							++opaque;
						}
					}
				}
				QVERIFY2(opaque > 10, qPrintable(name));
			}
		}
		QCOMPARE(st::menuIconCopy.size(), QSize(20, 20));
		// The tint is applied.
		const auto tinted = style::IconPixmap(
			QStringLiteral("check"),
			QColor(255, 0, 0),
			24,
			1.).toImage();
		auto red = false;
		for (auto y = 0; y != 24 && !red; ++y) {
			for (auto x = 0; x != 24 && !red; ++x) {
				const auto pixel = tinted.pixelColor(x, y);
				red = (pixel.alpha() > 200) && (pixel.red() > 200) && (pixel.green() < 40);
			}
		}
		QVERIFY(red);
	}

	void rtlHelpers() {
		style::SetRightToLeft(true);
		QCOMPARE(style::rtlrect(10, 5, 30, 20, 200), QRect(160, 5, 30, 20));
		QCOMPARE(style::rtlmargins(QMargins(1, 2, 3, 4)), QMargins(3, 2, 1, 4));
		auto parent = Ui::RpWidget();
		parent.resize(300, 100);
		auto child = Ui::RpWidget(&parent);
		child.resize(50, 20);
		child.moveToLeft(10, 0);
		QCOMPARE(child.x(), 240);
		child.moveToRight(10, 0);
		QCOMPARE(child.x(), 10);
		style::SetRightToLeft(false);
		child.moveToLeft(10, 0);
		QCOMPARE(child.x(), 10);
		QCOMPARE(style::rtlrect(10, 5, 30, 20, 200), QRect(10, 5, 30, 20));
	}

	void animations() {
		auto animation = Ui::Animations::Simple();
		QCOMPARE(animation.value(3.), 3.);
		auto calls = 0;
		animation.start([&] { ++calls; }, 0., 1., 60, anim::linear);
		QVERIFY(animation.animating());
		QTRY_VERIFY_WITH_TIMEOUT(!animation.animating(), 2000);
		QVERIFY(calls >= 1);
		QCOMPARE(animation.value(1.), 1.);

		// A callback may destroy the animation it belongs to.
		auto owned = std::make_unique<Ui::Animations::Simple>();
		auto destroyed = false;
		owned->start([&] {
			owned = nullptr;
			destroyed = true;
		}, 0., 1., 40, anim::linear);
		QTRY_VERIFY_WITH_TIMEOUT(destroyed, 2000);

		Ui::SetAnimationsDisabled(true);
		auto instant = Ui::Animations::Simple();
		instant.start([] {}, 0., 1., 500, anim::linear);
		QVERIFY(!instant.animating());
		QCOMPARE(instant.value(1.), 1.);
		Ui::SetAnimationsDisabled(false);

		QCOMPARE(anim::standard(0.), 0.);
		QVERIFY(qAbs(anim::standard(1.) - 1.) < 0.001);
		QVERIFY(anim::standard(0.5) > 0.8); // fast start, long tail
	}

	void widgets() {
		auto window = Ui::RpWidget();
		window.resize(400, 300);

		const auto button = new Ui::RoundButton(
			&window,
			QStringLiteral("Send"),
			st::defaultRoundButton);
		QCOMPARE(button->height(), st::defaultRoundButton.height);
		QVERIFY(button->width() >= st::defaultRoundButton.minWidth);
		auto clicks = 0;
		button->setClickedCallback([&] { ++clicks; });
		button->click();
		button->setDisabled(true);
		button->click();
		QCOMPARE(clicks, 1);

		const auto label = new Ui::FlatLabel(
			&window,
			QStringLiteral("one two three four five six seven eight nine ten"));
		const auto natural = label->width();
		QCOMPARE(natural, label->naturalWidth());
		label->resizeToWidth(natural / 3);
		QVERIFY(label->height() >= 3 * st::normalFont->height);
		label->setMaxLines(1);
		QCOMPARE(label->height(), st::normalFont->height);

		const auto scroll = new Ui::ScrollArea(&window);
		scroll->setGeometry(0, 0, 200, 100);
		const auto layout = scroll->setOwnedWidget(
			Ui::object_ptr<Ui::VerticalLayout>(scroll));
		for (auto i = 0; i != 10; ++i) {
			layout->add(Ui::object_ptr<Ui::FixedHeightWidget>(layout, 40));
		}
		layout->resizeToWidth(200);
		QCOMPARE(layout->count(), 10);
		QCOMPARE(layout->height(), 400);
		QCoreApplication::processEvents();
		QCOMPARE(scroll->scrollTopMax(), 300);
		auto nearBottom = 0;
		QObject::connect(scroll, &Ui::ScrollArea::nearBottom, [&] { ++nearBottom; });
		scroll->scrollToY(300);
		QCOMPARE(scroll->scrollTop(), 300);
		QVERIFY(scroll->isAtBottom());
		QVERIFY(nearBottom > 0);
		scroll->scrollToY(1000);
		QCOMPARE(scroll->scrollTop(), 300); // clamped
		QCOMPARE(layout->y(), -300);

		const auto field = new Ui::InputField(
			&window,
			st::defaultInputField,
			QStringLiteral("Name"));
		field->setMaxLength(5);
		auto changes = 0;
		QObject::connect(field, &Ui::InputField::changed, [&] { ++changes; });
		field->setText(QStringLiteral("abcdefgh"));
		QCOMPARE(field->text(), QStringLiteral("abcde"));
		QVERIFY(changes >= 1);

		const auto code = new Ui::CodeInput(&window, 5);
		auto finished = QString();
		QObject::connect(code, &Ui::CodeInput::finished, [&](const QString &value) {
			finished = value;
		});
		code->setCode(QString::fromUtf8("۱۲٣45"));
		QCOMPARE(code->code(), QStringLiteral("12345"));
		QCOMPARE(finished, QStringLiteral("12345"));
		QCOMPARE(Ui::LatinizeDigits(QString::fromUtf8("۰۹۱۲")), QStringLiteral("0912"));
	}

	void inputFieldEmoji() {
		// The editor keeps its text; an emoji of the pack takes the place of
		// its picture, whatever the fonts have for it.
		QVERIFY(Ui::Emoji::Available());
		auto window = QWidget();
		window.resize(400, 100);
		const auto field = new Ui::InputField(
			&window,
			st::defaultInputField,
			QString(),
			QString::fromUtf8("a😀b👨‍👩‍👧c"));
		field->resizeToWidth(380);
		QCOMPARE(field->text(), QString::fromUtf8("a😀b👨‍👩‍👧c"));
		const auto document = field->rawTextEdit()->document();
		document->documentLayout()->documentSize(); // laid out
		const auto block = document->firstBlock();
		QVERIFY(block.layout() && block.layout()->lineCount() == 1);
		const auto line = block.layout()->lineAt(0);
		const auto width = [&](int from, int till) {
			return std::abs(line.cursorToX(till) - line.cursorToX(from));
		};
		const auto one = width(1, 3);
		const auto family = width(4, 4 + int(QString::fromUtf8("👨‍👩‍👧").size()));
		QVERIFY(one > 10);
		QVERIFY2(std::abs(one - family) < 1., qPrintable(QString::number(one) + u' ' + QString::number(family)));
		// Typing keeps it so.
		field->insertText(QString::fromUtf8("🇮🇷"));
		QVERIFY(field->text().endsWith(QString::fromUtf8("🇮🇷")));
	}

	void images() {
		auto image = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::red);
		const auto round = Ui::Images::Round(image, 16);
		QCOMPARE(qAlpha(round.pixel(0, 0)), 0);
		QCOMPARE(qAlpha(round.pixel(32, 32)), 255);
		const auto userpic = Ui::PrepareUserpic(image, 40, 2.);
		QCOMPARE(userpic.size(), QSize(80, 80));
		QCOMPARE(qAlpha(userpic.pixel(1, 1)), 0);
		// Stable across runs and platforms.
		QCOMPARE(
			Ui::UserpicColorIndex(QStringLiteral("u1")),
			Ui::UserpicColorIndex(QStringLiteral("u1")));
		QVERIFY(Ui::UserpicColorIndex(QStringLiteral("some-id")) < 8);
		QCOMPARE(Ui::UserpicInitial(QStringLiteral("  eleza")), QStringLiteral("E"));
		auto mixed = QImage(32, 32, QImage::Format_ARGB32_Premultiplied);
		mixed.fill(Qt::black);
		for (auto y = 0; y != 32; ++y) {
			for (auto x = 16; x != 32; ++x) {
				mixed.setPixel(x, y, qRgba(255, 255, 255, 255));
			}
		}
		const auto blurred = Ui::Images::Blur(mixed, 6);
		const auto middle = qRed(blurred.pixel(16, 16));
		QVERIFY(middle > 60 && middle < 200); // the edge became a gradient
	}

};

QTEST_MAIN(TestUiStyle)
#include "test_ui_style.moc"
