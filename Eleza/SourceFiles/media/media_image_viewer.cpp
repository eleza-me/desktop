#include "media/media_image_viewer.h"

#include "core/core_application.h"
#include "data/data_image_decode.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "media/media_video_player.h"
#include "support/file_dialogs.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "window/window_main_window.h"
#include "styles/style_media.h"

#include <QtCore/QFileInfo>
#include <QtCore/QThreadPool>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>

#include <cmath>

namespace Media {
namespace {

constexpr auto kDragThreshold = 4;
constexpr auto kZoomStep = 1.25;
constexpr auto kShowDuration = 140;
constexpr auto kMaxDecodedSide = 8192;
constexpr auto kFlyDuration = 200;
// How close to either end the viewer asks its host for more.
constexpr auto kPreloadItems = 3;

// The sound of videos, switched for the whole viewer and kept for the next.
auto ViewerMuted = false;

[[nodiscard]] QRectF Mix(const QRectF &a, const QRectF &b, double t) {
	return QRectF(
		a.x() + (b.x() - a.x()) * t,
		a.y() + (b.y() - a.y()) * t,
		a.width() + (b.width() - a.width()) * t,
		a.height() + (b.height() - a.height()) * t);
}

// `size` scaled to cover `box`, centred on it.
[[nodiscard]] QRectF Cover(QSizeF size, const QRectF &box) {
	if (size.isEmpty()) {
		return box;
	}
	const auto scale = std::max(
		box.width() / size.width(),
		box.height() / size.height());
	const auto scaled = size * scale;
	return QRectF(
		box.center().x() - scaled.width() / 2.,
		box.center().y() - scaled.height() / 2.,
		scaled.width(),
		scaled.height());
}

} // namespace

ImageViewer::ImageViewer(
	QWidget *parent,
	std::vector<ViewerItem> items,
	int index,
	ViewerOptions options)
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
	_paging = std::move(options.paging);
	_origin = std::move(options.origin);
	for (auto i = 0; i != int(_items.size()); ++i) {
		_serials.push_back(++_lastSerial);
	}
	_showAnimation.start([this] { update(); }, 0., 1., kShowDuration);
	if (!_items.empty()) {
		showItem(std::clamp(index, 0, int(_items.size()) - 1));
	}
	// Out of the item's place on screen, when it is on screen.
	const auto from = originRect();
	if (!from.isEmpty() && !_image.isNull() && !_video) {
		_flyFrom = QRectF(from);
		_flying = true;
		_flyAnimation.start([this] {
			if (!_flyAnimation.animating() && !_closing) {
				_flying = false;
			}
			update();
		}, 0., 1., kFlyDuration);
	}
}

ImageViewer::~ImageViewer() {
	QObject::disconnect(_sourceConnection);
	QObject::disconnect(_previewConnection);
}

ImageViewer *ShowImageViewer(
		Window::MainWindow *window,
		std::vector<ViewerItem> items,
		int index,
		ViewerOptions options) {
	if (!window || items.empty()) {
		return nullptr;
	}
	const auto viewer = new ImageViewer(
		window->bodyWidget(),
		std::move(items),
		index,
		std::move(options));
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
	if (_previousFocus) {
		_previousFocus->setFocus();
	}
	if (const auto closed = std::exchange(_closed, nullptr)) {
		closed();
	}
	// Into the place of the item it closes on, when that is on screen.
	const auto to = originRect();
	if (!to.isEmpty()
		&& !_image.isNull()
		&& !_video
		&& _zoom == 1.
		&& !Ui::AnimationsDisabled()) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		for (const auto button : { _closeButton, _saveButton, _previous, _next }) {
			button->hide();
		}
		_flyFrom = QRectF(to);
		_flying = true;
		_flyAnimation.start([this] {
			update();
			if (!_flyAnimation.animating()) {
				finishClose();
			}
		}, _flyAnimation.value(1.), 0., kFlyDuration);
		return;
	}
	finishClose();
}

void ImageViewer::finishClose() {
	hide();
	deleteLater();
}

