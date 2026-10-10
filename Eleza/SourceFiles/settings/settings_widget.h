// Settings::Widget — the side column of the «تنظیمات» tab: the own profile row
// (opens the «من» tab) and the list of sections; a section opens its page in the
// main column (route settings/<section>).
#pragma once

#include "settings/settings_logic.h"
#include "window/window_section_widget.h"

#include <vector>

namespace Ui {
class ScrollArea;
class SettingsButton;
class TopBar;
class VerticalLayout;
} // namespace Ui

namespace Settings {

class Widget final : public Window::SectionWidget {
public:
	Widget(QWidget *parent, Window::SessionController *controller);

	QString sectionKey() const override;
	void setInnerFocus() override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	class ProfileRow;

	void build();
	Ui::SettingsButton *addSection(Section section, const style::icon *icon);
	void refreshActive();
	void refreshValues();
	void refreshUpdateValue();

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	std::vector<std::pair<Section, Ui::SettingsButton*>> _sections;

};

} // namespace Settings
