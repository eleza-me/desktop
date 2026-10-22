// Widget-free rules of the chat screen (compiled into the unit tests as is):
// which messages stick together, where day separators and the unread divider
// go, how a changed timeline maps onto the rows that are already laid out,
// what a text selection means as a quote, how messages are copied and found.
#pragma once

#include "data/data_types.h"
#include "ui/text/text_entity.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QSize>

#include <vector>

namespace History {

// ---- rows

// Messages of one sender sent within this window share a group (one tail).
inline constexpr auto kGroupWindowMs = qint64(5 * 60 * 1000);

struct RowFlags {
	bool attachedToPrevious = false; // tight spacing, small corner on the sender side
	bool attachedToNext = false;     // no tail
	bool showDate = false;           // a day separator above the row
	bool showUnread = false;         // the «unread messages» divider above the row

	friend bool operator==(const RowFlags &, const RowFlags &) = default;
};

// The local calendar day of a message (today for a message without a time).
[[nodiscard]] QDate MessageDate(const Data::Message &message);
[[nodiscard]] QDateTime MessageDateTime(const Data::Message &message);

// A centred pill instead of a bubble (service messages, and
// self-destructing media).
[[nodiscard]] bool IsService(const Data::Message &message);

// `next` continues the group of `previous`.
[[nodiscard]] bool CanAttach(
	const Data::Message &previous,
	const Data::Message &next);

// One entry per message. The unread divider goes above the first incoming
// message with seq >= firstUnreadSeq (0 = no divider).
[[nodiscard]] std::vector<RowFlags> ComputeRowFlags(
	const QVector<Data::Message> &items,
	qint64 firstUnreadSeq);

// ---- list diffing

struct RowId {
	QString key;             // Data::Message::key()
	QString clientMessageId; // links a sent message to its pending row

	[[nodiscard]] static RowId From(const Data::Message &message);
};

struct ListDiff {
	std::vector<int> newToOld; // index in the old list, -1 for a new row
	std::vector<int> oldToNew; // index in the new list, -1 for a removed row
	int added = 0;
	int removed = 0;
	int addedAtFront = 0;      // new rows before the first surviving one
	int addedAtBack = 0;       // new rows after the last surviving one
	bool reordered = false;    // surviving rows changed their relative order

