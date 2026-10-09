// History:: row rules: grouping, day separators, the unread divider, list
// diffing, quotes from a selection, emoji-only detection, copy and search.
#include "history/history_logic.h"

#include <QtTest/QtTest>

namespace {

[[nodiscard]] QString U(const char *utf8) {
	return QString::fromUtf8(utf8);
}

[[nodiscard]] qint64 At(int day, int hour, int minute, int second = 0) {
	return QDateTime(QDate(2026, 10, day), QTime(hour, minute, second)).toMSecsSinceEpoch();
}

[[nodiscard]] Data::Message Text(qint64 seq, bool mine, qint64 at, const QString &text = u"x"_s) {
	auto result = Data::Message();
	result.chat = u"c"_s;
	result.seq = seq;
	result.mine = mine;
	result.sender = mine ? u"me"_s : u"peer"_s;
	result.kind = Data::MessageKind::Text;
	result.text = text;
	result.createdAt = at;
	return result;
}

[[nodiscard]] Data::Message Service(qint64 seq, qint64 at) {
	auto result = Text(seq, false, at, QString());
	result.kind = Data::MessageKind::System;
	result.system = u"conversation.joined"_s;
	return result;
}

[[nodiscard]] std::vector<History::RowId> Ids(const QStringList &keys) {
	auto result = std::vector<History::RowId>();
	for (const auto &key : keys) {
		result.push_back({ key, QString() });
	}
	return result;
}

} // namespace

