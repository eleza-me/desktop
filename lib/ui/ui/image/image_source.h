// Ui::ImageSource — where widgets get remote / cached images from.
//
// ElezaUi does not know about the network or the media cache: the application
// implements this interface on top of them (Data::… media cache) and hands shared
// pointers to widgets (Ui::ImageView, Ui::UserpicWidget, list rows).
//
//   class MediaImageSource : public Ui::ImageSource {
//       QImage request(QSize size) override {
//           if (auto ready = cache.find(id, size)) return *ready;   // decoded and scaled
//           cache.load(id, size, this, [=] { emit updated(); });    // async, then notify
//           return QImage();
//       }
//       QImage preview() const override { return blurhashThumb; }   // optional
//   };
#pragma once

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtGui/QImage>

#include <memory>

namespace Ui {

class ImageSource : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;

	// The image for a target size in device pixels. Returns a null image when
	// it is not ready yet: the source starts loading and emits updated() later
	// (then the widget calls request() again). May return a smaller image
	// first and a better one after another updated().
	[[nodiscard]] virtual QImage request(QSize size) = 0;

	// A tiny placeholder available right away (blurhash, cached thumbnail).
	[[nodiscard]] virtual QImage preview() const {
		return QImage();
	}

	// Loading failed for good; widgets show a neutral placeholder.
	[[nodiscard]] virtual bool failed() const {
		return false;
	}

Q_SIGNALS:
	void updated();

};

// An ImageSource around an image that is already in memory.
class StaticImageSource final : public ImageSource {
public:
	explicit StaticImageSource(QImage image, QObject *parent = nullptr)
	: ImageSource(parent)
	, _image(std::move(image)) {
	}

	QImage request(QSize size) override {
		Q_UNUSED(size);
		return _image;
	}

private:
	QImage _image;

};

[[nodiscard]] inline std::shared_ptr<ImageSource> MakeStaticImageSource(
		QImage image) {
	return std::make_shared<StaticImageSource>(std::move(image));
}

} // namespace Ui
