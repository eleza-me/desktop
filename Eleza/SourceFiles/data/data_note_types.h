// The wire shapes of notes (backend docs/notes.md) and the small rules that go
// with them, the same as the mobile apps': how a note's length is counted,
// which text can be shared, which notes are still showing, and the plain text
// message a reply to a note travels as. Plain structs and functions, parsed
// once, no Qt objects.
//
//   auto feed = Data::NotesFeed::FromJson(json);   // GET /v1/notes
//   feed = feed.fresh(nowSeconds);                 // without the ones that ran out
//   Data::NoteText::IsValid(text, maxChars);       // what «اشتراک» may send
//   Data::NoteReply::Build(note, answer);          // "↩︎ «note»\nanswer"
//   if (const auto reply = Data::NoteReply::Parse(message.text)) { … }
#pragma once

#include "data/data_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>

namespace Data {

// Who a note is shared with.
enum class NoteAudience {
	Mutuals,      // the people I follow who follow me back (the default)
	CloseFriends, // only my close-friends list (the stories' list)
};

[[nodiscard]] QString NoteAudienceWire(NoteAudience audience);
// Anything unknown or absent reads as Mutuals.
[[nodiscard]] NoteAudience NoteAudienceFromWire(const QString &value);

struct Note {
	QString id;
	QString text;
	NoteAudience audience = NoteAudience::Mutuals;
	qint64 createdAt = 0;  // unix seconds
	qint64 expiresAt = 0;  // unix seconds, 0: not told

	[[nodiscard]] bool valid() const {
		return !text.isEmpty();
	}
	// Its time is over: not shown any more, whatever the last read said.
	[[nodiscard]] bool expired(qint64 now) const {
		return expiresAt > 0 && now >= expiresAt;
	}
	// Null when the object holds no note (absent, null, no text).
	[[nodiscard]] static std::optional<Note> FromJson(const QJsonValue &json);
	friend bool operator==(const Note &, const Note &) = default;
};

// A friend's note with their card (the story tray's card).
struct NoteItem {
	User user;
	Note note;

	friend bool operator==(const NoteItem &, const NoteItem &) = default;
};

// GET /v1/notes: my own note (if I have one) and my friends' notes, in the
// server's order (already ranked: shown as they come).
struct NotesFeed {
	std::optional<Note> me;
	QVector<NoteItem> items;

	// Leaves out the items nobody can answer (a deleted account, a card
	// without an id), a second note of the same person and anything past
	// `limit` items.
	[[nodiscard]] static NotesFeed FromJson(
		const QJsonObject &json,
		int limit = kMaxItems);
	// Without the notes whose time ran out by `now`, in the same order.
	[[nodiscard]] NotesFeed fresh(qint64 now) const;
	// The earliest expiry after `now` of the notes it holds, 0 without one.
	[[nodiscard]] qint64 nextExpiry(qint64 now) const;
	[[nodiscard]] const NoteItem *item(const QString &userId) const;
	[[nodiscard]] bool empty() const {
		return !me && items.isEmpty();
	}

	// The server sends at most notes.feed_max (100 by default; 1000 at most).
	static constexpr auto kMaxItems = 1000;

	friend bool operator==(const NotesFeed &, const NotesFeed &) = default;
};

// The rules of a note's text, shared by the composer and the store.
namespace NoteText {

// notes.max_chars when the server does not say.
inline constexpr auto kDefaultMaxChars = 60;
// The counter shows up for the last this many characters.
inline constexpr auto kCounterFrom = 10;

// The length as the server counts it: in code points (an emoji made of
// several code points counts as several, so what the counter allows is
// never refused).
[[nodiscard]] int Length(const QString &text);
// One line: every run of white space (line breaks too) becomes one space,
// the ends are trimmed — what the server stores.
[[nodiscard]] QString Cleaned(const QString &text);
// Something is left after cleaning, and not more than `maxChars`.
[[nodiscard]] bool IsValid(const QString &text, int maxChars);
// What may still be typed (negative: that many too many).
[[nodiscard]] int Left(const QString &text, int maxChars);
// At most `maxChars` code points, cut between characters (an emoji made of
// several code points, a letter with its marks, is never split).
[[nodiscard]] QString Limited(const QString &text, int maxChars);

} // namespace NoteText

// A reply to a note as it travels: a plain text message in the 1:1 chat with
// the author, so that every app version shows it as it is. The first line
// quotes the note, the answer follows (the same on Android and iOS):
//
//     ↩︎ «<note>»
//     <answer>
//
// The first app versions wrote a short label before the quote
// («↩︎ نوتت: «<note>»», «↩︎ Your note: «<note>»»): it is read too.
struct NoteReply {
	QString note;   // the note that was answered (one line)
	QString reply;  // the answer
	int replyOffset = 0; // where the answer starts in the message text (UTF-16)

	// The message text of a reply to `note`.
	[[nodiscard]] static QString Build(const QString &note, const QString &reply);
	// The note and the answer of a note reply's text (either format), or
	// nothing for any other text.
	[[nodiscard]] static std::optional<NoteReply> Parse(const QString &text);
	// A one-line preview (the chat list, notifications): a note reply shows
	// «↩︎ <answer>», any other text is returned as it is.
	[[nodiscard]] static QString Preview(const QString &text);
	// The entities of the whole message moved onto the answer alone (the
	// ones of the quoted line are dropped, the ones across it are cut).
	[[nodiscard]] QVector<Entity> replyEntities(
		const QVector<Entity> &entities) const;

	friend bool operator==(const NoteReply &, const NoteReply &) = default;
};

} // namespace Data
