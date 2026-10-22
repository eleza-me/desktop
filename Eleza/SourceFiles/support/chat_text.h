// The words for a chat and for a message where only one line fits: the chat
// list, notifications, reply headers, the forward box.
//
//   Support::ChatTitle(&session, chat)          «سارا احمدی», «پیام‌های ذخیره‌شده», a topic…
//   Support::MessagePreview(message)            the text, or «عکس» / «پیام صوتی» / «فایل: name.pdf»…
//   Support::MessagePreview(&session, message)  the same, with the names of people in the notes
//                                               of a group («سارا، علی را اضافه کرد»)
//   Support::MemberName(&session, userId)       how a member of a group is called in one word or two
//   Support::ServiceNoteLine(&session, message) a note's line with where the names of the people
//                                               in it are («سارا، علی را اضافه کرد»: both open a profile)
#pragma once

#include "data/data_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Support {

[[nodiscard]] QString ChatTitle(Main::Session *session, const Data::Chat &chat);

// «عکس یکبارمصرف», «… منقضی شد»: self-destructing media (empty for any
// other message).
[[nodiscard]] QString SelfDestructLabel(const Data::Message &message);
// «۱۰ ثانیه», «۱ دقیقه»: a timer of self-destructing media.
[[nodiscard]] QString SelfDestructTimerText(int seconds);
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

// A name in a service note: UTF-16 offset and length in NoteLine::text and
// whose name it is.
struct NoteName {
	int offset = 0;
	int length = 0;
	QString userId;

	friend bool operator==(const NoteName &, const NoteName &) = default;
};

// A service note as one line, and the names in it that open a profile.
struct NoteLine {
	QString text;
	QVector<NoteName> names;
};

// Somebody a note names. `linkable`: a tap on the name opens their profile
// (never me — a note about me speaks to me — and never a deleted account).
struct NotePerson {
	QString name;
	QString userId;
	bool me = false;
	bool linkable = false;
};

// The note of a group (Data::GroupRules::NoteOf) with `actor` (its sender)
// and `target` (who it names). Each name is isolated in the sentence and
// its place recorded, exact in every language and word order and for two
// people of the same name. Empty for a message that is not such a note.
[[nodiscard]] NoteLine GroupNoteLine(
	const Data::Message &message,
	const NotePerson &actor,
	const NotePerson &target);

// The line of a service message with the people it names (the notes of a
// group); a message without names is MessagePreview(session, message).
[[nodiscard]] NoteLine ServiceNoteLine(
	Main::Session *session,
	const Data::Message &message);

// The display name of a person, their @handle without one, «یک عضو» while
// the card is unknown (it is requested).
[[nodiscard]] QString MemberName(Main::Session *session, const QString &userId);

// «۳ ساعت», «۲ روز»: how long a restriction still lasts (Data::GroupRules::RestrictionLeft).
[[nodiscard]] QString GroupTimeLeft(qint64 untilSeconds, qint64 nowMs);

} // namespace Support
