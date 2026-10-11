#include "support/chat_text.h"

#include "base/platform_file.h"
#include "data/data_group_rules.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"

namespace Support {
namespace {

// The note of a group: `actor` did it, `target` is who it names (both
// already worded; empty = me).
[[nodiscard]] QString GroupNote(
		const Data::Message &message,
		const QString &actor,
		const QString &target,
		bool actorIsMe,
		bool targetIsMe) {
	using Note = Data::GroupRules::Note;
	switch (Data::GroupRules::NoteOf(message.system)) {
	case Note::Created:
		return actorIsMe
			? tr::lng_groups_note_created_you(tr::now)
			: tr::lng_groups_note_created(tr::now, lt_name, actor);
	case Note::Added:
		return actorIsMe
			? tr::lng_groups_note_added_you(tr::now, lt_user, target)
			: targetIsMe
			? tr::lng_groups_note_added_me(tr::now, lt_name, actor)
			: tr::lng_groups_note_added(tr::now, lt_name, actor, lt_user, target);
	case Note::Left:
		return actorIsMe
			? tr::lng_groups_note_left_you(tr::now)
			: tr::lng_groups_note_left(tr::now, lt_name, actor);
	case Note::Removed:
		return actorIsMe
			? tr::lng_groups_note_removed_you(tr::now, lt_user, target)
			: targetIsMe
			? tr::lng_groups_note_removed_me(tr::now, lt_name, actor)
			: tr::lng_groups_note_removed(tr::now, lt_name, actor, lt_user, target);
	case Note::TitleChanged:
		return actorIsMe
			? tr::lng_groups_note_title_you(tr::now)
			: tr::lng_groups_note_title(tr::now, lt_name, actor);
	case Note::PhotoChanged:
		return actorIsMe
			? tr::lng_groups_note_photo_you(tr::now)
			: tr::lng_groups_note_photo(tr::now, lt_name, actor);
	case Note::Joined:
		return actorIsMe
			? tr::lng_groups_note_joined_you(tr::now)
			: tr::lng_groups_note_joined(tr::now, lt_name, actor);
	case Note::Pinned:
		return actorIsMe
			? tr::lng_groups_note_pinned_you(tr::now)
			: tr::lng_groups_note_pinned(tr::now, lt_name, actor);
	case Note::None: break;
	}
	return QString();
}

} // namespace

QString MemberName(Main::Session *session, const QString &userId) {
	if (session && !userId.isEmpty()) {
		auto &users = session->data().users();
		const auto user = (userId == users.myId() && users.me().valid())
			? users.me().card()
			: users.get(userId);
		if (user.deleted) {
			return tr::lng_deleted_account(tr::now);
		} else if (!user.displayName.trimmed().isEmpty()) {
			return Data::Json::CleanName(user.displayName.simplified());
		} else if (!user.handle.isEmpty()) {
			return Lang::Handle(user.handle);
		}
	}
	return tr::lng_groups_someone(tr::now);
}

QString GroupTimeLeft(qint64 untilSeconds, qint64 nowMs) {
	const auto left = Data::GroupRules::RestrictionLeft(untilSeconds, nowMs);
	if (!left) {
		return QString();
	}
	using Unit = Data::GroupRules::TimeUnit;
	const auto count = int(std::min(left->amount, qint64(100000)));
	switch (left->unit) {
	case Unit::Minutes: return tr::lng_groups_time_minutes(tr::now, lt_count, count);
	case Unit::Hours: return tr::lng_groups_time_hours(tr::now, lt_count, count);
	case Unit::Days: return tr::lng_groups_time_days(tr::now, lt_count, count);
	}
	return QString();
}

QString MessagePreview(Main::Session *session, const Data::Message &message) {
	if (message.kind == Data::MessageKind::System
		&& !message.call
		&& message.serviceEvent() == Data::ServiceEvent::Group) {
		const auto me = session ? session->data().users().myId() : QString();
		const auto target = Data::GroupRules::NoteUser(message.system);
		const auto actorIsMe = message.mine
			|| (!me.isEmpty() && message.sender == me);
		const auto targetIsMe = !me.isEmpty() && (target == me);
		return GroupNote(
			message,
			MemberName(session, message.sender),
			MemberName(session, target),
			actorIsMe,
			targetIsMe);
	}
	return MessagePreview(message);
}

QString ChatTitle(Main::Session *session, const Data::Chat &chat) {
	if (chat.saved) {
		return tr::lng_saved_messages(tr::now);
	} else if (chat.peerDeleted) {
		return tr::lng_deleted_account(tr::now);
	} else if (!chat.title.isEmpty()) {
		return chat.title;
	} else if (chat.isGroup) {
		return (chat.group && !chat.group->title.isEmpty())
			? chat.group->title
			: tr::lng_groups_group(tr::now);
	}
	if (session && !chat.peerUserId.isEmpty()) {
		const auto user = session->data().users().get(chat.peerUserId);
		if (user.deleted) {
			return tr::lng_deleted_account(tr::now);
		} else if (!user.displayName.isEmpty()) {
			return user.displayName;
		} else if (!user.handle.isEmpty()) {
			return Lang::Handle(user.handle);
		}
	}
	return chat.anonymous ? tr::lng_anonymous(tr::now) : QString();
}

QString MessageKindLabel(const Data::Message &message) {
	switch (message.kind) {
	case Data::MessageKind::Photo: return tr::lng_media_photo(tr::now);
	case Data::MessageKind::Video: return tr::lng_media_video(tr::now);
	case Data::MessageKind::Gif: return tr::lng_media_gif(tr::now);
	case Data::MessageKind::Voice: return tr::lng_media_voice(tr::now);
	case Data::MessageKind::VideoNote:
		return tr::lng_media_video_message(tr::now);
	case Data::MessageKind::Audio: return tr::lng_media_music(tr::now);
	case Data::MessageKind::File: return tr::lng_media_file(tr::now);
	case Data::MessageKind::Sticker: return tr::lng_media_sticker(tr::now);
	case Data::MessageKind::Location: return tr::lng_media_location(tr::now);
	case Data::MessageKind::LiveLocation:
		return tr::lng_media_live_location(tr::now);
	case Data::MessageKind::ForwardedPost:
		return tr::lng_media_eli_post(tr::now);
	case Data::MessageKind::Call: return tr::lng_calls_voice_call(tr::now);
	case Data::MessageKind::Text:
	case Data::MessageKind::System:
	case Data::MessageKind::Unknown:
		break;
	}
	return QString();
}

QString MessagePreview(const Data::Message &message) {
	if (message.call) {
		// A call record, whatever its outcome (the chat shows the details).
		return message.call->video
			? tr::lng_calls_video_call(tr::now)
			: tr::lng_calls_voice_call(tr::now);
	} else if (message.underReview) {
		// Only a group keeps such a message in sight (Message::visible).
		return tr::lng_groups_under_review(tr::now);
	}
	if (message.kind == Data::MessageKind::System) {
		switch (message.serviceEvent()) {
		case Data::ServiceEvent::Text: return message.text;
		case Data::ServiceEvent::Joined:
			return tr::lng_chat_listener_joined(tr::now);
		case Data::ServiceEvent::Ended:
			return tr::lng_chat_conversation_has_ended(tr::now);
		case Data::ServiceEvent::Pinned:
			return tr::lng_chat_message_was_pinned(tr::now);
		case Data::ServiceEvent::ThemeChanged:
			return tr::lng_chat_chat_theme_changed(tr::now);
		case Data::ServiceEvent::Continued:
			return tr::lng_chat_conversation_continued(tr::now);
		case Data::ServiceEvent::Group:
			// Without a session nobody has a name.
			return GroupNote(
				message,
				tr::lng_groups_someone(tr::now),
				tr::lng_groups_someone(tr::now),
				message.mine,
				false);
		case Data::ServiceEvent::None: break;
		}
		return QString();
	}
	const auto label = MessageKindLabel(message);
	// One line: line breaks become spaces. It is shown next to our own words
	// (the kind, the time), so direction overrides and invisible marks of the
	// sender are dropped; a file shows the name it will be saved under
	// ("x<RLO>fdp.exe" is not "xexe.pdf").
	auto text = (message.kind == Data::MessageKind::File && !message.text.trimmed().isEmpty())
		? base::platform::SafeFileName(message.text)
		: Data::Json::CleanName(message.text.simplified());
	if (label.isEmpty()) {
		return text.isEmpty() ? tr::lng_support_notify_new_message(tr::now) : text;
	} else if (message.kind == Data::MessageKind::Sticker
		&& message.sticker
		&& !message.sticker->emoji.isEmpty()) {
		return message.sticker->emoji + QLatin1Char(' ') + label;
	} else if (text.isEmpty()) {
		return label;
	}
	// A caption follows the kind; a file shows its name.
	return label + QStringLiteral(": ") + text;
}

} // namespace Support
