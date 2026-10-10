// History:: composer rules: spoiler ranges under edits (UTF-16 offsets,
// surrogate pairs), preparing a text for sending, the draft debounce.
#include "history/history_compose_logic.h"

#include <QtTest/QtTest>

namespace {

[[nodiscard]] QString U(const char *utf8) {
	return QString::fromUtf8(utf8);
}

[[nodiscard]] Data::Entity Spoiler(int offset, int length) {
	auto result = Data::Entity();
	result.type = u"spoiler"_s;
	result.offset = offset;
	result.length = length;
	return result;
}

[[nodiscard]] QString Ranges(const QVector<Data::Entity> &entities) {
	auto parts = QStringList();
	for (const auto &entity : entities) {
		parts.push_back(u"%1+%2"_s.arg(entity.offset).arg(entity.length));
	}
	return parts.join(u' ');
}

// The spoiler's text after an edit: what the user sees marked.
[[nodiscard]] QString Marked(const QString &text, const QVector<Data::Entity> &entities) {
	auto parts = QStringList();
	for (const auto &entity : entities) {
		parts.push_back(text.mid(entity.offset, entity.length));
	}
	return parts.join(u'|');
}

struct FakeTimer {
	qint64 now = 0;
	qint64 fireAt = -1;
	QVector<Data::Draft> saved;
	int cleared = 0;

	[[nodiscard]] History::DraftSaver::Delegate delegate() {
		auto result = History::DraftSaver::Delegate();
		result.save = [this](const Data::Draft &draft) { saved.push_back(draft); };
		result.clear = [this] { ++cleared; };
		result.arm = [this](qint64 delay) { fireAt = now + delay; };
		result.disarm = [this] { fireAt = -1; };
		return result;
	}
	// Moves the clock, firing the timer when its time comes.
	void advance(History::DraftSaver &saver, qint64 to) {
		while (fireAt >= 0 && fireAt <= to) {
			now = fireAt;
			fireAt = -1;
			saver.timeout(now);
		}
		now = to;
	}
};

[[nodiscard]] Data::Draft Draft(const QString &text, qint64 reply = 0) {
	auto result = Data::Draft();
	result.text = text;
	result.replyToSeq = reply;
	return result;
}

} // namespace

