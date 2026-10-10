#include "support/chat_text.h"

#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"

namespace Support {

QString ChatTitle(Main::Session *session, const Data::Chat &chat) {
	if (chat.saved) {
		return tr::lng_saved_messages(tr::now);
	} else if (chat.peerDeleted) {
		return tr::lng_deleted_account(tr::now);
	} else if (!chat.title.isEmpty()) {
		return chat.title;
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
	case Data::MessageKind::Call: return tr::lng_media_call(tr::now);
	case Data::MessageKind::Text:
	case Data::MessageKind::System:
	case Data::MessageKind::Unknown:
		break;
	}
	return QString();
}

QString MessagePreview(const Data::Message &message) {
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
		case Data::ServiceEvent::None: break;
		}
		return QString();
	}
	const auto label = MessageKindLabel(message);
	// One line: line breaks become spaces.
	auto text = message.text.simplified();
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
