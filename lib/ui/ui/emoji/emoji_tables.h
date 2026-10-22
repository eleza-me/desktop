// The data that goes with Eleza/Resources/emoji/emoji.pack (Telegram's emoji), generated
// into emoji_tables_auto.cpp by codegen/scripts/gen_emoji_tables.py. Read through
// Ui::Emoji (emoji_index.h); nothing else should need these.
#pragma once

namespace Ui::Emoji::Tables {

// Strings separated by single spaces (no emoji contains a space), cut into
// literals of a few thousand code units; `count` strings in all.
struct List {
	const char16_t *const *chunks = nullptr;
	int chunkCount = 0;
	int count = 0;
};

inline constexpr auto kPageCount = 8;

// Every emoji of the pack in its order (record (page, index) is
// kPages[page][index]), without U+FE0F.
extern const List kPages[kPageCount];
// The panel's categories: smileys & people, animals & nature, food & drink,
// activity, travel & places, objects, symbols, flags.
extern const List kPanel[kPageCount];
// The emoji that take a skin tone in the panel.
extern const List kColored;
// Old spellings and the spelling of the pack, item by item.
extern const List kAliasOld;
extern const List kAliasNew;
// Lines «emoji\tword|word|…» separated by '\n' (Persian words first).
extern const List kKeywords;

// Symbols (BMP) whose emoji form is written with U+FE0F after them.
extern const char16_t kToFe0f[];
// Symbols (BMP, U+00A9…U+3299) that are emoji of the pack on their own.
extern const char16_t kDataChars[];

} // namespace Ui::Emoji::Tables
