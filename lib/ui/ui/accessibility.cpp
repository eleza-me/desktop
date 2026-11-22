#include "ui/accessibility.h"

#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_basic.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QStyleHints>
#include <QtWidgets/QAccessibleWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <vector>

namespace Ui::Accessibility {
namespace {

// Qt also moves the focus "with Tab" by itself: when the focused widget is
// hidden (a box closes) the next Tab stop gets it with TabFocusReason. Only a
// Tab the user pressed counts as the keyboard's.
bool TabKeyPressed = false;

class KeyWatcher final : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		switch (event->type()) {
		case QEvent::KeyPress: {
			const auto key = static_cast<QKeyEvent*>(event)->key();
			TabKeyPressed = (key == Qt::Key_Tab) || (key == Qt::Key_Backtab);
		} break;
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		case QEvent::TouchBegin:
			TabKeyPressed = false;
			break;
		default: break;
		}
		return QObject::eventFilter(object, event);
	}

};

// A button of the widget library (and of the app: every AbstractButton).
class ButtonInterface final : public QAccessibleWidget {
public:
	explicit ButtonInterface(AbstractButton *button)
	: QAccessibleWidget(button, QAccessible::Button) {
	}

	[[nodiscard]] AbstractButton *button() const {
		return static_cast<AbstractButton*>(widget());
	}

	QAccessible::Role role() const override {
		return button()->accessibilityRole();
	}

	QString text(QAccessible::Text t) const override {
		const auto button = this->button();
		switch (t) {
		case QAccessible::Name: {
			if (const auto set = button->accessibleName(); !set.isEmpty()) {
				return set;
			} else if (const auto own = button->accessibilityName();
					!own.isEmpty()) {
				return own;
			}
			return TooltipText(button);
		}
		case QAccessible::Description: {
			if (const auto set = button->accessibleDescription();
					!set.isEmpty()) {
				return set;
			}
			// The tooltip tells more than the name when the name is the
			// button's own text.
			const auto tooltip = TooltipText(button);
			const auto name = text(QAccessible::Name);
			return (tooltip != name) ? tooltip : QString();
		}
		case QAccessible::Value:
			return button->accessibilityValue();
		default: break;
		}
		return QAccessibleWidget::text(t);
	}

	QAccessible::State state() const override {
		auto result = QAccessibleWidget::state();
		const auto button = this->button();
		if (button->isDisabled()) {
			result.disabled = true;
		}
		if (const auto checked = button->accessibilityChecked()) {
			result.checkable = true;
			result.checked = *checked;
		}
		return result;
	}

	QStringList actionNames() const override {
		auto result = QAccessibleWidget::actionNames();
		if (!button()->isDisabled()
			&& button()->accessibilityRole() != QAccessible::StaticText
			&& button()->accessibilityRole() != QAccessible::Graphic) {
			// A radio button is chosen, not toggled off again.
			const auto toggle = button()->accessibilityChecked().has_value()
				&& (button()->accessibilityRole() != QAccessible::RadioButton);
			result.prepend(toggle
				? QAccessibleActionInterface::toggleAction()
				: QAccessibleActionInterface::pressAction());
		}
		return result;
	}

	void doAction(const QString &actionName) override {
		if (actionName == QAccessibleActionInterface::pressAction()
			|| actionName == QAccessibleActionInterface::toggleAction()) {
			button()->click();
		} else {
			QAccessibleWidget::doAction(actionName);
		}
	}

	QStringList keyBindingsForAction(const QString &actionName) const override {
		if (actionName == QAccessibleActionInterface::pressAction()
			|| actionName == QAccessibleActionInterface::toggleAction()) {
			return { QStringLiteral("Space") };
		}
		return QAccessibleWidget::keyBindingsForAction(actionName);
	}

};

class ItemsInterface;

