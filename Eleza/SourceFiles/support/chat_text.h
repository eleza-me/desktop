// The words for a chat and for a message where only one line fits: the chat
// list, notifications, reply headers, the forward box.
//
//   Support::ChatTitle(&session, chat)          «سارا احمدی», «پیام‌های ذخیره‌شده», a topic…
//   Support::MessagePreview(message)            the text, or «عکس» / «پیام صوتی» / «فایل: name.pdf»…
#pragma once

#include "data/data_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Support {

[[nodiscard]] QString ChatTitle(Main::Session *session, const Data::Chat &chat);

// The label of a media kind («عکس», «ویدیو», …); empty for plain text.
[[nodiscard]] QString MessageKindLabel(const Data::Message &message);
// One line for a message; never empty for a visible message.
[[nodiscard]] QString MessagePreview(const Data::Message &message);

} // namespace Support
