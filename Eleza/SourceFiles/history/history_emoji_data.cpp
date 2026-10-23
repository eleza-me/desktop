#include "history/history_emoji_data.h"

#include "history/history_logic.h"
#include "lang_auto.h"
#include "ui/emoji/emoji_index.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QThreadPool>

#include <algorithm>

namespace History {
namespace {

// Telegram's first rows of «recent», roughly, before anything was picked.
const char16_t *const kDefaultRecent[] = {
	u"\U0001F602", u"\U0001F618", u"❤", u"\U0001F60D", u"\U0001F60A",
	u"\U0001F601", u"\U0001F44D", u"☺", u"\U0001F614", u"\U0001F604",
	u"\U0001F62D", u"\U0001F48B", u"\U0001F612", u"\U0001F633", u"\U0001F61C",
	u"\U0001F648", u"\U0001F609", u"\U0001F603", u"\U0001F622", u"\U0001F61D",
	u"\U0001F631", u"\U0001F621", u"\U0001F60F", u"\U0001F61E", u"\U0001F605",
	u"\U0001F61A", u"\U0001F64A", u"\U0001F60C", u"\U0001F600", u"\U0001F60B",
	u"\U0001F606", u"\U0001F44C",
};

[[nodiscard]] QStringList Words(const QString &text) {
	return NormalizeForSearch(text).split(u' ', Qt::SkipEmptyParts);
}

struct SearchState {
	std::shared_ptr<const EmojiSearchIndex> index;
	bool building = false;
	std::vector<std::pair<QPointer<QObject>, Fn<void()>>> waiting;
};

[[nodiscard]] SearchState &Search() {
	static auto result = SearchState();
	return result;
}

} // namespace

const QVector<EmojiCategory> &EmojiCategories() {
	static const auto result = [] {
		auto list = QVector<EmojiCategory>();
		for (auto i = 0; i != Ui::Emoji::kPanelCategoryCount; ++i) {
			list.push_back({
				Ui::Emoji::PanelCategoryIcon(i),
				Ui::Emoji::PanelCategory(i),
			});
		}
		return list;
	}();
	return result;
}

QString EmojiCategoryTitle(int index) {
	switch (index) {
	case 0: return tr::lng_history_emoji_smileys(tr::now);
	case 1: return tr::lng_history_emoji_nature(tr::now);
	case 2: return tr::lng_history_emoji_food(tr::now);
	case 3: return tr::lng_history_emoji_activity(tr::now);
	case 4: return tr::lng_history_emoji_travel(tr::now);
	case 5: return tr::lng_history_emoji_objects(tr::now);
	case 6: return tr::lng_history_emoji_symbols(tr::now);
	case 7: return tr::lng_history_emoji_flags(tr::now);
	}
	return QString();
}

std::shared_ptr<const EmojiSearchIndex> EmojiSearchIndex::Build() {
	auto result = std::make_shared<EmojiSearchIndex>();
	const auto words = Ui::Emoji::SearchWords();
	for (auto c = 0; c != Ui::Emoji::kPanelCategoryCount; ++c) {
		for (const auto &base : Ui::Emoji::PanelCategory(c)) {
			const auto i = words.constFind(Ui::Emoji::Strip(base));
			if (i == words.cend() || i->isEmpty()) {
				continue;
			}
			auto row = Row();
			row.base = base;
			auto tokens = QSet<QString>();
			for (const auto &phrase : *i) {
				auto list = Words(phrase);
				for (const auto &word : list) {
					tokens.insert(word);
				}
				row.phrases.push_back(std::move(list));
			}
			row.tokens = QStringList(tokens.begin(), tokens.end());
			row.tokens.sort();
			result->_rows.push_back(std::move(row));
		}
	}
	return result;
}

QStringList EmojiSearchIndex::search(const QString &query, int limit) const {
	auto result = QStringList();
	const auto trimmed = query.trimmed();
	if (trimmed.isEmpty() || limit <= 0) {
		return result;
	}
	auto seen = QSet<QString>();
	for (const auto &range : Ui::Emoji::FindAll(trimmed)) {
		const auto code = trimmed.mid(range.from, range.till - range.from);
		if (!seen.contains(Ui::Emoji::Strip(code))) {
			seen.insert(Ui::Emoji::Strip(code));
			result.push_back(code);
		}
	}
	const auto terms = Words(trimmed);
	if (terms.isEmpty()) {
		return result.mid(0, limit);
	}
	const auto hasWord = [](const QStringList &tokens, const QString &term) {
		return std::binary_search(tokens.begin(), tokens.end(), term);
	};
	const auto hasPrefix = [](const QStringList &tokens, const QString &term) {
		// The first token not less than the term is the one it may start.
		const auto i = std::lower_bound(tokens.begin(), tokens.end(), term);
		return (i != tokens.end()) && i->startsWith(term);
	};
	auto phrase = QStringList();
	auto whole = QStringList();
	auto partial = QStringList();
	for (const auto &row : _rows) {
		const auto key = Ui::Emoji::Strip(row.base);
		if (seen.contains(key)) {
			continue;
		}
		const auto all = std::all_of(terms.begin(), terms.end(), [&](const QString &term) {
			return hasPrefix(row.tokens, term);
		});
		if (!all) {
			continue;
		}
		seen.insert(key);
		const auto inPhrase = std::any_of(
			row.phrases.begin(),
			row.phrases.end(),
			[&](const QStringList &words) {
				return std::all_of(terms.begin(), terms.end(), [&](const QString &term) {
					return words.contains(term);
				});
			});
		if (inPhrase) {
			phrase.push_back(row.base);
		} else if (std::all_of(terms.begin(), terms.end(), [&](const QString &term) {
				return hasWord(row.tokens, term);
			})) {
			whole.push_back(row.base);
		} else {
			partial.push_back(row.base);
		}
	}
	result += phrase;
	result += whole;
	result += partial;
	return result.mid(0, limit);
}

std::shared_ptr<const EmojiSearchIndex> EmojiSearch(
		QObject *guard,
		Fn<void()> ready) {
	auto &state = Search();
	if (state.index) {
		return state.index;
	}
	if (ready) {
		state.waiting.emplace_back(guard, std::move(ready));
	}
	if (!state.building) {
		state.building = true;
		QThreadPool::globalInstance()->start([] {
			auto index = EmojiSearchIndex::Build();
			const auto app = QCoreApplication::instance();
			if (!app) {
				return;
			}
			QMetaObject::invokeMethod(app, [index = std::move(index)] {
				auto &state = Search();
				state.index = index;
				state.building = false;
				auto waiting = std::exchange(state.waiting, {});
				for (const auto &[guard, callback] : waiting) {
					if (guard) {
						callback();
					}
				}
			}, Qt::QueuedConnection);
		});
	}
	return nullptr;
}

QMap<QString, int> DefaultEmojiUses() {
	auto result = QMap<QString, int>();
	const auto count = int(std::size(kDefaultRecent));
	for (auto i = 0; i != count; ++i) {
		result.insert(QString::fromUtf16(kDefaultRecent[i]), count - i);
	}
	return result;
}

QStringList SortRecentEmoji(QMap<QString, int> &uses) {
	auto list = std::vector<std::pair<QString, int>>();
	list.reserve(uses.size());
	for (auto i = uses.begin(); i != uses.end(); ++i) {
		list.emplace_back(i.key(), i.value());
	}
	std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
		return a.second > b.second;
	});
	auto result = QStringList();
	for (auto i = 0; i != int(list.size()); ++i) {
		if (i < kMaxRecentEmoji) {
			result.push_back(list[i].first);
		} else {
			uses.remove(list[i].first);
		}
	}
	return result;
}

void CountEmojiUse(
		QMap<QString, int> &uses,
		QStringList &shown,
		const QString &emoji) {
	if (emoji.isEmpty()) {
		return;
	}
	auto i = uses.find(emoji);
	if (i == uses.end()) {
		if (uses.size() >= kMaxRecentEmoji && !shown.isEmpty()) {
			uses.remove(shown.back());
			shown.back() = emoji;
		} else {
			shown.push_back(emoji);
		}
		i = uses.insert(emoji, 0);
	}
	++i.value();
}

QString EmojiWithTone(const QString &base, const QMap<QString, int> &tones) {
	if (!Ui::Emoji::TakesTone(base)) {
		return base;
	}
	const auto i = tones.constFind(Ui::Emoji::Strip(base));
	return (i != tones.cend()) ? Ui::Emoji::WithTone(base, *i) : base;
}

} // namespace History