	[[nodiscard]] bool unchanged() const {
		return !added && !removed && !reordered;
	}
};

// A row survives when its key is the same, or when the new row is the server
// copy ("seq:N") of a pending one ("local:<client message id>").
[[nodiscard]] ListDiff DiffRows(
	const std::vector<RowId> &was,
	const std::vector<RowId> &now);

// The old row the reading position holds on to through `diff` when it was
// at old row `anchor`: that row while it survives, else the nearest
// surviving one above it (what is above stays in place), else the nearest
// one below; -1 when no row survives.
[[nodiscard]] int SurvivingAnchor(const ListDiff &diff, int anchor);

// Whether two versions of a message look the same on screen (so the row
// keeps its layout).
[[nodiscard]] bool SameForView(const Data::Message &a, const Data::Message &b);
// The same, for changes that cannot change the row's height.
[[nodiscard]] bool SameLayout(const Data::Message &a, const Data::Message &b);

// ---- text

// The message text with its entities (server spoilers / formatting plus
// links, mentions and hashtags found in the text).
// `botCommands`: a chat with a bot, where «/command» in the text is a link
// that sends it.
[[nodiscard]] Ui::Text::TextWithEntities TextForMessage(
	const Data::Message &message,
	bool botCommands = false);
[[nodiscard]] Ui::Text::Entities ConvertEntities(
	const QString &text,
	const QVector<Data::Entity> &entities);

// 1..3 when the text is nothing but that many emoji, 0 otherwise.
[[nodiscard]] int JumboEmojiCount(const QString &text);
[[nodiscard]] bool IsEmojiCodepoint(char32_t code);

// A selection inside a message as a quote for a reply: whitespace at the
// edges is dropped, the range never splits a surrogate pair and is cut to
// `maxLength` UTF-16 units. `text` is always an exact substring of the
// message at `offset`. Nullopt for an empty selection.
[[nodiscard]] std::optional<Data::Quote> MakeQuote(
	const QString &messageText,
	Ui::Text::Selection selection,
	int maxLength);
// Where a quote is inside the message it was taken from: its own offset if
// the text is still there, else the first occurrence, else nullopt.
[[nodiscard]] std::optional<Ui::Text::Selection> FindQuote(
	const QString &messageText,
	const Data::Quote &quote);

// ---- copying

struct CopyItem {
	QString sender;
	QDateTime time;
	QString text;
};
// One message: just its text. Several: «[sender, date time]\ntext» blocks
// separated by an empty line.
[[nodiscard]] QString FormatForCopy(const std::vector<CopyItem> &items);
// What a message is as plain text (its text, or the label of its media
// followed by the caption / file name).
[[nodiscard]] QString MessagePlainText(
	const Data::Message &message,
	const QString &kindLabel);

// ---- search in the loaded messages

// Lower case, Arabic yeh / kaf folded to the Persian ones, diacritics and
// zero-width joiners dropped.
[[nodiscard]] QString NormalizeForSearch(const QString &text);
// Indexes of the messages that contain every word of the query, newest first.
[[nodiscard]] std::vector<int> SearchMessages(
	const QVector<Data::Message> &items,
	const QString &query);

// ---- reactions, media, permissions

struct ReactionCount {
	QString emoji;
	qint64 count = 0;
	bool mine = false;
};
// Most used first, then by emoji (a stable order while counts change).
[[nodiscard]] std::vector<ReactionCount> SortedReactions(
	const Data::Message &message);
// The message after I pick `emoji` (the same one again removes it).
void ApplyMyReaction(Data::Message &message, const QString &emoji);

// The box a photo / video takes in a bubble.
[[nodiscard]] QSize MediaBoxSize(
	int width,
	int height,
	Data::MessageKind kind,
	int maxWidth);

// Bar heights 0..1 of a voice message (a gentle fake when there is none).
[[nodiscard]] std::vector<float> WaveformBars(
	const QByteArray &waveform,
	int bars);

enum class FileType {
	Generic,
	Image,
	Video,
	Audio,
	Document,
	Archive,
	Code,
};
[[nodiscard]] FileType FileTypeFor(const QString &name, const QString &mime);
// "pdf", "zip": what the icon of a file shows (at most four letters).
[[nodiscard]] QString FileExtensionLabel(const QString &name);

// Text and captions of my own messages, within the edit window.
[[nodiscard]] bool CanEdit(
	const Data::Message &message,
	qint64 nowMs,
	qint64 editWindowSeconds);
[[nodiscard]] bool HasCopyableText(const Data::Message &message);
[[nodiscard]] bool CanForward(const Data::Message &message);

// The reply keyboard a bot left under the field: the markup of the newest
// incoming message that carries one, unless that message removes it. `after`
// skips the messages up to this seq (a one-time keyboard that was used).
[[nodiscard]] std::optional<Data::ReplyMarkup> CurrentReplyKeyboard(
	const QVector<Data::Message> &items,
	qint64 after = 0);
// The seq of the message that keyboard came with (0 if there is none).
[[nodiscard]] qint64 ReplyKeyboardSeq(const QVector<Data::Message> &items);

// States in which nobody can write to a conversation any more.
[[nodiscard]] bool ConversationEnded(const QString &state);

// ---- self-destructing media

// It may be sent here: a direct chat with a person — not a group, Saved
// Messages, a bot, an anonymous conversation or a deleted account.
[[nodiscard]] bool SelfDestructAllowed(const Data::Chat &chat);

// ---- the floating buttons over the list («to the end», «@»)

// The first stop of «to the end»: the oldest of the `unread` newest
// messages of somebody else — the notes of a group too, the count is made
// of them — when it is below `bottomSeq` (the last message on screen); 0:
// straight to the end. Never at or below `lastReadSeq`.
[[nodiscard]] qint64 DownFirstStop(
	const QVector<Data::Message> &items,
	int unread,
	qint64 lastReadSeq,
	qint64 bottomSeq);
// The buttons float on the side of my own bubbles (the physical right in
// both directions): in a group the other side is the column of the
// senders' pictures. A direct chat keeps them at the end of the line.
[[nodiscard]] bool FloatingButtonsAtPhysicalRight(bool group, bool rtl);
// The next unread mention to go to: the first of `seqs` (oldest first)
// above `lastReadSeq`; 0 when the list holds none any more.
[[nodiscard]] qint64 NextMention(const QVector<qint64> &seqs, qint64 lastReadSeq);

} // namespace History