// One painted item: it has no QObject of its own.
class ItemInterface final
	: public QAccessibleInterface
	, public QAccessibleActionInterface {
public:
	ItemInterface(QWidget *widget, ItemsSource *source, int index)
	: _widget(widget)
	, _source(source)
	, _index(index) {
	}

	[[nodiscard]] int index() const {
		return _index;
	}
	[[nodiscard]] QWidget *owner() const {
		return _widget.data();
	}

	bool isValid() const override {
		return _widget && (_index < _source->accessibilityItemsCount());
	}
	QObject *object() const override {
		return nullptr;
	}
	QWindow *window() const override {
		return _widget ? _widget->window()->windowHandle() : nullptr;
	}
	QList<QPair<QAccessibleInterface*, QAccessible::Relation>> relations(
			QAccessible::Relation match) const override {
		Q_UNUSED(match);
		return {};
	}
	QAccessibleInterface *focusChild() const override {
		return nullptr;
	}
	QAccessibleInterface *childAt(int x, int y) const override {
		Q_UNUSED(x);
		Q_UNUSED(y);
		return nullptr;
	}
	QAccessibleInterface *parent() const override {
		return _widget
			? QAccessible::queryAccessibleInterface(_widget.data())
			: nullptr;
	}
	QAccessibleInterface *child(int index) const override {
		Q_UNUSED(index);
		return nullptr;
	}
	int childCount() const override {
		return 0;
	}
	int indexOfChild(const QAccessibleInterface *child) const override {
		Q_UNUSED(child);
		return -1;
	}

	QString text(QAccessible::Text t) const override {
		if (!isValid()) {
			return QString();
		}
		const auto item = _source->accessibilityItem(_index);
		switch (t) {
		case QAccessible::Name: return item.name;
		case QAccessible::Description: return item.description;
		default: break;
		}
		return QString();
	}
	void setText(QAccessible::Text t, const QString &text) override {
		Q_UNUSED(t);
		Q_UNUSED(text);
	}
	QRect rect() const override {
		if (!isValid()) {
			return QRect();
		}
		const auto local = _source->accessibilityItem(_index).rect;
		return QRect(_widget->mapToGlobal(local.topLeft()), local.size());
	}
	QAccessible::Role role() const override {
		return isValid()
			? _source->accessibilityItem(_index).role
			: QAccessible::NoRole;
	}
	QAccessible::State state() const override {
		auto result = QAccessible::State();
		if (!isValid()) {
			result.invalid = true;
			return result;
		}
		const auto item = _source->accessibilityItem(_index);
		const auto focusable = (_widget->focusPolicy() & Qt::TabFocus) != 0;
		result.focusable = focusable;
		result.focused = _widget->hasFocus()
			&& (_source->accessibilityFocusedItem() == _index);
		result.selectable = (item.role == QAccessible::PageTab)
			|| (item.role == QAccessible::ListItem);
		result.selected = item.selected;
		result.checkable = item.checkable;
		result.checked = item.checked;
		if (!_widget->isVisible()) {
			result.invisible = true;
		} else if (!_widget->visibleRegion().intersects(item.rect)) {
			result.offscreen = true;
		}
		return result;
	}

	void *interface_cast(QAccessible::InterfaceType type) override {
		return (type == QAccessible::ActionInterface)
			? static_cast<QAccessibleActionInterface*>(this)
			: nullptr;
	}

	QStringList actionNames() const override {
		return (isValid() && _source->accessibilityItem(_index).pressable)
			? QStringList{ QAccessibleActionInterface::pressAction() }
			: QStringList();
	}
	void doAction(const QString &actionName) override {
		if (isValid()
			&& actionName == QAccessibleActionInterface::pressAction()
			&& _source->accessibilityItem(_index).pressable) {
			_source->accessibilityItemPress(_index);
		}
	}
	QStringList keyBindingsForAction(
			const QString &actionName) const override {
		Q_UNUSED(actionName);
		return {};
	}

private:
	QPointer<QWidget> _widget;
	ItemsSource *_source = nullptr;
	int _index = 0;

};

class ItemsInterface final : public QAccessibleWidget {
public:
	ItemsInterface(QWidget *widget, ItemsSource *source)
	: QAccessibleWidget(widget, QAccessible::List)
	, _source(source) {
	}
	~ItemsInterface() {
		dropChildren(0);
	}

	// The items past `count` are gone.
	void dropChildren(int count) {
		while (int(_children.size()) > count) {
			QAccessible::deleteAccessibleInterface(_children.back());
			_children.pop_back();
		}
	}

	QAccessible::Role role() const override {
		return _source->accessibilityRole();
	}

