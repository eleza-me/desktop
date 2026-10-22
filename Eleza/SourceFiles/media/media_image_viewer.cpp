#include "media/media_image_viewer.h"

#include "core/core_application.h"
#include "data/data_image_decode.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "media/media_video_player.h"
#include "support/file_dialogs.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "window/window_main_window.h"
#include "styles/style_media.h"

#include <QtCore/QFileInfo>
#include <QtCore/QThreadPool>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>

#include <cmath>

namespace Media {
namespace {

constexpr auto kDragThreshold = 4;
constexpr auto kZoomStep = 1.25;
constexpr auto kShowDuration = 140;
constexpr auto kMaxDecodedSide = 8192;

} // namespace

ImageViewer::ImageViewer(
	QWidget *parent,
	std::vector<ViewerItem> items,
	int index)
: RpWidget(parent)
, _items(std::move(items))
, _closeButton(new Ui::IconButton(this, st::viewerClose))
, _saveButton(new Ui::IconButton(this, st::viewerSave))
, _previous(new Ui::IconButton(this, st::viewerPrevious))
, _next(new Ui::IconButton(this, st::viewerNext))
, _loading([this] { update(); }) {
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	_previousFocus = QApplication::focusWidget();
	if (parent) {
		setGeometry(parent->rect());
		parent->installEventFilter(this);
	}
	_closeButton->setClickedCallback([this] { close(); });
	_saveButton->setClickedCallback([this] { save(); });
	_previous->setClickedCallback([this] { step(-1); });
	_next->setClickedCallback([this] { step(1); });
	_showAnimation.start([this] { update(); }, 0., 1., kShowDuration);
	if (!_items.empty()) {
		showItem(std::clamp(index, 0, int(_items.size()) - 1));
	}
}

ImageViewer::~ImageViewer() {
	QObject::disconnect(_sourceConnection);
	QObject::disconnect(_previewConnection);
}

ImageViewer *ShowImageViewer(
		Window::MainWindow *window,
		std::vector<ViewerItem> items,
		int index) {
	if (!window || items.empty()) {
		return nullptr;
	}
	const auto viewer = new ImageViewer(
		window->bodyWidget(),
		std::move(items),
		index);
	window->registerDarkOverlay(viewer);
	if (window->sessionController()) {
		// The account's photos do not stay over the intro after a sign-out.
		QObject::connect(
			&Core::App(),
			&Core::Application::sessionShownChanged,
			viewer,
			[=] {
				if (!window->sessionController()) {
					viewer->close();
				}
			});
	}
	viewer->show();
	viewer->raise();
	viewer->setFocus();
	return viewer;
}

bool ImageViewer::eventFilter(QObject *object, QEvent *event) {
	if (object == parentWidget() && event->type() == QEvent::Resize) {
		setGeometry(parentWidget()->rect());
	}
	return RpWidget::eventFilter(object, event);
}

void ImageViewer::close() {
	if (_closing) {
		return;
	}
	_closing = true;
	hide();
	if (_previousFocus) {
		_previousFocus->setFocus();
	}
	if (const auto closed = std::exchange(_closed, nullptr)) {
		closed();
	}
	deleteLater();
}

void ImageViewer::showItem(int index) {
	if (index < 0 || index >= int(_items.size()) || index == _index) {
		return;
	}
	QObject::disconnect(_sourceConnection);
	QObject::disconnect(_previewConnection);
	_index = index;
	_image = QImage();
	_imageFinal = false;
	_failed = false;
	_zoom = 1.;
	_offset = QPointF();
	if (_video) {
		delete std::exchange(_video, nullptr);
	}
	const auto &current = item();
	_caption.setText(st::viewerCaptionStyle, current.caption.trimmed());
	if (!current.videoPath.isEmpty()) {
		_video = new VideoPlayerWidget(this);
		_video->setPoster(current.videoPoster);
		_video->setSource(current.videoPath);
		_video->show();
		_video->setFocusProxy(this);
		QObject::connect(
			_video,
			&VideoPlayerWidget::videoSizeChanged,
			this,
			[this] { updateVideoGeometry(); });
		QObject::connect(
			_video,
			&VideoPlayerWidget::fullscreenChanged,
			this,
			[this](bool fullscreen) {
				if (!fullscreen) {
					updateVideoGeometry();
					_closeButton->raise();
					_saveButton->raise();
					_previous->raise();
					_next->raise();
				}
			});
		_imageFinal = true;
		updateVideoGeometry();
		_video->play();
	} else if (current.loadVideo) {
		if (!current.videoPoster.isNull()) {
			setImage(current.videoPoster, false);
		}
		loadVideo(index);
	} else if (!current.image.isNull()) {
		setImage(current.image, true);
	} else if (!current.filePath.isEmpty()) {
		loadFile(current.filePath, index);
	} else if (current.source) {
		_sourceConnection = QObject::connect(
			current.source.get(),
			&Ui::ImageSource::updated,
			this,
			[this] { refreshImage(); });
		if (current.preview) {
			_previewConnection = QObject::connect(
				current.preview.get(),
				&Ui::ImageSource::updated,
				this,
				[this] { refreshImage(); });
		}
		refreshImage();
	} else {
		_failed = true;
	}
	if (!_imageFinal && !_failed) {
		_loading.start();
	}
	updateControls();
	update();
	if (_indexChanged) {
		_indexChanged(_index);
	}
}

void ImageViewer::refreshImage() {
	const auto &current = item();
	if (!current.source) {
		return;
	}
	const auto ratio = devicePixelRatioF();
	const auto wanted = contentArea().size() * ratio;
	const auto full = current.source->request(wanted);
	if (!full.isNull()) {
		setImage(full, true);
		return;
	} else if (current.source->failed()) {
		_failed = true;
		_loading.stop();
		update();
		return;
	}
	if (_image.isNull()) {
		auto small = current.preview
			? current.preview->request(wanted)
			: QImage();
		if (small.isNull()) {
			small = current.source->preview();
		}
		if (!small.isNull()) {
			setImage(small, false);
		}
	}
}

void ImageViewer::loadFile(const QString &path, int index) {
	const auto weak = QPointer<ImageViewer>(this);
	QThreadPool::globalInstance()->start([=] {
		// A downloaded file is as untrusted as the bytes it came from.
		const auto image = Data::DecodeImageFile(path, kMaxDecodedSide);
		QMetaObject::invokeMethod(qApp, [=] {
			if (!weak || weak->_index != index) {
				return;
			} else if (image.isNull()) {
				weak->_failed = true;
				weak->_loading.stop();
				weak->update();
			} else {
				weak->setImage(image, true);
			}
		}, Qt::QueuedConnection);
	});
}

void ImageViewer::loadVideo(int index) {
	const auto weak = QPointer<ImageViewer>(this);
	_items[index].loadVideo(this, [=](const QString &path) {
		if (!weak || index < 0 || index >= int(weak->_items.size())) {
			return;
		} else if (path.isEmpty()) {
			if (weak->_index == index) {
				weak->_failed = true;
				weak->_loading.stop();
				weak->update();
			}
			return;
		}
		auto &item = weak->_items[index];
		item.videoPath = path;
		item.loadVideo = nullptr;
		if (weak->_index == index) {
			// Shown again, now as the player.
			weak->_index = -1;
			weak->showItem(index);
		}
	});
}

void ImageViewer::setImage(const QImage &image, bool final) {
	// The preview and the full picture have the same proportions: keep the
	// zoom and the position when the better one arrives.
	_image = image;
	_imageFinal = final;
	if (final) {
		_loading.stop();
	}
	clampOffset();
	update();
}

QRect ImageViewer::contentArea() const {
	const auto padding = int(st::viewerPadding);
	const auto horizontal = std::min(padding, width() / 8);
	return QRect(
		horizontal,
		st::viewerTopHeight,
		std::max(width() - 2 * horizontal, 1),
		std::max(height() - st::viewerTopHeight - horizontal, 1));
}

double ImageViewer::fitScale() const {
	if (_image.isNull()) {
		return 1.;
	}
	// The logical size of the picture: a preview stands for the full one.
	const auto area = contentArea();
	const auto ratio = devicePixelRatioF();
	const auto natural = QSizeF(_image.size()) / ratio;
	const auto fit = std::min(
		area.width() / natural.width(),
		area.height() / natural.height());
	// A small final picture is not blown up; a preview always fills.
	return _imageFinal ? std::min(fit, 1.) : fit;
}

QRectF ImageViewer::imageRect() const {
	if (_image.isNull()) {
		return QRectF();
	}
	const auto area = contentArea();
	const auto ratio = devicePixelRatioF();
	const auto scale = fitScale() * _zoom;
	const auto size = QSizeF(_image.size()) / ratio * scale;
	const auto center = QPointF(area.center()) + QPointF(0.5, 0.5) + _offset;
	return QRectF(
		center.x() - size.width() / 2.,
		center.y() - size.height() / 2.,
		size.width(),
		size.height());
}

void ImageViewer::clampOffset() {
	if (_image.isNull()) {
		_offset = QPointF();
		return;
	}
	const auto ratio = devicePixelRatioF();
	const auto size = QSizeF(_image.size()) / ratio * (fitScale() * _zoom);
	// The picture may be moved only as far as it overflows the window.
	const auto maxX = std::max((size.width() - width()) / 2., 0.);
	const auto maxY = std::max((size.height() - height()) / 2., 0.);
	_offset.setX(std::clamp(_offset.x(), -maxX, maxX));
	_offset.setY(std::clamp(_offset.y(), -maxY, maxY));
}

void ImageViewer::setZoom(double zoom, QPointF around) {
	zoom = std::clamp(zoom, 1., kMaxZoom);
	if (zoom == _zoom || _image.isNull() || _video) {
		return;
	}
	// Keep the point under `around` in place.
	const auto area = contentArea();
	const auto center = QPointF(area.center()) + QPointF(0.5, 0.5);
	const auto relative = around - center - _offset;
	const auto factor = zoom / _zoom;
	_offset -= relative * (factor - 1.);
	_zoom = zoom;
	if (_zoom == 1.) {
		_offset = QPointF();
	}
	clampOffset();
	setCursor((_zoom > 1.) ? Qt::OpenHandCursor : Qt::ArrowCursor);
	update();
}

void ImageViewer::step(int delta) {
	const auto index = _index + delta;
	if (index >= 0 && index < int(_items.size())) {
		showItem(index);
	}
}

void ImageViewer::save() {
	const auto &current = item();
	auto name = current.saveName;
	const auto path = !current.videoPath.isEmpty()
		? current.videoPath
		: current.filePath;
	if (!path.isEmpty()) {
		if (name.isEmpty()) {
			name = QFileInfo(path).fileName();
		}
		Support::SaveFileAs(this, path, name);
	} else if (!_image.isNull() && _imageFinal) {
		Support::SaveImageAs(
			this,
			_image,
			name.isEmpty() ? QStringLiteral("photo.jpg") : name);
	}
}

void ImageViewer::copy() {
	if (!_image.isNull() && _imageFinal && !_video) {
		QGuiApplication::clipboard()->setImage(_image);
	}
}

QRect ImageViewer::captionRect() const {
	if (_caption.isEmpty()) {
		return QRect();
	}
	const auto &padding = st::viewerCaptionPadding;
	const auto maxText = std::min(
		int(st::viewerCaptionMaxWidth),
		width() - 2 * int(st::viewerNavSkip) - padding.left() - padding.right());
	if (maxText <= 0) {
		return QRect();
	}
	const auto textWidth = std::min(
		_caption.countMaxLineWidth(maxText, st::viewerCaptionLines),
		maxText);
	const auto textHeight = _caption.countHeight(
		maxText,
		st::viewerCaptionLines);
	const auto boxWidth = textWidth + padding.left() + padding.right();
	const auto boxHeight = textHeight + padding.top() + padding.bottom();
	const auto bottom = int(st::viewerCaptionBottom);
	return QRect(
		(width() - boxWidth) / 2,
		height() - bottom - boxHeight,
		boxWidth,
		boxHeight);
}

void ImageViewer::updateControls() {
	const auto count = int(_items.size());
	_previous->setVisible(count > 1 && _index > 0);
	_next->setVisible(count > 1 && _index + 1 < count);
	const auto &current = item();
	_saveButton->setVisible(!current.videoPath.isEmpty()
		|| !current.filePath.isEmpty()
		|| !current.image.isNull()
		|| current.source != nullptr);

	const auto skip = int(st::viewerNavSkip);
	const auto top = (st::viewerTopHeight - _closeButton->height()) / 2;
	// Close and save sit at the trailing top corner.
	_closeButton->moveToRight(skip, top, width());
	_saveButton->moveToRight(skip + _closeButton->width() + 4, top, width());
	// «Previous» is on the leading side, «next» on the trailing side; the
	// chevrons point outwards in both directions of the interface.
	const auto navTop = (height() - _previous->height()) / 2;
	_previous->moveToLeft(skip, navTop, width());
	_next->moveToRight(skip, navTop, width());
	const auto rtl = style::RightToLeft();
	_previous->setIconOverride(
		rtl ? &st::viewerNext.icon : nullptr,
		rtl ? &st::viewerNext.iconOver : nullptr);
	_next->setIconOverride(
		rtl ? &st::viewerPrevious.icon : nullptr,
		rtl ? &st::viewerPrevious.iconOver : nullptr);
	_closeButton->raise();
	_saveButton->raise();
	_previous->raise();
	_next->raise();
}

void ImageViewer::updateVideoGeometry() {
	if (!_video || _video->fullscreen()) {
		return;
	}
	auto area = contentArea();
	if (const auto caption = captionRect(); !caption.isEmpty()) {
		// The player is a widget over our painting: the caption gets its own
		// strip under it.
		area.setBottom(std::min(area.bottom(), caption.top() - 12));
	}
	auto size = _video->videoSize();
	if (size.isEmpty()) {
		size = item().videoPoster.size();
	}
	if (size.isEmpty()) {
		size = QSize(16, 9);
	}
	const auto scaled = size.scaled(area.size(), Qt::KeepAspectRatio);
	_video->setGeometry(
		area.x() + (area.width() - scaled.width()) / 2,
		area.y() + (area.height() - scaled.height()) / 2,
		scaled.width(),
		scaled.height());
}

void ImageViewer::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (_index >= 0) {
		updateControls();
		updateVideoGeometry();
		clampOffset();
		if (!_imageFinal && item().source) {
			refreshImage();
		}
	}
}

