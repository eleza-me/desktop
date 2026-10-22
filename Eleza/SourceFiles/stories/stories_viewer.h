// The full-window story viewer (over the window's body, like the image
// viewer): segmented progress on top, a photo for its duration or a video to
// its end, a shared post as a card, the caption, the reply field with the
// quick reactions (or «پاسخ برای این استوری بسته است»), and for my own
// stories «N بیننده». It goes on to the next person of the queue after the
// last story and closes after the last person.
//
// Mouse: a click on the trailing half goes on, the leading half goes back
// (RTL-aware); holding the button pauses. Keys: ← → (by the direction),
// Space pauses, Esc closes. Seen marks are batched by Data::Stories and sent
// when the viewer closes.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Window {
class SessionController;
} // namespace Window

namespace Stories {

// Opens `userId`'s stories at the first unseen one. `queue` is who comes
// next (the tray's order); it may contain `userId` (the viewer starts there).
void OpenUserStories(
	Window::SessionController *controller,
	const QString &userId,
	const QStringList &queue = QStringList());

// Opens one story of a known owner; «این استوری دیگر در دسترس نیست» when it
// is gone.
void OpenStory(
	Window::SessionController *controller,
	const QString &ownerId,
	const QString &storyId);

// A story a chat message points at (a reply, a reaction, a story sent on):
// asks the server first (GET /v1/stories/{id}, decided for me, whoever sent
// it), then the viewer at that story with the owner's others around it; a
// 404 is a toast and the message's card turns «gone».
void OpenStoryFromChat(
	Window::SessionController *controller,
	const QString &storyId);

} // namespace Stories
