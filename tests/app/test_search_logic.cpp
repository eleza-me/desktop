// The widget-free logic of the search tab: recent searches, the field's
// debounce, the follow button's states and the suggestion reason line.
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "search/search_debounce.h"
#include "search/search_people.h"
#include "search/search_recent.h"

#include <QtCore/QJsonArray>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>

using namespace Search;

class TestSearchLogic : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}
	// Names inside the reason line are wrapped into bidi isolates.
	static QString Plain(QString text) {
		return text.remove(QChar(0x2066)).remove(QChar(0x2068)).remove(QChar(0x2069));
	}
	static Data::User Named(const char *id, const char *name) {
		auto user = Data::User();
		user.id = QString::fromLatin1(id);
		user.displayName = QString::fromUtf8(name);
		return user;
	}

private Q_SLOTS:
	void init() {
		Lang::SetCurrent(Lang::Language::Persian);
	}

	// ---- recent searches

	void recentNormalize() {
		QCOMPARE(RecentList::Normalize(QStringLiteral("  sara   ahmadi \n")), QStringLiteral("sara ahmadi"));
		QVERIFY(RecentList::Normalize(QStringLiteral("   ")).isEmpty());
		QVERIFY(RecentList::Normalize(QStringLiteral("@")).isEmpty());
		QVERIFY(RecentList::Normalize(QStringLiteral(" # ")).isEmpty());
		QCOMPARE(RecentList::Normalize(QStringLiteral("@sara")), QStringLiteral("@sara"));
		QCOMPARE(RecentList::Normalize(QString(200, QLatin1Char('a'))).size(), RecentList::kMaxLength);
	}

	void recentAddMovesUp() {
		auto recent = RecentList();
		QVERIFY(recent.empty());
		QVERIFY(recent.add(QStringLiteral("one")));
		QVERIFY(recent.add(QStringLiteral("two")));
		QVERIFY(recent.add(U("تهران")));
		QCOMPARE(recent.list(), (QStringList{ U("تهران"), QStringLiteral("two"), QStringLiteral("one") }));
		// The newest again: nothing to save.
		QVERIFY(!recent.add(U(" تهران ")));
		// An older one moves to the top, without a duplicate.
		QVERIFY(recent.add(QStringLiteral("one")));
		QCOMPARE(recent.list(), (QStringList{ QStringLiteral("one"), U("تهران"), QStringLiteral("two") }));
		// Case does not make a new entry, the newest spelling stays.
		QVERIFY(recent.add(QStringLiteral("TWO")));
		QCOMPARE(recent.list(), (QStringList{ QStringLiteral("TWO"), QStringLiteral("one"), U("تهران") }));
		QVERIFY(recent.add(QStringLiteral("two")));
		QCOMPARE(recent.list().front(), QStringLiteral("two"));
		QCOMPARE(recent.list().size(), 3);
		// Nothing worth remembering.
		QVERIFY(!recent.add(QStringLiteral("  ")));
		QVERIFY(!recent.add(QStringLiteral("#")));
		QCOMPARE(recent.list().size(), 3);
	}

	void recentLimit() {
		auto recent = RecentList(3);
		for (auto i = 0; i != 10; ++i) {
			QVERIFY(recent.add(QString::number(i)));
		}
		QCOMPARE(recent.list(), (QStringList{ QStringLiteral("9"), QStringLiteral("8"), QStringLiteral("7") }));
		auto full = RecentList();
		for (auto i = 0; i != 50; ++i) {
			full.add(QString::number(i));
		}
		QCOMPARE(full.list().size(), RecentList::kLimit);
		QCOMPARE(full.list().front(), QStringLiteral("49"));
	}

	void recentRemoveAndClear() {
		auto recent = RecentList();
		recent.add(QStringLiteral("one"));
		recent.add(QStringLiteral("two"));
		QVERIFY(!recent.remove(QStringLiteral("three")));
		QVERIFY(recent.remove(QStringLiteral(" ONE ")));
		QCOMPARE(recent.list(), QStringList{ QStringLiteral("two") });
		QVERIFY(recent.clear());
		QVERIFY(recent.empty());
		QVERIFY(!recent.clear());
	}

	void recentStorage() {
		auto recent = RecentList();
		recent.add(QStringLiteral("one"));
		recent.add(U("کتاب خوب"));
		recent.add(QStringLiteral("@sara"));
		const auto stored = recent.toJson();
		QVERIFY(stored.isArray());
		QCOMPARE(stored.toArray().size(), 3);

		auto restored = RecentList();
		restored.add(QStringLiteral("stale"));
		restored.fromJson(stored);
		QCOMPARE(restored.list(), recent.list());

		// Garbage in the store: wrong types, blanks, duplicates, too many.
		auto garbage = QJsonArray{ 5, QStringLiteral("  "), QStringLiteral("a"), QStringLiteral("A"), QJsonValue(), QStringLiteral(" b ") };
		for (auto i = 0; i != 60; ++i) {
			garbage.push_back(QStringLiteral("q%1").arg(i));
		}
		restored.fromJson(garbage);
		QCOMPARE(restored.list().size(), RecentList::kLimit);
		QCOMPARE(restored.list().mid(0, 3), (QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("q0") }));
		restored.fromJson(QJsonValue(QStringLiteral("not an array")));
		QVERIFY(restored.empty());
	}

	// ---- debounce

	void debounceWaitsForAPause() {
		auto debounce = Debouncer(60);
		auto fired = QSignalSpy(&debounce, &Debouncer::fired);
		debounce.edited(QStringLiteral("s"));
		QTest::qWait(20);
		debounce.edited(QStringLiteral("sa"));
		QTest::qWait(20);
		debounce.edited(QStringLiteral("sar"));
		QTest::qWait(20);
		debounce.edited(QStringLiteral(" sara "));
		QVERIFY(debounce.pending());
		QCOMPARE(fired.size(), 0);
		QVERIFY(fired.wait(1000));
		QCOMPARE(fired.size(), 1);
		QCOMPARE(fired.front().front().toString(), QStringLiteral("sara"));
		QCOMPARE(debounce.current(), QStringLiteral("sara"));
		QVERIFY(!debounce.pending());
	}

	void debounceEnterFiresNow() {
		auto debounce = Debouncer(10'000);
		auto fired = QSignalSpy(&debounce, &Debouncer::fired);
		debounce.edited(QStringLiteral("tehran"));
		QCOMPARE(fired.size(), 0);
		debounce.flush();
		QCOMPARE(fired.size(), 1);
		QCOMPARE(fired.front().front().toString(), QStringLiteral("tehran"));
		// Enter again: the same query is not sent twice.
		debounce.flush();
		QCOMPARE(fired.size(), 1);
		QVERIFY(!debounce.pending());
	}

	void debounceClearingIsInstant() {
		auto debounce = Debouncer(10'000);
		auto fired = QSignalSpy(&debounce, &Debouncer::fired);
		debounce.set(QStringLiteral("book"));
		QCOMPARE(fired.size(), 1);
		debounce.edited(QStringLiteral("boo"));
		QVERIFY(debounce.pending());
		// Emptied (Esc, the clear button): back to browse at once, and the
		// pending "boo" never goes out.
		debounce.edited(QStringLiteral("   "));
		QCOMPARE(fired.size(), 2);
		QVERIFY(fired.back().front().toString().isEmpty());
		QVERIFY(!debounce.pending());
		// Empty again: nothing.
		debounce.edited(QString());
		QCOMPARE(fired.size(), 2);
	}

	void debounceSameQueryIsDropped() {
		auto debounce = Debouncer(40);
		auto fired = QSignalSpy(&debounce, &Debouncer::fired);
		debounce.set(QStringLiteral("sara"));
		QCOMPARE(fired.size(), 1);
		// Typed a letter and erased it before the pause ended.
		debounce.edited(QStringLiteral("saraa"));
		debounce.edited(QStringLiteral("sara"));
		QVERIFY(!debounce.pending());
		QTest::qWait(120);
		QCOMPARE(fired.size(), 1);
		// Only spaces changed.
		debounce.edited(QStringLiteral("sara "));
		QTest::qWait(120);
		QCOMPARE(fired.size(), 1);
		// set() of the same text does nothing either.
		debounce.set(QStringLiteral(" sara"));
		QCOMPARE(fired.size(), 1);
	}

	void debounceCancel() {
		auto debounce = Debouncer(30);
		auto fired = QSignalSpy(&debounce, &Debouncer::fired);
		debounce.set(QStringLiteral("a"));
		debounce.edited(QStringLiteral("ab"));
		debounce.cancel();
		QTest::qWait(100);
		QCOMPARE(fired.size(), 1);
		QCOMPARE(debounce.current(), QStringLiteral("a"));
		// The very first empty query is still announced once (the initial state).
		auto fresh = Debouncer(30);
		auto freshFired = QSignalSpy(&fresh, &Debouncer::fired);
		fresh.set(QString());
		QCOMPARE(freshFired.size(), 1);
		fresh.set(QString());
		QCOMPARE(freshFired.size(), 1);
	}

	// ---- the follow button

	void followStates() {
		auto profile = Data::Profile();
		QCOMPARE(FollowStateOf(profile), FollowState::None);
		profile.followsYou = true;
		QCOMPARE(FollowStateOf(profile), FollowState::FollowsYou);
		profile.followState = QStringLiteral("requested");
		QCOMPARE(FollowStateOf(profile), FollowState::Requested);
		profile.followState = QStringLiteral("following");
		QCOMPARE(FollowStateOf(profile), FollowState::Following);

		auto graph = Data::GraphState();
		graph.follow = QStringLiteral("none");
		QCOMPARE(FollowStateOf(graph), FollowState::None);
		graph.followsYou = true;
		QCOMPARE(FollowStateOf(graph), FollowState::FollowsYou);
		graph.follow = QStringLiteral("following");
		QCOMPARE(FollowStateOf(graph), FollowState::Following);

		QCOMPARE(FollowText(FollowState::None), tr::lng_follow(tr::now));
		QCOMPARE(FollowText(FollowState::FollowsYou), tr::lng_follow_back(tr::now));
		QCOMPARE(FollowText(FollowState::Requested), tr::lng_follow_requested(tr::now));
		QCOMPARE(FollowText(FollowState::Following), tr::lng_following(tr::now));
	}

	void followTransitions() {
		QVERIFY(ClickFollows(FollowState::None));
		QVERIFY(ClickFollows(FollowState::FollowsYou));
		QVERIFY(!ClickFollows(FollowState::Following));
		QVERIFY(!ClickFollows(FollowState::Requested));

		QCOMPARE(NextFollowState(FollowState::None, false, false), FollowState::Following);
		QCOMPARE(NextFollowState(FollowState::FollowsYou, false, true), FollowState::Following);
		// A private account answers with a request.
		QCOMPARE(NextFollowState(FollowState::None, true, false), FollowState::Requested);
		// Unfollowing / withdrawing returns to where it was.
		QCOMPARE(NextFollowState(FollowState::Following, false, false), FollowState::None);
		QCOMPARE(NextFollowState(FollowState::Following, false, true), FollowState::FollowsYou);
		QCOMPARE(NextFollowState(FollowState::Requested, true, false), FollowState::None);

		auto profile = Data::Profile();
		for (const auto state : { FollowState::Following, FollowState::Requested, FollowState::FollowsYou, FollowState::None }) {
			ApplyFollowState(profile, state);
			QCOMPARE(FollowStateOf(profile), state);
		}
	}

	// ---- «آدم‌هایی که شاید بشناسی»

	void suggestionReasons() {
		auto profile = Data::Profile();
		profile.handle = QStringLiteral("nika");
		profile.reason = QStringLiteral("mutual");
		profile.mutuals = { Named("a", "سارا"), Named("b", "علی") };
		profile.mutualCount = 3;
		QCOMPARE(Plain(SuggestionReason(profile)), U("سارا و ۲ نفر دیگر دنبالش می‌کنند"));
		profile.mutualCount = 2;
		QCOMPARE(Plain(SuggestionReason(profile)), U("سارا و علی دنبالش می‌کنند"));
		profile.mutuals = { Named("a", "سارا") };
		profile.mutualCount = 5;
		QCOMPARE(Plain(SuggestionReason(profile)), U("سارا و ۴ نفر دیگر دنبالش می‌کنند"));
		profile.mutualCount = 1;
		QCOMPARE(Plain(SuggestionReason(profile)), U("سارا دنبالش می‌کند"));
		profile.mutuals.clear();
		profile.mutualCount = 12;
		QCOMPARE(SuggestionReason(profile), U("۱۲ نفر از کسانی که دنبال می‌کنی دنبالش می‌کنند"));
		profile.mutualCount = 0;
		QCOMPARE(SuggestionReason(profile), tr::lng_search_suggested(tr::now));

		profile.reason = QStringLiteral("contact");
		QCOMPARE(SuggestionReason(profile), U("در مخاطبان تو"));
		profile.reason = QStringLiteral("follows_you");
		QCOMPARE(SuggestionReason(profile), U("تو را دنبال می‌کند"));
		profile.reason = QStringLiteral("interest");
		QCOMPARE(SuggestionReason(profile), U("هم‌علاقه با تو"));
		profile.reason = QStringLiteral("province");
		QCOMPARE(SuggestionReason(profile), U("از استان تو"));
		profile.reason = QStringLiteral("popular");
		QCOMPARE(SuggestionReason(profile), tr::lng_search_suggested(tr::now));
		profile.reason = QStringLiteral("active");
		QCOMPARE(SuggestionReason(profile), tr::lng_search_suggested(tr::now));
		// No reason / a reason we have no words for: the handle.
		profile.reason = QString();
		QCOMPARE(Plain(SuggestionReason(profile)), QStringLiteral("@nika"));
		profile.reason = QStringLiteral("something_new");
		QCOMPARE(Plain(SuggestionReason(profile)), QStringLiteral("@nika"));
		// The handle is always left-to-right.
		QCOMPARE(HandleText(QStringLiteral("nika")), QChar(0x2066) + QStringLiteral("@nika") + QChar(0x2069));
		QVERIFY(HandleText(QString()).isEmpty());
		profile.handle = QString();
		QVERIFY(SuggestionReason(profile).isEmpty());

		// The name is isolated from the sentence around it.
		profile.reason = QStringLiteral("mutual");
		profile.handle = QStringLiteral("nika");
		profile.mutuals = { Named("a", "Sara") };
		profile.mutualCount = 1;
		QVERIFY(SuggestionReason(profile).startsWith(QChar(0x2068) + QStringLiteral("Sara") + QChar(0x2069)));

		Lang::SetCurrent(Lang::Language::English);
		profile.reason = QStringLiteral("mutual");
		profile.mutuals = { Named("a", "Sara") };
		profile.mutualCount = 2;
		QCOMPARE(Plain(SuggestionReason(profile)), QStringLiteral("Followed by Sara and 1 other"));
	}

	void newSuggestionReasons() {
		auto profile = Data::Profile();
		profile.handle = QStringLiteral("nika");
		profile.reason = QStringLiteral("groups");
		profile.groupCount = 3;
		QCOMPARE(SuggestionReason(profile), U("۳ گروه مشترک"));
		// An older server: no count, the generic line.
		profile.groupCount = 0;
		QCOMPARE(SuggestionReason(profile), tr::lng_search_suggested(tr::now));
		profile.reason = QStringLiteral("new");
		QCOMPARE(SuggestionReason(profile), U("تازه به الزا پیوسته"));

		// Somebody I know who just joined: the tie and that they are new.
		profile.joinedRecently = true;
		profile.reason = QStringLiteral("contact");
		QCOMPARE(SuggestionReason(profile), U("در مخاطبان تو · تازه پیوسته"));
		profile.reason = QStringLiteral("groups");
		profile.groupCount = 1;
		QCOMPARE(SuggestionReason(profile), U("۱ گروه مشترک · تازه پیوسته"));
		// Not for a tie that is no personal one, nor for the generic line.
		profile.reason = QStringLiteral("interest");
		QCOMPARE(SuggestionReason(profile), U("هم‌علاقه با تو"));
		profile.reason = QStringLiteral("new");
		QCOMPARE(SuggestionReason(profile), U("تازه به الزا پیوسته"));
		profile.reason = QStringLiteral("groups");
		profile.groupCount = 0;
		QCOMPARE(SuggestionReason(profile), tr::lng_search_suggested(tr::now));

		Lang::SetCurrent(Lang::Language::English);
		profile.groupCount = 2;
		QCOMPARE(SuggestionReason(profile), QStringLiteral("2 groups in common · Just joined"));
		profile.joinedRecently = false;
		profile.groupCount = 1;
		QCOMPARE(SuggestionReason(profile), QStringLiteral("1 group in common"));
	}

	void suggestionQueue() {
		const auto person = [](const char *id) {
			auto result = Data::Profile();
			result.id = QString::fromLatin1(id);
			return result;
		};
		const auto ids = [](const QVector<Data::Profile> &list) {
			auto result = QStringList();
			for (const auto &item : list) {
				result.push_back(item.id);
			}
			return result.join(' ');
		};
		auto queue = SuggestionQueue(3, 2);
		QVERIFY(queue.empty());
		QVERIFY(queue.needsRefill());
		// Me, somebody hidden here and a repeat are left out.
		queue.replace(
			{ person("a"), person("me"), person("b"), person("x"), person("c"), person("a"), person("d"), person("e") },
			{ QStringLiteral("x") },
			QStringLiteral("me"));
		QCOMPARE(ids(queue.visible()), QStringLiteral("a b c"));
		QCOMPARE(queue.waiting(), 2);
		QVERIFY(!queue.needsRefill());

		// A followed person leaves, the next one takes the place.
		QVERIFY(queue.remove(QStringLiteral("b")));
		QCOMPARE(ids(queue.visible()), QStringLiteral("a c d"));
		QCOMPARE(queue.waiting(), 1);
		QVERIFY(queue.needsRefill());
		QVERIFY(!queue.remove(QStringLiteral("zz")));

		// More from the server: only the new ones, at the end; the rows on
		// screen keep their places, the handled never come back.
		QCOMPARE(queue.refill({ person("b"), person("e"), person("f"), person("g") }, {}, QStringLiteral("me")), 2);
		QCOMPARE(ids(queue.people()), QStringLiteral("a c d e f g"));
		QCOMPARE(queue.refill({}, {}, QStringLiteral("me")), 0);

		// A fresh first page replaces the line, still without the handled.
		queue.replace({ person("b"), person("z"), person("a") }, {}, QStringLiteral("me"));
		QCOMPARE(ids(queue.people()), QStringLiteral("z a"));
		// Unless the follow was taken back.
		queue.forget(QStringLiteral("b"));
		QVERIFY(!queue.handled(QStringLiteral("b")));
		QCOMPARE(queue.refill({ person("b") }, {}, QStringLiteral("me")), 1);
		QVERIFY(queue.find(QStringLiteral("b")) != nullptr);
		QVERIFY(queue.find(QStringLiteral("q")) == nullptr);

		QVERIFY(LeavesSuggestions(FollowState::Following));
		QVERIFY(LeavesSuggestions(FollowState::Requested));
		QVERIFY(!LeavesSuggestions(FollowState::None));
		QVERIFY(!LeavesSuggestions(FollowState::FollowsYou));
	}

	void refreshThrottle() {
		auto throttle = RefreshThrottle(600'000);
		// Never loaded (or only failed so far): due.
		QVERIFY(throttle.due(1'000));
		throttle.loaded(1'000);
		QVERIFY(!throttle.due(1'000));
		QVERIFY(!throttle.due(600'999));
		QVERIFY(throttle.due(601'000));
		// The clock went back (a changed date): due.
		QVERIFY(throttle.due(500));
		QCOMPARE(RefreshThrottle::kSuggestionsMs, qint64(600'000));
	}

};

QTEST_GUILESS_MAIN(TestSearchLogic)
#include "test_search_logic.moc"
