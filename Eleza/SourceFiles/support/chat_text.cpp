#include "support/chat_text.h"

#include "base/platform_file.h"
#include "data/data_group_rules.h"
#include "data/data_json.h"
#include "data/data_note_types.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"

namespace Support {
namespace {

// Where the names go in a note while it is worded (private use: never in
// a translation, never typed by a person).
constexpr auto kActorMark = QChar(0xE000);
constexpr auto kTargetMark = QChar(0xE001);

// The note of a group with marks for the names: `actorIsMe` / `targetIsMe`
// choose the wording that speaks to me.
[[nodiscard]] QString GroupNotePattern(
		const Data::Message &message,
		bool actorIsMe,
		bool targetIsMe) {
	using Note = Data::GroupRules::Note;
	const auto actor = QString(kActorMark);
	const auto target = QString(kTargetMark);
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

// Puts the names where the marks are, each isolated (a Latin name in a
// Persian sentence keeps its neighbours in place), and records where they
// landed.
[[nodiscard]] NoteLine FillNames(
		const QString &pattern,
		const NotePerson &actor,
		const NotePerson &target) {
	auto result = NoteLine();
	result.text.reserve(pattern.size() + actor.name.size() + target.name.size() + 4);
	for (const auto ch : pattern) {
		if (ch != kActorMark && ch != kTargetMark) {
			result.text.push_back(ch);
			continue;
		}
		const auto &person = (ch == kActorMark) ? actor : target;
		const auto isolated = Lang::Isolated(person.name);
		const auto start = isolated.indexOf(person.name);
		const auto offset = int(result.text.size()) + std::max(int(start), 0);
		result.text += isolated;
		if (person.linkable && !person.userId.isEmpty() && !person.name.isEmpty()) {
			result.names.push_back({ offset, int(person.name.size()), person.userId });
		}
	}
	return result;
}

} // namespace

NoteLine GroupNoteLine(
		const Data::Message &message,
		const NotePerson &actor,
		const NotePerson &target) {
	const auto pattern = GroupNotePattern(message, actor.me, target.me);
	return pattern.isEmpty() ? NoteLine() : FillNames(pattern, actor, target);
}

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

NoteLine ServiceNoteLine(Main::Session *session, const Data::Message &message) {
	if (message.kind == Data::MessageKind::System
		&& !message.call
		&& message.serviceEvent() == Data::ServiceEvent::Group) {
		const auto me = session ? session->data().users().myId() : QString();
		const auto person = [&](const QString &userId, bool isMe) {
			auto result = NotePerson();
			result.userId = userId;
			result.me = isMe;
			result.name = MemberName(session, userId);
			const auto deleted = session
				&& !userId.isEmpty()
				&& session->data().users().peek(userId).deleted;
			result.linkable = !isMe && !userId.isEmpty() && !deleted;
			return result;
		};
		const auto target = Data::GroupRules::NoteUser(message.system);
		return GroupNoteLine(
			message,
			person(message.sender, message.mine || (!me.isEmpty() && message.sender == me)),
			person(target, !me.isEmpty() && (target == me)));
	} else if (const auto taker = Data::SelfDestructScreenshotTaker(message.system);
		message.kind == Data::MessageKind::System && !taker.isEmpty() && session) {
		// «‹name› took a screenshot of a self-destructing message»: our own
		// line, the name opening their profile. A card without a name
		// leaves the server's line.
		auto &users = session->data().users();
		const auto me = users.myId();
		if (!me.isEmpty() && taker == me) {
			return NoteLine{ tr::lng_history_sd_screenshot_you(tr::now), {} };
		}
		const auto card = users.get(taker);
		const auto name = card.deleted
			? tr::lng_deleted_account(tr::now)
			: !card.displayName.trimmed().isEmpty()
			? Data::Json::CleanName(card.displayName.simplified())
			: !card.handle.isEmpty()
			? Lang::Handle(card.handle)
			: QString();
		if (!name.isEmpty()) {
			auto person = NotePerson();
			person.name = name;
			person.userId = taker;
			person.linkable = !card.deleted;
			return FillNames(
				tr::lng_history_sd_screenshot(tr::now, lt_name, QString(kActorMark)),
				person,
				NotePerson());
		}
	}
	return NoteLine{ MessagePreview(message), {} };
}

QString MessagePreview(Main::Session *session, const Data::Message &message) {
	return ServiceNoteLine(session, message).text;
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

QString SelfDestructLabel(const Data::Message &message) {
	if (!message.selfDestruct) {
		return QString();
	}
	const auto expired = message.selfDestruct->expired();
	switch (message.kind) {
	case Data::MessageKind::Photo:
		return expired
			? tr::lng_history_sd_photo_expired(tr::now)
			: tr::lng_history_sd_photo(tr::now);
	case Data::MessageKind::Video:
		return expired
			? tr::lng_history_sd_video_expired(tr::now)
			: tr::lng_history_sd_video(tr::now);
	default:
		// This app is told only that it is one (not which).
		return expired
			? tr::lng_history_sd_expired(tr::now)
			: tr::lng_history_sd_media(tr::now);
	}
}

QString SelfDestructTimerText(int seconds) {
	return (seconds >= 60 && seconds % 60 == 0)
		? tr::lng_history_sd_minutes(tr::now, lt_count, seconds / 60)
		: tr::lng_history_sd_seconds(tr::now, lt_count, seconds);
}

QString MessageKindLabel(const Data::Message &message) {
	if (message.selfDestruct) {
		return SelfDestructLabel(message);
	}
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
	case Data::MessageKind::StoryReply: return tr::lng_stories_chat_reply(tr::now);
	case Data::MessageKind::StoryReaction:
		return tr::lng_stories_chat_reaction(tr::now);
	case Data::MessageKind::StoryShare:
		return tr::lng_stories_chat_share_preview(tr::now);
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
		case Data::ServiceEvent::Group: {
			// Without a session nobody has a name.
			auto someone = NotePerson();
			someone.name = tr::lng_groups_someone(tr::now);
			auto actor = someone;
			actor.me = message.mine;
			return GroupNoteLine(message, actor, someone).text;
		}
		case Data::ServiceEvent::None: break;
		}
		return QString();
	}
	if (message.selfDestruct) {
		// Never the caption (it is not sent here) nor the server's text.
		return SelfDestructLabel(message);
	}
	const auto label = MessageKindLabel(message);
	// One line: line breaks become spaces. It is shown next to our own words
	// (the kind, the time), so direction overrides and invisible marks of the
	// sender are dropped; a file shows the name it will be saved under
	// ("x<RLO>fdp.exe" is not "xexe.pdf").
	auto text = (message.kind == Data::MessageKind::File && !message.text.trimmed().isEmpty())
		? base::platform::SafeFileName(message.text)
		: Data::Json::CleanName((label.isEmpty()
			// A reply to a note shows «↩︎ <answer>», not the quoted note.
			? Data::NoteReply::Preview(message.text)
			: message.text).simplified());
	if (label.isEmpty()) {
		return text.isEmpty() ? tr::lng_support_notify_new_message(tr::now) : text;
	} else if (message.kind == Data::MessageKind::Sticker
		&& message.sticker
		&& !message.sticker->emoji.isEmpty()) {
		return message.sticker->emoji + QLatin1Char(' ') + label;
	} else if (message.kind == Data::MessageKind::StoryReaction) {
		// «🔥 واکنش به استوری»
		return (message.story && !message.story->emoji.isEmpty())
			? (Data::Json::CleanName(message.story->emoji) + QLatin1Char(' ') + label)
			: label;
	} else if (text.isEmpty()) {
		return label;
	}
	// A caption follows the kind; a file shows its name.
	return label + QStringLiteral(": ") + text;
}

} // namespace Support