	int childCount() const override {
		return items() + QAccessibleWidget::childCount();
	}

	QAccessibleInterface *child(int index) const override {
		if (index < 0) {
			return nullptr;
		}
		const auto count = items();
		if (index >= count) {
			return QAccessibleWidget::child(index - count);
		}
		auto &children = const_cast<ItemsInterface*>(this)->_children;
		while (int(children.size()) <= index) {
			const auto item = new ItemInterface(
				widget(),
				_source,
				int(children.size()));
			children.push_back(QAccessible::registerAccessibleInterface(item));
		}
		return QAccessible::accessibleInterface(children[index]);
	}

	int indexOfChild(const QAccessibleInterface *child) const override {
		if (!child) {
			return -1;
		}
		for (auto i = 0, count = int(_children.size()); i != count; ++i) {
			if (QAccessible::accessibleInterface(_children[i]) == child) {
				return (i < items()) ? i : -1;
			}
		}
		const auto result = QAccessibleWidget::indexOfChild(child);
		return (result >= 0) ? (items() + result) : -1;
	}

	QAccessibleInterface *childAt(int x, int y) const override {
		const auto local = widget()->mapFromGlobal(QPoint(x, y));
		for (auto i = 0, count = items(); i != count; ++i) {
			if (_source->accessibilityItem(i).rect.contains(local)) {
				return child(i);
			}
		}
		return QAccessibleWidget::childAt(x, y);
	}

	QAccessibleInterface *focusChild() const override {
		const auto focused = _source->accessibilityFocusedItem();
		if (widget()->hasFocus() && focused >= 0 && focused < items()) {
			return child(focused);
		}
		return QAccessibleWidget::focusChild();
	}

private:
	[[nodiscard]] int items() const {
		return _source->accessibilityItemsCount();
	}

	ItemsSource *_source = nullptr;
	std::vector<QAccessible::Id> _children;

};

QAccessibleInterface *Factory(const QString &classname, QObject *object) {
	// Asked for each class up the chain: answer only for the object's own
	// class, before Qt's generic QWidget interface is reached.
	if (!object
		|| !object->isWidgetType()
		|| classname != QLatin1String(object->metaObject()->className())) {
		return nullptr;
	}
	const auto widget = static_cast<QWidget*>(object);
	if (const auto button = qobject_cast<AbstractButton*>(widget)) {
		return new ButtonInterface(button);
	} else if (const auto source = dynamic_cast<ItemsSource*>(widget)) {
		return new ItemsInterface(widget, source);
	}
	return nullptr;
}

[[nodiscard]] ItemsInterface *ExistingItems(QWidget *widget) {
	if (!widget || !QAccessible::isActive()) {
		return nullptr;
	}
	return dynamic_cast<ItemsInterface*>(
		QAccessible::queryAccessibleInterface(widget));
}

[[nodiscard]] bool TabStopOnly(const QWidget *widget) {
	return qobject_cast<const AbstractButton*>(widget)
		|| widget->property("_tab_stop_only").toBool();
}

// Before the buttons and the painted lists were Tab stops Qt gave such a
// focus to an input or a section: give it to the one that would have had it.
void PassOnFocus(QWidget *widget) {
	const auto window = widget->window();
	for (auto candidate = widget->nextInFocusChain();
			candidate && candidate != widget;
			candidate = candidate->nextInFocusChain()) {
		if (candidate->window() == window
			&& (candidate->focusPolicy() & Qt::TabFocus)
			&& !TabStopOnly(candidate)
			&& candidate->isVisibleTo(window)
			&& candidate->isEnabled()
			&& !candidate->focusProxy()) {
			candidate->setFocus(Qt::ActiveWindowFocusReason);
			return;
		}
	}
	widget->clearFocus();
}

// `draw(inset)` strokes the shape `inset` pixels inside the widget's edge.
void PaintRingWith(QPainter &p, const auto &draw) {
	auto hq = PainterHighQualityEnabler(p);
	p.save();
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	// A thin line of the canvas inside keeps the ring visible on filled
	// (brand) buttons.
	p.setPen(QPen(st::bgCanvas->c, 1.));
	draw(st::focusRingWidth + 0.5);
	p.setPen(QPen(st::focusRingFg->c, st::focusRingWidth));
	draw(st::focusRingWidth / 2.);
	p.restore();
}

