// The info screen of a group: its picture, name and description, my
// notification level, the rows of what I may do (add members, invite links,
// join requests, permissions, edit, …) and the members, paged.
//
//   controller->showMainSection(Groups::InfoFactory(groupId), { .tab = Window::Tab::Chats });
//   controller->openRoute(Window::Routes::Group(groupId));   // the same
//
// The rows follow the header of the group (Data::Chat::group): what the
// server lists in `permissions` and `rights` is what is offered. The member
// list reads itself again when Data::Groups::membersChanged names the group.
#pragma once

#include "data/data_types.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"

namespace Ui {
class FlatLabel;
class ScrollArea;
class SettingsButton;
class TopBar;
class VerticalLayout;
} // namespace Ui

namespace Groups {

[[nodiscard]] Window::SectionFactory InfoFactory(const QString &groupId);

class InfoWidget final : public Window::SectionWidget {
public:
	InfoWidget(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &groupId);

	[[nodiscard]] QString sectionKey() const override;
	void setInnerFocus() override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	class Cover;
	class MembersList;

	void rebuild();
	void refresh();
	void updateGeometries();
	// What decides which rows there are: when it changes the rows are
	// built again, otherwise only their values follow.
	[[nodiscard]] QString shape(const Data::Group &group) const;
	Ui::SettingsButton *addRow(
		const QString &text,
		const style::icon *icon,
		Fn<void()> clicked,
		bool danger = false);
	void addTitle(const QString &text);
	void addDivider();
	void close();

	QString _groupId;
	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	Cover *_cover = nullptr;
	Ui::FlatLabel *_about = nullptr;
	Ui::SettingsButton *_notify = nullptr;
	Ui::SettingsButton *_requests = nullptr;
	Ui::SettingsButton *_slowMode = nullptr;
	Ui::SettingsButton *_hideMembers = nullptr;
	Ui::FlatLabel *_membersTitle = nullptr;
	MembersList *_members = nullptr;
	QString _shape;
	bool _closing = false;

};

} // namespace Groups
