#include "dialogs/dialogs_logic.h"

#include "lang_auto.h"
#include "support/chat_text.h"

#include <QtCore/QHash>

#include <algorithm>

namespace Dialogs {
namespace {

// Server drafts carry seconds, local ones milliseconds.
[[nodiscard]] qint64 Milliseconds(qint64 value) {
	return (value > 0 && value < 100'000'000'000LL) ? (value * 1000) : value;
}

} // namespace

bool InFolder(const Data::Chat &chat, Folder folder) {
	const auto request = chat.isRequest()
		|| (chat.request == u"incoming");
	switch (folder) {
	case Folder::Main: return !chat.archived && !request;
	case Folder::Requests: return request;
	case Folder::Archived: return chat.archived && !request;
	}
	return false;
}

qint64 SortTime(const Data::Chat &chat) {
	const auto draft = (chat.draft && !chat.draft->empty())
		? Milliseconds(chat.draft->updatedAt)
		: 0;
	return std::max(chat.lastActivityAt, draft);
}

void SortChats(QVector<Data::Chat> &chats, const QStringList &pinnedOrder) {
	auto positions = QHash<QString, int>();
	for (auto i = 0; i != pinnedOrder.size(); ++i) {
		positions.insert(pinnedOrder[i], i);
	}
	const auto unknown = int(pinnedOrder.size());
	std::stable_sort(chats.begin(), chats.end(), [&](
			const Data::Chat &a,
			const Data::Chat &b) {
		if (a.pinned != b.pinned) {
			return a.pinned;
		} else if (a.pinned) {
			const auto ai = positions.value(a.id, unknown);
			const auto bi = positions.value(b.id, unknown);
			if (ai != bi) {
				return ai < bi;
			}
		}
		const auto at = SortTime(a);
		const auto bt = SortTime(b);
		if (at != bt) {
			return at > bt;
		}
		return a.id < b.id;
	});
}

QString NormalizeQuery(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	for (const auto ch : text.trimmed().toLower()) {
		switch (ch.unicode()) {
		case 0x064A: // Arabic yeh
		case 0x0649: // alef maksura
			result.append(QChar(0x06CC));
			break;
		case 0x0643: // Arabic kaf
			result.append(QChar(0x06A9));
			break;
		case 0x200C: // zero-width non-joiner
		case 0x200D:
		case 0x0640: // tatweel
			break;
		default:
			if (ch.unicode() >= 0x06F0 && ch.unicode() <= 0x06F9) {
				// Persian digits.
				result.append(QChar(u'0' + (ch.unicode() - 0x06F0)));
			} else if (ch.unicode() >= 0x0660 && ch.unicode() <= 0x0669) {
				// Arabic-Indic digits.
				result.append(QChar(u'0' + (ch.unicode() - 0x0660)));
			} else {
				result.append(ch);
			}
		}
	}
	while (result.startsWith(QLatin1Char('@'))) {
		result.remove(0, 1);
	}
	return result.simplified();
}

bool MatchesQuery(
		const Data::Chat &chat,
		const QString &title,
		const QString &handle,
		const QString &query) {
	if (query.isEmpty()) {
		return true;
	} else if (NormalizeQuery(title).contains(query)) {
		return true;
	} else if (!handle.isEmpty() && handle.toLower().contains(query)) {
		return true;
	} else if (chat.draft
		&& !chat.draft->empty()
		&& NormalizeQuery(chat.draft->text).contains(query)) {
		return true;
	} else if (chat.lastMessage
		&& chat.lastMessage->visible()
		&& NormalizeQuery(chat.lastMessage->text).contains(query)) {
		return true;
	}
	return false;
}

QStringList MovePinned(QStringList order, int from, int to) {
	if (from < 0 || from >= order.size() || order.isEmpty()) {
		return order;
	}
	to = std::clamp(to, 0, int(order.size()) - 1);
	if (from != to) {
		order.move(from, to);
	}
	return order;
}

Tick TickFor(const Data::Chat &chat) {
	if (!chat.lastMessage
		|| !chat.lastMine
		|| !chat.lastMessage->visible()
		|| (chat.draft && !chat.draft->empty())
		|| chat.lastMessage->kind == Data::MessageKind::System
		|| chat.saved) {
		return Tick::None;
	}
	const auto &message = *chat.lastMessage;
	switch (message.status) {
	case Data::MessageStatus::Queued:
	case Data::MessageStatus::Sending: return Tick::Pending;
	case Data::MessageStatus::Failed: return Tick::Failed;
	case Data::MessageStatus::Read: return Tick::Read;
	case Data::MessageStatus::Sent: break;
	}
	if (message.local()) {
		return Tick::Pending;
	}
	return (chat.peerReadSeq >= message.seq) ? Tick::Read : Tick::Sent;
}

QString ActivityText(const QString &kind) {
	if (kind.isEmpty() || kind == u"cancel") {
		return QString();
	} else if (kind == u"recording_voice") {
		return tr::lng_action_recording_voice(tr::now);
	} else if (kind == u"uploading_photo") {
		return tr::lng_action_sending_photo(tr::now);
	} else if (kind == u"uploading_video") {
		return tr::lng_action_sending_video(tr::now);
	} else if (kind == u"uploading_file") {
		return tr::lng_action_sending_file(tr::now);
	}
	return tr::lng_action_typing(tr::now);
}

QString ActivityText(const QString &kind, const QString &name) {
	const auto text = ActivityText(kind);
	return (text.isEmpty() || name.isEmpty())
		? text
		: tr::lng_action_member(tr::now, lt_name, name, lt_action, text);
}

Preview PreviewFor(
		const Data::Chat &chat,
		const QString &activityKind,
		const QString &senderName,
		const QString &noteText,
		const QString &activityName) {
	auto result = Preview();
	const auto activity = ActivityText(activityKind, activityName);
	if (!activity.isEmpty()) {
		result.kind = Preview::Kind::Activity;
		result.text = activity;
		return result;
	} else if (chat.draft && !chat.draft->empty()) {
		result.kind = Preview::Kind::Draft;
		result.prefix = tr::lng_dialogs_draft(tr::now);
		result.text = chat.draft->text.simplified();
		return result;
	} else if (!chat.lastMessage || !chat.lastMessage->visible()) {
		return result;
	}
	const auto &message = *chat.lastMessage;
	if (message.kind == Data::MessageKind::System) {
		result.kind = Preview::Kind::Service;
		result.text = noteText.isEmpty()
			? Support::MessagePreview(message)
			: noteText;
		if (result.text.isEmpty()) {
			result.kind = Preview::Kind::Empty;
		}
		return result;
	}
	const auto label = Support::MessageKindLabel(message);
	const auto caption = message.text.simplified();
	result.thumbnail = !message.mediaSpoiler
		&& !message.attachment.isEmpty()
		&& (message.kind == Data::MessageKind::Photo
			|| message.kind == Data::MessageKind::Video
			|| message.kind == Data::MessageKind::Gif);
	if (chat.lastMine && !chat.saved) {
		// «تو: {text}» with an empty text gives the bare prefix.
		result.prefix = tr::lng_dialogs_you(
			tr::now,
			lt_text,
			QString()).trimmed();
	} else if (chat.isGroup && !senderName.isEmpty()) {
		result.prefix = tr::lng_groups_preview_from(
			tr::now,
			lt_name,
			senderName);
	}
	if (message.selfDestruct) {
		// Its label only: no caption, no thumbnail.
		result.kind = Preview::Kind::Media;
		result.text = Support::MessagePreview(message);
		result.thumbnail = false;
		return result;
	}
	if (message.underReview) {
		// A moderator holds it back: the row says so, nothing of it shows.
		result.kind = Preview::Kind::Service;
		result.text = Support::MessagePreview(message);
		result.thumbnail = false;
		return result;
	}
	if (!label.isEmpty() && caption.isEmpty()) {
		result.kind = Preview::Kind::Media;
		result.text = Support::MessagePreview(message);
	} else if (!label.isEmpty() && result.thumbnail) {
		// The thumbnail says "photo"; the words are the caption.
		result.kind = Preview::Kind::Message;
		result.text = caption;
	} else {
		result.kind = Preview::Kind::Message;
		result.text = Support::MessagePreview(message);
	}
	return result;
}

bool CanDeleteForBoth(const Data::Chat &chat) {
	return !chat.isConversation
		&& !chat.saved
		&& !chat.isBot
		&& !chat.peerDeleted
		&& !chat.peerUserId.isEmpty();
}

bool CanPin(const Data::Chat &chat) {
	return !chat.isConversation && !InFolder(chat, Folder::Requests);
}

QString MuteDurationText(const QString &duration) {
	if (duration == u"1h") {
		return tr::lng_dialogs_mute_1h(tr::now);
	} else if (duration == u"8h") {
		return tr::lng_dialogs_mute_8h(tr::now);
	} else if (duration == u"2d") {
		return tr::lng_dialogs_mute_2d(tr::now);
	} else if (duration == u"forever") {
		return tr::lng_dialogs_mute_forever(tr::now);
	}
	return duration;
}

} // namespace Dialogs