[[nodiscard]] QRectF Inset(const QRectF &rect, double inset) {
	return rect.marginsRemoved(QMarginsF(inset, inset, inset, inset));
}

} // namespace

void Install() {
	static auto installed = false;
	if (!installed) {
		installed = true;
		QAccessible::installFactory(Factory);
	}
	static auto watcher = QPointer<KeyWatcher>();
	if (!watcher) {
		if (const auto app = QCoreApplication::instance()) {
			watcher = new KeyWatcher(app);
			app->installEventFilter(watcher);
		}
	}
}

ItemsSource::ItemsSource() {
	Install();
}

void ItemFocused(QWidget *widget, int index) {
	if (!widget || !QAccessible::isActive() || index < 0) {
		return;
	}
	auto event = QAccessibleEvent(widget, QAccessible::Focus);
	event.setChild(index);
	QAccessible::updateAccessibility(&event);
}

void ItemsChanged(QWidget *widget) {
	if (const auto items = ExistingItems(widget)) {
		const auto source = dynamic_cast<ItemsSource*>(widget);
		items->dropChildren(source ? source->accessibilityItemsCount() : 0);
		auto event = QAccessibleEvent(widget, QAccessible::ObjectReorder);
		QAccessible::updateAccessibility(&event);
	}
}

void NameChanged(QWidget *widget, int index) {
	if (!widget || !QAccessible::isActive()) {
		return;
	}
	auto event = QAccessibleEvent(widget, QAccessible::NameChanged);
	if (index >= 0) {
		event.setChild(index);
	}
	QAccessible::updateAccessibility(&event);
}

void StateChanged(QWidget *widget, QAccessible::State changed, int index) {
	if (!widget || !QAccessible::isActive()) {
		return;
	}
	auto event = QAccessibleStateChangeEvent(widget, changed);
	if (index >= 0) {
		event.setChild(index);
	}
	QAccessible::updateAccessibility(&event);
}

bool KeyboardFocusReason(Qt::FocusReason reason) {
	switch (reason) {
	case Qt::TabFocusReason:
	case Qt::BacktabFocusReason: return TabKeyPressed;
	case Qt::ShortcutFocusReason: return true;
	default: break;
	}
	return false;
}

void SetTabStop(QWidget *widget) {
	widget->setFocusPolicy(Qt::TabFocus);
	widget->setProperty("_tab_stop_only", true);
}

void TabStopFocusIn(
		QWidget *widget,
		Qt::FocusReason reason,
		bool &keyboardFocus) {
	const auto byTab = (reason == Qt::TabFocusReason)
		|| (reason == Qt::BacktabFocusReason);
	if (KeyboardFocusReason(reason)) {
		keyboardFocus = true;
		return;
	} else if (byTab) {
		// Qt moved the focus on from a widget that was hidden.
		keyboardFocus = false;
	}
	if ((!byTab && reason != Qt::ActiveWindowFocusReason) || keyboardFocus) {
		// Given by the app, or the keyboard's focus given back.
		return;
	}
	// Qt chose the next Tab stop by itself: the first one of a window that
	// becomes active, the next one after a focused widget that hides.
	const auto guard = QPointer<QWidget>(widget);
	const auto flag = &keyboardFocus;
	QMetaObject::invokeMethod(widget, [=] {
		if (guard && guard->hasFocus() && !*flag) {
			PassOnFocus(guard);
		}
	}, Qt::QueuedConnection);
}

