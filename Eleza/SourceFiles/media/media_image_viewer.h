// Media::ImageViewer — the full-window overlay for photos (and videos): zoom
// with the wheel / + − / double click, drag to pan, ← → between the items of
// the list it was opened with, save as, Esc or a click outside closes.
//
//   auto items = std::vector<Media::ViewerItem>();
//   for (const auto &message : photos) {
//       auto item = Media::ViewerItem();
//       item.source = Support::MakeImageSource(&session, ref, Data::MediaVariant::Full, { .original = true });
//       item.preview = thumbSource;              // optional: what the bubble already shows
//       item.caption = message.text;
//       item.saveName = u"photo.jpg"_s;
//       items.push_back(std::move(item));
//   }
//   Media::ShowImageViewer(controller->window(), std::move(items), indexOfClicked);
//
// An item is one of: `source` (remote, through Support::MakeImageSource), `image`
// (already decoded), `filePath` (a local picture, decoded on a worker thread) or
// `videoPath` (a local video file: played by Media::VideoPlayerWidget).
#pragma once

#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_source.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <QtCore/QPointer>

#include <functional>
#include <memory>
#include <vector>

namespace Ui {
class IconButton;
} // namespace Ui

namespace Window {
class MainWindow;
} // namespace Window

namespace Media {

class VideoPlayerWidget;

struct ViewerItem {
	std::shared_ptr<Ui::ImageSource> source;
	std::shared_ptr<Ui::ImageSource> preview; // shown while `source` loads
	QImage image;
	QString filePath;
	QString videoPath;
	QImage videoPoster;
	QString caption;
	QString saveName; // suggested name for «save as»
};

class ImageViewer final : public Ui::RpWidget {
public:
	ImageViewer(QWidget *parent, std::vector<ViewerItem> items, int index);
	~ImageViewer();

	void showItem(int index);
	[[nodiscard]] int currentIndex() const {
		return _index;
	}
	void close();
	// Called after the viewer closed (before it is destroyed).
	void setClosedCallback(std::function<void()> callback) {
		_closed = std::move(callback);
	}
	// Called with the index when another item is shown (to load more, to
	// scroll the list behind).
	void setIndexChangedCallback(std::function<void(int index)> callback) {
		_indexChanged = std::move(callback);
	}

	static constexpr auto kMaxZoom = 8.;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	[[nodiscard]] const ViewerItem &item() const {
		return _items[_index];
	}
	void refreshImage();
	void loadFile(const QString &path, int index);
	void setImage(const QImage &image, bool final);
	void updateControls();
	void updateVideoGeometry();
	[[nodiscard]] QRect contentArea() const;
	// Widget pixels per image pixel when the image is fitted.
	[[nodiscard]] double fitScale() const;
	[[nodiscard]] QRectF imageRect() const;
	void setZoom(double zoom, QPointF around);
	void clampOffset();
	void step(int delta);
	void save();
	void copy();
	[[nodiscard]] QRect captionRect() const;

	std::vector<ViewerItem> _items;
	int _index = -1;
	QImage _image; // what is painted
	bool _imageFinal = false;
	bool _failed = false;
	QMetaObject::Connection _sourceConnection;
	QMetaObject::Connection _previewConnection;
	double _zoom = 1.; // 1 = fitted
	QPointF _offset; // of the image center from the area center
	bool _pressed = false;
	bool _dragged = false;
	bool _pressedOutside = false;
	QPoint _pressPosition;
	QPointF _pressOffset;

	Ui::IconButton *_closeButton = nullptr;
	Ui::IconButton *_saveButton = nullptr;
	Ui::IconButton *_previous = nullptr;
	Ui::IconButton *_next = nullptr;
	VideoPlayerWidget *_video = nullptr;
	Ui::Text::String _caption;
	Ui::InfiniteRadialAnimation _loading;
	Ui::Animations::Simple _showAnimation;
	QPointer<QWidget> _previousFocus;
	std::function<void()> _closed;
	std::function<void(int index)> _indexChanged;
	bool _closing = false;

};

// Shows the viewer over the whole window. The viewer deletes itself when closed.
ImageViewer *ShowImageViewer(
	Window::MainWindow *window,
	std::vector<ViewerItem> items,
	int index = 0);

} // namespace Media