void ImageViewer::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto shown = _showAnimation.value(1.);
	p.setOpacity(shown);
	p.fillRect(rect(), st::viewerBg);
	if (_index < 0) {
		return;
	}

	if (!_video) {
		if (!_image.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(imageRect(), _image);
		}
		if (_failed) {
			p.setFont(st::viewerErrorFont);
			p.setPen(st::mediaOverFgDim);
			p.drawText(rect(), Qt::AlignCenter, tr::lng_error_load(tr::now));
		} else if (!_imageFinal) {
			const auto size = int(st::viewerSpinnerSize);
			_loading.draw(
				p,
				QRect((width() - size) / 2, (height() - size) / 2, size, size),
				3.,
				st::mediaOverFg->c);
		}
	}

	// "2 / 5" at the leading top corner.
	if (_items.size() > 1) {
		const auto text = tr::lng_viewer_counter(
			tr::now,
			lt_index,
			Lang::Num(_index + 1),
			lt_total,
			Lang::Num(qint64(_items.size())));
		p.setFont(st::viewerCounterFont);
		p.setPen(st::viewerCounterFg);
		p.drawTextLeft(
			st::viewerNavSkip + 8,
			(st::viewerTopHeight - st::viewerCounterFont->height) / 2,
			width(),
			text);
	}

	if (const auto caption = captionRect(); !caption.isEmpty()) {
		const auto &padding = st::viewerCaptionPadding;
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::viewerCaptionBg);
			p.drawRoundedRect(
				caption,
				st::viewerCaptionRadius,
				st::viewerCaptionRadius);
		}
		p.setPen(st::viewerCaptionFg);
		auto context = Ui::Text::PaintContext();
		context.position = caption.topLeft()
			+ QPoint(padding.left(), padding.top());
		context.availableWidth = caption.width()
			- padding.left()
			- padding.right();
		context.elisionLines = st::viewerCaptionLines;
		context.autoDirection = true;
		_caption.draw(p, context);
	}
}

