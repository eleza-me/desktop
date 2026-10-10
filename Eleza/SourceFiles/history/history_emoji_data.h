// The emoji the composer's panel offers: a compact categorised set with
// English and Persian keywords for the search field.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace History {

struct EmojiItem {
	QString text;
	QString keywords; // lower case, space separated
};

struct EmojiCategory {
	QString icon; // the emoji shown on the category's tab
	QVector<EmojiItem> items;
};

[[nodiscard]] const QVector<EmojiCategory> &EmojiCategories();
// The title of a category (the order of EmojiCategories()).
[[nodiscard]] QString EmojiCategoryTitle(int index);
// Emoji whose keywords contain every word of the query.
[[nodiscard]] QStringList SearchEmoji(const QString &query, int limit = 80);

} // namespace History
