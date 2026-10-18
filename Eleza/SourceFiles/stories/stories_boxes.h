// The boxes of stories: posting a story from a photo / video (caption,
// audience), sharing an Eli post to my story, the viewers of my story and a
// people picker for the hide-from and close-friends lists.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QStringList>

namespace Window {
class SessionController;
} // namespace Window

namespace Stories {

// «+»: picks a photo or a video, then shows the box (caption, audience).
void ShowComposeBox(Window::SessionController *controller);
// The same box for a file that is already chosen.
void ShowComposeBoxForFile(
	Window::SessionController *controller,
	const QString &path);

// «اشتراک در استوری» from a post's menu.
void ShareToStory(Window::SessionController *controller, const QString &postId);

// «N بیننده»: who saw my story, newest first, with their reactions.
void ShowViewersBox(Window::SessionController *controller, const QString &storyId);

// People to add to a list: the recent chats and the people I follow, search
// by name; `preselected` start ticked. `done` gets the ticked ids.
void PickPeople(
	Window::SessionController *controller,
	const QString &title,
	const QStringList &preselected,
	Fn<void(const QStringList &ids)> done);

} // namespace Stories
