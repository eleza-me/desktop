#include "window/window_section_widget.h"

#include "ui/widgets/top_bar.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"

#include <QtGui/QPainter>

namespace Window {

SectionWidget::SectionWidget(QWidget *parent, SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _bg(st::bgCanvas) {
	setFocusPolicy(Qt::StrongFocus);
}

void SectionWidget::setBackButtonShown(bool shown) {
	_backButtonShown = shown;
	if (_topBar) {
		_topBar->setBackShown(shown);
	}
}

void SectionWidget::setBackground(style::color color) {
	_bg = color;
	update();
}

void SectionWidget::setTopBar(Ui::TopBar *bar) {
	_topBar = bar;
	if (!bar) {
		return;
	}
	bar->setBackShown(_backButtonShown);
	QObject::connect(bar, &Ui::TopBar::backRequested, this, [this] {
		if (_controller) {
			_controller->showBack();
		}
	});
}

Ui::TopBar *SectionWidget::topBar() const {
	return _topBar.data();
}

void SectionWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.fillRect(rect(), _bg);
}

} // namespace Window
