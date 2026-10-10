// Small layout helpers shared by the profile sections.
#pragma once

#include "ui/rp_widget.h"

namespace Profile {

// Keeps its only child in a centred column of at most `maxWidth` (the web
// client's desktop layout) while the scroll area around it spans the section.
class CenteredWrap final : public Ui::RpWidget {
public:
	CenteredWrap(QWidget *parent, int maxWidth);

	template <typename Widget>
	Widget *setChild(Ui::object_ptr<Widget> child) {
		const auto result = child.data();
		setChildWidget(child.release());
		return result;
	}
	// The child fills at least this height (empty states are centred in it).
	void setMinHeight(int height);
	[[nodiscard]] Ui::RpWidget *child() const {
		return _child;
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	void setChildWidget(Ui::RpWidget *child);

	Ui::RpWidget *_child = nullptr;
	int _maxWidth = 0;
	int _minHeight = 0;
	bool _inResize = false;

};

// The column a section of `width` gives its content: [left, width).
struct Column {
	int left = 0;
	int width = 0;
};
[[nodiscard]] Column CenteredColumn(int outerWidth, int maxWidth);

} // namespace Profile
