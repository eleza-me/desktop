#include "window/window_placeholder_section.h"

#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/top_bar.h"
#include "window/window_session_controller.h"
#include "styles/style_window.h"

namespace Window {
namespace {

[[nodiscard]] const style::icon *TabIcon(Tab tab) {
	switch (tab) {
	case Tab::Eli: return &st::placeholderIconEli;
	case Tab::Chats: return &st::placeholderIconChats;
	case Tab::Search: return &st::placeholderIconSearch;
	case Tab::Me: return &st::placeholderIconMe;
	case Tab::Settings: return &st::placeholderIconSettings;
	}
	return nullptr;
}

} // namespace

QString TabTitle(Tab tab) {
	switch (tab) {
	case Tab::Eli: return tr::lng_tab_eli(tr::now);
	case Tab::Chats: return tr::lng_messages(tr::now);
	case Tab::Search: return tr::lng_tab_search(tr::now);
	case Tab::Me: return tr::lng_tab_me(tr::now);
	case Tab::Settings: return tr::lng_settings(tr::now);
	}
	return QString();
}

QString TabEmptyHint(Tab tab) {
	switch (tab) {
	case Tab::Eli: return tr::lng_main_pick_post(tr::now);
	case Tab::Chats: return tr::lng_main_pick_chat(tr::now);
	case Tab::Search: return tr::lng_main_pick_search(tr::now);
	case Tab::Me: return tr::lng_main_your_profile(tr::now);
	case Tab::Settings: return tr::lng_settings(tr::now);
	}
	return QString();
}

PlaceholderSection::PlaceholderSection(
	QWidget *parent,
	SessionController *controller,
	Tab tab)
: SectionWidget(parent, controller)
, _bar(new Ui::TopBar(this)) {
	_bar->setTitle(TabTitle(tab));
	setTopBar(_bar);
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	descriptor.icon = TabIcon(tab);
	descriptor.title = tr::lng_soon(tr::now);
	descriptor.text = tr::lng_main_section_soon(tr::now);
	_placeholder = new Ui::EmptyPlaceholder(this, std::move(descriptor));
}

SectionFactory PlaceholderSection::Factory(Tab tab) {
	return [tab](QWidget *parent, SessionController *controller) {
		return Ui::Owned<SectionWidget>(
			new PlaceholderSection(parent, controller, tab));
	};
}

void PlaceholderSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_placeholder->setGeometry(
		0,
		_bar->height(),
		width(),
		std::max(height() - _bar->height(), 0));
}

PlaceholderMainSection::PlaceholderMainSection(
	QWidget *parent,
	SessionController *controller,
	int depth)
: SectionWidget(parent, controller)
, _depth(depth)
, _bar(new Ui::TopBar(this)) {
	const auto tab = controller->currentTab();
	_bar->setTitle(TabTitle(tab));
	_bar->setSubtitle(Lang::Num(depth));
	setTopBar(_bar);
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	descriptor.icon = TabIcon(tab);
	descriptor.title = tr::lng_soon(tr::now);
	descriptor.text = tr::lng_main_section_soon(tr::now);
	descriptor.buttonText = tr::lng_continue(tr::now);
	descriptor.buttonCallback = [controller, depth] {
		controller->showMainSection(Factory(depth + 1));
	};
	_placeholder = new Ui::EmptyPlaceholder(this, std::move(descriptor));
}

SectionFactory PlaceholderMainSection::Factory(int depth) {
	return [depth](QWidget *parent, SessionController *controller) {
		return Ui::Owned<SectionWidget>(
			new PlaceholderMainSection(parent, controller, depth));
	};
}

QString PlaceholderMainSection::sectionKey() const {
	return QStringLiteral("debug/main/%1").arg(_depth);
}

void PlaceholderMainSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_placeholder->setGeometry(
		0,
		_bar->height(),
		width(),
		std::max(height() - _bar->height(), 0));
}

RoutePlaceholderSection::RoutePlaceholderSection(
	QWidget *parent,
	SessionController *controller,
	const Route &route)
: SectionWidget(parent, controller)
, _key(route.toString())
, _bar(new Ui::TopBar(this)) {
	const auto tab = RouteTab(route, controller->currentTab());
	_bar->setTitle(TabTitle(tab));
	_bar->setSubtitle(_key);
	setTopBar(_bar);
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	descriptor.icon = TabIcon(tab);
	descriptor.title = tr::lng_soon(tr::now);
	descriptor.text = tr::lng_main_section_soon(tr::now);
	_placeholder = new Ui::EmptyPlaceholder(this, std::move(descriptor));
}

SectionFactory RoutePlaceholderSection::Factory(const Route &route) {
	return [route](QWidget *parent, SessionController *controller) {
		return Ui::Owned<SectionWidget>(
			new RoutePlaceholderSection(parent, controller, route));
	};
}

QString RoutePlaceholderSection::sectionKey() const {
	return _key;
}

void RoutePlaceholderSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_placeholder->setGeometry(
		0,
		_bar->height(),
		width(),
		std::max(height() - _bar->height(), 0));
}

} // namespace Window
