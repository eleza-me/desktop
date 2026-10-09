#include "ui/wrap/vertical_layout.h"

#include "ui/ui_utility.h"

#include <algorithm>

namespace Ui {

VerticalLayout::VerticalLayout(QWidget *parent) : RpWidget(parent) {
}

void VerticalLayout::insertChild(
		int atPosition,
		RpWidget *child,
		const style::margins &margin,
		std::optional<style::align> align) {
	atPosition = std::clamp(atPosition, 0, count());
	child->setParent(this);
	child->show();
	_rows.insert(_rows.begin() + atPosition, Row{ child, margin, align, 0 });
	QObject::connect(child, &RpWidget::heightChanged, this, [this] {
		relayout();
	});
	QObject::connect(child, &RpWidget::shownChanged, this, [this] {
		relayout();
	});
	QObject::connect(child, &QObject::destroyed, this, [this, child] {
		const auto i = std::find_if(_rows.begin(), _rows.end(), [&](const Row &row) {
			return row.widget == child;
		});
		if (i != _rows.end()) {
			_rows.erase(i);
			relayout();
		}
	});
	relayout();
}

void VerticalLayout::removeChild(RpWidget *child) {
	const auto i = std::find_if(_rows.begin(), _rows.end(), [&](const Row &row) {
		return row.widget == child;
	});
	if (i != _rows.end()) {
		_rows.erase(i);
		delete child;
		relayout();
	}
}

void VerticalLayout::clear() {
	const auto rows = std::exchange(_rows, {});
	for (const auto &row : rows) {
		delete row.widget;
	}
	relayout();
}

void VerticalLayout::reorderRows(int oldIndex, int newIndex) {
	if (oldIndex < 0
		|| newIndex < 0
		|| oldIndex >= count()
		|| newIndex >= count()
		|| oldIndex == newIndex) {
		return;
	}
	const auto row = _rows[oldIndex];
	_rows.erase(_rows.begin() + oldIndex);
	_rows.insert(_rows.begin() + newIndex, row);
	relayout();
}

void VerticalLayout::setVerticalShift(int index, int shift) {
	if (index >= 0 && index < count() && _rows[index].verticalShift != shift) {
		_rows[index].verticalShift = shift;
		relayout();
	}
}

void VerticalLayout::relayout() {
	if (_inResize) {
		return;
	}
	// Several rows usually change together: coalesce into one pass.
	if (_relayoutPending) {
		return;
	}
	_relayoutPending = true;
	PostponeCall(this, [this] {
		if (std::exchange(_relayoutPending, false)) {
			resizeToWidth(width());
		}
	});
}

int VerticalLayout::resizeGetHeight(int newWidth) {
	_inResize = true;
	_relayoutPending = false;
	auto top = 0;
	for (const auto &row : _rows) {
		const auto widget = row.widget;
		if (widget->isHidden()) {
			continue;
		}
		const auto available = std::max(
			newWidth - row.margin.left() - row.margin.right(),
			0);
		if (row.align) {
			widget->resizeToNaturalWidth(available);
		} else {
			widget->resizeToWidth(available);
		}
		auto left = row.margin.left();
		if (row.align) {
			if (*row.align & Qt::AlignHCenter) {
				left += (available - widget->width()) / 2;
			} else if (*row.align & Qt::AlignRight) {
				left += available - widget->width();
			}
		}
		widget->moveToLeft(
			left,
			top + row.margin.top() + row.verticalShift,
			newWidth);
		top += row.margin.top() + widget->height() + row.margin.bottom();
	}
	_inResize = false;
	return top;
}

void VerticalLayout::visibleTopBottomUpdated(
		int visibleTop,
		int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
	for (const auto &row : _rows) {
		const auto widget = row.widget;
		if (!widget->isHidden()) {
			widget->setVisibleTopBottom(
				visibleTop - widget->y(),
				visibleBottom - widget->y());
		}
	}
}

} // namespace Ui
