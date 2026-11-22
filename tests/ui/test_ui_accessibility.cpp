// The keyboard and screen readers on the custom painted widgets
// (ui/accessibility.h): Tab stops, Space / Enter, the interfaces of buttons,
// two-state controls and painted items.
#include "ui/accessibility.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_widgets.h"

#include <QtGui/QAccessible>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>

namespace {

class Rail final
	: public Ui::RpWidget
	, public Ui::Accessibility::ItemsSource {
public:
	using RpWidget::RpWidget;

	int pressed = -1;

	int accessibilityItemsCount() const override {
		return 3;
	}
	Ui::Accessibility::Item accessibilityItem(int index) const override {
		return {
			.name = QStringLiteral("Tab %1").arg(index),
			.description = QString(),
			.role = QAccessible::PageTab,
			.rect = QRect(0, index * 10, 10, 10),
			.selected = (index == 1),
		};
	}
	void accessibilityItemPress(int index) override {
		pressed = index;
	}
	QAccessible::Role accessibilityRole() const override {
		return QAccessible::PageTabList;
	}

};

[[nodiscard]] QAccessibleInterface *Interface(QObject *object) {
	return QAccessible::queryAccessibleInterface(object);
}

void Press(QAccessibleInterface *iface, const QString &action) {
	QVERIFY(iface->actionInterface() != nullptr);
	QVERIFY(iface->actionInterface()->actionNames().contains(action));
	iface->actionInterface()->doAction(action);
}

} // namespace

