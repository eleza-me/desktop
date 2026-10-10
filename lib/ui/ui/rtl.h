// RTL helpers. The interface direction follows the language (Persian = RTL): widgets
// place children with "left" meaning the leading side, through these helpers and
// Ui::RpWidget::moveToLeft / moveToRight.
#pragma once

#include "ui/style/style_core_types.h"

namespace Ui {

[[nodiscard]] inline bool RightToLeft() {
	return style::RightToLeft();
}

[[nodiscard]] inline Qt::LayoutDirection LayoutDirection() {
	return style::RightToLeft() ? Qt::RightToLeft : Qt::LeftToRight;
}

// Direction of a piece of text by its first strong character
// (falls back to the interface direction).
[[nodiscard]] bool IsRtlText(const QString &text);

// Mirrors a logical alignment for painting: al_left -> right in RTL.
[[nodiscard]] inline style::align VisualAlign(style::align logical) {
	if (!style::RightToLeft()) {
		return logical;
	}
	const auto vertical = logical & Qt::AlignVertical_Mask;
	if (logical & Qt::AlignLeft) {
		return vertical | Qt::AlignRight;
	} else if (logical & Qt::AlignRight) {
		return vertical | Qt::AlignLeft;
	}
	return logical;
}

} // namespace Ui
