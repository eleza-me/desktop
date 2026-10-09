#include "ui/wrap/fade_wrap.h"

#include "ui/ui_utility.h"
#include "styles/style_basic.h"

#include <QtGui/QPainter>

#include <cmath>

namespace Ui {

FadeWrapBase::FadeWrapBase(QWidget *parent, object_ptr<RpWidget> entity)
: RpWidget(parent)
, _entity(entity.release())
, _duration(st::motionFast) {
	_entity->setParent(this);
	_entity->move(0, 0);
	_entity->show();
	resize(_entity->size());
	QObject::connect(_entity, &RpWidget::sizeChanged, this, [this](QSize size) {
		if (size != this->size()) {
			resize(size);
		}
	});
}

FadeWrapBase *FadeWrapBase::setDuration(int duration) {
	_duration = duration;
	return this;
}

int FadeWrapBase::naturalWidth() const {
	return _entity->naturalWidth();
}

FadeWrapBase *FadeWrapBase::toggle(bool shown, anim::type animated) {
	if (_shown == shown) {
		if (animated == anim::type::instant) {
			finishAnimating();
		}
		return this;
	}
	const auto from = _animation.value(_shown ? 1. : 0.);
	_shown = shown;
	if (animated == anim::type::instant) {
		finishAnimating();
		return this;
	}
	if (_cache.isNull()) {
		RpWidget::show();
		_entity->show();
		_cache = GrabWidget(_entity);
		_entity->hide();
	}
	_animation.start(
		[this] { animationCallback(); },
		from,
		shown ? 1. : 0.,
		_duration,
		anim::linear);
	if (!_animation.animating()) {
		finishAnimating();
	}
	update();
	return this;
}

void FadeWrapBase::animationCallback() {
	update();
	if (!_animation.animating()) {
		finishAnimating();
	}
}

void FadeWrapBase::finishAnimating() {
	_animation.stop();
	_cache = QPixmap();
	_entity->setVisible(_shown);
	setVisible(_shown);
	update();
}

void FadeWrapBase::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	if (_cache.isNull()) {
		return;
	}
	auto p = QPainter(this);
	p.setOpacity(_animation.value(_shown ? 1. : 0.));
	p.drawPixmap(0, 0, _cache);
}

void FadeWrapBase::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (_entity->size() != size()) {
		_entity->resize(size());
	}
}

int FadeWrapBase::resizeGetHeight(int newWidth) {
	_entity->resizeToWidth(newWidth);
	return _entity->height();
}

SlideWrapBase::SlideWrapBase(
	QWidget *parent,
	object_ptr<RpWidget> entity,
	const style::margins &padding)
: RpWidget(parent)
, _entity(entity.release())
, _padding(padding)
, _duration(st::motion) {
	_entity->setParent(this);
	_entity->show();
	_entity->moveToLeft(_padding.left(), _padding.top(), width());
	resize(
		_entity->width() + _padding.left() + _padding.right(),
		fullHeight());
	QObject::connect(_entity, &RpWidget::heightChanged, this, [this] {
		if (!_inResize && !_animation.animating()) {
			resize(width(), _shown ? fullHeight() : 0);
		}
	});
}

int SlideWrapBase::fullHeight() const {
	return _padding.top() + _entity->height() + _padding.bottom();
}

SlideWrapBase *SlideWrapBase::setDuration(int duration) {
	_duration = duration;
	return this;
}

SlideWrapBase *SlideWrapBase::toggle(bool shown, anim::type animated) {
	if (_shown == shown) {
		if (animated == anim::type::instant) {
			finishAnimating();
		}
		return this;
	}
	const auto from = _animation.value(_shown ? 1. : 0.);
	_shown = shown;
	if (animated == anim::type::instant) {
		finishAnimating();
		return this;
	}
	RpWidget::show();
	_animation.start(
		[this] { animationCallback(); },
		from,
		shown ? 1. : 0.,
		_duration,
		anim::standard);
	if (!_animation.animating()) {
		finishAnimating();
	} else {
		animationCallback();
	}
	return this;
}

void SlideWrapBase::animationCallback() {
	const auto ratio = _animation.value(_shown ? 1. : 0.);
	const auto newHeight = int(std::lround(fullHeight() * ratio));
	if (newHeight != height()) {
		resize(width(), newHeight);
	}
	if (!_animation.animating()) {
		finishAnimating();
	}
}

void SlideWrapBase::finishAnimating() {
	_animation.stop();
	const auto newHeight = _shown ? fullHeight() : 0;
	if (newHeight != height()) {
		resize(width(), newHeight);
	}
	setVisible(_shown);
}

int SlideWrapBase::resizeGetHeight(int newWidth) {
	_inResize = true;
	_entity->resizeToWidth(
		std::max(newWidth - _padding.left() - _padding.right(), 0));
	_entity->moveToLeft(_padding.left(), _padding.top(), newWidth);
	_inResize = false;
	const auto ratio = _animation.value(_shown ? 1. : 0.);
	return int(std::lround(fullHeight() * ratio));
}

PaddingWrapBase::PaddingWrapBase(
	QWidget *parent,
	object_ptr<RpWidget> entity,
	const style::margins &padding)
: RpWidget(parent)
, _entity(entity.release())
, _padding(padding) {
	_entity->setParent(this);
	_entity->show();
	_entity->moveToLeft(_padding.left(), _padding.top(), width());
	resize(
		_entity->width() + _padding.left() + _padding.right(),
		_entity->height() + _padding.top() + _padding.bottom());
	QObject::connect(_entity, &RpWidget::heightChanged, this, [this] {
		if (!_inResize) {
			resize(
				width(),
				_entity->height() + _padding.top() + _padding.bottom());
		}
	});
}

void PaddingWrapBase::setBackground(std::optional<style::color> color) {
	_bg = color;
	update();
}

int PaddingWrapBase::naturalWidth() const {
	const auto inner = _entity->naturalWidth();
	return (inner < 0)
		? inner
		: (inner + _padding.left() + _padding.right());
}

void PaddingWrapBase::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	if (_bg) {
		auto p = QPainter(this);
		p.fillRect(rect(), *_bg);
	}
}

int PaddingWrapBase::resizeGetHeight(int newWidth) {
	_inResize = true;
	_entity->resizeToWidth(
		std::max(newWidth - _padding.left() - _padding.right(), 0));
	_entity->moveToLeft(_padding.left(), _padding.top(), newWidth);
	_inResize = false;
	return _entity->height() + _padding.top() + _padding.bottom();
}

} // namespace Ui