class TestHistoryCompose : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void diffTexts() {
		QCOMPARE(History::DiffTexts(u"hello"_s, u"hello"_s), (History::TextEdit{ 5, 0, 0 }));
		QCOMPARE(History::DiffTexts(u"hello"_s, u"hello!"_s), (History::TextEdit{ 5, 0, 1 }));
		QCOMPARE(History::DiffTexts(u"hello"_s, u"hllo"_s), (History::TextEdit{ 1, 1, 0 }));
		QCOMPARE(History::DiffTexts(u"hello world"_s, u"hello brave world"_s), (History::TextEdit{ 6, 0, 6 }));
		QCOMPARE(History::DiffTexts(u"abc"_s, u"xyz"_s), (History::TextEdit{ 0, 3, 3 }));
		QCOMPARE(History::DiffTexts(QString(), u"ab"_s), (History::TextEdit{ 0, 0, 2 }));
		QCOMPARE(History::DiffTexts(u"ab"_s, QString()), (History::TextEdit{ 0, 2, 0 }));
		// Repeated characters: the hint says where the caret was.
		QCOMPARE(History::DiffTexts(u"aaa"_s, u"aaaa"_s), (History::TextEdit{ 3, 0, 1 }));
		QCOMPARE(History::DiffTexts(u"aaa"_s, u"aaaa"_s, 1), (History::TextEdit{ 1, 0, 1 }));
		QCOMPARE(History::DiffTexts(u"aaa"_s, u"aaaa"_s, 0), (History::TextEdit{ 0, 0, 1 }));
		// A hint that does not fit the change is ignored.
		QCOMPARE(History::DiffTexts(u"hello world"_s, u"hello brave world"_s, 0), (History::TextEdit{ 6, 0, 6 }));
		QCOMPARE(History::DiffTexts(u"hello"_s, u"hello!"_s, 99), (History::TextEdit{ 5, 0, 1 }));
	}

	void diffKeepsSurrogatePairsWhole() {
		// U+1F600 and U+1F601 share the high surrogate.
		const auto was = U("a😀b");
		const auto now = U("a😁b");
		const auto edit = History::DiffTexts(was, now);
		QCOMPARE(edit, (History::TextEdit{ 1, 2, 2 }));
		// U+1F600 and U+1F300 share the low surrogate... no: make one that does.
		const auto first = QString(QChar(0xD83D)) + QChar(0xDE00);
		const auto second = QString(QChar(0xD83C)) + QChar(0xDE00);
		QCOMPARE(History::DiffTexts(u"x"_s + first, u"x"_s + second), (History::TextEdit{ 1, 2, 2 }));
	}

	void typingAroundSpoiler() {
		const auto text = u"say secret now"_s;
		const auto spoiler = QVector<Data::Entity>{ Spoiler(4, 6) };
		// Before the range: it shifts.
		auto now = u"I say secret now"_s;
		auto result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Marked(now, result), u"secret"_s);
		// Right before it: not extended.
		now = u"say Xsecret now"_s;
		result = History::AdjustEntities(spoiler, text, now, 4);
		QCOMPARE(Marked(now, result), u"secret"_s);
		// Inside: extended.
		now = u"say secXret now"_s;
		result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Marked(now, result), u"secXret"_s);
		// Right after it: not extended.
		now = u"say secretX now"_s;
		result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Marked(now, result), u"secret"_s);
		// After it: untouched.
		now = u"say secret now!"_s;
		result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Ranges(result), u"4+6"_s);
	}

	void deletingAroundSpoiler() {
		const auto text = u"say secret now"_s;
		const auto spoiler = QVector<Data::Entity>{ Spoiler(4, 6) };
		auto now = u"say sect now"_s; // inside
		QCOMPARE(Marked(now, History::AdjustEntities(spoiler, text, now)), u"sect"_s);
		now = u"sacret now"_s; // across the start: "y se" removed
		QCOMPARE(Marked(now, History::AdjustEntities(spoiler, text, now)), u"cret"_s);
		now = u"say secow"_s; // across the end: "ret n" removed
		QCOMPARE(Marked(now, History::AdjustEntities(spoiler, text, now)), u"sec"_s);
		now = u"say  now"_s; // all of it
		QVERIFY(History::AdjustEntities(spoiler, text, now).isEmpty());
		now = u"sw"_s; // a selection around it replaced
		QVERIFY(History::AdjustEntities(spoiler, text, now).isEmpty());
		now = QString();
		QVERIFY(History::AdjustEntities(spoiler, text, now).isEmpty());
	}

	void replacingSelection() {
		const auto text = u"say secret now"_s;
		const auto spoiler = QVector<Data::Entity>{ Spoiler(4, 6) };
		// "cre" -> "XYZW": inside, the range grows by one.
		auto now = u"say seXYZWt now"_s;
		QCOMPARE(Marked(now, History::AdjustEntities(spoiler, text, now)), u"seXYZWt"_s);
		// "ay sec" -> "Q": the head goes, what was typed is not a spoiler.
		now = u"sQret now"_s;
		QCOMPARE(Marked(now, History::AdjustEntities(spoiler, text, now)), u"ret"_s);
	}

	void surrogatePairs() {
		// Offsets are UTF-16 units: an emoji counts as two.
		const auto text = U("😀 راز 😀 بود");
		const auto word = int(text.indexOf(U("راز")));
		QCOMPARE(word, 3);
		const auto spoiler = QVector<Data::Entity>{ Spoiler(word, 3) };
		// An emoji typed at the very start shifts the range by two.
		auto now = U("🔥😀 راز 😀 بود");
		auto result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Ranges(result), u"5+3"_s);
		QCOMPARE(Marked(now, result), U("راز"));
		// The first emoji removed (Backspace removes both units).
		now = U(" راز 😀 بود");
		result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Ranges(result), u"1+3"_s);
		// An emoji typed inside the spoiler.
		now = U("😀 را🤫ز 😀 بود");
		result = History::AdjustEntities(spoiler, text, now);
		QCOMPARE(Ranges(result), u"3+5"_s);
		QCOMPARE(Marked(now, result), U("را🤫ز"));
		// A spoiler over an emoji, then one emoji replaced by another with the
		// same high surrogate: the range stays on whole characters.
		const auto faces = U("a😀b");
		const auto over = QVector<Data::Entity>{ Spoiler(1, 2) };
		now = U("a😁b");
		result = History::AdjustEntities(over, faces, now);
		for (const auto &entity : result) {
			QVERIFY(!now[entity.offset].isLowSurrogate());
			QVERIFY(!now[entity.offset + entity.length - 1].isHighSurrogate());
		}
	}

	void severalRanges() {
		const auto text = u"aa BB cc DD ee"_s;
		const auto two = QVector<Data::Entity>{ Spoiler(3, 2), Spoiler(9, 2) };
		auto now = u"aa BB cXc DD ee"_s;
		auto result = History::AdjustEntities(two, text, now);
		QCOMPARE(Marked(now, result), u"BB|DD"_s);
		now = u"aa B ee"_s; // "B cc DD" removed
		result = History::AdjustEntities(two, text, now);
		QCOMPARE(Marked(now, result), u"B"_s);
	}

	void normalize() {
		const auto text = U("ab😀cd");
		auto result = History::NormalizeEntities(text, { Spoiler(3, 1), Spoiler(-5, 2), Spoiler(90, 4), Spoiler(5, 40) });
		// -5+2 is outside, 90+4 too; 3+1 sat inside the emoji and takes it whole.
		QCOMPARE(Ranges(result), u"2+2 5+1"_s);
		// Overlapping and touching spoilers merge.
		result = History::NormalizeEntities(u"0123456789"_s, { Spoiler(4, 3), Spoiler(0, 2), Spoiler(2, 1), Spoiler(6, 2) });
		QCOMPARE(Ranges(result), u"0+3 4+4"_s);
		auto bold = Spoiler(0, 4);
		bold.type = u"bold"_s;
		result = History::NormalizeEntities(u"0123456789"_s, { bold, Spoiler(2, 4) });
		QCOMPARE(int(result.size()), 2);
	}

	void toggleSpoiler() {
		const auto text = u"0123456789"_s;
		auto entities = History::ToggleSpoiler(text, {}, 2, 5);
		QCOMPARE(Ranges(entities), u"2+3"_s);
		QVERIFY(History::RangeIsSpoiler(entities, 2, 5));
		QVERIFY(History::RangeIsSpoiler(entities, 3, 4));
		QVERIFY(!History::RangeIsSpoiler(entities, 3, 6));
		QVERIFY(!History::RangeIsSpoiler(entities, 3, 3));
		// A partly marked selection becomes fully marked.
		entities = History::ToggleSpoiler(text, entities, 4, 8);
		QCOMPARE(Ranges(entities), u"2+6"_s);
		// A marked part in the middle is unmarked: the range splits.
		entities = History::ToggleSpoiler(text, entities, 4, 6);
		QCOMPARE(Ranges(entities), u"2+2 6+2"_s);
		// Across two ranges and the gap: not all marked, so it is marked.
		entities = History::ToggleSpoiler(text, entities, 3, 7);
		QCOMPARE(Ranges(entities), u"2+6"_s);
		entities = History::ToggleSpoiler(text, entities, 0, 10);
		QCOMPARE(Ranges(entities), u"0+10"_s);
		entities = History::ToggleSpoiler(text, entities, 0, 10);
		QVERIFY(entities.isEmpty());
		// An empty selection changes nothing.
		QVERIFY(History::ToggleSpoiler(text, {}, 4, 4).isEmpty());
		// Never half an emoji.
		const auto emoji = U("a😀b");
		entities = History::ToggleSpoiler(emoji, {}, 2, 3);
		QCOMPARE(Ranges(entities), u"1+2"_s);
	}

	void prepareForSend() {
		const auto text = u"  \n hello secret  \n"_s;
		const auto word = int(text.indexOf(u"secret"_s));
		auto prepared = History::PrepareForSend(text, { Spoiler(word, 6 + 3) });
		QCOMPARE(prepared.text, u"hello secret"_s);
		QCOMPARE(Ranges(prepared.entities), u"6+6"_s);
		// A spoiler over leading whitespace only disappears.
		prepared = History::PrepareForSend(text, { Spoiler(0, 3) });
		QVERIFY(prepared.entities.isEmpty());
		prepared = History::PrepareForSend(u" \n\t "_s, { Spoiler(0, 2) });
		QVERIFY(prepared.empty());
		prepared = History::PrepareForSend(u"abcdef"_s, { Spoiler(0, 1), Spoiler(2, 1), Spoiler(4, 1) }, 2);
		QCOMPARE(int(prepared.entities.size()), 2);
	}

	void splitForSend() {
		auto prepared = History::PreparedText();
		prepared.text = u"aaaa bbbb cccc dddd"_s;
		prepared.entities = { Spoiler(5, 9) }; // "bbbb cccc"
		auto parts = History::SplitForSend(prepared, 100);
		QCOMPARE(int(parts.size()), 1);
		parts = History::SplitForSend(prepared, 10);
		QCOMPARE(int(parts.size()), 2);
		QCOMPARE(parts[0].text, u"aaaa bbbb"_s);
		QCOMPARE(parts[1].text, u"cccc dddd"_s);
		QCOMPARE(Marked(parts[0].text, parts[0].entities), u"bbbb"_s);
		QCOMPARE(Marked(parts[1].text, parts[1].entities), u"cccc"_s);
		// No space to cut at: whole characters still.
		prepared.text = U("😀😀😀");
		prepared.entities.clear();
		parts = History::SplitForSend(prepared, 3);
		QCOMPARE(int(parts.size()), 3);
		for (const auto &part : parts) {
			QCOMPARE(part.text, U("😀"));
		}
	}

	void linkForPreview() {
		QCOMPARE(History::FirstLinkForPreview(U("ببین eleza.me/docs و https://example.com"), {}), u"https://eleza.me/docs"_s);
		QCOMPARE(History::FirstLinkForPreview(u"see https://example.com/a?b=1."_s, {}), u"https://example.com/a?b=1"_s);
		QVERIFY(History::FirstLinkForPreview(U("بدون پیوند @sara"), {}).isEmpty());
		// Hidden inside a spoiler: no card that gives it away.
		const auto text = u"x https://secret.example.com y https://open.example.com"_s;
		QCOMPARE(
			History::FirstLinkForPreview(text, { Spoiler(0, 30) }),
			u"https://open.example.com"_s);
	}

	void draftWaitsForTheLastKeystroke() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.reset(Data::Draft());
		saver.changed(Draft(u"h"_s), timer.now);
		timer.advance(saver, 500);
		saver.changed(Draft(u"he"_s), timer.now);
		timer.advance(saver, 1500);
		saver.changed(Draft(u"hel"_s), timer.now);
		// Two seconds after the first keystroke: nothing yet.
		timer.advance(saver, 2100);
		QVERIFY(timer.saved.isEmpty());
		timer.advance(saver, 3499);
		QVERIFY(timer.saved.isEmpty());
		// Two seconds after the last one.
		timer.advance(saver, 3500);
		QCOMPARE(int(timer.saved.size()), 1);
		QCOMPARE(timer.saved.back().text, u"hel"_s);
		QVERIFY(!saver.dirty());
		// Nothing changed since: nothing more is saved.
		timer.advance(saver, 60000);
		saver.flush();
		QCOMPARE(int(timer.saved.size()), 1);
	}

	void draftTimerFiredEarly() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.changed(Draft(u"a"_s), 0);
		// A stale timeout (the timer was not re-armed in time).
		saver.timeout(1200);
		QVERIFY(timer.saved.isEmpty());
		QCOMPARE(timer.fireAt, timer.now + 800);
		saver.timeout(2000);
		QCOMPARE(int(timer.saved.size()), 1);
	}

	void draftFlushOnLeave() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.changed(Draft(u"unfinished"_s, 7), timer.now);
		timer.advance(saver, 300);
		saver.flush();
		QCOMPARE(int(timer.saved.size()), 1);
		QCOMPARE(timer.saved.back().replyToSeq, qint64(7));
		QCOMPARE(timer.fireAt, qint64(-1));
		timer.advance(saver, 10000);
		QCOMPARE(int(timer.saved.size()), 1);
	}

	void draftCancelledBySend() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.changed(Draft(u"hello"_s), timer.now);
		timer.advance(saver, 1000);
		saver.sent();
		timer.advance(saver, 10000);
		saver.flush();
		QVERIFY(timer.saved.isEmpty());
		// Nothing was stored, so there is nothing to clear either.
		QCOMPARE(timer.cleared, 0);

		// A stored draft is cleared when its text is sent.
		saver.changed(Draft(u"again"_s), timer.now);
		timer.advance(saver, timer.now + 2000);
		QCOMPARE(int(timer.saved.size()), 1);
		saver.changed(Draft(u"again!"_s), timer.now);
		saver.sent();
		timer.advance(saver, timer.now + 5000);
		QCOMPARE(int(timer.saved.size()), 1);
		QCOMPARE(timer.cleared, 1);
	}

	void draftClearedWhenEmptied() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.reset(Draft(u"old"_s));
		saver.changed(Draft(u"   "_s), timer.now);
		timer.advance(saver, 2000);
		QCOMPARE(timer.cleared, 1);
		QVERIFY(timer.saved.isEmpty());
		// Typed and erased back to the stored text: not dirty.
		saver.reset(Draft(u"old"_s));
		saver.changed(Draft(u"old!"_s), timer.now);
		saver.changed(Draft(u"old"_s), timer.now + 10);
		QVERIFY(!saver.dirty());
		QCOMPARE(timer.fireAt, qint64(-1));
	}

	void draftReplyAndEntitiesCount() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.reset(Draft(u"text"_s));
		saver.changed(Draft(u"text"_s, 5), timer.now);
		QVERIFY(saver.dirty());
		timer.advance(saver, 2000);
		QCOMPARE(timer.saved.back().replyToSeq, qint64(5));
		auto marked = Draft(u"text"_s, 5);
		marked.entities = { Spoiler(0, 2) };
		saver.changed(marked, timer.now);
		QVERIFY(saver.dirty());
	}

	void draftSuspendedWhileEditing() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.changed(Draft(u"draft"_s), timer.now);
		timer.advance(saver, 2000);
		QCOMPARE(int(timer.saved.size()), 1);
		saver.setSuspended(true);
		saver.changed(Draft(u"the edited message"_s), timer.now);
		timer.advance(saver, 9000);
		saver.flush();
		QCOMPARE(int(timer.saved.size()), 1);
		QCOMPARE(saver.stored().text, u"draft"_s);
		saver.setSuspended(false);
	}

	void remoteDraft() {
		auto timer = FakeTimer();
		auto saver = History::DraftSaver(timer.delegate());
		saver.reset(Draft(u"mine"_s));
		const auto remote = std::make_optional(Draft(u"from the phone"_s));
		// The field has the focus: the local text stays.
		QVERIFY(!saver.acceptsRemote(remote, true));
		QVERIFY(saver.acceptsRemote(remote, false));
		QVERIFY(saver.acceptsRemote(std::nullopt, false)); // cleared elsewhere
		QVERIFY(!saver.acceptsRemote(std::make_optional(Draft(u"mine"_s)), false));
		// Unsaved local changes win even without the focus.
		saver.changed(Draft(u"mine, typed"_s), timer.now);
		QVERIFY(!saver.acceptsRemote(remote, false));
		timer.advance(saver, 2000);
		QVERIFY(saver.acceptsRemote(remote, false));
	}
};

QTEST_GUILESS_MAIN(TestHistoryCompose)
#include "test_history_compose.moc"
