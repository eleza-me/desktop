// The emoji panel's data: Telegram's categories, search by Persian and
// English words, the recent emoji counted by use, skin tones.
#include "history/history_emoji_data.h"
#include "ui/emoji/emoji_index.h"

#include <QtTest/QtTest>

namespace {

[[nodiscard]] QString U(const char *utf8) {
	return QString::fromUtf8(utf8);
}

} // namespace

class TestHistoryEmoji : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void categories() {
		const auto &categories = History::EmojiCategories();
		QCOMPARE(categories.size(), 8);
		for (auto i = 0; i != int(categories.size()); ++i) {
			QVERIFY(!categories[i].items.isEmpty());
			QVERIFY(!History::EmojiCategoryTitle(i).isEmpty());
			QVERIFY(Ui::Emoji::Find(categories[i].icon) >= 0);
		}
		QCOMPARE(categories[0].items.front(), U("😀"));
		QVERIFY(categories[7].items.contains(U("🇮🇷")));
	}

	void search() {
		const auto index = History::EmojiSearchIndex::Build();
		// Persian, English, a phrase first, a prefix, two words.
		QVERIFY(index->search(U("قلب")).contains(U("❤")));
		QCOMPARE(index->search(U("red heart")).value(0), U("❤"));
		QVERIFY(index->search(U("ایران")).contains(U("🇮🇷")));
		QVERIFY(index->search(U("grinn")).contains(U("😀")));
		QVERIFY(index->search(U("GRINNING face")).contains(U("😀")));
		QVERIFY(index->search(U("خنده")).contains(U("😂")));
		// Arabic letters typed for Persian ones.
		QVERIFY(index->search(QString::fromUtf8("ا\u064Aران")).contains(U("🇮🇷")));
		// An emoji typed into the query finds itself first.
		QCOMPARE(index->search(U("👍🏽")).value(0), U("👍🏽"));
		QVERIFY(index->search(U("zzzzqqq")).isEmpty());
		QVERIFY(index->search(QString()).isEmpty());
		QCOMPARE(int(index->search(U("a"), 5).size()), 5);
	}

	void recent() {
		auto uses = History::DefaultEmojiUses();
		auto shown = History::SortRecentEmoji(uses);
		QCOMPARE(shown.value(0), U("😂"));
		QCOMPARE(shown.size(), uses.size());
		// Picked again: counted, the order shown stays while the panel is open.
		const auto place = shown.indexOf(U("😀"));
		QVERIFY(place > 0);
		History::CountEmojiUse(uses, shown, U("😀"));
		QCOMPARE(shown.indexOf(U("😀")), place);
		QCOMPARE(uses.value(U("😀")), History::DefaultEmojiUses().value(U("😀")) + 1);
		// Something new is added at the end; with the list full it takes the
		// place of the last one.
		History::CountEmojiUse(uses, shown, U("🐻"));
		QCOMPARE(shown.back(), U("🐻"));
		while (uses.size() < History::kMaxRecentEmoji) {
			History::CountEmojiUse(uses, shown, Ui::Emoji::PanelCategory(2).value(uses.size()));
		}
		const auto before = shown.size();
		const auto last = shown.back();
		History::CountEmojiUse(uses, shown, U("🚗"));
		QCOMPARE(shown.size(), before);
		QCOMPARE(shown.back(), U("🚗"));
		QVERIFY(!uses.contains(last));
		// Used most: first the next time the panel is shown.
		for (auto i = 0; i != 100; ++i) {
			History::CountEmojiUse(uses, shown, U("🚗"));
		}
		shown = History::SortRecentEmoji(uses);
		QCOMPARE(shown.front(), U("🚗"));
		QVERIFY(shown.size() <= History::kMaxRecentEmoji);
	}

	void tones() {
		auto tones = QMap<QString, int>();
		QCOMPARE(History::EmojiWithTone(U("👍"), tones), U("👍"));
		tones.insert(U("👍"), 3);
		QCOMPARE(History::EmojiWithTone(U("👍"), tones), U("👍🏾"));
		// Only what takes a tone.
		tones.insert(U("😀"), 3);
		QCOMPARE(History::EmojiWithTone(U("😀"), tones), U("😀"));
	}

};

QTEST_GUILESS_MAIN(TestHistoryEmoji)
#include "test_history_emoji.moc"
