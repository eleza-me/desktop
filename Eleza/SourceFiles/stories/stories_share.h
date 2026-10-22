// «ارسال به…» (Instagram's paper plane, backend docs/stories.md §7): a
// story goes to people and groups as `story_share` messages. The sheet lists
// Saved Messages, my recent chats (people and groups I may write to) and the
// people I follow, searches people on the server, ticks at most
// `stories.share_max_recipients`, takes an optional message and sends; then
// «ارسال شد», or who was left out and why.
#pragma once

#include <QtCore/QString>

namespace Window {
class SessionController;
} // namespace Window

namespace Stories {

void ShowShareBox(
	Window::SessionController *controller,
	const QString &storyId);

} // namespace Stories