void ImageViewer::keyPressEvent(QKeyEvent *e) {
	const auto rtl = style::RightToLeft();
	const auto center = QPointF(contentArea().center());
	switch (e->key()) {
	case Qt::Key_Escape:
	case Qt::Key_Back:
		if (_zoom > 1.) {
			setZoom(1., center);
		} else {
			close();
		}
		break;
	case Qt::Key_Left:
		if (_video) {
			_video->seekBy(-5000);
		} else {
			step(rtl ? 1 : -1);
		}
		break;
	case Qt::Key_Right:
		if (_video) {
			_video->seekBy(5000);
		} else {
			step(rtl ? -1 : 1);
		}
		break;
	case Qt::Key_PageUp:
	case Qt::Key_Up: step(-1); break;
	case Qt::Key_PageDown:
	case Qt::Key_Down: step(1); break;
	case Qt::Key_Plus:
	case Qt::Key_Equal: setZoom(_zoom * kZoomStep, center); break;
	case Qt::Key_Minus: setZoom(_zoom / kZoomStep, center); break;
	case Qt::Key_0: setZoom(1., center); break;
	case Qt::Key_Space:
		if (_video) {
			_video->toggle();
		}
		break;
	case Qt::Key_F:
		if (_video) {
			_video->setFullscreen(!_video->fullscreen());
		}
		break;
	case Qt::Key_M:
		if (_video) {
			_video->setMuted(!_video->muted());
		}
		break;
	case Qt::Key_S:
		if (e->modifiers() & Qt::ControlModifier) {
			save();
		}
		break;
	case Qt::Key_C:
		if (e->modifiers() & Qt::ControlModifier) {
			copy();
		}
		break;
	default:
		break;
	}
	// Nothing reaches the window behind the viewer.
	e->accept();
}

