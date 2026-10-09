#include "ui/widgets/chips.h"

#include "ui/widgets/buttons.h"
#include "ui/ui_utility.h"

#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <cmath>

namespace Ui {

class ChipsRow::Fade final : public RpWidget {
public:
	Fade(QWidget *parent, const style::ChipsRow &st, bool physicalLeft)
	: RpWidget(parent)
	, _st(st)
	, _left(physicalLeft) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		auto gradient = QLinearGradient(0, 0, width(), 0);
		const auto solid = _st.bg->c;
		auto clear = solid;
		clear.setAlpha(0);
		gradient.setColorAt(0., _left ? solid : clear);
		gradient.setColorAt(1., _left ? clear : solid);
		p.fillRect(rect(), gradient);
	}

private:
	const style::ChipsRow &_st;
	bool _left = false;

};

ChipsRow::ChipsRow(QWidget *parent, const style::ChipsRow &st)
: RpWidget(parent)
, _st(st)
, _inner(new RpWidget(this))
, _fadeStart(new Fade(this, st, true))
, _fadeEnd(new Fade(this, st, false)) {
	_fadeStart->hide();
	_fadeEnd->hide();
	resize(width(), _st.height);
}

void ChipsRow::addChip(
		const QString &id,
		const QString &text,
		const style::icon *icon) {
	const auto button = new PillButton(_inner, text, _st.chip);
	if (icon) {
		button->setIcon(icon);
	}
	button->show();
	button->installEventFilter(this);
	button->setActive(id == _active && !id.isEmpty(), false);
	button->setClickedCallback([this, id] {
		if (_dragMoved) {
			return;
		}
		if (_toggleable && _active == id) {
			setActive(QString());
			emit activated(QString());
		} else if (_active != id) {
			setActive(id);
			emit activated(id);
		}
	});
	_chips.push_back(Chip{ id, button });
	layoutChips();
}

void ChipsRow::clearChips() {
	for (const auto &chip : _chips) {
		delete chip.button;
	}
	_chips.clear();
	_scroll = _scrollTarget = 0;
	layoutChips();
}

void ChipsRow::setActive(const QString &id, anim::type animated) {
	_active = id;
	for (const auto &chip : _chips) {
		const auto active = (chip.id == id) && !id.isEmpty();
		chip.button->setActive(active, animated == anim::type::normal);
		if (active) {
			ensureVisible(chip.button);
		}
	}
}

int ChipsRow::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return _st.height;
}

int ChipsRow::scrollMax() const {
	return std::max(_contentWidth - width(), 0);
}

void ChipsRow::layoutChips() {
	auto left = _st.padding.left();
	const auto top = (_st.height - _st.chip.height) / 2;
	for (const auto &chip : _chips) {
		chip.button->resize(chip.button->naturalWidth(), _st.chip.height);
		left += chip.button->width() + _st.skip;
	}
	_contentWidth = _chips.empty()
		? 0
		: (left - _st.skip + _st.padding.right());
	_inner->resize(std::max(_contentWidth, width()), height());
	left = _st.padding.left();
	for (const auto &chip : _chips) {
		chip.button->moveToLeft(left, top, _inner->width());
		left += chip.button->width() + _st.skip;
	}
	setScroll(_scroll);
}

void ChipsRow::setScroll(int scroll, bool animated) {
	scroll = std::clamp(scroll, 0, scrollMax());
	if (animated && scroll != _scroll) {
		_scrollTarget = scroll;
		_scrollAnimation.start([this] {
			setScroll(int(std::lround(_scrollAnimation.value(_scrollTarget))));
		}, _scroll, scroll, st::motion, anim::easeOutCubic);
		if (_scrollAnimation.animating()) {
			return;
		}
	}
	_scroll = scroll;
	// The content starts at the leading side.
	const auto x = style::RightToLeft()
		? (width() - _inner->width() + _scroll)
		: -_scroll;
	_inner->move(x, 0);
	updateFades();
}

void ChipsRow::ensureVisible(PillButton *button) {
	const auto logicalLeft = style::RightToLeft()
		? (_inner->width() - button->x() - button->width())
		: button->x();
	const auto margin = _st.padding.left();
	if (logicalLeft - margin < _scroll) {
		setScroll(logicalLeft - margin, true);
	} else if (logicalLeft + button->width() + margin > _scroll + width()) {
		setScroll(logicalLeft + button->width() + margin - width(), true);
	}
}

void ChipsRow::updateFades() {
	const auto size = _st.fadeWidth;
	const auto atStart = (_scroll > 0);
	const auto atEnd = (_scroll < scrollMax());
	// Physical sides: the leading edge is on the right in RTL.
	const auto showLeft = style::RightToLeft() ? atEnd : atStart;
	const auto showRight = style::RightToLeft() ? atStart : atEnd;
	_fadeStart->setGeometry(0, 0, size, height());
	_fadeEnd->setGeometry(width() - size, 0, size, height());
	_fadeStart->setVisible(showLeft);
	_fadeEnd->setVisible(showRight);
	_fadeStart->raise();
	_fadeEnd->raise();
}

void ChipsRow::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	layoutChips();
}

void ChipsRow::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	if (_st.bg->c.alpha() > 0) {
		auto p = QPainter(this);
		p.fillRect(rect(), _st.bg);
	}
}

void ChipsRow::wheelEvent(QWheelEvent *e) {
	if (scrollMax() <= 0) {
		e->ignore();
		return;
	}
	const auto pixel = e->pixelDelta();
	const auto angle = e->angleDelta();
	auto delta = 0;
	if (!pixel.isNull()) {
		delta = (std::abs(pixel.x()) > std::abs(pixel.y()))
			? pixel.x()
			: pixel.y();
	} else {
		const auto raw = (std::abs(angle.x()) > std::abs(angle.y()))
			? angle.x()
			: angle.y();
		delta = int(std::lround(raw / 120. * 80.));
	}
	if (!delta) {
		e->ignore();
		return;
	}
	// Wheel "down" moves towards the end of the row.
	const auto horizontal = std::abs(angle.x()) > std::abs(angle.y());
	const auto direction = (horizontal && style::RightToLeft()) ? 1 : -1;
	_scrollAnimation.stop();
	setScroll(_scroll + direction * delta, pixel.isNull());
	e->accept();
}

bool ChipsRow::eventFilter(QObject *object, QEvent *event) {
	// Drag the row by its chips.
	const auto type = event->type();
	if (type == QEvent::MouseButtonPress) {
		const auto mouse = static_cast<QMouseEvent*>(event);
		if (mouse->button() == Qt::LeftButton) {
			_dragging = true;
			_dragMoved = false;
			_dragStartX = mouse->globalPosition().toPoint().x();
			_dragStartScroll = _scroll;
		}
	} else if (type == QEvent::MouseMove && _dragging) {
		const auto mouse = static_cast<QMouseEvent*>(event);
		const auto delta = mouse->globalPosition().toPoint().x() - _dragStartX;
		if (!_dragMoved && std::abs(delta) > QApplication::startDragDistance()) {
			_dragMoved = true;
		}
		if (_dragMoved) {
			_scrollAnimation.stop();
			setScroll(_dragStartScroll
				+ (style::RightToLeft() ? delta : -delta));
		}
	} else if (type == QEvent::MouseButtonRelease && _dragging) {
		_dragging = false;
		if (_dragMoved) {
			// Let the click handler see the flag, then reset it.
			PostponeCall(this, [this] { _dragMoved = false; });
		}
	}
	return RpWidget::eventFilter(object, event);
}

} // namespace Ui
