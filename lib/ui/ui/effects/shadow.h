// Soft drop shadows (menus, boxes, toasts) and the 1px separator line.
//
//   Ui::Shadow::Paint(p, bodyRect, st::radiusLg, st::defaultShadow);  // then fill the body
#pragma once

#include "ui/rp_widget.h"
#include "styles/style_basic.h"

class QPainter;

namespace Ui {

namespace Shadow {

// Paints a blurred shadow around a rounded rect `body` (nothing inside is needed:
// paint the body over it afterwards).
void Paint(
	QPainter &p,
	const QRect &body,
	int radius,
	const style::Shadow &st,
	double opacity = 1.);

// Drops cached shadow patches (palette changes do it automatically).
void ClearCache();

} // namespace Shadow

// A separator line / hairline filled with a color.
class PlainShadow final : public RpWidget {
public:
	explicit PlainShadow(QWidget *parent);
	PlainShadow(QWidget *parent, style::color color);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	style::color _color;

};

} // namespace Ui
