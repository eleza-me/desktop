// The profile screen: somebody's profile by id or handle, and my own (the «من»
// tab). A header (Profile::Cover), the open-conversation card, a sticky tab bar
// and the post lists hosted through the "eli.posts" embed.
#pragma once

#include "data/data_social_types.h"
#include "profile/profile_logic.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"

#include <optional>

namespace Ui {
class EmptyPlaceholder;
class IconButton;
class PopupMenu;
class ScrollArea;
class SkeletonList;
class TopBar;
} // namespace Ui

namespace Profile {

struct Source {
	enum class Type {
		Me,
		UserId,
		Handle,
	};
	Type type = Type::Me;
	QString value;

	[[nodiscard]] static Source Me() {
		return {};
	}
	[[nodiscard]] static Source UserId(const QString &id) {
		return { Type::UserId, id };
	}
	[[nodiscard]] static Source Handle(const QString &handle) {
		return { Type::Handle, handle };
	}
};

enum class PostsTab {
	Posts,
	Replies,
	Media,
	Bookmarks,
};

[[nodiscard]] Window::SectionFactory Factory(
	const Source &source,
	PostsTab tab = PostsTab::Posts);

class Inner;

class Widget final : public Window::SectionWidget {
public:
	Widget(
		QWidget *parent,
		Window::SessionController *controller,
		const Source &source,
		PostsTab tab);
	~Widget();

	QString sectionKey() const override;
	void showFinished() override;
	// Switches the post list (the side column's «ذخیره‌ها» link).
	void showTab(PostsTab tab);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	[[nodiscard]] bool isMe() const;
	[[nodiscard]] ViewerState viewerState() const;
	void load();
	void loaded(const Data::Profile &profile);
	void failed(const Api::Error &error);
	void loadExtras();
	void apply();
	void refreshBar();
	void refreshPresence();
	void refreshRequests();
	void updateGeometries();

	void toggleFollow();
	void openChat();
	void showMenu();
	void openPhoto();
	void copyHandle();
	void copyLink();
	void togglePostNotifications();
	void toggleMute();
	void toggleBlock();
	void removeFollower();
	void report();
	void applyRelationship(const Data::GraphState &state);

	Source _source;
	PostsTab _initialTab = PostsTab::Posts;
	Data::Profile _profile;
	bool _loaded = false;
	bool _loading = false;
	bool _extrasLoaded = false;
	bool _unavailable = false;
	std::optional<Api::Error> _error;

	FollowUi _follow = FollowUi::None;
	bool _followBusy = false;
	bool _blocked = false;
	bool _justUnblocked = false;
	bool _muted = false;
	bool _postNotifications = false;
	bool _openingChat = false;
	bool _watchingPresence = false;

	Ui::TopBar *_bar = nullptr;
	Ui::IconButton *_bell = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Inner *_inner = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	Ui::EmptyPlaceholder *_placeholder = nullptr;
	QPointer<Ui::PopupMenu> _menu;

};

} // namespace Profile
