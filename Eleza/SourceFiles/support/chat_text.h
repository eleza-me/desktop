// The words for a chat and for a message where only one line fits: the chat
// list, notifications, reply headers, the forward box.
//
//   Support::ChatTitle(&session, chat)          «سارا احمدی», «پیام‌های ذخیره‌شده», a topic…
//   Support::MessagePreview(message)            the text, or «عکس» / «پیام صوتی» / «فایل: name.pdf»…
//   Support::MessagePreview(&session, message)  the same, with the names of people in the notes
//                                               of a group («سارا، علی را اضافه کرد»)
//   Support::MemberName(&session, userId)       how a member of a group is called in one word or two
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

// With a session the notes of a group name the people in them (their cards
// are asked for when unknown; Data::Users::changed tells when to word the
// row again). Everything else is MessagePreview(message).
[[nodiscard]] QString MessagePreview(
	Main::Session *session,
	const Data::Message &message);

// The display name of a person, their @handle without one, «یک عضو» while
// the card is unknown (it is requested).
[[nodiscard]] QString MemberName(Main::Session *session, const QString &userId);

// «۳ ساعت», «۲ روز»: how long a restriction still lasts (Data::GroupRules::RestrictionLeft).
[[nodiscard]] QString GroupTimeLeft(qint64 untilSeconds, qint64 nowMs);

} // namespace Support
