// What the screens of groups share: one centred column inside a scroll area
// (the info screen, recent actions), like the pages of the settings.
//
//   _content = new Ui::VerticalLayout();
//   _scroll->setOwnedWidget(Ui::object_ptr<Groups::ColumnWrap>(_scroll, _content));
#pragma once

#include "ui/rp_widget.h"
#include "ui/wrap/vertical_layout.h"

namespace Groups {

class ColumnWrap final : public Ui::RpWidget {
public:
	ColumnWrap(QWidget *parent, Ui::VerticalLayout *content);

protected:
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	Ui::VerticalLayout *_content = nullptr;
	bool _inResize = false;

};

} // namespace Groups
