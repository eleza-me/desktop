#include "boxes/photo_crop_box.h"

#include "base/platform_file.h"
#include "data/data_image_decode.h"
#include "lang_auto.h"
#include "support/file_dialogs.h"
#include "ui/painter.h"
#include "styles/style_boxes.h"

#include <QtCore/QDir>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtCore/QUuid>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>

#include <cmath>

namespace Boxes {
namespace {

constexpr auto kMaxZoom = 4.;
constexpr auto kMaxDecodedSide = 2560;
constexpr auto kKeyboardPan = 16;
constexpr auto kKeyboardZoom = 0.08;

} // namespace

// The picture under the circular mask. `zoom` 1 = the image covers the square
// exactly; the center is kept in image coordinates.
class PhotoCropBox::Area final : public Ui::RpWidget {
public:
	Area(QWidget *parent, const QImage &image)
	: RpWidget(parent)
	, _image(image)
	, _center(image.width() / 2., image.height() / 2.) {
		setCursor(Qt::OpenHandCursor);
		resize(st::cropSize, st::cropSize);
	}

	void setZoomChanged(std::function<void(double progress)> callback) {
		_zoomChanged = std::move(callback);
	}

	// 0..1 between "covers the square" and kMaxZoom.
	void setZoomProgress(double progress) {
		setZoom(1. + std::clamp(progress, 0., 1.) * (kMaxZoom - 1.), false);
	}
	[[nodiscard]] double zoomProgress() const {
		return (_zoom - 1.) / (kMaxZoom - 1.);
	}
	void zoomBy(double delta) {
		setZoom(_zoom + delta * (kMaxZoom - 1.), true);
	}
	void panBy(QPointF widgetDelta) {
		const auto scale = this->scale();
		_center -= QPointF(widgetDelta.x() / scale, widgetDelta.y() / scale);
		clampCenter();
		update();
	}

	// The visible square in image coordinates.
	[[nodiscard]] QRectF sourceRect() const {
		const auto side = width() / scale();
		return QRectF(
			_center.x() - side / 2.,
			_center.y() - side / 2.,
			side,
			side);
	}

	[[nodiscard]] QImage result() const {
		const auto source = sourceRect();
		const auto side = std::clamp(
			int(std::lround(source.width())),
			1,
			int(kOutputSize));
		auto result = QImage(side, side, QImage::Format_RGB32);
		result.fill(Qt::white);
		auto p = QPainter(&result);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(QRectF(0, 0, side, side), _image, source);
		p.end();
		return result;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		p.fillRect(rect(), st::cropBg);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(QRectF(rect()), _image, sourceRect());

			// Everything outside the circle is dimmed.
			auto outside = QPainterPath();
			outside.addRect(QRectF(rect()));
			auto circle = QPainterPath();
			const auto stroke = st::cropCircleStroke / 2.;
			circle.addEllipse(QRectF(rect()).adjusted(stroke, stroke, -stroke, -stroke));
			p.fillPath(outside.subtracted(circle), st::cropDimFg);
			p.setPen(QPen(st::cropCircleFg->c, st::cropCircleStroke));
			p.setBrush(Qt::NoBrush);
			p.drawPath(circle);
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_dragging = true;
			_dragLast = e->position();
			setCursor(Qt::ClosedHandCursor);
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (_dragging) {
			const auto now = e->position();
			panBy(now - _dragLast);
			_dragLast = now;
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && _dragging) {
			_dragging = false;
			setCursor(Qt::OpenHandCursor);
		}
	}
	void wheelEvent(QWheelEvent *e) override {
		const auto delta = e->angleDelta().y();
		if (!delta) {
			e->ignore();
			return;
		}
		// Zoom around the point under the cursor.
		const auto position = e->position();
		const auto before = imagePoint(position);
		setZoom(_zoom * std::pow(1.0015, delta), true);
		const auto after = imagePoint(position);
		_center += (before - after);
		clampCenter();
		update();
		e->accept();
	}

private:
	[[nodiscard]] double baseScale() const {
		// Widget pixels per image pixel when the image just covers the square.
		const auto side = std::min(_image.width(), _image.height());
		return width() / double(std::max(side, 1));
	}
	[[nodiscard]] double scale() const {
		return baseScale() * _zoom;
	}
	[[nodiscard]] QPointF imagePoint(QPointF widgetPoint) const {
		const auto scale = this->scale();
		return _center + QPointF(
			(widgetPoint.x() - width() / 2.) / scale,
			(widgetPoint.y() - height() / 2.) / scale);
	}
	void setZoom(double zoom, bool notify) {
		zoom = std::clamp(zoom, 1., kMaxZoom);
		if (zoom == _zoom) {
			return;
		}
		_zoom = zoom;
		clampCenter();
		update();
		if (notify && _zoomChanged) {
			_zoomChanged(zoomProgress());
		}
	}
	void clampCenter() {
		const auto half = width() / scale() / 2.;
		_center.setX(std::clamp(
			_center.x(),
			half,
			std::max(_image.width() - half, half)));
		_center.setY(std::clamp(
			_center.y(),
			half,
			std::max(_image.height() - half, half)));
	}

