// The emoji panel's data, as Telegram's: its eight categories (the emoji pack's
// tables, ui/emoji/emoji_index.h), search by Persian and English words (CLDR),
// the recent emoji counted by use and the skin tone chosen per emoji. No
// widgets (tests/app/test_history_emoji.cpp).
#pragma once

#include "base/basic_types.h"

#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <memory>
#include <vector>

class QObject;

namespace History {

inline constexpr auto kMaxRecentEmoji = 48;

struct EmojiCategory {
	QString icon; // the emoji on the category's tab
	QStringList items; // the bases (without a skin tone)
};

[[nodiscard]] const QVector<EmojiCategory> &EmojiCategories();
// The title of a category (the order of EmojiCategories()).
[[nodiscard]] QString EmojiCategoryTitle(int index);

// The words of the emoji in the panel's order, read once.
class EmojiSearchIndex final {
public:
	// Any thread: a few thousand rows to split and normalize.
	[[nodiscard]] static std::shared_ptr<const EmojiSearchIndex> Build();

	// The emoji (bases) whose words start with every word of `query`, best
	// first: those with one phrase holding every word («red heart»), then
	// those holding each as a whole word, then those where a word only starts
	// one, each group in the panel's order. Emoji typed into the query find
	// themselves first.
	[[nodiscard]] QStringList search(const QString &query, int limit = 200) const;

private:
	struct Row {
		QString base;
		QStringList tokens; // every word of every phrase, sorted
		std::vector<QStringList> phrases; // the words of each phrase
	};
	std::vector<Row> _rows;

};

// The index once it is built, or null: then it is being built on a pool
// thread and `ready` is called (on the GUI thread, while `guard` lives) when it
// is there.
[[nodiscard]] std::shared_ptr<const EmojiSearchIndex> EmojiSearch(
	QObject *guard,
	Fn<void()> ready);

// What «recent» starts with before anything was picked (Telegram's first rows).
[[nodiscard]] QMap<QString, int> DefaultEmojiUses();
// The recent emoji in the order shown: the most used first, at most
// kMaxRecentEmoji (the counts of the others are dropped from `uses`).
[[nodiscard]] QStringList SortRecentEmoji(QMap<QString, int> &uses);
// `emoji` was picked: counted. While the panel is open the shown order stays
// (no jumping): a new emoji takes the place of the last one when the list is
// full, or is added at the end.
void CountEmojiUse(
	QMap<QString, int> &uses,
	QStringList &shown,
	const QString &emoji);

// `base` as the panel shows it: with the tone chosen for it, if any.
[[nodiscard]] QString EmojiWithTone(
	const QString &base,
	const QMap<QString, int> &tones);

} // namespace History
