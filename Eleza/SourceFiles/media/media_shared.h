// The shared media of a chat or a group, as in Telegram: tabs Media (photos
// and videos), Files, Links, Music, Voice (voice messages and round videos),
// GIFs — the ones the server counts something in (docs/shared-media.md of the
// backend), each with its size in the top bar — paged from the server newest
// first. Photos and videos in a grid with month headings (Ctrl + wheel
// changes the columns), the rest as rows; songs and voices play on the app's
// player, pictures open in the viewer. Select mode («انتخاب», Ctrl + click)
// forwards or deletes several items at once.
//
//   controller->openRoute(Window::Routes::ChatMedia(chatId));    // a direct chat
//   controller->openRoute(Window::Routes::GroupMedia(groupId));  // a group
//
// What the chat has cached shows at once while the first page is on its way;
// a server without the media routes (404) leaves the page with what is cached.
// Nothing is polled: the counts are read when the page opens, the tabs live on
// the chat's history signals afterwards.
#pragma once

#include "window/window_registry.h"

namespace Media {

[[nodiscard]] Window::SectionFactory SharedMediaFactory(const QString &chatId);

} // namespace Media
