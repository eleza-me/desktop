#include "ui/layers/layer_widget.h"

#include "ui/layers/box_content.h"
#include "ui/layers/box_layer_widget.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

#include <algorithm>

namespace Ui {

LayerStackWidget::LayerStackWidget(QWidget *parent) : RpWidget(parent) {
	hide();
	setFocusPolicy(Qt::StrongFocus);
	if (parent) {
		parent->installEventFilter(this);
		setGeometry(parent->rect());
	}
}

LayerStackWidget::~LayerStackWidget() = default;

LayerWidget *LayerStackWidget::topLayer() const {
	return _layers.empty() ? nullptr : _layers.back()->widget;
}

LayerStackWidget::Entry *LayerStackWidget::find(LayerWidget *widget) {
	for (const auto &entry : _layers) {
		if (entry->widget == widget) {
			return entry.get();
		}
	}
	for (const auto &entry : _hiding) {
		if (entry->widget == widget) {
			return entry.get();
		}
	}
	return nullptr;
}

void LayerStackWidget::showBox(
		object_ptr<BoxContent> box,
		LayerOption option,
		anim::type animated) {
	showLayer(
		Owned<LayerWidget>(new BoxLayerWidget(this, std::move(box))),
		option,
		animated);
}

void LayerStackWidget::showSheet(
		object_ptr<BoxContent> content,
		LayerOption option,
		anim::type animated) {
	showLayer(
		Owned<LayerWidget>(new SheetLayerWidget(this, std::move(content))),
		option,
		animated);
}

void LayerStackWidget::showLayer(
		object_ptr<LayerWidget> layer,
		LayerOption option,
		anim::type animated) {
	const auto widget = layer.release();
	if (!widget) {
		return;
	}
	const auto wasEmpty = _layers.empty();
	if (parentWidget()) {
		setGeometry(parentWidget()->rect());
	}
	if (option == LayerOption::CloseOther) {
		while (!_layers.empty()) {
			auto entry = std::move(_layers.back());
			_layers.pop_back();
			startTransition(*entry, false, anim::type::instant);
			_hiding.push_back(std::move(entry));
		}
	} else if (!_layers.empty()) {
		// The previous layer waits hidden under the new one.
		auto &previous = *_layers.back();
		previous.animation.stop();
		previous.cache = QPixmap();
		previous.widget->hide();
	}
	widget->setParent(this);
	widget->setClosedCallback([this, widget] {
		closeLayer(widget, anim::type::normal);
	});
	auto entry = std::make_unique<Entry>();
	entry->widget = widget;
	const auto raw = entry.get();
	_layers.push_back(std::move(entry));

	RpWidget::show();
	raise();
	widget->parentResized();
	startTransition(*raw, true, animated);
	updateDim(animated);
	destroyHidden();
	if (wasEmpty) {
		emit layersShownChanged(true);
	}
}

void LayerStackWidget::startTransition(
		Entry &entry,
		bool shown,
		anim::type animated) {
	const auto from = entry.animation.value(entry.shown ? 1. : 0.);
	const auto first = entry.cache.isNull() && !entry.animation.animating();
	entry.shown = shown;
	if (animated == anim::type::instant || AnimationsDisabled()) {
		entry.animation.stop();
		finishTransition(entry);
		return;
	}
	if (entry.cache.isNull()) {
		entry.widget->show();
		SendPendingMoveResizeEvents(entry.widget);
		entry.cache = GrabWidget(entry.widget);
	}
	entry.widget->hide();
	const auto widget = entry.widget;
	entry.animation.start(
		[this, widget] { transitionCallback(widget); },
		(first && shown) ? 0. : from,
		shown ? 1. : 0.,
		shown ? st::layerShowDuration : st::layerHideDuration,
		shown ? anim::standard : anim::easeInCubic);
	update();
}

void LayerStackWidget::transitionCallback(LayerWidget *widget) {
	update();
	if (const auto entry = find(widget)) {
		if (!entry->animation.animating()) {
			finishTransition(*entry);
		}
	}
}

void LayerStackWidget::finishTransition(Entry &entry) {
	entry.cache = QPixmap();
	if (entry.shown) {
		entry.widget->show();
		entry.widget->raise();
		entry.widget->showFinished();
		if (entry.widget == topLayer()) {
			entry.widget->setInnerFocus();
		}
	} else {
		entry.widget->hide();
		PostponeCall(this, [this] { destroyHidden(); });
	}
	update();
}

void LayerStackWidget::destroyHidden() {
	for (auto i = _hiding.begin(); i != _hiding.end();) {
		if (!(*i)->animation.animating()) {
			(*i)->widget->deleteLater();
			i = _hiding.erase(i);
		} else {
			++i;
		}
	}
	updateVisibility();
}

void LayerStackWidget::updateVisibility() {
	if (_layers.empty() && _hiding.empty() && !_dimAnimation.animating()) {
		if (!isHidden()) {
			hide();
			if (const auto parent = parentWidget()) {
				parent->setFocus();
			}
			emit layersShownChanged(false);
		}
	}
}

void LayerStackWidget::closeLayer(LayerWidget *widget, anim::type animated) {
	const auto i = std::find_if(
		_layers.begin(),
		_layers.end(),
		[&](const std::unique_ptr<Entry> &entry) {
			return entry->widget == widget;
		});
	if (i == _layers.end()) {
		return;
	}
	const auto wasTop = (i + 1 == _layers.end());
	auto entry = std::move(*i);
	_layers.erase(i);
	if (!wasTop) {
		// A layer under the visible one: just drop it.
		entry->widget->hide();
		entry->shown = false;
		_hiding.push_back(std::move(entry));
		destroyHidden();
		return;
	}
	const auto raw = entry.get();
	_hiding.push_back(std::move(entry));
	startTransition(*raw, false, animated);
	if (!_layers.empty()) {
		auto &next = *_layers.back();
		next.widget->parentResized();
		startTransition(next, true, animated);
	}
	updateDim(animated);
	if (animated == anim::type::instant) {
		destroyHidden();
	}
}

void LayerStackWidget::hideTopLayer(anim::type animated) {
	if (const auto top = topLayer()) {
		closeLayer(top, animated);
	}
}

void LayerStackWidget::hideLayers(anim::type animated) {
	if (_layers.empty()) {
		return;
	}
	auto top = std::move(_layers.back());
	_layers.pop_back();
	while (!_layers.empty()) {
		auto entry = std::move(_layers.back());
		_layers.pop_back();
		entry->widget->hide();
		entry->shown = false;
		_hiding.push_back(std::move(entry));
	}
	const auto raw = top.get();
	_hiding.push_back(std::move(top));
	startTransition(*raw, false, animated);
	updateDim(animated);
	if (animated == anim::type::instant) {
		destroyHidden();
	}
}

void LayerStackWidget::updateDim(anim::type animated) {
	const auto shown = !_layers.empty();
	if (_dimShown == shown) {
		return;
	}
	const auto from = _dimAnimation.value(_dimShown ? 1. : 0.);
	_dimShown = shown;
	if (animated == anim::type::instant || AnimationsDisabled()) {
		_dimAnimation.stop();
		update();
		updateVisibility();
		return;
	}
	_dimAnimation.start([this] {
		update();
		if (!_dimAnimation.animating()) {
			PostponeCall(this, [this] { destroyHidden(); });
		}
	}, from, shown ? 1. : 0., st::layerShowDuration, anim::linear);
}

void LayerStackWidget::paintEntry(QPainter &p, Entry &entry) {
	if (entry.cache.isNull()) {
		return;
	}
	const auto progress = entry.animation.value(entry.shown ? 1. : 0.);
	const auto geometry = entry.widget->geometry();
	p.save();
	switch (entry.widget->transition()) {
	case LayerWidget::Transition::Scale: {
		const auto scale = 0.94 + 0.06 * progress;
		const auto center = QPointF(geometry.center()) + QPointF(0.5, 0.5);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.setOpacity(progress);
		p.translate(center);
		p.scale(scale, scale);
		p.translate(-center);
		p.drawPixmap(geometry.topLeft(), entry.cache);
	} break;
	case LayerWidget::Transition::SlideFromEnd: {
		const auto distance = geometry.width() * (1. - progress);
		const auto shift = style::RightToLeft() ? -distance : distance;
		p.drawPixmap(
			QPointF(geometry.x() + shift, geometry.y()),
			entry.cache);
	} break;
	case LayerWidget::Transition::Fade: {
		p.setOpacity(progress);
		p.drawPixmap(geometry.topLeft(), entry.cache);
	} break;
	}
	p.restore();
}

void LayerStackWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	const auto dim = _dimAnimation.value(_dimShown ? 1. : 0.);
	if (dim > 0.) {
		p.setOpacity(dim);
		p.fillRect(rect(), st::overlayScrim);
		p.setOpacity(1.);
	}
	for (const auto &entry : _hiding) {
		paintEntry(p, *entry);
	}
	if (!_layers.empty()) {
		paintEntry(p, *_layers.back());
	}
}

void LayerStackWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	for (const auto &entry : _layers) {
		entry->widget->parentResized();
	}
}

void LayerStackWidget::mousePressEvent(QMouseEvent *e) {
	// Presses that reach us are outside of the top layer's widget, or on its
	// shadow area.
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto top = topLayer();
	if (top && top->closeByOutsideClick()) {
		hideTopLayer();
	}
}

void LayerStackWidget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		const auto top = topLayer();
		if (top && top->closeByEscape()) {
			hideTopLayer();
		}
		e->accept();
		return;
	}
	e->accept(); // modal: keys do not leak to the content below
}

bool LayerStackWidget::eventFilter(QObject *object, QEvent *event) {
	if (object == parentWidget() && event->type() == QEvent::Resize) {
		setGeometry(parentWidget()->rect());
	}
	return RpWidget::eventFilter(object, event);
}

} // namespace Ui