	QImage _image;
	QPointF _center;
	double _zoom = 1.;
	bool _dragging = false;
	QPointF _dragLast;
	std::function<void(double progress)> _zoomChanged;

};

// "−  ———●———  +"
class PhotoCropBox::Slider final : public Ui::RpWidget {
public:
	explicit Slider(QWidget *parent) : RpWidget(parent) {
		setCursor(style::cur_pointer);
		resize(
			st::cropSliderWidth
				+ 2 * (st::cropZoomOutIcon.width() + st::cropSliderIconSkip),
			st::cropSliderHeight);
	}

	void setChanged(std::function<void(double value)> callback) {
		_changed = std::move(callback);
	}
	void setValue(double value) {
		value = std::clamp(value, 0., 1.);
		if (_value != value) {
			_value = value;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto line = lineRect();
		// The zoom always grows to the right, in both directions of the UI.
		st::cropZoomOutIcon.paintInCenter(
			p,
			QRect(0, 0, line.x() - st::cropSliderIconSkip, height()));
		st::cropZoomInIcon.paintInCenter(
			p,
			QRect(
				line.x() + line.width() + st::cropSliderIconSkip,
				0,
				width() - line.x() - line.width() - st::cropSliderIconSkip,
				height()));
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		const auto radius = line.height() / 2.;
		p.setBrush(st::cropSliderBg);
		p.drawRoundedRect(line, radius, radius);
		const auto knobX = line.x() + _value * line.width();
		p.setBrush(st::cropSliderFg);
		p.drawRoundedRect(
			QRectF(line.x(), line.y(), knobX - line.x(), line.height()),
			radius,
			radius);
		const auto knob = st::cropSliderKnob / 2.;
		p.drawEllipse(QPointF(knobX, height() / 2.), knob, knob);
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_dragging = true;
			apply(e->position().x());
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (_dragging) {
			apply(e->position().x());
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_dragging = false;
		}
	}

private:
	[[nodiscard]] QRect lineRect() const {
		const auto side = st::cropZoomOutIcon.width() + st::cropSliderIconSkip;
		return QRect(
			side,
			(height() - st::cropSliderLine) / 2,
			width() - 2 * side,
			st::cropSliderLine);
	}
	void apply(double x) {
		const auto line = lineRect();
		const auto value = std::clamp(
			(x - line.x()) / double(std::max(line.width(), 1)),
			0.,
			1.);
		if (_value != value) {
			_value = value;
			update();
			if (_changed) {
				_changed(value);
			}
		}
	}

	double _value = 0.;
	bool _dragging = false;
	std::function<void(double value)> _changed;

};

PhotoCropBox::PhotoCropBox(
	QWidget *parent,
	const QImage &image,
	std::function<void(const QImage &square)> done)
: BoxContent(parent)
, _image(image)
, _done(std::move(done)) {
}

void PhotoCropBox::prepare() {
	setTitle(tr::lng_boxes_crop_title(tr::now));
	_area = new Area(this, _image);
	_slider = new Slider(this);
	_area->setZoomChanged([this](double progress) {
		_slider->setValue(progress);
	});
	_slider->setChanged([this](double value) {
		_area->setZoomProgress(value);
	});
	addButton(tr::lng_boxes_crop_done(tr::now), [this] {
		submit();
	}, st::defaultRoundButton);
	addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });
	const auto &padding = st::cropPadding;
	setDimensions(
		st::cropSize + padding.left() + padding.right(),
		padding.top()
			+ st::cropSize
			+ st::cropSliderHeight
			+ st::cropHintHeight
			+ padding.bottom());
	QObject::connect(this, &RpWidget::paintRequest, this, [this] {
		auto p = Painter(this);
		p.setFont(st::cropHintFont);
		p.setPen(st::cropHintFg);
		p.drawTextCentered(
			QRect(
				0,
				_slider->y() + _slider->height(),
				width(),
				st::cropHintHeight - 8),
			tr::lng_boxes_crop_hint(tr::now));
	});
}

void PhotoCropBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	const auto &padding = st::cropPadding;
	_area->move((width() - _area->width()) / 2, padding.top());
	_slider->move(
		(width() - _slider->width()) / 2,
		_area->y() + _area->height());
}

void PhotoCropBox::keyPressEvent(QKeyEvent *e) {
	switch (e->key()) {
	case Qt::Key_Plus:
	case Qt::Key_Equal:
		_area->zoomBy(kKeyboardZoom);
		break;
	case Qt::Key_Minus:
		_area->zoomBy(-kKeyboardZoom);
		break;
	case Qt::Key_Left: _area->panBy(QPointF(kKeyboardPan, 0)); break;
	case Qt::Key_Right: _area->panBy(QPointF(-kKeyboardPan, 0)); break;
	case Qt::Key_Up: _area->panBy(QPointF(0, kKeyboardPan)); break;
	case Qt::Key_Down: _area->panBy(QPointF(0, -kKeyboardPan)); break;
	default:
		BoxContent::keyPressEvent(e);
		return;
	}
}

void PhotoCropBox::submit() {
	const auto result = _area->result();
	const auto done = _done;
	closeBox();
	if (done) {
		done(result);
	}
}

void PickAndCropPhoto(
		QWidget *parent,
		std::function<void(Ui::object_ptr<Ui::BoxContent> box)> show,
		std::function<void(const QImage &square)> done,
		std::function<void()> failed) {
	const auto weak = QPointer<QWidget>(parent);
	Support::PickImage(parent, [=](const QString &path) {
		QThreadPool::globalInstance()->start([=] {
			auto image = Data::DecodeImageFile(path, kMaxDecodedSide);
			if (!image.isNull() && image.hasAlphaChannel()) {
				// A transparent picture gets a white backing, like the server does.
				auto flat = QImage(image.size(), QImage::Format_RGB32);
				flat.fill(Qt::white);
				auto p = QPainter(&flat);
				p.drawImage(0, 0, image);
				p.end();
				image = flat;
			}
			QMetaObject::invokeMethod(qApp, [=] {
				if (!weak) {
					return;
				} else if (image.isNull()
					|| image.width() < 8
					|| image.height() < 8) {
					if (failed) {
						failed();
					}
					return;
				}
				show(Ui::Box<PhotoCropBox>(image, done));
			}, Qt::QueuedConnection);
		});
	});
}

void SaveCroppedPhoto(
		const QImage &square,
		QObject *guard,
		std::function<void(const QString &path)> done) {
	const auto weak = QPointer<QObject>(guard);
	const auto bound = (guard != nullptr);
	const auto directory = QDir(base::platform::PrivateTempDirectory()).filePath(
		QStringLiteral("eleza-photo"));
	QThreadPool::globalInstance()->start([=] {
		QDir().mkpath(directory);
		auto path = QDir(directory).filePath(
			QUuid::createUuid().toString(QUuid::WithoutBraces)
				+ QStringLiteral(".jpg"));
		if (!square.save(path, "JPEG", 90)) {
			path = QString();
		}
		QMetaObject::invokeMethod(qApp, [=] {
			if (done && (!bound || weak)) {
				done(path);
			}
		}, Qt::QueuedConnection);
	});
}

} // namespace Boxes
