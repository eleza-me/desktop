// The widget-free rules of the chat list: which chats a folder shows, their order
// (pinned first in the user's own order, then by the newest of the last activity
// and the draft), local search, what the second line of a row says and which
// delivery mark the row gets. Covered by tests/app/test_dialogs_logic.cpp.
#pragma once

#include "data/data_types.h"

#include <QtCore/QStringList>

namespace Dialogs {

enum class Folder {
	Main,     // direct chats and conversations that are neither archived nor requests
	Requests, // message requests
	Archived,
};

[[nodiscard]] bool InFolder(const Data::Chat &chat, Folder folder);

// max(last activity, draft time), unix milliseconds.
[[nodiscard]] qint64 SortTime(const Data::Chat &chat);

// Pinned chats first: the ones named in `pinnedOrder` in that order, then the
// other pinned ones by time; then everything else, newest first.
void SortChats(QVector<Data::Chat> &chats, const QStringList &pinnedOrder);

// Lower case, Arabic yeh / kaf folded into the Persian letters, Persian and
// Arabic digits into Latin ones, zero-width joiners and a leading "@" removed:
// what the search compares.
[[nodiscard]] QString NormalizeQuery(const QString &text);
// `query` must be normalized. Matches the title, the peer's handle and the
// text of the last message (or of the draft).
[[nodiscard]] bool MatchesQuery(
	const Data::Chat &chat,
	const QString &title,
	const QString &handle,
	const QString &query);

// The row moved from `from` to `to` inside the pinned block.
[[nodiscard]] QStringList MovePinned(QStringList order, int from, int to);

enum class Tick {
	None,
	Pending,
	Failed,
	Sent,
	Read,
};
// The delivery mark of my own last message; None when the last message is not
// mine or a draft hides it.
[[nodiscard]] Tick TickFor(const Data::Chat &chat);

// «در حال نوشتن…» and friends; empty for "cancel" and unknown kinds fall back
// to typing.
[[nodiscard]] QString ActivityText(const QString &kind);

struct Preview {
	enum class Kind {
		Empty,
		Message,  // prefix = «تو:» for my own message (or empty), text = the words
		Media,    // text = the kind label alone («عکس», «پیام صوتی»): painted as an accent
		Service,  // a system message
		Draft,    // prefix = «پیش‌نویس:», text = the draft
		Activity, // text = «در حال نوشتن…»
	};
	Kind kind = Kind::Empty;
	QString prefix;
	QString text;
	// The last message has a picture worth a mini thumbnail.
	bool thumbnail = false;
};
// A group row names who wrote the last message («سارا: سلام») and words its
// notes with people's names: the list passes both (`senderName` is the first
// name of the sender, `noteText` the worded note of a system message).
// `activityName`: in a group, the first name of the member whose activity
// it is («سارا در حال نوشتن…»).
[[nodiscard]] Preview PreviewFor(
	const Data::Chat &chat,
	const QString &activityKind = QString(),
	const QString &senderName = QString(),
	const QString &noteText = QString(),
	const QString &activityName = QString());
// «typing…», or «Sara is typing…» with a name.
[[nodiscard]] QString ActivityText(const QString &kind, const QString &name);

// Whether the «برای … هم حذف شود» checkbox makes sense: a direct chat with a
// person (not «Saved messages», not a bot, not a conversation).
[[nodiscard]] bool CanDeleteForBoth(const Data::Chat &chat);
// Pinning is offered for direct chats and groups outside the requests.
[[nodiscard]] bool CanPin(const Data::Chat &chat);

// "1h" -> «یک ساعت» … for the mute submenu.
[[nodiscard]] QString MuteDurationText(const QString &duration);

} // namespace Dialogs
