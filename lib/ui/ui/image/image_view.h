// Ui::ImageView — shows an image from a Ui::ImageSource with rounded corners.
//
//   const auto view = Ui::CreateChild<Ui::ImageView>(this);
//   view->setRadius(st::radiusMd);
//   view->setSource(source);            // blurred preview / skeleton until it is ready
//   view->setGeometry(...);
//
// The image covers the widget (center crop) or fits inside it (setFit).
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/image/image_source.h"
#include "ui/widgets/misc_widgets.h"

#include <memory>

namespace Ui {

class ImageView : public RpWidget {
public:
	explicit ImageView(QWidget *parent);

	void setSource(std::shared_ptr<ImageSource> source);
	void setImage(const QImage &image);
	void setRadius(int radius);
	void setRadius(int topLeft, int topRight, int bottomRight, int bottomLeft);
	// true: the whole image is visible (letterboxed); false (default): cover.
	void setFit(bool fit);
	// Keeps showing the blurred preview (spoiler media) until revealed.
	void setBlurred(bool blurred);
	[[nodiscard]] bool loaded() const {
		return !_image.isNull();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	void refreshFromSource();
	void invalidate();
	[[nodiscard]] QImage prepare(const QImage &image, bool blurred) const;

	std::shared_ptr<ImageSource> _source;
	QMetaObject::Connection _connection;
	QImage _image;
	QImage _preview;
	QImage _prepared;
	QImage _preparedPreview;
	QSize _requestedSize;
	int _radii[4] = { 0, 0, 0, 0 };
	bool _fit = false;
	bool _blurred = false;
	Animations::Simple _fadeAnimation;
	SkeletonAnimation _skeleton;

};

} // namespace Ui
