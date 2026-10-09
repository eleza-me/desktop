// The side column of the «من» tab: my profile in short and the quick links
// (edit, bookmarks, follow requests, share, settings). In the wide layout the
// main column shows the full profile next to it.
#pragma once

#include "window/window_section_widget.h"

namespace Ui {
class ScrollArea;
class SettingsButton;
class TopBar;
class VerticalLayout;
} // namespace Ui

namespace Profile {

class SideWidget final : public Window::SectionWidget {
public:
	SideWidget(QWidget *parent, Window::SessionController *controller);

	QString sectionKey() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	class Summary;

	void refresh();
	void ensureMainProfile();
	void openProfile();
	void openBookmarks();

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_layout = nullptr;
	Summary *_summary = nullptr;
	Ui::SettingsButton *_requests = nullptr;

};

} // namespace Profile
