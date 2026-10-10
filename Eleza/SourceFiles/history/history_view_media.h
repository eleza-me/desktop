// The attachment of a message row: photo / video / gif, round video, voice and
// music, file, sticker, location, a shared feed post, a call record. Created
// by the element, painted inside the rect the element gives it.
#pragma once

#include "history/history_view_element.h"
#include "ui/image/image_source.h"

#include <QtGui/QImage>

namespace History::View {

struct MediaPaint {
	const ChatColors *colors = nullptr;
	bool outgoing = false;
	bool hasCaption = false;
	bool selected = false;
	Zone hovered = Zone::None;
	// Corner radii of the picture: top-left, top-right, bottom-right, bottom-left.
	int radii[4] = { 0, 0, 0, 0 };
};

class Media {
public:
	Media(Delegate *delegate, Element *parent)
	: _delegate(delegate)
	, _parent(parent) {
	}
	virtual ~Media() = default;

	// The size inside a bubble that can be `maxWidth` wide.
	[[nodiscard]] virtual QSize countSize(int maxWidth, bool hasCaption) = 0;
	virtual void paint(Painter &p, const QRect &rect, const MediaPaint &context) = 0;
	[[nodiscard]] virtual HitResult hitTest(QPoint point, const QRect &rect) const;

	// Photos and videos fill the bubble edge to edge.
	[[nodiscard]] virtual bool fullBleed() const {
		return false;
	}
	// Stickers and round videos have no bubble.
	[[nodiscard]] virtual bool bubbleless() const {
		return false;
	}
	// The row keeps a free corner at the bottom where the time fits.
	[[nodiscard]] virtual bool hasMetaCorner() const {
		return false;
	}
	virtual void unload() {
	}
	// The same message in another state (progress, played, ...).
	virtual void dataChanged() {
	}
	// The picture as it is shown now (copy image, the viewer's first frame).
	[[nodiscard]] virtual QImage currentImage() const {
		return QImage();
	}
	[[nodiscard]] virtual std::shared_ptr<Ui::ImageSource> imageSource() const {
		return nullptr;
	}

protected:
	[[nodiscard]] const Data::Message &data() const {
		return _parent->data();
	}
	[[nodiscard]] Delegate *delegate() const {
		return _delegate;
	}
	[[nodiscard]] Element *parent() const {
		return _parent;
	}
	void repaint() const {
		_delegate->elementRepaint(_parent);
	}
	// Repaints the row when the source gets its image.
	void watch(const std::shared_ptr<Ui::ImageSource> &source);

private:
	Delegate *_delegate = nullptr;
	Element *_parent = nullptr;
	QMetaObject::Connection _watching;

};

// Null for messages without an attachment part (plain text, service).
[[nodiscard]] std::unique_ptr<Media> CreateMedia(
	Delegate *delegate,
	Element *parent);

// A picture from a local file, decoded on a worker thread (the preview of a
// photo that is still being sent, thumbnails of the send-files box).
[[nodiscard]] std::shared_ptr<Ui::ImageSource> MakeLocalImageSource(
	const QString &path,
	int maxSide = 1280);

// Paints `image` covering `rect` with the given corner radii, keeping the
// prepared copy in `cache` (pass the same cache for the same picture).
struct PreparedImage {
	QImage image;
	qint64 sourceKey = 0;
	QSize size;
	int corners = 0;
	bool blurred = false;
	double ratio = 0.;
};
void PaintCover(
	QPainter &p,
	const QRect &rect,
	const QImage &source,
	const int radii[4],
	bool blurred,
	PreparedImage &cache);

// A ring with 0..1 progress and an icon inside (upload / download).
void PaintProgressRing(
	QPainter &p,
	const QRect &circle,
	double progress,
	const style::icon &icon,
	const QColor &bg,
	const QColor &fg);

// «۲۳:۴۱», «0:42 / 1:10»… helpers shared with the composer bars.
[[nodiscard]] QString MediaDurationText(qint64 durationMs);

} // namespace History::View
