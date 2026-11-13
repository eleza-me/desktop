// What screen readers (NVDA, Narrator, Orca) see of the custom painted widgets,
// and the keyboard focus ring.
//
// Every Ui::AbstractButton gets an interface by itself (ui/widgets/buttons.h:
// name from its text, its tooltip or accessibleName(), a role, the checked /
// unavailable state and the «press» action). A widget that paints several
// items itself (the navigation rail, the story strip, the rows of a list)
// derives from ItemsSource and shows them as children:
//
//   class Rail final : public Ui::RpWidget, public Ui::Accessibility::ItemsSource {
//       int accessibilityItemsCount() const override { return _items.size(); }
//       Ui::Accessibility::Item accessibilityItem(int index) const override {
//           return { .name = label(index), .role = QAccessible::PageTab, .rect = ... };
//       }
//       void accessibilityItemPress(int index) override { choose(index); }
//   };
//   Ui::Accessibility::ItemFocused(this, index);    // the keyboard moved to it
//   Ui::Accessibility::ItemsChanged(this);          // rows added / removed
//
// All notifications are free while no assistive technology is listening.
#pragma once

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtGui/QAccessible>

#include <vector>

class QPainter;
class QPainterPath;
class QWidget;

namespace Ui {
class AbstractButton;
} // namespace Ui

namespace Ui::Accessibility {

// Registers the interfaces of the widget library (once; also done by the
// first button or items source that is created).
void Install();

struct Item {
	QString name;
	QString description;
	QAccessible::Role role = QAccessible::Button;
	QRect rect; // in the widget's coordinates
	bool selected = false; // the open tab, the open chat
	bool checked = false;
	bool checkable = false;
	bool pressable = true; // accessibilityItemPress() does something
};

class ItemsSource {
public:
	ItemsSource();
	virtual ~ItemsSource() = default;

	[[nodiscard]] virtual int accessibilityItemsCount() const = 0;
	[[nodiscard]] virtual Item accessibilityItem(int index) const = 0;
	virtual void accessibilityItemPress(int index) = 0;
	// The item the keyboard is on, -1 for none.
	[[nodiscard]] virtual int accessibilityFocusedItem() const {
		return -1;
	}
	// Of the widget itself: List, PageTabList, ToolBar…
	[[nodiscard]] virtual QAccessible::Role accessibilityRole() const {
		return QAccessible::List;
	}

};

// The keyboard moved to the item `index` of an ItemsSource widget.
void ItemFocused(QWidget *widget, int index);
// The items were added, removed or reordered.
void ItemsChanged(QWidget *widget);
// The name or the state of a widget (or of its item `index`) changed.
void NameChanged(QWidget *widget, int index = -1);
void StateChanged(QWidget *widget, QAccessible::State changed, int index = -1);

// Whether a focus of `reason` that comes now came from the user's keyboard
// (a Tab they pressed, a shortcut): only then the ring is painted and Space /
// Enter act. Qt moves the focus on its own too (the window's activation, a
// focused widget that hides), and a key meant for something else must not
// press the button it lands on.
[[nodiscard]] bool KeyboardFocusReason(Qt::FocusReason reason);

// Makes a painted widget (the rail, the story strip) a Tab stop that the
// mouse never focuses, like the buttons.
void SetTabStop(QWidget *widget);
// For such a Tab stop (and a button), from its focusInEvent(): updates
// `keyboardFocus` (whether the focus is the keyboard's) and passes a focus
// that Qt gave by itself on to the widget that had it before such stops
// existed (an input, a section).
void TabStopFocusIn(QWidget *widget, Qt::FocusReason reason, bool &keyboardFocus);

// Tab / Shift+Tab through `regions` (and everything inside them) in this
// order and around, instead of the order the widgets were created in: from a
// focusNextPrevChild() override. False when none of them has a Tab stop.
bool FocusNextInRegions(const std::vector<QWidget*> &regions, bool next);
// The direct children of `parent` that are shown, in reading order (from
// the leading side, then from the top): the columns of a window.
[[nodiscard]] std::vector<QWidget*> ColumnsInReadingOrder(QWidget *parent);

// The focus ring of the app's style, painted inside `rect`.
void PaintFocusRing(QPainter &p, const QRectF &rect, double radius);
void PaintFocusRingEllipse(QPainter &p, const QRectF &rect);
void PaintFocusRingPath(QPainter &p, const QPainterPath &path);

} // namespace Ui::Accessibility