QRect ImageViewer::originRect() const {
	if (!_origin || _index < 0) {
		return QRect();
	}
	const auto global = _origin(item());
	if (global.isEmpty()) {
		return QRect();
	}
	const auto local = QRect(mapFromGlobal(global.topLeft()), global.size());
	return local.intersects(rect()) ? local : QRect();
}

void ImageViewer::applyMuted(bool muted) {
	ViewerMuted = muted;
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
		_video->setMuted(ViewerMuted);
		QObject::connect(
			_video,
			&VideoPlayerWidget::mutedChanged,
			this,
			[this](bool muted) { applyMuted(muted); });
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
		loadVideo(_serials[index]);
	} else if (!current.image.isNull()) {
		setImage(current.image, true);
	} else if (!current.filePath.isEmpty()) {
		loadFile(current.filePath, _serials[index]);
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
	maybeLoadMore();
}

// ---- paging through the host

void ImageViewer::maybeLoadMore() {
	if (!_paging.load || _items.empty() || _closing) {
		return;
	}
	if (_paging.moreAfter
		&& !_loadingAfter
		&& _index + kPreloadItems >= int(_items.size()) - 1) {
		loadMore(1);
	}
	if (_paging.moreBefore && !_loadingBefore && _index <= kPreloadItems) {
		loadMore(-1);
	}
}

void ImageViewer::loadMore(int direction) {
	auto &loading = (direction > 0) ? _loadingAfter : _loadingBefore;
	loading = true;
	const auto edge = (direction > 0) ? _items.back().key : _items.front().key;
	const auto weak = QPointer<ImageViewer>(this);
	_paging.load(direction, edge, this, [=](
			std::vector<ViewerItem> items,
			bool more) {
		if (!weak) {
			return;
		}
		((direction > 0) ? weak->_loadingAfter : weak->_loadingBefore) = false;
		((direction > 0) ? weak->_paging.moreAfter : weak->_paging.moreBefore)
			= more && !items.empty();
		weak->insertItems(direction, std::move(items));
	});
}

void ImageViewer::insertItems(int direction, std::vector<ViewerItem> items) {
	auto known = QSet<QString>();
	for (const auto &item : _items) {
		if (!item.key.isEmpty()) {
			known.insert(item.key);
		}
	}
	auto fresh = std::vector<ViewerItem>();
	for (auto &item : items) {
		if (item.key.isEmpty() || !known.contains(item.key)) {
			known.insert(item.key);
			fresh.push_back(std::move(item));
		}
	}
	if (fresh.empty()) {
		return;
	}
	const auto count = int(fresh.size());
	auto serials = std::vector<int>();
	for (auto i = 0; i != count; ++i) {
		serials.push_back(++_lastSerial);
	}
	if (direction > 0) {
		_items.insert(
			_items.end(),
			std::make_move_iterator(fresh.begin()),
			std::make_move_iterator(fresh.end()));
		_serials.insert(_serials.end(), serials.begin(), serials.end());
	} else {
		// In front: the item on screen stays where it is.
		_items.insert(
			_items.begin(),
			std::make_move_iterator(fresh.begin()),
			std::make_move_iterator(fresh.end()));
		_serials.insert(_serials.begin(), serials.begin(), serials.end());
		_index += count;
		if (_stripScrub >= 0) {
			_stripScrub += count;
		}
		if (_stripPressIndex >= 0) {
			_stripPressIndex += count;
		}
	}
	updateControls();
	updateVideoGeometry();
	update();
	if (_indexChanged) {
		_indexChanged(_index);
	}
	maybeLoadMore();
}

