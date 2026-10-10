// What a tab shows until its feature module registers a real side section:
// a top bar with the tab's name and a "coming soon" empty state.
#pragma once

#include "window/window_section_widget.h"
#include "window/window_registry.h"

namespace Ui {
class EmptyPlaceholder;
class TopBar;
} // namespace Ui

namespace Window {

class PlaceholderSection final : public SectionWidget {
public:
	PlaceholderSection(
		QWidget *parent,
		SessionController *controller,
		Tab tab);

	[[nodiscard]] static SectionFactory Factory(Tab tab);

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	Ui::TopBar *_bar = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;

};

#ifdef ELEZA_DEBUG_TOOLS
// A stand-in main column section ("debug/main" route): a top bar and a button that
// pushes one more. Lets the navigation be tried before the feature modules exist.
class PlaceholderMainSection final : public SectionWidget {
public:
	PlaceholderMainSection(
		QWidget *parent,
		SessionController *controller,
		int depth);

	[[nodiscard]] static SectionFactory Factory(int depth);

	QString sectionKey() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	int _depth = 0;
	Ui::TopBar *_bar = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;

};

#endif // ELEZA_DEBUG_TOOLS

// What a canonical route (window_routes.h) opens until the module that owns it
// registers a handler: the tab's title, the route and a "coming soon" state.
class RoutePlaceholderSection final : public SectionWidget {
public:
	RoutePlaceholderSection(
		QWidget *parent,
		SessionController *controller,
		const Route &route);

	[[nodiscard]] static SectionFactory Factory(const Route &route);

	QString sectionKey() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	QString _key;
	Ui::TopBar *_bar = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;

};

// Title of a tab for top bars: «الی», «پیام‌ها», «جستجو», «من», «تنظیمات».
[[nodiscard]] QString TabTitle(Tab tab);
// The hint shown in the empty main column of a tab.
[[nodiscard]] QString TabEmptyHint(Tab tab);

} // namespace Window
