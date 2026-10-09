// The "eli.posts" embed (window/window_embeds.h): post cards inside somebody
// else's scroll area — the tabs of a profile, the posts of a search, the
// bookmarks.
//
//   params: source = "user" | "user_replies" | "user_media" | "bookmarks" | "search"
//           user   = <user id>        (the user* sources)
//           query  = <text>           (search)
//           pinned = <post id>        (optional: the card marked «سنجاق‌شده»)
//
// It has no scroll of its own: the host tells the visible range, asks for the
// next page when its scroll comes near the end, and follows heightChanged().
#pragma once

#include "window/window_embeds.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Eli {

class PostList;

class PostsEmbed final : public Window::EmbeddedList {
public:
	PostsEmbed(
		QWidget *parent,
		Window::SessionController *controller,
		const QVariantMap &params);

	void setVisibleRange(int top, int bottom) override;
	void loadMore() override;
	void reload() override;

protected:
	int resizeGetHeight(int newWidth) override;
	void keyPressEvent(QKeyEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;

private:
	PostList *_list = nullptr;
	bool _inResize = false;

};

} // namespace Eli
