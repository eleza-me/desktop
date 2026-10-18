// The circle around a face that says somebody has stories: brand for unseen,
// amber with a star for an unseen close-friends story, grey when all are
// seen. Shared by the tray, the feed cards and the profile.
//
//   Stories::PaintRing(p, userpicRect, session.data().stories().ring(userId));
//   // the ring lies outside `userpicRect` by RingOutset()
#pragma once

#include "data/data_story_types.h"

#include <QtCore/QRect>

class QPainter;

namespace Stories {

// How far the ring reaches outside the face (gap + width).
[[nodiscard]] int RingOutset();

// `progress` in 0..1 draws a posting ring instead (my circle while posting).
void PaintRing(
	QPainter &p,
	const QRect &userpic,
	Data::StoryRing ring,
	double progress = -1.);

} // namespace Stories
