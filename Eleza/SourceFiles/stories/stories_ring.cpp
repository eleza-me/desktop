#include "stories/stories_ring.h"

#include "ui/painter.h"
#include "ui/rtl.h"
#include "styles/style_stories.h"

#include <QtGui/QConicalGradient>
#include <QtGui/QPainterPath>

#include <cmath>

namespace Stories {
namespace {

void PaintStar(QPainter &p, QPointF center, double radius) {
	auto path = QPainterPath();
	const auto inner = radius * 0.45;
	for (auto i = 0; i != 10; ++i) {
		const auto angle = (-90. + i * 36.) * M_PI / 180.;
		const auto r = (i % 2) ? inner : radius;
		const auto point = center + QPointF(std::cos(angle) * r, std::sin(angle) * r);
		if (!i) {
			path.moveTo(point);
		} else {
			path.lineTo(point);
		}
	}
	path.closeSubpath();
	p.fillPath(path, st::storiesRingStar);
}

} // namespace

int RingOutset() {
	return st::storiesRingGap + st::storiesRingWidth;
}

void PaintRing(
		QPainter &p,
		const QRect &userpic,
		Data::StoryRing ring,
		double progress) {
	if (ring == Data::StoryRing::None && progress < 0.) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto width = double(st::storiesRingWidth);
	const auto inset = st::storiesRingGap + width / 2.;
	const auto rect = QRectF(userpic).adjusted(-inset, -inset, inset, inset);
	p.save();
	p.setBrush(Qt::NoBrush);
	if (progress >= 0.) {
		p.setPen(QPen(st::storiesRingProgressBg->c, width));
		p.drawEllipse(rect);
		auto pen = QPen(st::storiesRingUnseen->c, width);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		const auto span = int(std::clamp(progress, 0.02, 1.) * 360 * 16);
		p.drawArc(rect, 90 * 16, style::RightToLeft() ? span : -span);
		p.restore();
		return;
	}
	switch (ring) {
	case Data::StoryRing::Unseen:
		p.setPen(QPen(st::storiesRingUnseen->c, width));
		p.drawEllipse(rect);
		break;
	case Data::StoryRing::Seen:
		p.setPen(QPen(st::storiesRingSeen->c, width * 0.75));
		p.drawEllipse(rect);
		break;
	case Data::StoryRing::CloseFriends: {
		auto gradient = QConicalGradient(rect.center(), 90.);
		gradient.setColorAt(0., st::storiesRingCloseStart->c);
		gradient.setColorAt(0.5, st::storiesRingCloseEnd->c);
		gradient.setColorAt(1., st::storiesRingCloseStart->c);
		p.setPen(QPen(QBrush(gradient), width));
		p.drawEllipse(rect);
		// The star badge on the ring at the top trailing corner.
		const auto radius = rect.width() / 2.;
		const auto angle = (style::RightToLeft() ? -135. : -45.) * M_PI / 180.;
		const auto center = rect.center()
			+ QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
		const auto badge = std::max(rect.width() * 0.12, 6.);
		p.setPen(Qt::NoPen);
		p.setBrush(st::storiesRingCloseStart);
		p.drawEllipse(center, badge, badge);
		PaintStar(p, center, badge * 0.62);
	} break;
	case Data::StoryRing::None:
		break;
	}
	p.restore();
}

} // namespace Stories