void ImageViewer::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::BackButton) {
		close();
		return;
	} else if (e->button() != Qt::LeftButton) {
		return;
	}
	setFocus();
	_pressed = true;
	_dragged = false;
	_pressPosition = e->pos();
	_pressOffset = _offset;
	const auto over = _video
		? _video->geometry().contains(e->pos())
		: imageRect().contains(QPointF(e->pos()));
	_pressedOutside = !over && !captionRect().contains(e->pos());
	if (_zoom > 1. && !_pressedOutside) {
		setCursor(Qt::ClosedHandCursor);
	}
}

void ImageViewer::mouseMoveEvent(QMouseEvent *e) {
	if (!_pressed) {
		return;
	}
	const auto delta = e->pos() - _pressPosition;
	if (!_dragged && delta.manhattanLength() >= kDragThreshold) {
		_dragged = true;
	}
	if (_dragged && _zoom > 1. && !_pressedOutside) {
		_offset = _pressOffset + QPointF(delta);
		clampOffset();
		update();
	}
}

void ImageViewer::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_pressed) {
		return;
	}
	_pressed = false;
	if (_zoom > 1.) {
		setCursor(Qt::OpenHandCursor);
	}
	if (!_dragged && _pressedOutside) {
		close();
	}
}

void ImageViewer::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || _video || _image.isNull()) {
		return;
	}
	if (!imageRect().contains(QPointF(e->pos()))) {
		return;
	}
	// Fitted <-> the real size of the picture (at least 2x).
	if (_zoom > 1.) {
		setZoom(1., QPointF(e->pos()));
	} else {
		const auto real = 1. / std::max(fitScale(), 0.01);
		setZoom(std::clamp(real, 2., kMaxZoom), QPointF(e->pos()));
	}
}

void ImageViewer::wheelEvent(QWheelEvent *e) {
	const auto delta = e->angleDelta().y();
	if (delta && !_video) {
		setZoom(_zoom * std::pow(1.0015, delta), e->position());
	}
	e->accept();
}

} // namespace Media