int ImageViewer::indexOfSerial(int serial) const {
	for (auto i = 0; i != int(_serials.size()); ++i) {
		if (_serials[i] == serial) {
			return i;
		}
	}
	return -1;
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

void ImageViewer::loadFile(const QString &path, int serial) {
	const auto weak = QPointer<ImageViewer>(this);
	QThreadPool::globalInstance()->start([=] {
		// A downloaded file is as untrusted as the bytes it came from.
		const auto image = Data::DecodeImageFile(path, kMaxDecodedSide);
		QMetaObject::invokeMethod(qApp, [=] {
			if (!weak || weak->_index != weak->indexOfSerial(serial)) {
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

void ImageViewer::loadVideo(int serial) {
	const auto weak = QPointer<ImageViewer>(this);
	const auto start = indexOfSerial(serial);
	if (start < 0) {
		return;
	}
	_items[start].loadVideo(this, [=](const QString &path) {
		// Items may have joined in front meanwhile.
		const auto index = weak ? weak->indexOfSerial(serial) : -1;
		if (index < 0) {
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
	const auto bottom = stripShown()
		? int(st::viewerStripHeight + st::viewerStripBottom)
		: horizontal;
	return QRect(
		horizontal,
		st::viewerTopHeight,
		std::max(width() - 2 * horizontal, 1),
		std::max(height() - st::viewerTopHeight - bottom, 1));
}

// ---- the strip

bool ImageViewer::stripShown() const {
	return (_items.size() > 1) && !(_video && _video->fullscreen());
}

QRect ImageViewer::stripRect() const {
	if (!stripShown()) {
		return QRect();
	}
	return QRect(
		0,
		height() - st::viewerStripBottom - st::viewerStripHeight,
		width(),
		st::viewerStripHeight);
}

int ImageViewer::stripStep() const {
	return st::viewerStripThumb + st::viewerStripSkip;
}

QRect ImageViewer::stripThumbRect(int index, int centered) const {
	const auto strip = stripRect();
	const auto current = int(st::viewerStripCurrent);
	const auto thumb = int(st::viewerStripThumb);
	const auto skip = int(st::viewerStripSkip);
	const auto offset = index - centered;
	// Later items on the trailing side, as the arrows.
	const auto sign = style::RightToLeft() ? -1 : 1;
	const auto center = strip.x() + strip.width() / 2;
	if (offset == 0) {
		return QRect(
			center - current / 2,
			strip.y() + (strip.height() - current) / 2,
			current,
			current);
	}
	const auto distance = current / 2
		+ skip
		+ (std::abs(offset) - 1) * (thumb + skip);
	const auto left = (offset * sign > 0)
		? (center + distance)
		: (center - distance - thumb);
	return QRect(left, strip.y() + (strip.height() - thumb) / 2, thumb, thumb);
}

int ImageViewer::stripIndexAt(QPoint position, int centered) const {
	if (!stripRect().contains(position)) {
		return -1;
	}
	const auto half = width() / (2 * stripStep()) + 2;
	const auto from = std::max(centered - half, 0);
	const auto till = std::min(centered + half + 1, int(_items.size()));
	for (auto i = from; i != till; ++i) {
		if (stripThumbRect(i, centered).contains(position)) {
			return i;
		}
	}
	return -1;
}

QImage ImageViewer::stripImage(int index) {
	const auto &entry = _items[index];
	const auto size = QSize(st::viewerStripCurrent, st::viewerStripCurrent)
		* devicePixelRatioF();
	const auto watch = [&](const std::shared_ptr<Ui::ImageSource> &source) {
		if (source && !_stripWatched.contains(source.get())) {
			_stripWatched.insert(source.get());
			QObject::connect(
				source.get(),
				&Ui::ImageSource::updated,
				this,
				[this] { update(stripRect()); });
		}
	};
	if (entry.preview) {
		watch(entry.preview);
		auto result = entry.preview->request(size);
		if (!result.isNull()) {
			return result;
		}
		result = entry.preview->preview();
		if (!result.isNull()) {
			return result;
		}
	}
	if (!entry.image.isNull()) {
		return entry.image;
	} else if (!entry.videoPoster.isNull()) {
		return entry.videoPoster;
	} else if (entry.source) {
		// The full picture only once it is here (the item was shown).
		watch(entry.source);
		return entry.source->preview();
	}
	return QImage();
}

void ImageViewer::paintStrip(QPainter &p) {
	if (!stripShown()) {
		return;
	}
	const auto centered = (_stripScrub >= 0) ? _stripScrub : _index;
	const auto half = width() / (2 * stripStep()) + 2;
	const auto from = std::max(centered - half, 0);
	const auto till = std::min(centered + half + 1, int(_items.size()));
	auto hq = PainterHighQualityEnabler(p);
	for (auto i = from; i != till; ++i) {
		const auto box = QRectF(stripThumbRect(i, centered));
		if (!box.intersects(QRectF(rect()))) {
			continue;
		}
		auto path = QPainterPath();
		path.addRoundedRect(box, st::viewerStripRadius, st::viewerStripRadius);
		p.save();
		p.setClipPath(path);
		p.fillRect(box, st::mediaOverBg);
		const auto image = stripImage(i);
		if (!image.isNull()) {
			p.drawImage(Cover(QSizeF(image.size()), box), image);
		}
		if (i != centered) {
			p.fillRect(box, st::viewerStripDim);
		}
		p.restore();
	}
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
	const auto bottom = int(st::viewerCaptionBottom)
		+ (stripShown()
			? int(st::viewerStripHeight + st::viewerStripBottom)
			: 0);
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
	if (_closing) {
		_previous->hide();
		_next->hide();
	}
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
	const auto fly = _flying ? _flyAnimation.value(_closing ? 0. : 1.) : 1.;
	const auto shown = _flying ? fly : _showAnimation.value(1.);
	p.setOpacity(shown);
	p.fillRect(rect(), st::viewerBg);
	if (_index < 0) {
		return;
	}
	if (_flying) {
		// The picture between its place on screen (0) and here (1).
		p.setOpacity(1.);
		if (!_image.isNull()) {
			const auto fitted = imageRect();
			const auto clip = Mix(_flyFrom, QRectF(rect()), fly);
			const auto target = Mix(
				Cover(fitted.size(), _flyFrom),
				fitted,
				fly);
			auto hq = PainterHighQualityEnabler(p);
			p.save();
			auto path = QPainterPath();
			const auto radius = (1. - fly) * st::viewerStripRadius;
			path.addRoundedRect(clip, radius, radius);
			p.setClipPath(path);
			p.drawImage(target, _image);
			p.restore();
		}
		if (_closing || fly < 1.) {
			return;
		}
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

	paintStrip(p);

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
	if (stripShown() && stripRect().contains(e->pos())) {
		_stripPressed = true;
		_stripDragged = false;
		_stripPressPosition = e->pos();
		_stripPressIndex = stripIndexAt(e->pos(), _index);
		return;
	}
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
	if (_stripPressed) {
		// Scrubbing: the strip follows the pointer, the item it stops on
		// is shown when the button is let go.
		const auto dx = e->pos().x() - _stripPressPosition.x();
		if (!_stripDragged && std::abs(dx) >= kDragThreshold) {
			_stripDragged = true;
		}
		if (_stripDragged) {
			const auto moved = int(std::lround(
				double(style::RightToLeft() ? dx : -dx) / stripStep()));
			const auto scrub = std::clamp(
				_index + moved,
				0,
				int(_items.size()) - 1);
			if (scrub != _stripScrub) {
				_stripScrub = scrub;
				update(stripRect());
			}
		}
		return;
	} else if (!_pressed) {
		if (stripIndexAt(e->pos(), _index) >= 0) {
			setCursor(style::cur_pointer);
		} else {
			setCursor((_zoom > 1.) ? Qt::OpenHandCursor : Qt::ArrowCursor);
		}
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
	if (e->button() == Qt::LeftButton && _stripPressed) {
		_stripPressed = false;
		const auto target = _stripDragged
			? _stripScrub
			: ((stripIndexAt(e->pos(), _index) == _stripPressIndex)
				? _stripPressIndex
				: -1);
		_stripScrub = -1;
		_stripPressIndex = -1;
		if (target >= 0 && target != _index) {
			showItem(target);
		} else {
			update(stripRect());
		}
		return;
	} else if (e->button() != Qt::LeftButton || !_pressed) {
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
	if (delta && stripShown() && stripRect().contains(e->position().toPoint())) {
		// Over the strip the wheel goes through the items.
		step((delta > 0) ? -1 : 1);
	} else if (delta && !_video) {
		setZoom(_zoom * std::pow(1.0015, delta), e->position());
	}
	e->accept();
}

} // namespace Media
