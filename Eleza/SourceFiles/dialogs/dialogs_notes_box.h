// The boxes of notes (dialogs_notes.h has the row):
//
//  * «نوت جدید» — my face under a speech bubble that shows what is typed, one
//    line of at most notes.max_chars characters (emoji allowed, counted as
//    the server counts them; the counter shows for the last ten), who sees it
//    (mutual followers or close friends), «اشتراک» (off while the text is
//    empty or unchanged) and, while I have one, «حذف نوت».
//  * a friend's note — the note over their face, «name · time» and one field
//    «پیام به …»: the answer goes to our 1:1 chat as the plain text message
//    that quotes the note (Data::NoteReply), «فرستاده شد» and the box closes.
#pragma once

#include <QtCore/QString>

namespace Window {
class SessionController;
} // namespace Window

namespace Dialogs {

void ShowNoteComposer(Window::SessionController *controller);
void ShowFriendNote(Window::SessionController *controller, const QString &userId);
void DeleteMyNote(Window::SessionController *controller);
// Opens the 1:1 chat with the author of a note.
void OpenNoteChat(Window::SessionController *controller, const QString &userId);

} // namespace Dialogs
