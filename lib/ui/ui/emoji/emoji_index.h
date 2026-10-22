// Which emoji Telegram's pack has, where, and where they are in a text.
//
//   for (const auto &range : Ui::Emoji::FindAll(text)) {
//       // text.mid(range.from, range.till - range.from) is drawn as picture range.id
//   }
//   const auto id = Ui::Emoji::Find(u"👍🏽");   // one emoji, or Ui::Emoji::kNone
//
// An emoji found in a text is one of: a keycap (0–9 # * [FE0F] 20E3); a flag (two
// regional indicators); a subdivision flag (🏴 + tag letters + cancel tag); a
// chain of elements joined by ZWJ, an element being a pictograph with an optional
// variation selector and an optional skin tone. A sequence the pack does not know
// as a whole falls apart into the longest leading parts it knows (the ZWJ between
// them stays text); what it does not know at all is left to the fonts. A symbol
// followed by U+FE0E (text style) stays text. Emoji are compared without
// variation selectors: the pack's table has none, texts have them or not.
//
// An id: page << 12 | index in the page for an emoji of the pack; a handshake
// drawn from two halves (IsHandshake) for 🤝 with a skin tone and 🫱‍🫲 with
// two; kMirrored on top for a «facing right» sequence (ZWJ ➡), drawn mirrored.
//
// Plain data, no GUI: any thread.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QStringView>

#include <vector>

namespace Ui::Emoji {

using Id = int;

inline constexpr Id kNone = -1;
// On an id: draw the picture mirrored (the sequence ends with ZWJ ➡).
inline constexpr Id kMirrored = 0x40000000;
// At most this many emoji of one text become pictures (the rest stay with the
// fonts): a text of thousands of emoji would cost a picture each.
inline constexpr auto kMaxPerText = 512;
inline constexpr auto kToneCount = 5;
inline constexpr auto kPanelCategoryCount = 8;

[[nodiscard]] inline int Page(Id id) {
	return (id & ~kMirrored) >> 12;
}
[[nodiscard]] inline int Slot(Id id) {
	return id & 0xFFF;
}
[[nodiscard]] inline bool IsMirrored(Id id) {
	return (id >= 0) && (id & kMirrored);
}
[[nodiscard]] bool IsHandshake(Id id);
// The skin tone (0…4, -1 = none) of the left / right hand of a handshake id.
[[nodiscard]] int HandshakeTone(Id id, bool right);

// One emoji written as `code` (with or without variation selectors, an older
// spelling, a handshake of two tones, a «facing right» form), or kNone.
[[nodiscard]] Id Find(QStringView code);
// The characters of pack emoji `id`, without variation selectors (null for
// handshakes and kNone).
[[nodiscard]] QString Code(Id id);
// How many emoji the tables list on `page` (for the pack's sanity check).
[[nodiscard]] int PageSize(int page);

struct Range {
	int from = 0;
	int till = 0;
	Id id = kNone;

	friend inline bool operator==(const Range &a, const Range &b) {
		return (a.from == b.from) && (a.till == b.till) && (a.id == b.id);
	}
};

// Every emoji of `text` the pack draws, in order, at most `limit` of them.
[[nodiscard]] std::vector<Range> FindAll(
	QStringView text,
	int limit = kMaxPerText);
// Whether `text` may hold an emoji at all (a quick check before FindAll).
[[nodiscard]] bool MayHaveEmoji(QStringView text);

// `code` without U+FE0E / U+FE0F.
[[nodiscard]] QString Strip(QStringView code);
// The skin tone modifier 0…4 (U+1F3FB…U+1F3FF).
[[nodiscard]] QString Tone(int tone);
// The emoji takes a skin tone in the panel.
[[nodiscard]] bool TakesTone(QStringView base);
// `base` with skin tone `tone` (0…4, -1 = none): after the person, before a
// trailing ZWJ part (a profession, a gender, a hair style) and before a
// «facing right» ending.
[[nodiscard]] QString WithTone(const QString &base, int tone);
// The emoji as it is put into a message: U+FE0F after the symbols that are text
// unless asked to be emoji (the symbols of a ZWJ sequence, a keycap's digit), so
// that other apps and older systems draw an emoji too. Nothing is added where a
// skin tone, U+FE0F or U+FE0E follows already.
[[nodiscard]] QString Qualify(QStringView code);

// The panel's categories (0 smileys & people … 7 flags), in Telegram's order.
[[nodiscard]] const QStringList &PanelCategory(int index);
// The emoji on a category's tab, qualified (what a font draws as an emoji).
[[nodiscard]] QString PanelCategoryIcon(int index);

} // namespace Ui::Emoji