bool FocusNextInRegions(const std::vector<QWidget*> &regions, bool next) {
	if (regions.empty()) {
		return false;
	}
	const auto window = regions.front()->window();
	// As QWidget::focusNextPrevChild() picks them (macOS without the full
	// keyboard access: only the inputs).
	const auto flag = (QGuiApplication::styleHints()->tabFocusBehavior()
		== Qt::TabFocusAllControls)
		? Qt::TabFocus
		: Qt::StrongFocus;
	auto buckets = std::vector<std::vector<QWidget*>>(regions.size());
	auto w = window;
	do {
		if ((w->focusPolicy() & flag) == flag
			&& !w->focusProxy()
			&& w->isVisibleTo(window)
			&& w->isEnabled()) {
			for (auto i = 0, count = int(regions.size()); i != count; ++i) {
				if (regions[i] && IsAncestorOf(regions[i], w)) {
					buckets[i].push_back(w);
					break;
				}
			}
		}
		w = w->nextInFocusChain();
	} while (w && w != window);
	// A container with Tab stops inside (a section that gives its focus on
	// to its field or list) is not a stop of its own: what is inside it is.
	for (auto &bucket : buckets) {
		auto stops = std::vector<QWidget*>();
		for (const auto widget : bucket) {
			const auto container = std::any_of(
				bucket.begin(),
				bucket.end(),
				[&](QWidget *other) {
					return (other != widget) && IsAncestorOf(widget, other);
				});
			if (!container) {
				stops.push_back(widget);
			}
		}
		bucket = std::move(stops);
	}
	auto ordered = std::vector<QWidget*>();
	for (const auto &bucket : buckets) {
		ordered.insert(ordered.end(), bucket.begin(), bucket.end());
	}
	if (ordered.empty()) {
		return false;
	}
	const auto count = int(ordered.size());
	const auto focused = QApplication::focusWidget();
	const auto i = std::find(ordered.begin(), ordered.end(), focused);
	auto index = 0;
	if (i != ordered.end()) {
		const auto at = int(i - ordered.begin());
		index = (at + (next ? 1 : count - 1)) % count;
	} else {
		// The focus is on a container (a section): from its region on.
		auto region = -1;
		for (auto r = 0, total = int(regions.size()); r != total; ++r) {
			if (regions[r] && focused && IsAncestorOf(regions[r], focused)) {
				region = r;
				break;
			}
		}
		auto before = 0;
		for (auto r = 0; r < region; ++r) {
			before += int(buckets[r].size());
		}
		const auto inside = (region >= 0) ? int(buckets[region].size()) : 0;
		index = !next
			? ((before + count - 1) % count)
			: inside
			? before
			: (before % count);
	}
	const auto target = ordered[index];
	target->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
	// Our scroll areas do not follow the focus by themselves.
	for (auto parent = target->parentWidget(); parent; parent = parent->parentWidget()) {
		if (const auto scroll = qobject_cast<ScrollArea*>(parent)) {
			const auto inner = scroll->widget();
			if (inner && IsAncestorOf(inner, target)) {
				const auto top = target->mapTo(inner, QPoint()).y();
				scroll->scrollToY(top, top + target->height());
			}
		}
	}
	return true;
}

std::vector<QWidget*> ColumnsInReadingOrder(QWidget *parent) {
	auto result = std::vector<QWidget*>();
	for (const auto child : parent->children()) {
		if (child->isWidgetType()) {
			const auto widget = static_cast<QWidget*>(child);
			if (!widget->isHidden() && !widget->isWindow()) {
				result.push_back(widget);
			}
		}
	}
	const auto rtl = style::RightToLeft();
	const auto leading = [&](QWidget *widget) {
		return rtl
			? (parent->width() - widget->x() - widget->width())
			: widget->x();
	};
	std::stable_sort(result.begin(), result.end(), [&](QWidget *a, QWidget *b) {
		const auto la = leading(a);
		const auto lb = leading(b);
		return (la != lb) ? (la < lb) : (a->y() < b->y());
	});
	return result;
}

void PaintFocusRing(QPainter &p, const QRectF &rect, double radius) {
	PaintRingWith(p, [&](double inset) {
		const auto inner = Inset(rect, inset);
		const auto r = std::clamp(
			radius - inset,
			0.,
			std::max(std::min(inner.width(), inner.height()) / 2., 0.));
		p.drawRoundedRect(inner, r, r);
	});
}

void PaintFocusRingEllipse(QPainter &p, const QRectF &rect) {
	PaintRingWith(p, [&](double inset) {
		p.drawEllipse(Inset(rect, inset));
	});
}

void PaintFocusRingPath(QPainter &p, const QPainterPath &path) {
	// The path is the outer edge; no inner line for a free shape.
	auto hq = PainterHighQualityEnabler(p);
	p.save();
	p.setOpacity(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(st::focusRingFg->c, st::focusRingWidth));
	p.drawPath(path);
	p.restore();
}

} // namespace Ui::Accessibility
