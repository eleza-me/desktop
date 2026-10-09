// Boxes::PhotoCropBox — choose the square part of a photo that becomes a round
// userpic: drag to move, wheel / slider / + − to zoom, a circular mask shows
// what will be visible.
//
//   Boxes::PickAndCropPhoto(this, [=](Ui::object_ptr<Ui::BoxContent> box) {
//       controller->show(std::move(box));              // or window->show(...)
//   }, [=](const QImage &square) {
//       ... show it, upload it (Boxes::SaveCroppedPhoto writes the JPEG off-thread) ...
//   });
//
//   // with an image that is already in memory
//   controller->show(Ui::Box<Boxes::PhotoCropBox>(image, [=](const QImage &square) { ... }));
//
// The result is a square QImage of at most kOutputSize pixels.
#pragma once

#include "ui/layers/box_content.h"

#include <QtGui/QImage>

namespace Boxes {

class PhotoCropBox final : public Ui::BoxContent {
public:
	PhotoCropBox(
		QWidget *parent,
		const QImage &image,
		std::function<void(const QImage &square)> done);

	static constexpr auto kOutputSize = 800;

protected:
	void prepare() override;
	void keyPressEvent(QKeyEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	class Area;
	class Slider;

	void submit();

	QImage _image;
	std::function<void(const QImage &square)> _done;
	Area *_area = nullptr;
	Slider *_slider = nullptr;

};

// Opens the file dialog, decodes the chosen picture on a worker thread
// (EXIF rotation applied, huge photos scaled down) and shows the crop box
// through `show`. A file that is not a picture shows `failed` (a toast).
void PickAndCropPhoto(
	QWidget *parent,
	std::function<void(Ui::object_ptr<Ui::BoxContent> box)> show,
	std::function<void(const QImage &square)> done,
	std::function<void()> failed = nullptr);

// Writes the cropped photo as a JPEG into the temporary directory on a worker
// thread (for Data::Upload); `done` gets the path ("" on failure).
void SaveCroppedPhoto(
	const QImage &square,
	QObject *guard,
	std::function<void(const QString &path)> done);

} // namespace Boxes
