#include "history/history_reactions_strip.h"

#include "ui/painter.h"
#include "styles/style_history.h"

#include <QtGui/QMouseEvent>

namespace History {

ReactionsStrip::ReactionsStrip(
	QWidget *parent,
	const QStringList &emoji,
	const QString &selected,
	bool expanded)
: RpWidget(parent)
, _emoji(emoji)
, _selected(selected)
, _expanded(expanded) {
	setMouseTracking(true);
	setCursor(style::cur_pointer);
}

bool ReactionsStrip::hasToggle() const {
	return _emoji.size() > kQuickCount;
}

int ReactionsStrip::shownCount() const {
	return _expanded
		? int(_emoji.size())
		: std::min(int(_emoji.size()), int(kQuickCount));
}

int ReactionsStrip::columns() const {
	return std::min(int(_emoji.size()), int(kQuickCount)) + (hasToggle() ? 1 : 0);
}

int ReactionsStrip::naturalWidth() const {
	return 2 * st::historyReactionsStripPadding
		+ columns() * st::historyReactionsStripCell;
}

int ReactionsStrip::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	const auto perRow = std::max(columns(), 1);
	// The chevron takes the last cell of the first row.
	const auto cells = shownCount() + (hasToggle() ? 1 : 0);
	const auto rows = (cells + perRow - 1) / perRow;
	return (st::historyReactionsStripHeight - st::historyReactionsStripCell)
		+ rows * st::historyReactionsStripCell;
}

QRect ReactionsStrip::cellRect(int index) const {
	const auto perRow = std::max(columns(), 1);
	const auto cell = st::historyReactionsStripCell;
	const auto padding = st::historyReactionsStripPadding;
	const auto top = (st::historyReactionsStripHeight - cell) / 2;
	// Position in the grid: the chevron sits at the end of the first row,
	// the emoji after the quick ones continue on the next rows.
	auto slot = index;
	if (hasToggle()) {
		if (index == int(_emoji.size())) {
			slot = perRow - 1;
		} else if (index >= perRow - 1) {
			slot = index + 1;
		}
	}
	const auto column = slot % perRow;
	const auto row = slot / perRow;
	return style::rtlrect(
		padding + column * cell,
		top + row * cell,
		cell,
		cell,
		width());
}

int ReactionsStrip::cellAt(QPoint position) const {
	for (auto i = 0, count = shownCount(); i != count; ++i) {
		if (cellRect(i).contains(position)) {
			return i;
		}
	}
	if (hasToggle() && cellRect(int(_emoji.size())).contains(position)) {
		return int(_emoji.size());
	}
	return -1;
}

void ReactionsStrip::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setFont(st::historyReactionsStripFont);
	for (auto i = 0, count = shownCount(); i != count; ++i) {
		const auto rect = cellRect(i);
		const auto mine = (_emoji[i] == _selected);
		if (mine || _over == i) {
			p.setPen(Qt::NoPen);
			p.setBrush(mine
				? st::historyReactionsStripSelectedBg
				: st::historyReactionsStripOverBg);
			p.drawEllipse(rect.marginsRemoved(QMargins(1, 1, 1, 1)));
		}
		p.setPen(st::textPrimary);
		p.drawText(rect, Qt::AlignCenter, _emoji[i]);
	}
	if (hasToggle()) {
		const auto rect = cellRect(int(_emoji.size()));
		if (_over == int(_emoji.size())) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyReactionsStripOverBg);
			p.drawEllipse(rect.marginsRemoved(QMargins(1, 1, 1, 1)));
		}
		(_expanded
			? st::historyReactionsStripCollapse
			: st::historyReactionsStripExpand).paintInCenter(p, rect);
	}
}

void ReactionsStrip::mouseMoveEvent(QMouseEvent *e) {
	const auto over = cellAt(e->pos());
	if (_over != over) {
		_over = over;
		update();
	}
}

void ReactionsStrip::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = cellAt(e->pos());
	}
}

void ReactionsStrip::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	const auto index = cellAt(e->pos());
	if (index < 0 || index != pressed) {
		return;
	} else if (index == int(_emoji.size())) {
		if (const auto callback = _expand) {
			callback();
		}
	} else if (const auto callback = _chosen) {
		callback(_emoji[index]);
	}
}

void ReactionsStrip::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over != -1) {
		_over = -1;
		update();
	}
}

} // namespace History