class TestHistoryLogic : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void groupsBySenderAndTime() {
		const auto items = QVector<Data::Message>{
			Text(1, false, At(5, 10, 0)),
			Text(2, false, At(5, 10, 1)),
			Text(3, false, At(5, 10, 4)),
			Text(4, true, At(5, 10, 5)),
			Text(5, true, At(5, 10, 20)), // more than five minutes later
			Text(6, false, At(5, 10, 21)),
		};
		const auto flags = History::ComputeRowFlags(items, 0);
		QCOMPARE(int(flags.size()), 6);
		QVERIFY(flags[0].showDate);
		QVERIFY(!flags[0].attachedToPrevious && flags[0].attachedToNext);
		QVERIFY(flags[1].attachedToPrevious && flags[1].attachedToNext);
		QVERIFY(flags[2].attachedToPrevious && !flags[2].attachedToNext);
		QVERIFY(!flags[3].attachedToPrevious && !flags[3].attachedToNext);
		QVERIFY(!flags[4].attachedToPrevious && !flags[4].attachedToNext);
		QVERIFY(!flags[5].attachedToPrevious);
		for (auto i = 1; i != 6; ++i) {
			QVERIFY(!flags[i].showDate);
		}
	}

	void windowBoundary() {
		const auto first = Text(1, true, At(5, 10, 0, 0));
		QVERIFY(History::CanAttach(first, Text(2, true, At(5, 10, 4, 59))));
		QVERIFY(!History::CanAttach(first, Text(2, true, At(5, 10, 5, 0))));
		QVERIFY(!History::CanAttach(first, Text(2, false, At(5, 10, 0, 30))));
		auto other = Text(2, true, At(5, 10, 0, 30));
		other.sender = u"me-as-mask"_s;
		QVERIFY(!History::CanAttach(first, other));
	}

	void daySeparators() {
		const auto items = QVector<Data::Message>{
			Text(1, false, At(5, 23, 58)),
			Text(2, false, At(5, 23, 59)),
			Text(3, false, At(6, 0, 1)), // two minutes later, the next day
			Text(4, true, At(8, 9, 0)),
		};
		const auto flags = History::ComputeRowFlags(items, 0);
		QVERIFY(flags[0].showDate);
		QVERIFY(!flags[1].showDate);
		QVERIFY(flags[2].showDate);
		QVERIFY(flags[3].showDate);
		// A new day never continues yesterday's group.
		QVERIFY(flags[1].attachedToPrevious);
		QVERIFY(!flags[1].attachedToNext);
		QVERIFY(!flags[2].attachedToPrevious);
	}

	void serviceBreaksGroups() {
		const auto items = QVector<Data::Message>{
			Text(1, false, At(5, 10, 0)),
			Service(2, At(5, 10, 1)),
			Text(3, false, At(5, 10, 2)),
		};
		const auto flags = History::ComputeRowFlags(items, 0);
		QVERIFY(!flags[0].attachedToNext);
		QVERIFY(!flags[1].attachedToPrevious && !flags[1].attachedToNext);
		QVERIFY(!flags[2].attachedToPrevious);
		QVERIFY(History::IsService(items[1]));
		auto call = Service(4, At(5, 10, 3));
		call.call = Data::CallRecord{ u"missed"_s, 0 };
		QVERIFY(!History::IsService(call));
	}

	void unreadDivider() {
		const auto items = QVector<Data::Message>{
			Text(1, false, At(5, 10, 0)),
			Text(2, true, At(5, 10, 1)),
			Text(3, true, At(5, 10, 2)),
			Text(4, false, At(5, 10, 3)),
			Text(5, false, At(5, 10, 4)),
		};
		// My own message at the boundary is skipped: the divider waits for the
		// first incoming one.
		const auto flags = History::ComputeRowFlags(items, 3);
		QVERIFY(!flags[2].showUnread);
		QVERIFY(flags[3].showUnread);
		QVERIFY(!flags[4].showUnread);
		// The divider splits a group.
		QVERIFY(!flags[3].attachedToPrevious);
		QVERIFY(flags[4].attachedToPrevious);
		// Nothing above it: no divider.
		const auto all = History::ComputeRowFlags(items, 1);
		QVERIFY(!all[0].showUnread);
		const auto none = History::ComputeRowFlags(items, 0);
		for (const auto &flag : none) {
			QVERIFY(!flag.showUnread);
		}
	}

	void diffAppendAndPrepend() {
		const auto was = Ids({ u"seq:3"_s, u"seq:4"_s, u"seq:5"_s });
		auto diff = History::DiffRows(was, Ids({ u"seq:3"_s, u"seq:4"_s, u"seq:5"_s, u"seq:6"_s }));
		QCOMPARE(diff.added, 1);
		QCOMPARE(diff.removed, 0);
		QCOMPARE(diff.addedAtBack, 1);
		QCOMPARE(diff.addedAtFront, 0);
		QCOMPARE(diff.newToOld[3], -1);
		QCOMPARE(diff.newToOld[0], 0);
		QVERIFY(!diff.reordered);

		diff = History::DiffRows(was, Ids({ u"seq:1"_s, u"seq:2"_s, u"seq:3"_s, u"seq:4"_s, u"seq:5"_s }));
		QCOMPARE(diff.added, 2);
		QCOMPARE(diff.addedAtFront, 2);
		QCOMPARE(diff.addedAtBack, 0);
		QCOMPARE(diff.oldToNew[0], 2);
		QCOMPARE(diff.oldToNew[2], 4);

		diff = History::DiffRows(was, was);
		QVERIFY(diff.unchanged());
	}

	void diffRemoveAndReplace() {
		const auto was = Ids({ u"seq:3"_s, u"seq:4"_s, u"seq:5"_s });
		auto diff = History::DiffRows(was, Ids({ u"seq:3"_s, u"seq:5"_s }));
		QCOMPARE(diff.removed, 1);
		QCOMPARE(diff.added, 0);
		QCOMPARE(diff.oldToNew[1], -1);
		QCOMPARE(diff.oldToNew[2], 1);

		// A window around another message: nothing survives.
		diff = History::DiffRows(was, Ids({ u"seq:90"_s, u"seq:91"_s }));
		QCOMPARE(diff.removed, 3);
		QCOMPARE(diff.added, 2);
		QCOMPARE(diff.addedAtBack, 2);
		QCOMPARE(diff.addedAtFront, 0);

		diff = History::DiffRows({}, Ids({ u"seq:1"_s }));
		QCOMPARE(diff.added, 1);
		diff = History::DiffRows(was, {});
		QCOMPARE(diff.removed, 3);
	}

	void diffPendingBecomesSent() {
		auto was = Ids({ u"seq:3"_s });
		was.push_back({ u"local:abc"_s, u"abc"_s });
		was.push_back({ u"local:def"_s, u"def"_s });
		auto now = Ids({ u"seq:3"_s });
		now.push_back({ u"seq:4"_s, u"abc"_s });
		now.push_back({ u"local:def"_s, u"def"_s });
		const auto diff = History::DiffRows(was, now);
		QVERIFY(diff.unchanged());
		QCOMPARE(diff.newToOld[1], 1);
		QCOMPARE(diff.newToOld[2], 2);

		// The peer's message lands before my pending one.
		auto later = Ids({ u"seq:3"_s, u"seq:4"_s });
		later.push_back({ u"seq:5"_s, u"def"_s });
		later.push_back({ u"local:abc"_s, u"abc"_s });
		const auto moved = History::DiffRows(was, later);
		QCOMPARE(moved.added, 1);
		QVERIFY(moved.reordered);
	}

	void sameForView() {
		auto a = Text(1, true, At(5, 10, 0), u"hello"_s);
		auto b = a;
		QVERIFY(History::SameForView(a, b));
		b.status = Data::MessageStatus::Read;
		QVERIFY(!History::SameForView(a, b));
		QVERIFY(History::SameLayout(a, b));
		b = a;
		b.text = u"hello!"_s;
		QVERIFY(!History::SameLayout(a, b));
		b = a;
		b.reactions.insert(U("❤️"), 1);
		QVERIFY(!History::SameLayout(a, b));
		b = a;
		b.uploadProgress = 0.5;
		QVERIFY(!History::SameForView(a, b));
	}

	void jumboEmoji() {
		QCOMPARE(History::JumboEmojiCount(U("😍")), 1);
		QCOMPARE(History::JumboEmojiCount(U("😍🔥")), 2);
		QCOMPARE(History::JumboEmojiCount(U(" 😍 🔥 👍 ")), 3);
		QCOMPARE(History::JumboEmojiCount(U("😍🔥👍🙏")), 0);
		QCOMPARE(History::JumboEmojiCount(U("ok 👍")), 0);
		QCOMPARE(History::JumboEmojiCount(U("سلام")), 0);
		QCOMPARE(History::JumboEmojiCount(QString()), 0);
		QCOMPARE(History::JumboEmojiCount(u"123"_s), 0);
		// Sequences count once: a skin tone, a joined family, a flag, a heart
		// with a variation selector.
		QCOMPARE(History::JumboEmojiCount(U("👍🏽")), 1);
		QCOMPARE(History::JumboEmojiCount(QString::fromUcs4(U"\U0001F468\u200D\U0001F469\u200D\U0001F467")), 1);
		QCOMPARE(History::JumboEmojiCount(U("🇮🇷")), 1);
		QCOMPARE(History::JumboEmojiCount(U("❤️")), 1);
		QCOMPARE(History::JumboEmojiCount(QString::fromUcs4(U"\U0001F64B\u200D\u2642\uFE0F")), 1);
		QCOMPARE(History::JumboEmojiCount(U("❤️🇮🇷👍🏽")), 3);
	}

	void quoteFromSelection() {
		const auto text = U("سلام 😀 دنیا، build شمارهٔ v2 آماده است");
		const auto from = int(text.indexOf(u"build"_s));
		const auto till = int(text.indexOf(u"v2"_s)) + 2;
		auto quote = History::MakeQuote(text, { from, till }, 0);
		QVERIFY(quote.has_value());
		QCOMPARE(quote->offset, from);
		QCOMPARE(quote->text, U("build شمارهٔ v2"));
		QCOMPARE(text.mid(quote->offset, quote->text.size()), quote->text);

		// Whitespace at the edges is not part of the quote.
		quote = History::MakeQuote(text, { from - 1, till + 1 }, 0);
		QCOMPARE(quote->offset, from);
		QCOMPARE(quote->text, U("build شمارهٔ v2"));

		// A selection that starts / ends inside the emoji takes all of it.
		const auto emoji = int(text.indexOf(U("😀")));
		quote = History::MakeQuote(text, { emoji + 1, emoji + 2 }, 0);
		QCOMPARE(quote->offset, emoji);
		QCOMPARE(quote->text, U("😀"));
		quote = History::MakeQuote(text, { 0, emoji + 1 }, 0);
		QCOMPARE(quote->text, U("سلام 😀"));

		// The limit cuts whole characters and leaves no trailing space.
		quote = History::MakeQuote(text, { 0, int(text.size()) }, emoji + 1);
		QCOMPARE(quote->text, U("سلام"));
		QCOMPARE(quote->offset, 0);
		quote = History::MakeQuote(text, { 0, int(text.size()) }, emoji + 2);
		QCOMPARE(quote->text, U("سلام 😀"));

		QVERIFY(!History::MakeQuote(text, { 3, 3 }, 0).has_value());
		QVERIFY(!History::MakeQuote(text, { 4, 5 }, 0).has_value()); // just the space
		QVERIFY(!History::MakeQuote(QString(), { 0, 5 }, 0).has_value());
		// Out of range selections are clamped.
		quote = History::MakeQuote(u"abc"_s, { -4, 99 }, 0);
		QCOMPARE(quote->text, u"abc"_s);
	}

	void findQuote() {
		const auto text = U("یک دو سه دو یک");
		const auto second = int(text.lastIndexOf(U("دو")));
		auto quote = Data::Quote{ U("دو"), second };
		auto found = History::FindQuote(text, quote);
		QVERIFY(found.has_value());
		QCOMPARE(found->from, second);
		// The message was edited: the first occurrence is used.
		quote.offset = 40;
		found = History::FindQuote(text, quote);
		QCOMPARE(found->from, int(text.indexOf(U("دو"))));
		quote.text = U("چهار");
		QVERIFY(!History::FindQuote(text, quote).has_value());
	}

	void copyFormat() {
		const auto time = QDateTime(QDate(2026, 10, 5), QTime(21, 41));
		QCOMPARE(History::FormatForCopy({ { u"Sara"_s, time, u"hello"_s } }), u"hello"_s);
		const auto several = History::FormatForCopy({
			{ u"Sara"_s, time, u"hello"_s },
			{ u"Hadi"_s, time.addSecs(60), u"hi\nthere"_s },
		});
		QCOMPARE(several, u"[Sara, 2026-10-05 21:41]\nhello\n\n[Hadi, 2026-10-05 21:42]\nhi\nthere"_s);
		QVERIFY(History::FormatForCopy({}).isEmpty());
	}

	void search() {
		auto items = QVector<Data::Message>{
			Text(1, false, At(5, 10, 0), U("کتاب‌های تازه رسید")),
			Text(2, true, At(5, 10, 1), U("Hello World")),
			Text(3, false, At(5, 10, 2), U("كتاب عربي")), // Arabic kaf / yeh
			Text(4, false, At(5, 10, 3), U("ساعت ۵ می‌بینمت")),
			Service(5, At(5, 10, 4)),
		};
		items[4].text = U("کتاب");
		auto found = History::SearchMessages(items, U("کتاب"));
		QCOMPARE(int(found.size()), 2);
		QCOMPARE(found[0], 2); // newest first
		QCOMPARE(found[1], 0);
		found = History::SearchMessages(items, u"hello world"_s);
		QCOMPARE(int(found.size()), 1);
		found = History::SearchMessages(items, u"WORLD hello"_s);
		QCOMPARE(int(found.size()), 1);
		found = History::SearchMessages(items, u"5"_s);
		QCOMPARE(int(found.size()), 1);
		QCOMPARE(found[0], 3);
		found = History::SearchMessages(items, U("میبینمت")); // without the ZWNJ
		QCOMPARE(int(found.size()), 1);
		QVERIFY(History::SearchMessages(items, u"  "_s).empty());
		QVERIFY(History::SearchMessages(items, u"nothing"_s).empty());
	}

	void reactions() {
		auto message = Text(1, false, At(5, 10, 0));
		message.reactions.insert(U("👍"), 2);
		message.reactions.insert(U("❤️"), 2);
		message.reactions.insert(U("🔥"), 5);
		message.myReaction = U("👍");
		auto sorted = History::SortedReactions(message);
		QCOMPARE(int(sorted.size()), 3);
		QCOMPARE(sorted[0].emoji, U("🔥"));
		QVERIFY(sorted[1].count == 2 && sorted[2].count == 2);
		QVERIFY(sorted[1].emoji < sorted[2].emoji);

		History::ApplyMyReaction(message, U("🔥")); // switch
		QCOMPARE(message.myReaction, U("🔥"));
		QCOMPARE(message.reactions.value(U("👍")), qint64(1));
		QCOMPARE(message.reactions.value(U("🔥")), qint64(6));
		History::ApplyMyReaction(message, U("🔥")); // the same again removes it
		QVERIFY(message.myReaction.isEmpty());
		QCOMPARE(message.reactions.value(U("🔥")), qint64(5));
		History::ApplyMyReaction(message, U("😮"));
		QCOMPARE(message.reactions.value(U("😮")), qint64(1));
		History::ApplyMyReaction(message, U("😮"));
		QVERIFY(!message.reactions.contains(U("😮")));
	}

	void mediaBox() {
		const auto wide = History::MediaBoxSize(1200, 800, Data::MessageKind::Photo, 360);
		QCOMPARE(wide.width(), 360);
		QCOMPARE(wide.height(), 240);
		const auto tall = History::MediaBoxSize(800, 2400, Data::MessageKind::Photo, 360);
		QVERIFY(tall.height() <= 380);
		QVERIFY(tall.width() >= 120 && tall.width() < 360);
		const auto unknown = History::MediaBoxSize(0, 0, Data::MessageKind::Video, 360);
		QCOMPARE(unknown, QSize(360, 240));
		const auto tiny = History::MediaBoxSize(64, 64, Data::MessageKind::Photo, 360);
		QCOMPARE(tiny, QSize(200, 200));
		const auto note = History::MediaBoxSize(384, 384, Data::MessageKind::VideoNote, 360);
		QCOMPARE(note.width(), note.height());
		const auto narrow = History::MediaBoxSize(1200, 800, Data::MessageKind::Photo, 90);
		QVERIFY(narrow.width() <= 120);
	}

	void waveform() {
		const auto fake = History::WaveformBars(QByteArray(), 40);
		QCOMPARE(int(fake.size()), 40);
		auto bytes = QByteArray();
		for (auto i = 0; i != 80; ++i) {
			bytes.push_back(char(uchar(i < 40 ? 255 : 0)));
		}
		const auto bars = History::WaveformBars(bytes, 4);
		QCOMPARE(int(bars.size()), 4);
		QCOMPARE(bars[0], 1.f);
		QVERIFY(bars[3] > 0.f && bars[3] < 0.2f); // silence still shows a stub
		QVERIFY(History::WaveformBars(bytes, 0).empty());
	}

	void fileTypes() {
		QCOMPARE(History::FileTypeFor(u"plan.PDF"_s, QString()), History::FileType::Document);
		QCOMPARE(History::FileTypeFor(u"shots.zip"_s, QString()), History::FileType::Archive);
		QCOMPARE(History::FileTypeFor(u"noext"_s, u"video/mp4"_s), History::FileType::Video);
		QCOMPARE(History::FileTypeFor(u"main.cpp"_s, QString()), History::FileType::Code);
		QCOMPARE(History::FileTypeFor(u"x.bin"_s, QString()), History::FileType::Generic);
		QCOMPARE(History::FileExtensionLabel(u"plan.PDF"_s), u"pdf"_s);
		QVERIFY(History::FileExtensionLabel(u"archive.tar.backup"_s).isEmpty());
		QVERIFY(History::FileExtensionLabel(u"noext"_s).isEmpty());
	}

	void permissions() {
		const auto now = At(5, 12, 0);
		auto mine = Text(1, true, At(5, 11, 0));
		QVERIFY(History::CanEdit(mine, now, 48 * 3600));
		QVERIFY(!History::CanEdit(mine, now, 60));
		QVERIFY(History::CanEdit(mine, now, 0));
		auto theirs = Text(2, false, At(5, 11, 0));
		QVERIFY(!History::CanEdit(theirs, now, 0));
		auto pending = mine;
		pending.seq = 0;
		QVERIFY(!History::CanEdit(pending, now, 0));
		QVERIFY(!History::CanForward(pending));
		auto sticker = mine;
		sticker.kind = Data::MessageKind::Sticker;
		QVERIFY(!History::CanEdit(sticker, now, 0));
		QVERIFY(History::CanForward(sticker));
		auto forwarded = mine;
		forwarded.forwarded = true;
		QVERIFY(!History::CanEdit(forwarded, now, 0));
		auto photo = mine;
		photo.kind = Data::MessageKind::Photo;
		photo.text = QString();
		QVERIFY(History::CanEdit(photo, now, 0));
		QVERIFY(!History::HasCopyableText(photo));
		QVERIFY(History::HasCopyableText(mine));
		auto file = mine;
		file.kind = Data::MessageKind::File;
		QVERIFY(!History::CanEdit(file, now, 0)); // its text is the file name
		QVERIFY(History::ConversationEnded(u"expired"_s));
		QVERIFY(!History::ConversationEnded(u"active"_s));
	}

	void messageText() {
		auto message = Text(1, false, At(5, 10, 0), U("ببین https://eleza.me و @sara #تهران"));
		message.entities.push_back(Data::Entity{ u"spoiler"_s, 0, 4, QString(), QString() });
		const auto text = History::TextForMessage(message);
		auto spoilers = 0;
		auto links = 0;
		for (const auto &entity : text.entities) {
			if (entity.type == Ui::Text::EntityType::Spoiler) {
				++spoilers;
			} else if (Ui::Text::IsLinkType(entity.type)) {
				++links;
			}
		}
		QCOMPARE(spoilers, 1);
		QCOMPARE(links, 3);
	}
};

QTEST_GUILESS_MAIN(TestHistoryLogic)
#include "test_history_logic.moc"
