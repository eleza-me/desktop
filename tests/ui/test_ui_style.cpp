// The style runtime and a few widgets: palette switching, fonts, icons, animations.
#include "ui/effects/animations.h"
#include "ui/image/image_prepare.h"
#include "ui/image/userpic.h"
#include "ui/style/style_core.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
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
		// What does not fit is cut from the insertion, not from the end.
		field->setText(QStringLiteral("abc"));
		const auto edit = field->findChild<QTextEdit*>();
		QVERIFY(edit != nullptr);
		auto caret = edit->textCursor();
		caret.setPosition(1);
		edit->setTextCursor(caret);
		field->insertText(QStringLiteral("XYZW"));
		QCOMPARE(field->text(), QStringLiteral("aXYbc"));
		// The limit is in code points and a surrogate pair is never split.
		const auto smile = QString::fromUcs4(U"\U0001F600");
		field->setText(smile + smile + smile + smile + smile + smile);
		QCOMPARE(field->text(), smile + smile + smile + smile + smile);
		field->setText(QStringLiteral("abcd"));
		field->setCursorToEnd();
		field->insertText(smile + smile);
		QCOMPARE(field->text(), QStringLiteral("abcd") + smile);
		field->setMaxLength(2);
		QCOMPARE(field->text(), QStringLiteral("ab"));
		field->setMaxLength(5);

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

	void toastAction() {
		const auto check = [](bool rtl) {
			style::SetRightToLeft(rtl);
			auto window = QWidget();
			window.resize(600, 400);
			window.show();
			auto undone = 0;
			auto config = Ui::Toast::Config();
			config.text = QStringLiteral("Repost removed");
			config.action = QStringLiteral("Undo");
			config.actionClicked = [&] { ++undone; };
			Ui::Toast::Show(&window, std::move(config));
			const auto toast = window.findChild<Ui::RpWidget*>(
				QString(),
				Qt::FindDirectChildrenOnly);
			QVERIFY(toast != nullptr);
			QVERIFY(!toast->testAttribute(Qt::WA_TransparentForMouseEvents));
			const auto &st = st::defaultToast;
			const auto body = toast->rect().marginsRemoved(st.shadow.extend);
			const auto y = body.center().y();
			// The action sits at the end of the line: on the left in RTL.
			const auto text = rtl
				? QPoint(body.right() - st.padding.left() - 2, y)
				: QPoint(body.x() + st.padding.left() + 2, y);
			const auto action = rtl
				? QPoint(body.x() + st.padding.right() + 2, y)
				: QPoint(body.right() - st.padding.right() - 2, y);
			QTest::mouseClick(toast, Qt::LeftButton, {}, text);
			QCOMPARE(undone, 0);
			QTest::mouseClick(toast, Qt::LeftButton, {}, action);
			QCOMPARE(undone, 1);
			// The toast hides on the click: a second one does nothing.
			QTest::mouseClick(toast, Qt::LeftButton, {}, action);
			QCOMPARE(undone, 1);
		};
		check(false);
		check(true);
		style::SetRightToLeft(false);
		// Without an action a toast lets the mouse through.
		auto window = QWidget();
		window.resize(600, 400);
		Ui::Toast::Show(&window, QStringLiteral("Copied"));
		const auto toast = window.findChild<Ui::RpWidget*>(
			QString(),
			Qt::FindDirectChildrenOnly);
		QVERIFY(toast != nullptr);
		QVERIFY(toast->testAttribute(Qt::WA_TransparentForMouseEvents));
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
