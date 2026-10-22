// Widget-free rules of the composer (compiled into the unit tests as is):
// keeping spoiler ranges in place while the text is edited, preparing a text
// for sending, and when a draft is saved.
//
// Offsets are UTF-16 code units everywhere — the units of QString, of the
// editor's document and of the API.
#pragma once

#include "data/data_types.h"

#include <vector>

namespace History {

// ---- entities under edits

// `removed` units at `position` were replaced by `added` units.
struct TextEdit {
	int position = 0;
	int removed = 0;
	int added = 0;

	[[nodiscard]] bool empty() const {
		return !removed && !added;
	}
	friend bool operator==(const TextEdit &, const TextEdit &) = default;
};

// The edit that turns `was` into `now`. `hint` is where the editor says the
// change starts (-1 = unknown): it decides between equivalent answers when
// the same character repeats (typing "a" inside "aaa"). The edit never
// splits a surrogate pair.
[[nodiscard]] TextEdit DiffTexts(
	const QString &was,
	const QString &now,
	int hint = -1);

// Moves / grows / cuts the ranges for an edit:
//   * text typed strictly inside a range joins it, at its edges it does not;
//   * ranges after the edit shift, ranges inside a removed part disappear.
[[nodiscard]] QVector<Data::Entity> AdjustEntities(
	QVector<Data::Entity> entities,
	const TextEdit &edit);
[[nodiscard]] QVector<Data::Entity> AdjustEntities(
	QVector<Data::Entity> entities,
	const QString &was,
	const QString &now,
	int hint = -1);

// Sorted, clamped to the text, never inside a surrogate pair, overlapping
// and touching spoilers merged; other entity types pass through clamped.
[[nodiscard]] QVector<Data::Entity> NormalizeEntities(
	const QString &text,
	QVector<Data::Entity> entities);

[[nodiscard]] bool RangeIsSpoiler(
	const QVector<Data::Entity> &entities,
	int from,
	int till);
// Marks [from, till) as a spoiler, or unmarks it when all of it already is.
[[nodiscard]] QVector<Data::Entity> ToggleSpoiler(
	const QString &text,
	QVector<Data::Entity> entities,
	int from,
	int till);

// An edit replaces every entity of a message. The composer marks spoilers
// and mentions itself; the other entities the message came with (bold,
// links, code...) go along with the text: each of `kept` joins `entities`
// where it overlaps none of them (the server takes no overlapping ranges).
[[nodiscard]] QVector<Data::Entity> WithKeptEntities(
	QVector<Data::Entity> entities,
	const QVector<Data::Entity> &kept);

// ---- sending

struct PreparedText {
	QString text;
	QVector<Data::Entity> entities;

	[[nodiscard]] bool empty() const {
		return text.isEmpty();
	}
};

// Trims the whitespace around the text and re-bases the entities; at most
// `maxEntities` are kept (0 = no limit).
[[nodiscard]] PreparedText PrepareForSend(
	const QString &text,
	const QVector<Data::Entity> &entities,
	int maxEntities = 0);

// A text longer than `maxLength` goes out as several messages, cut at a
// line break or a space when one is near.
[[nodiscard]] std::vector<PreparedText> SplitForSend(
	const PreparedText &prepared,
	int maxLength);

// The first address in the text that deserves a preview card ("" if none);
// an address hidden inside a spoiler does not get one.
[[nodiscard]] QString FirstLinkForPreview(
	const QString &text,
	const QVector<Data::Entity> &entities);

[[nodiscard]] bool SameDraft(const Data::Draft &a, const Data::Draft &b);

// ---- drafts

// Saves the composer's text as the chat's draft:
//   * not earlier than kDebounceMs after the last keystroke;
//   * at once when the chat is left (flush);
//   * never after the text was sent (sent() cancels a pending save and
//     clears what was stored).
// Time comes from the caller (the widget passes a monotonic clock, the
// tests a fake one); the timer is the delegate's.
class DraftSaver final {
public:
	static constexpr auto kDebounceMs = qint64(2000);

	struct Delegate {
		Fn<void(const Data::Draft &draft)> save;
		Fn<void()> clear;
		Fn<void(qint64 delayMs)> arm; // (re)starts the single-shot timer
		Fn<void()> disarm;
	};

	explicit DraftSaver(Delegate delegate);

	// What the field was filled with when the chat opened.
	void reset(const Data::Draft &loaded);
	// The field changed at `nowMs`.
	void changed(const Data::Draft &current, qint64 nowMs);
	// The timer fired.
	void timeout(qint64 nowMs);
	// Leaving the chat: save right now what is not saved yet.
	void flush();
	// The text went out as a message.
	void sent();
	// While a message is being edited the field does not hold a draft.
	void setSuspended(bool suspended);

	// A draft came from another device. The local text wins while the field
	// has the focus or holds something that is not saved yet.
	[[nodiscard]] bool acceptsRemote(
		const std::optional<Data::Draft> &remote,
		bool focused) const;

	[[nodiscard]] bool dirty() const {
		return _dirty;
	}
	[[nodiscard]] const Data::Draft &stored() const {
		return _stored;
	}

private:
	void store();

	Delegate _delegate;
	Data::Draft _stored;  // what the store has
	Data::Draft _current; // what the field has
	qint64 _changedAt = 0;
	bool _dirty = false;
	bool _suspended = false;

};

} // namespace History