class TestUiAccessibility : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void initTestCase() {
		style::Start(style::Scheme::Light);
		style::SetRightToLeft(false);
	}

	void tabStops() {
		auto parent = QWidget();
		// Nothing to do: no Tab stop (a userpic, an information row).
		const auto idle = new Ui::RoundButton(
			&parent,
			QStringLiteral("Idle"),
			st::defaultRoundButton);
		QCOMPARE(idle->focusPolicy(), Qt::NoFocus);
		QCOMPARE(idle->accessibilityRole(), QAccessible::StaticText);
		idle->setClickedCallback([] {});
		QCOMPARE(idle->focusPolicy(), Qt::TabFocus);
		QCOMPARE(idle->accessibilityRole(), QAccessible::Button);

		const auto connected = new Ui::IconButton(&parent, st::defaultIconButton);
		QObject::connect(
			connected,
			&Ui::AbstractButton::clickedSignal,
			connected,
			[] {});
		QCOMPARE(connected->focusPolicy(), Qt::TabFocus);
		// Click focus would take the caret from the fields.
		QVERIFY(!(connected->focusPolicy() & Qt::ClickFocus));

		const auto off = new Ui::IconButton(&parent, st::defaultIconButton);
		off->setKeyboardFocusable(false);
		off->setClickedCallback([] {});
		QCOMPARE(off->focusPolicy(), Qt::NoFocus);

		// A policy somebody set by hand stays.
		const auto strong = new Ui::IconButton(&parent, st::defaultIconButton);
		strong->setFocusPolicy(Qt::StrongFocus);
		strong->setClickedCallback([] {});
		QCOMPARE(strong->focusPolicy(), Qt::StrongFocus);

		// Two-state controls do something by themselves.
		const auto check = new Ui::Checkbox(&parent, QStringLiteral("C"));
		QCOMPARE(check->focusPolicy(), Qt::TabFocus);
		const auto toggle = new Ui::Toggle(&parent);
		QCOMPARE(toggle->focusPolicy(), Qt::TabFocus);
	}

	void buttonInterface() {
		auto parent = QWidget();
		const auto round = new Ui::RoundButton(
			&parent,
			QStringLiteral("Send"),
			st::defaultRoundButton);
		auto clicks = 0;
		round->setClickedCallback([&] { ++clicks; });
		const auto iface = Interface(round);
		QVERIFY(iface != nullptr);
		QCOMPARE(iface->role(), QAccessible::Button);
		QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Send"));
		round->setText(QStringLiteral("Sent"));
		QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Sent"));
		Press(iface, QAccessibleActionInterface::pressAction());
		QCOMPARE(clicks, 1);
		QVERIFY(iface->state().focusable);

		round->setDisabled(true);
		QVERIFY(iface->state().disabled);
		QVERIFY(!iface->actionInterface()->actionNames().contains(
			QAccessibleActionInterface::pressAction()));
		iface->actionInterface()->doAction(
			QAccessibleActionInterface::pressAction());
		QCOMPARE(clicks, 1);

		// An icon button is named by its tooltip, or by accessibleName.
		const auto icon = new Ui::IconButton(&parent, st::defaultIconButton);
		icon->setClickedCallback([] {});
		Ui::InstallTooltip(icon, [] { return QStringLiteral("Close"); });
		QCOMPARE(Ui::TooltipText(icon), QStringLiteral("Close"));
		QCOMPARE(Interface(icon)->text(QAccessible::Name), QStringLiteral("Close"));
		icon->setAccessibleName(QStringLiteral("Close the chat"));
		QCOMPARE(
			Interface(icon)->text(QAccessible::Name),
			QStringLiteral("Close the chat"));
		QCOMPARE(
			Interface(icon)->text(QAccessible::Description),
			QStringLiteral("Close"));
	}

	void twoStateInterfaces() {
		auto parent = QWidget();
		const auto check = new Ui::Checkbox(&parent, QStringLiteral("Remember"));
		const auto iface = Interface(check);
		QCOMPARE(iface->role(), QAccessible::CheckBox);
		QCOMPARE(iface->text(QAccessible::Name), QStringLiteral("Remember"));
		QVERIFY(iface->state().checkable);
		QVERIFY(!iface->state().checked);
		Press(iface, QAccessibleActionInterface::toggleAction());
		QVERIFY(check->checked());
		QVERIFY(iface->state().checked);

		const auto row = new Ui::SettingsButton(&parent, QStringLiteral("Sounds"));
		row->setToggle(true);
		const auto rowIface = Interface(row);
		QCOMPARE(rowIface->role(), QAccessible::CheckBox);
		QVERIFY(rowIface->state().checked);
		auto changed = 0;
		QObject::connect(
			row,
			&Ui::SettingsButton::toggledChanged,
			row,
			[&](bool) { ++changed; });
		Press(rowIface, QAccessibleActionInterface::toggleAction());
		QVERIFY(!row->toggled());
		QVERIFY(!rowIface->state().checked);
		QCOMPARE(changed, 1);

		const auto info = new Ui::SettingsButton(&parent, QStringLiteral("Version"));
		info->setValueText(QStringLiteral("1.0"));
		QCOMPARE(Interface(info)->role(), QAccessible::StaticText);
		QCOMPARE(Interface(info)->text(QAccessible::Value), QStringLiteral("1.0"));

		const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
		const auto a = new Ui::Radiobutton(&parent, group, 0, QStringLiteral("A"));
		const auto b = new Ui::Radiobutton(&parent, group, 1, QStringLiteral("B"));
		QCOMPARE(Interface(b)->role(), QAccessible::RadioButton);
		QVERIFY(Interface(a)->state().checked);
		QVERIFY(!Interface(b)->state().checked);
		Press(Interface(b), QAccessibleActionInterface::pressAction());
		QCOMPARE(group->value(), 1);
		QVERIFY(Interface(b)->state().checked);
		QVERIFY(!Interface(a)->state().checked);
	}

	void inputNames() {
		auto parent = QWidget();
		const auto field = new Ui::InputField(
			&parent,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			QStringLiteral("Name"));
		QCOMPARE(field->rawTextEdit()->accessibleName(), QStringLiteral("Name"));
		field->setPlaceholder(QStringLiteral("Bio"));
		QCOMPARE(field->rawTextEdit()->accessibleName(), QStringLiteral("Bio"));
	}

	void keyboard() {
		auto window = QWidget();
		window.resize(300, 200);
		const auto field = new Ui::InputField(
			&window,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			QStringLiteral("Field"));
		field->setGeometry(0, 0, 200, 40);
		auto clicks = 0;
		const auto button = new Ui::RoundButton(
			&window,
			QStringLiteral("OK"),
			st::defaultRoundButton);
		button->move(0, 100);
		button->setClickedCallback([&] { ++clicks; });
		window.show();
		window.activateWindow();
		QVERIFY(QTest::qWaitForWindowActive(&window));
		field->setFocusFast();
		QTRY_VERIFY(field->hasFocus());

		// Tab from the field reaches the button; Space and Enter press it.
		QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
		QTRY_VERIFY(button->hasFocus());
		QTest::keyClick(button, Qt::Key_Space);
		QCOMPARE(clicks, 1);
		QTest::keyClick(button, Qt::Key_Return);
		QCOMPARE(clicks, 2);
		// A held key clicks once.
		QTest::keyPress(button, Qt::Key_Space);
		QCOMPARE(clicks, 3);
		{
			auto repeat = QKeyEvent(
				QEvent::KeyPress,
				Qt::Key_Space,
				Qt::NoModifier,
				QStringLiteral(" "),
				true);
			QApplication::sendEvent(button, &repeat);
		}
		QCOMPARE(clicks, 3);
		// Ctrl+Enter is somebody else's shortcut.
		QTest::keyClick(button, Qt::Key_Return, Qt::ControlModifier);
		QCOMPARE(clicks, 3);

		// A focus that did not come from the keyboard does not press.
		field->setFocusFast();
		QTRY_VERIFY(field->hasFocus());
		QTest::mouseClick(field, Qt::LeftButton); // forgets the Tab
		button->setFocus(Qt::TabFocusReason); // as Qt does when a field hides
		QTest::keyClick(button, Qt::Key_Space);
		QCOMPARE(clicks, 3);
		// ...and the focus goes to the field Qt would have chosen before.
		QTRY_VERIFY(field->hasFocus());
	}

	void activationFocus() {
		// Qt gives a window that becomes active to its first Tab stop: the
		// button comes first here, the field gets the focus as before.
		auto window = QWidget();
		window.resize(300, 200);
		const auto button = new Ui::RoundButton(
			&window,
			QStringLiteral("OK"),
			st::defaultRoundButton);
		button->setClickedCallback([] {});
		const auto field = new Ui::InputField(
			&window,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			QStringLiteral("Field"));
		field->setGeometry(0, 100, 200, 40);
		window.show();
		window.activateWindow();
		QVERIFY(QTest::qWaitForWindowActive(&window));
		QTRY_VERIFY(field->hasFocus());
		QVERIFY(!button->hasFocus());
	}

	void readingOrder() {
		// The main column was created first, the side column (leading) after.
		auto window = QWidget();
		window.resize(400, 200);
		const auto main = new QWidget(&window);
		main->setGeometry(200, 0, 200, 200);
		const auto side = new QWidget(&window);
		side->setGeometry(0, 0, 200, 200);
		const auto make = [](QWidget *parent, int y) {
			const auto result = new Ui::RoundButton(
				parent,
				QStringLiteral("B"),
				st::defaultRoundButton);
			result->setClickedCallback([] {});
			result->move(0, y);
			return result;
		};
		const auto main1 = make(main, 0);
		const auto side1 = make(side, 0);
		const auto side2 = make(side, 50);
		window.show();
		window.activateWindow();
		QVERIFY(QTest::qWaitForWindowActive(&window));

		const auto columns = Ui::Accessibility::ColumnsInReadingOrder(&window);
		QCOMPARE(int(columns.size()), 2);
		QCOMPARE(columns[0], side);
		QCOMPARE(columns[1], main);

		side1->setFocus(Qt::OtherFocusReason);
		QVERIFY(Ui::Accessibility::FocusNextInRegions(columns, true));
		QCOMPARE(QApplication::focusWidget(), side2);
		QVERIFY(Ui::Accessibility::FocusNextInRegions(columns, true));
		QCOMPARE(QApplication::focusWidget(), main1);
		// Around, and back.
		QVERIFY(Ui::Accessibility::FocusNextInRegions(columns, true));
		QCOMPARE(QApplication::focusWidget(), side1);
		QVERIFY(Ui::Accessibility::FocusNextInRegions(columns, false));
		QCOMPARE(QApplication::focusWidget(), main1);
		// A region without Tab stops is skipped; none at all: nothing to do.
		side1->setKeyboardFocusable(false);
		side2->setKeyboardFocusable(false);
		QVERIFY(Ui::Accessibility::FocusNextInRegions(columns, true));
		QCOMPARE(QApplication::focusWidget(), main1);
		main1->setKeyboardFocusable(false);
		QVERIFY(!Ui::Accessibility::FocusNextInRegions(columns, true));
	}

	void items() {
		auto parent = QWidget();
		const auto rail = new Rail(&parent);
		rail->resize(10, 30);
		const auto iface = Interface(rail);
		QVERIFY(iface != nullptr);
		QCOMPARE(iface->role(), QAccessible::PageTabList);
		QCOMPARE(iface->childCount(), 3);
		const auto second = iface->child(1);
		QVERIFY(second != nullptr);
		QCOMPARE(second->text(QAccessible::Name), QStringLiteral("Tab 1"));
		QCOMPARE(second->role(), QAccessible::PageTab);
		QVERIFY(second->state().selected);
		QCOMPARE(second->parent(), iface);
		QCOMPARE(iface->indexOfChild(second), 1);
		QCOMPARE(iface->child(1), second); // cached
		Press(second, QAccessibleActionInterface::pressAction());
		QCOMPARE(rail->pressed, 1);
		QVERIFY(iface->child(3) == nullptr);
	}

};

QTEST_MAIN(TestUiAccessibility)
#include "test_ui_accessibility.moc"
