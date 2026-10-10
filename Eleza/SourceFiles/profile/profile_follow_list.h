// Followers / following of a profile and my incoming follow requests, as
// sections of the main column.
#pragma once

#include "data/data_social_types.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"

#include <optional>

namespace Ui {
class EmptyPlaceholder;
class ScrollArea;
class SearchField;
class SkeletonList;
class TabBar;
class TopBar;
} // namespace Ui

namespace Profile {

class CenteredWrap;
class PeopleList;

enum class FollowKind {
	Followers,
	Following,
};

// `profile` needs an id or a handle; the rest is loaded when missing.
[[nodiscard]] Window::SectionFactory FollowListFactory(
	const Data::Profile &profile,
	FollowKind kind);
[[nodiscard]] Window::SectionFactory FollowRequestsFactory();

class FollowListSection final : public Window::SectionWidget {
public:
	FollowListSection(
		QWidget *parent,
		Window::SessionController *controller,
		const Data::Profile &profile,
		FollowKind kind);

	QString sectionKey() const override;
	void setInnerFocus() override;
	bool onBackRequest() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	[[nodiscard]] bool mine() const;
	void refreshTitles();
	void reload();
	void loadProfile(int generation);
	void loadPage();
	void pageLoaded(const Data::GraphList &page);
	void failed(const Api::Error &error);
	void checkLoadMore();
	void refreshState();
	void updateGeometries();
	void confirmRemove(const QString &userId, const QString &name);

	Data::Profile _profile;
	FollowKind _kind = FollowKind::Followers;
	Ui::TopBar *_bar = nullptr;
	Ui::TabBar *_tabs = nullptr;
	Ui::SearchField *_search = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	CenteredWrap *_wrap = nullptr;
	PeopleList *_list = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;
	QString _query;
	QString _cursor;
	int _pages = 0;
	int _generation = 0;
	bool _loading = false;
	bool _loadedOnce = false;
	std::optional<Api::Error> _error;

};

class FollowRequestsSection final : public Window::SectionWidget {
public:
	FollowRequestsSection(
		QWidget *parent,
		Window::SessionController *controller);

	QString sectionKey() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void reload();
	void refreshState();
	void updateGeometries();

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	CenteredWrap *_wrap = nullptr;
	PeopleList *_list = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;
	bool _loading = false;
	std::optional<Api::Error> _error;

};

} // namespace Profile
