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
// Beyond the items it was opened with the viewer pages through its host
// (`ViewerOptions::paging`): close to either end it asks for the items past
// the one there, which join without moving the item on screen. A strip of
// every item runs along the bottom (click to jump, drag to scrub: only the
// item it stops on loads in full). The sound of videos is switched for the
// whole viewer (and remembered for the next one). With `origin` the viewer
// grows out of the item's place on screen and shrinks back into the place of
// the item it closes on (a bubble, a square of the shared media).
//
// An item is one of: `source` (remote, through Support::MakeImageSource), `image`
// (already decoded), `filePath` (a local picture, decoded on a worker thread) or
// `videoPath` (a local video file: played by Media::VideoPlayerWidget), or
// `loadVideo` (a video downloaded when it is shown).
#pragma once

#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_source.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <QtCore/QPointer>
#include <QtCore/QSet>

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
	// Identifies the item for its host (a message's key): paging skips the
	// ones the viewer has, `origin` finds its place on screen.
	QString key;
	std::shared_ptr<Ui::ImageSource> source;
	std::shared_ptr<Ui::ImageSource> preview; // shown while `source` loads
	QImage image;
	QString filePath;
	QString videoPath;
	QImage videoPoster;
	QString caption;
	QString saveName; // suggested name for «save as»
	// A video that is not downloaded yet: called when the item is shown,
	// `done` gets the local file (empty: it failed). `videoPoster` is shown
	// meanwhile. `guard` dies with the viewer.
	std::function<void(QObject *guard, std::function<void(QString)> done)> loadVideo;
};

struct ViewerPaging {
	// `direction` 1: the items after `edgeKey` (the last item held), -1:
	// the ones before it (the first). `done` gets them in the viewer's
	// order and whether more lie beyond; nothing and false end the paging
	// on that side (an error, a server without the route). `guard` dies
	// with the viewer.
	std::function<void(
		int direction,
		const QString &edgeKey,
		QObject *guard,
		std::function<void(std::vector<ViewerItem> items, bool more)> done)> load;
	bool moreAfter = false;
	bool moreBefore = false;
};

struct ViewerOptions {
	ViewerPaging paging;
	// Where the item is on screen now, in global coordinates (empty: not on
	// screen).
	std::function<QRect(const ViewerItem &item)> origin;
};

class ImageViewer final : public Ui::RpWidget {
public:
	ImageViewer(
		QWidget *parent,
		std::vector<ViewerItem> items,
		int index,
		ViewerOptions options = {});
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
	// By the serial number of the item (its index moves when items join in
	// front).
	void loadFile(const QString &path, int serial);
	void loadVideo(int serial);
	[[nodiscard]] int indexOfSerial(int serial) const;
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

	// Paging through the host.
	void maybeLoadMore();
	void loadMore(int direction);
	void insertItems(int direction, std::vector<ViewerItem> items);

	// The strip of every item.
	[[nodiscard]] bool stripShown() const;
	[[nodiscard]] QRect stripRect() const;
	[[nodiscard]] int stripStep() const;
	[[nodiscard]] QRect stripThumbRect(int index, int centered) const;
	[[nodiscard]] int stripIndexAt(QPoint position, int centered) const;
	[[nodiscard]] QImage stripImage(int index);
	void paintStrip(QPainter &p);

	// Growing out of / shrinking into the item's place on screen.
	[[nodiscard]] QRect originRect() const;
	void finishClose();
	void applyMuted(bool muted);

	std::vector<ViewerItem> _items;
	std::vector<int> _serials; // one per item, never reused
	int _lastSerial = 0;
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

	ViewerPaging _paging;
	std::function<QRect(const ViewerItem &item)> _origin;
	bool _loadingAfter = false;
	bool _loadingBefore = false;

	bool _stripPressed = false;
	bool _stripDragged = false;
	int _stripPressIndex = -1;
	int _stripScrub = -1; // the item the strip is dragged to (-1: none)
	QPoint _stripPressPosition;
	QSet<const Ui::ImageSource*> _stripWatched;

	// From the item's place on screen (0) to the fitted picture (1), and
	// back while closing.
	Ui::Animations::Simple _flyAnimation;
	QRectF _flyFrom;
	QRectF _flyTo;
	bool _flying = false;

};

// Shows the viewer over the whole window. The viewer deletes itself when closed.
ImageViewer *ShowImageViewer(
	Window::MainWindow *window,
	std::vector<ViewerItem> items,
	int index = 0,
	ViewerOptions options = {});

} // namespace Media
