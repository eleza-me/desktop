// Userpics: round avatars with a colored-initial fallback.
//
// Painting inside your own row:
//   Ui::PaintUserpic(p, rect, image);                          // any QImage, cropped to a circle
//   Ui::PaintUserpicPlaceholder(p, rect, user.id, user.name);  // color from a stable hash of the id
//   Ui::PaintOnlineDot(p, rect, st::bgCanvas);                 // call after the userpic
//
// As a widget:
//   const auto pic = Ui::CreateChild<Ui::UserpicWidget>(this, 40);
//   pic->setPlaceholder(user.id, user.name);
//   pic->setSource(session.media().userpic(user));             // std::shared_ptr<Ui::ImageSource>
//   pic->setOnline(true);
#pragma once

#include "ui/widgets/buttons.h"
#include "ui/image/image_source.h"

#include <memory>

namespace Ui {

enum class UserpicKind {
	User, // colored circle with the first letter
	Anonymous, // the "anonymous" surface with the mask icon
	Saved, // brand circle with the bookmark icon (saved messages)
	Deleted, // neutral circle
};

// 0..7, stable for the same id on every run and platform.
[[nodiscard]] int UserpicColorIndex(QStringView id);
[[nodiscard]] style::color UserpicColor(int index);
// The first letter of the name (a whole grapheme, e.g. an emoji).
[[nodiscard]] QString UserpicInitial(const QString &name);

// A circle-cropped image prepared for `size` logical pixels (cache this).
[[nodiscard]] QImage PrepareUserpic(const QImage &image, int size, double ratio);

void PaintUserpic(QPainter &p, const QRect &rect, const QImage &image);
void PaintUserpicPlaceholder(
	QPainter &p,
	const QRect &rect,
	QStringView id,
	const QString &name,
	UserpicKind kind = UserpicKind::User);
// The online badge at the bottom trailing corner; `border` is the row background.
void PaintOnlineDot(QPainter &p, const QRect &userpicRect, style::color border);

class UserpicWidget : public AbstractButton {
public:
	UserpicWidget(QWidget *parent, int size);

	void setPlaceholder(
		const QString &id,
		const QString &name,
		UserpicKind kind = UserpicKind::User);
	void setImage(const QImage &image);
	void setSource(std::shared_ptr<ImageSource> source);
	void setOnline(bool online);
	// The color around the online dot (the background the widget lies on).
	void setBorderColor(style::color color);
	void setSize(int size);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void refreshFromSource();

	int _size = 0;
	QString _id;
	QString _name;
	UserpicKind _kind = UserpicKind::User;
	std::shared_ptr<ImageSource> _source;
	QMetaObject::Connection _sourceConnection;
	QImage _original;
	QImage _prepared;
	double _preparedRatio = 0.;
	bool _online = false;
	style::color _border;

};

} // namespace Ui
