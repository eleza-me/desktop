#include "ui/image/image_view.h"

#include "ui/image/image_prepare.h"
#include "ui/painter.h"

#include <algorithm>

namespace Ui {

ImageView::ImageView(QWidget *parent)
: RpWidget(parent)
, _skeleton([this] { update(); }) {
}

void ImageView::setSource(std::shared_ptr<ImageSource> source) {
	if (_connection) {
		QObject::disconnect(_connection);
	}
	_source = std::move(source);
	_image = QImage();
	_preview = _source ? _source->preview() : QImage();
	_requestedSize = QSize();
	invalidate();
	if (_source) {
		_connection = QObject::connect(
			_source.get(),
			&ImageSource::updated,
			this,
			[this] { refreshFromSource(); });
		refreshFromSource();
	}
	update();
}

void ImageView::setImage(const QImage &image) {
	if (_connection) {
		QObject::disconnect(_connection);
	}
	_source = nullptr;
	_image = image;
	invalidate();
	update();
}

void ImageView::setRadius(int radius) {
	setRadius(radius, radius, radius, radius);
}

void ImageView::setRadius(
		int topLeft,
		int topRight,
		int bottomRight,
		int bottomLeft) {
	_radii[0] = topLeft;
	_radii[1] = topRight;
	_radii[2] = bottomRight;
	_radii[3] = bottomLeft;
	invalidate();
	update();
}

void ImageView::setFit(bool fit) {
	_fit = fit;
	invalidate();
	update();
}

void ImageView::setBlurred(bool blurred) {
	if (_blurred != blurred) {
		_blurred = blurred;
		invalidate();
		update();
	}
}

void ImageView::invalidate() {
	_prepared = QImage();
	_preparedPreview = QImage();
}

void ImageView::refreshFromSource() {
	if (!_source || width() <= 0 || height() <= 0) {
		return;
	}
	const auto wanted = size() * devicePixelRatioF();
	_requestedSize = wanted;
	const auto wasNull = _image.isNull();
	const auto image = _source->request(wanted);
	if (_preview.isNull()) {
		_preview = _source->preview();
		_preparedPreview = QImage();
	}
	if (!image.isNull()) {
		_image = image;
		_prepared = QImage();
		_skeleton.stop();
		if (wasNull && isVisible()) {
			_fadeAnimation.start(
				[this] { update(); },
				0.,
				1.,
				st::motionFast,
				anim::linear);
		}
	} else if (isVisible() && _preview.isNull() && !_source->failed()) {
		_skeleton.start();
	}
	update();
}

QImage ImageView::prepare(const QImage &image, bool blurred) const {
	const auto ratio = devicePixelRatioF();
	auto result = QImage();
	if (blurred) {
		result = Images::BlurredPreview(image, size(), ratio);
	} else if (_fit) {
		result = QImage(size() * ratio, QImage::Format_ARGB32_Premultiplied);
		result.setDevicePixelRatio(ratio);
		result.fill(Qt::transparent);
		const auto scaled = image.size().scaled(size(), Qt::KeepAspectRatio);
		auto p = QPainter(&result);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(
			QRect(
				(width() - scaled.width()) / 2,
				(height() - scaled.height()) / 2,
				scaled.width(),
				scaled.height()),
			image);
	} else {
		result = Images::PrepareCover(image, size(), ratio);
	}
	if (_radii[0] || _radii[1] || _radii[2] || _radii[3]) {
		result = Images::Round(
			std::move(result),
			_radii[0],
			_radii[1],
			_radii[2],
			_radii[3]);
	}
	return result;
}

void ImageView::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	if (width() <= 0 || height() <= 0) {
		return;
	}
	auto p = QPainter(this);
	const auto fade = _fadeAnimation.value(1.);
	const auto showImage = !_image.isNull() && !_blurred;
	if (!showImage || fade < 1.) {
		const auto &under = !_preview.isNull() ? _preview : _image;
		if (!under.isNull()) {
			if (_preparedPreview.isNull()) {
				_preparedPreview = prepare(under, true);
			}
			p.drawImage(rect(), _preparedPreview);
		} else {
			const auto radius = std::max(
				std::max(_radii[0], _radii[1]),
				std::max(_radii[2], _radii[3]));
			_skeleton.paint(p, QRectF(rect()), radius, rect());
		}
	}
	if (showImage) {
		if (_prepared.isNull()) {
			_prepared = prepare(_image, false);
		}
		p.setOpacity(fade);
		p.drawImage(rect(), _prepared);
	}
}

void ImageView::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	invalidate();
	if (_source && _requestedSize != size() * devicePixelRatioF()) {
		refreshFromSource();
	}
}

void ImageView::showEvent(QShowEvent *e) {
	if (_source && _image.isNull()) {
		refreshFromSource();
	}
	RpWidget::showEvent(e);
}

void ImageView::hideEvent(QHideEvent *e) {
	_skeleton.stop();
	RpWidget::hideEvent(e);
}

} // namespace Ui
