// The widget-free logic of the profile screens: relationship states and the
// buttons they give, the follow toggle, handle validation and its debounced
// check, bio parsing, links, the birthday picker ranges and the QR encoder.
#include "lang/lang_format.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "profile/profile_logic.h"
#include "profile/profile_qr.h"

#include <QtTest/QtTest>

using namespace Profile;

class TestProfileLogic : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}
	static ViewerState Other() {
		auto result = ViewerState();
		result.loaded = true;
		return result;
	}
	static Data::Birthday Date(const char *calendar, int year, int month, int day) {
		auto result = Data::Birthday();
		result.calendar = QString::fromLatin1(calendar);
		result.year = year;
		result.month = month;
		result.day = day;
		return result;
	}

private Q_SLOTS:
	void init() {
		Lang::SetCurrent(Lang::Language::Persian);
	}

	// ---- relationship -> follow button

	void followStates() {
		QCOMPARE(FollowUiOf(QStringLiteral("following"), false), FollowUi::Following);
		QCOMPARE(FollowUiOf(QStringLiteral("following"), true), FollowUi::Following);
		QCOMPARE(FollowUiOf(QStringLiteral("requested"), true), FollowUi::Requested);
		QCOMPARE(FollowUiOf(QStringLiteral("none"), true), FollowUi::FollowsYou);
		QCOMPARE(FollowUiOf(QStringLiteral("none"), false), FollowUi::None);
		QCOMPARE(FollowUiOf(QString(), false), FollowUi::None);

		auto state = Data::GraphState();
		state.follow = QStringLiteral("requested");
		QCOMPARE(FollowUiOf(state), FollowUi::Requested);

		QCOMPARE(FollowLookOf(FollowUi::None), FollowLook::Filled);
		QCOMPARE(FollowLookOf(FollowUi::FollowsYou), FollowLook::Filled);
		QCOMPARE(FollowLookOf(FollowUi::Following), FollowLook::Outline);
		QCOMPARE(FollowLookOf(FollowUi::Requested), FollowLook::Soft);
	}

	void followToggle() {
		// A public account: following at once.
		auto toggle = ToggleFollow(FollowUi::None, false, false);
		QVERIFY(toggle.follow);
		QVERIFY(!toggle.confirm);
		QCOMPARE(toggle.optimistic, FollowUi::Following);
		// A private account: a request.
		toggle = ToggleFollow(FollowUi::FollowsYou, true, true);
		QVERIFY(toggle.follow);
		QCOMPARE(toggle.optimistic, FollowUi::Requested);
		// Unfollowing asks first and returns to what the other side does.
		toggle = ToggleFollow(FollowUi::Following, false, true);
		QVERIFY(!toggle.follow);
		QVERIFY(toggle.confirm);
		QCOMPARE(toggle.optimistic, FollowUi::FollowsYou);
		toggle = ToggleFollow(FollowUi::Following, false, false);
		QCOMPARE(toggle.optimistic, FollowUi::None);
		// Withdrawing a request does not ask.
		toggle = ToggleFollow(FollowUi::Requested, true, false);
		QVERIFY(!toggle.follow);
		QVERIFY(!toggle.confirm);
		QCOMPARE(toggle.optimistic, FollowUi::None);

		QCOMPARE(FollowersDelta(FollowUi::None, FollowUi::Following), 1);
		QCOMPARE(FollowersDelta(FollowUi::Following, FollowUi::FollowsYou), -1);
		QCOMPARE(FollowersDelta(FollowUi::None, FollowUi::Requested), 0);
		QCOMPARE(FollowersDelta(FollowUi::Following, FollowUi::Following), 0);
	}

	// ---- relationship -> header model

	void modelNotLoaded() {
		const auto model = MakeButtonModel(ViewerState());
		QVERIFY(model.actions.isEmpty());
		QCOMPARE(model.content, Content::None);
	}

	void modelMe() {
		auto state = Other();
		state.me = true;
		state.isPrivate = true; // my own private account is never locked for me
		const auto model = MakeButtonModel(state);
		QCOMPARE(model.actions, (QVector<Action>{ Action::EditProfile, Action::ShareProfile }));
		QVERIFY(model.menu.isEmpty());
		QCOMPARE(model.content, Content::Tabs);
		QVERIFY(model.bookmarksTab);
		QVERIFY(model.statsClickable);
		QVERIFY(!model.conversationCard);
		QVERIFY(!model.presence);
	}

	void modelPublic() {
		auto state = Other();
		state.follow = FollowUi::FollowsYou;
		state.followsYou = true;
		const auto model = MakeButtonModel(state);
		QCOMPARE(model.actions, (QVector<Action>{ Action::Follow, Action::Message, Action::More }));
		QCOMPARE(model.content, Content::Tabs);
		QVERIFY(!model.bookmarksTab);
		QVERIFY(model.statsClickable);
		QVERIFY(model.presence);
		QVERIFY(model.conversationCard);
		QCOMPARE(model.menu, (QVector<MenuEntry>{
			MenuEntry::PostNotificationsOn,
			MenuEntry::ShareProfile,
			MenuEntry::CopyLink,
			MenuEntry::Mute,
			MenuEntry::RemoveFollower,
			MenuEntry::Report,
			MenuEntry::Block,
		}));
	}

	void modelMenuToggles() {
		auto state = Other();
		state.muted = true;
		state.postNotifications = true;
		state.hasHandle = false;
		const auto model = MakeButtonModel(state);
		QVERIFY(model.has(MenuEntry::Unmute));
		QVERIFY(!model.has(MenuEntry::Mute));
		QVERIFY(model.has(MenuEntry::PostNotificationsOff));
		QVERIFY(!model.has(MenuEntry::ShareProfile));
		QVERIFY(!model.has(MenuEntry::CopyLink));
		QVERIFY(!model.has(MenuEntry::RemoveFollower));
	}

	void modelPrivate() {
		auto state = Other();
		state.isPrivate = true;
		auto model = MakeButtonModel(state);
		QCOMPARE(model.content, Content::Locked);
		QVERIFY(model.has(Action::Follow));
		QVERIFY(!model.statsClickable);
		QVERIFY(!model.presence);

		state.follow = FollowUi::Requested;
		model = MakeButtonModel(state);
		QCOMPARE(model.content, Content::LockedRequested);

		state.follow = FollowUi::Following;
		model = MakeButtonModel(state);
		QCOMPARE(model.content, Content::Tabs);
		QVERIFY(model.statsClickable);
	}

	void modelBlocked() {
		auto state = Other();
		state.blocked = true;
		state.follow = FollowUi::Following;
		auto model = MakeButtonModel(state);
		QCOMPARE(model.actions, (QVector<Action>{ Action::More }));
		QCOMPARE(model.menu, (QVector<MenuEntry>{ MenuEntry::Report, MenuEntry::Unblock }));
		QCOMPARE(model.content, Content::Blocked);
		QVERIFY(!model.statsClickable);
		QVERIFY(!model.conversationCard);

		state.blocked = false;
		state.justUnblocked = true;
		state.follow = FollowUi::None;
		model = MakeButtonModel(state);
		QCOMPARE(model.content, Content::Unblocked);
		QVERIFY(model.has(Action::Follow));
		QVERIFY(model.has(MenuEntry::Block));
	}

	void modelDeleted() {
		auto state = Other();
		state.deleted = true;
		const auto model = MakeButtonModel(state);
		QVERIFY(model.actions.isEmpty());
		QCOMPARE(model.content, Content::Deleted);
	}

	// ---- handle

	void handleNormalize() {
		QCOMPARE(NormalizeHandle(QStringLiteral("  @Sara.K ")), QStringLiteral("sara.k"));
		QCOMPARE(NormalizeHandle(QStringLiteral("@@ali")), QStringLiteral("ali"));
		QCOMPARE(NormalizeHandle(U("کاربر۱۲")), U("کاربر12"));
		QCOMPARE(NormalizeHandle(QString()), QString());
	}

	void handleClassify() {
		const auto current = QStringLiteral("hadi");
		QCOMPARE(ClassifyHandle(QStringLiteral("hadi"), current), HandleState::Idle);
		QCOMPARE(ClassifyHandle(QStringLiteral("ab"), current), HandleState::Invalid);
		QCOMPARE(ClassifyHandle(QStringLiteral("a b c"), current), HandleState::Invalid);
		QCOMPARE(ClassifyHandle(QStringLiteral("Hadi2"), current), HandleState::Invalid);
		QCOMPARE(ClassifyHandle(U("هادی"), current), HandleState::Invalid);
		QCOMPARE(ClassifyHandle(QString(25, QLatin1Char('a')), current), HandleState::Invalid);
		QCOMPARE(ClassifyHandle(QString(24, QLatin1Char('a')), current), HandleState::Checking);
		QCOMPARE(ClassifyHandle(QStringLiteral("a1_2"), current), HandleState::Short);
		QCOMPARE(ClassifyHandle(QStringLiteral("12345"), current), HandleState::Short);
		QCOMPARE(ClassifyHandle(QStringLiteral("abc"), current), HandleState::Checking);
		QCOMPARE(ClassifyHandle(QStringLiteral("a.b_c9"), current), HandleState::Checking);

		QVERIFY(HandleBlocksSave(HandleState::Invalid));
		QVERIFY(HandleBlocksSave(HandleState::Short));
		QVERIFY(HandleBlocksSave(HandleState::Taken));
		QVERIFY(HandleBlocksSave(HandleState::Checking));
		QVERIFY(!HandleBlocksSave(HandleState::Idle));
		QVERIFY(!HandleBlocksSave(HandleState::Free));
		QVERIFY(!HandleBlocksSave(HandleState::Unknown));
		QVERIFY(HandleIsError(HandleState::Taken));
		QVERIFY(!HandleIsError(HandleState::Checking));
	}

	void handleDebounce() {
		auto asked = QStringList();
		auto answer = HandleChecker::Answer::Free;
		auto changes = 0;
		HandleChecker checker(nullptr, 30);
		checker.setCurrent(QStringLiteral("hadi"));
		checker.setRequest([&](const QString &handle, Fn<void(HandleChecker::Answer)> done) {
			asked.push_back(handle);
			done(answer);
		});
		checker.setChanged([&] { ++changes; });

		// Typing: only the last value is asked about, after the pause.
		checker.setValue(QStringLiteral("had"));
		QCOMPARE(checker.state(), HandleState::Checking);
		checker.setValue(QStringLiteral("hadi"));
		QCOMPARE(checker.state(), HandleState::Idle);
		checker.setValue(QStringLiteral("hadi2"));
		checker.setValue(QStringLiteral("hadi20"));
		QCOMPARE(checker.state(), HandleState::Checking);
		QVERIFY(asked.isEmpty());
		QTRY_COMPARE_WITH_TIMEOUT(checker.state(), HandleState::Free, 1000);
		QCOMPARE(asked, QStringList{ QStringLiteral("hadi20") });
		QVERIFY(changes >= 3);

		// Invalid input never reaches the server.
		checker.setValue(QStringLiteral("x"));
		QCOMPARE(checker.state(), HandleState::Invalid);
		QTest::qWait(80);
		QCOMPARE(asked.size(), 1);

		answer = HandleChecker::Answer::Taken;
		checker.setValue(QStringLiteral("sara"));
		QTRY_COMPARE_WITH_TIMEOUT(checker.state(), HandleState::Taken, 1000);

		answer = HandleChecker::Answer::Failed;
		checker.setValue(QStringLiteral("nika"));
		QTRY_COMPARE_WITH_TIMEOUT(checker.state(), HandleState::Unknown, 1000);

		// Saving said "taken" for a value the check had let through.
		checker.markTaken();
		QCOMPARE(checker.state(), HandleState::Taken);
		// Back to my own handle: nothing to check.
		checker.setValue(QStringLiteral("hadi"));
		QCOMPARE(checker.state(), HandleState::Idle);
	}

	void handleLateAnswer() {
		// An answer that arrives after the value changed is dropped.
		auto pending = QVector<Fn<void(HandleChecker::Answer)>>();
		HandleChecker checker(nullptr, 10);
		checker.setCurrent(QStringLiteral("hadi"));
		checker.setRequest([&](const QString &, Fn<void(HandleChecker::Answer)> done) {
			pending.push_back(std::move(done));
		});
		checker.setValue(QStringLiteral("first"));
		QTRY_COMPARE_WITH_TIMEOUT(pending.size(), 1, 1000);
		checker.setValue(QStringLiteral("second"));
		pending[0](HandleChecker::Answer::Taken);
		QCOMPARE(checker.state(), HandleState::Checking);
		QTRY_COMPARE_WITH_TIMEOUT(pending.size(), 2, 1000);
		pending[1](HandleChecker::Answer::Free);
		QCOMPARE(checker.state(), HandleState::Free);
	}

	// ---- bio and links

	void bioParsing() {
		using Ui::Text::EntityType;
		const auto bio = U("سازندهٔ الزا ✨ @sara و @ali.r در #تهران — https://eleza.me/u/hadi.");
		const auto parsed = ParseBio(bio);
		// Only direction marks are added: one in front of every "@" / "#".
		auto plain = parsed.text;
		plain.remove(QChar(0x200E));
		plain.remove(QChar(0x200F));
		QCOMPARE(plain, bio);
		QCOMPARE(parsed.text.size(), bio.size() + 3);
		QCOMPARE(parsed.text.count(QChar(0x200E)), 2); // the Latin handles
		QCOMPARE(parsed.text.count(QChar(0x200F)), 1); // the Persian tag
		auto mentions = QStringList();
		auto tags = QStringList();
		auto links = QStringList();
		for (const auto &entity : parsed.entities) {
			const auto part = parsed.text.mid(entity.offset, entity.length);
			if (entity.type == EntityType::Mention || entity.type == EntityType::Hashtag) {
				QVERIFY(entity.offset > 0);
				const auto before = parsed.text[entity.offset - 1].unicode();
				QVERIFY(before == 0x200E || before == 0x200F);
			}
			switch (entity.type) {
			case EntityType::Mention: mentions.push_back(part); break;
			case EntityType::Hashtag: tags.push_back(part); break;
			case EntityType::Url: links.push_back(part); break;
			default: break;
			}
		}
		// Handles keep their dots; a sentence's full stop is not part of them.
		QCOMPARE(mentions, (QStringList{ QStringLiteral("@sara"), QStringLiteral("@ali.r") }));
		QCOMPARE(tags, QStringList{ U("#تهران") });
		QCOMPARE(links.size(), 1);
		QVERIFY(links[0].startsWith(QStringLiteral("https://eleza.me/u/hadi")));

		const auto ending = ParseBio(QStringLiteral("ask @reza.k."));
		QCOMPARE(ending.entities.size(), 1);
		QCOMPARE(
			ending.text.mid(ending.entities[0].offset, ending.entities[0].length),
			QStringLiteral("@reza.k"));
		QVERIFY(ParseBio(QString()).entities.isEmpty());
		QVERIFY(ParseBio(QStringLiteral("no links here")).entities.isEmpty());
	}

	void links() {
		QCOMPARE(NormalizeLink(QStringLiteral(" sara.photo ")), QStringLiteral("https://sara.photo"));
		QCOMPARE(NormalizeLink(QStringLiteral("http://a.b/c")), QStringLiteral("http://a.b/c"));
		QCOMPARE(NormalizeLink(QStringLiteral("HTTPS://A.B")), QStringLiteral("HTTPS://A.B"));
		QCOMPARE(NormalizeLink(QStringLiteral("  ")), QString());

		QVERIFY(IsValidLink(QString()));
		QVERIFY(IsValidLink(QStringLiteral("example.com")));
		QVERIFY(IsValidLink(QStringLiteral("https://example.com/path?x=1")));
		QVERIFY(!IsValidLink(QStringLiteral("not a link")));
		QVERIFY(!IsValidLink(QStringLiteral("localhost")));
		QVERIFY(!IsValidLink(QStringLiteral("ftp://example.com")));
		QVERIFY(!IsValidLink(QStringLiteral("javascript://x.y")));

		QCOMPARE(DisplayLink(QStringLiteral("https://mina.example.org/")), QStringLiteral("mina.example.org"));
		QCOMPARE(DisplayLink(QStringLiteral("sara.photo")), QStringLiteral("sara.photo"));
		QCOMPARE(ProfileLink(QStringLiteral("ali.r")), QStringLiteral("https://eleza.me/u/ali.r"));
	}

	void texts() {
		QCOMPARE(MutualsText({}, 0), QString());
		QCOMPARE(
			MutualsText({ U("سارا") }, 1),
			tr::lng_profile_mutuals_one(tr::now, lt_a, U("سارا")));
		QCOMPARE(
			MutualsText({ U("سارا"), U("علی") }, 2),
			tr::lng_profile_mutuals_two(tr::now, lt_a, U("سارا"), lt_b, U("علی")));
		QCOMPARE(
			MutualsText({ U("سارا"), U("علی"), U("نیکا") }, 12),
			tr::lng_profile_mutuals_more(tr::now, lt_a, U("سارا"), lt_b, U("علی"), lt_n, U("۱۰")));

		QCOMPARE(ProvinceTitle(QStringLiteral("IR-07")), U("تهران"));
		QCOMPARE(ProvinceTitle(QStringLiteral("IR-31")), U("البرز"));
		QCOMPARE(ProvinceTitle(QStringLiteral("IR-32")), QStringLiteral("IR-32"));
		QCOMPARE(ProvinceTitle(QStringLiteral("XX")), QStringLiteral("XX"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(ProvinceTitle(QStringLiteral("IR-07")), QStringLiteral("Tehran"));
		Lang::SetCurrent(Lang::Language::Persian);

		const auto now = qint64(1'700'000'000'000);
		QCOMPARE(RemainingText(now / 1000 - 5, now), tr::lng_ended(tr::now));
		QCOMPARE(
			RemainingText(now / 1000 + 50 * 60, now),
			tr::lng_conv_minutes_left(tr::now, lt_count, 50.));
		QCOMPARE(
			RemainingText(now / 1000 + 5 * 3600 + 60, now),
			tr::lng_conv_hours_left(tr::now, lt_count, 5.));
		QCOMPARE(
			RemainingText(now / 1000 + 3 * 86400, now),
			tr::lng_conv_days_left(tr::now, lt_count, 3.));

		QCOMPARE(ReportReasonTitle(QStringLiteral("spam")), U("هرزنامه"));
		QCOMPARE(ReportReasonTitle(QStringLiteral("made_up")), QStringLiteral("made_up"));
		QCOMPARE(LtrText(QStringLiteral("@a")).size(), 4);
	}

	// ---- the birthday picker

	void jalaliMonthLengths() {
		const auto jalali = QStringLiteral("jalali");
		for (auto month = 1; month <= 6; ++month) {
			QCOMPARE(MonthDays(jalali, 1400, month), 31);
		}
		for (auto month = 7; month <= 11; ++month) {
			QCOMPARE(MonthDays(jalali, 1400, month), 30);
		}
		// 1399 and 1403 are leap years of the Solar Hijri calendar; 1400–1402 are not.
		QCOMPARE(MonthDays(jalali, 1399, 12), 30);
		QCOMPARE(MonthDays(jalali, 1400, 12), 29);
		QCOMPARE(MonthDays(jalali, 1401, 12), 29);
		QCOMPARE(MonthDays(jalali, 1402, 12), 29);
		QCOMPARE(MonthDays(jalali, 1403, 12), 30);
		QCOMPARE(MonthDays(jalali, 1375, 12), 30);
		// Without a year the longest form of the month is offered.
		QCOMPARE(MonthDays(jalali, 0, 12), 30);
		QCOMPARE(MonthDays(jalali, 1400, 0), 0);
		QCOMPARE(MonthDays(jalali, 1400, 13), 0);
		// Every month agrees with the calendar conversion.
		for (auto year = 1290; year <= 1410; ++year) {
			for (auto month = 1; month <= 12; ++month) {
				const auto days = MonthDays(jalali, year, month);
				QVERIFY2(
					Lang::JalaliToGregorian(year, month, days).isValid(),
					qPrintable(QStringLiteral("%1/%2").arg(year).arg(month)));
				QVERIFY2(
					!Lang::JalaliToGregorian(year, month, days + 1).isValid(),
					qPrintable(QStringLiteral("%1/%2").arg(year).arg(month)));
			}
		}
	}

	void gregorianMonthLengths() {
		const auto gregorian = QStringLiteral("gregorian");
		QCOMPARE(MonthDays(gregorian, 2023, 2), 28);
		QCOMPARE(MonthDays(gregorian, 2024, 2), 29);
		QCOMPARE(MonthDays(gregorian, 1900, 2), 28);
		QCOMPARE(MonthDays(gregorian, 2000, 2), 29);
		QCOMPARE(MonthDays(gregorian, 0, 2), 29);
		QCOMPARE(MonthDays(gregorian, 2023, 4), 30);
		QCOMPARE(MonthDays(gregorian, 2023, 12), 31);
	}

	void birthYears() {
		const auto today = QDate(2026, 10, 10); // 18 Mehr 1405
		const auto jalali = BirthYears(QStringLiteral("jalali"), today);
		QCOMPARE(jalali.from, 1405 - kBirthdayMinAge);
		QCOMPARE(jalali.till, 1405 - kBirthdayMaxAge);
		QCOMPARE(jalali.count(), kBirthdayMaxAge - kBirthdayMinAge + 1);
		const auto gregorian = BirthYears(QStringLiteral("gregorian"), today);
		QCOMPARE(gregorian.from, 2026 - kBirthdayMinAge);
		QCOMPARE(gregorian.till, 2026 - kBirthdayMaxAge);
	}

	void birthdayValidity() {
		const auto today = QDate(2026, 10, 10);
		QVERIFY(IsValidBirthday(Date("jalali", 0, 12, 30), today));
		QVERIFY(IsValidBirthday(Date("jalali", 1375, 12, 30), today));
		QVERIFY(!IsValidBirthday(Date("jalali", 1376, 12, 30), today));
		QVERIFY(!IsValidBirthday(Date("jalali", 1370, 7, 31), today));
		QVERIFY(!IsValidBirthday(Date("jalali", 1370, 0, 1), today));
		QVERIFY(!IsValidBirthday(Date("jalali", 1370, 1, 0), today));
		QVERIFY(!IsValidBirthday(Date("jalali", 1400, 1, 1), today)); // too young
		QVERIFY(!IsValidBirthday(Date("jalali", 1200, 1, 1), today)); // too old
		QVERIFY(!IsValidBirthday(Date("jalali", -5, 1, 1), today));
		QVERIFY(IsValidBirthday(Date("gregorian", 2000, 2, 29), today));
		QVERIFY(!IsValidBirthday(Date("gregorian", 2001, 2, 29), today));

		QCOMPARE(ClampBirthday(Date("jalali", 1376, 12, 30)).day, 29);
		QCOMPARE(ClampBirthday(Date("jalali", 1375, 12, 30)).day, 30);
		QCOMPARE(ClampBirthday(Date("gregorian", 2001, 2, 31)).day, 28);
		QCOMPARE(ClampBirthday(Date("jalali", 1370, 15, 0)).month, 12);
	}

	void birthdayConversion() {
		const auto today = QDate(2026, 10, 10);
		// 12 Mehr 1370 = 4 October 1991.
		auto converted = ConvertBirthday(
			Date("jalali", 1370, 7, 12),
			QStringLiteral("gregorian"),
			today);
		QCOMPARE(converted.calendar, QStringLiteral("gregorian"));
		QCOMPARE(converted.year, 1991);
		QCOMPARE(converted.month, 10);
		QCOMPARE(converted.day, 4);
		converted = ConvertBirthday(converted, QStringLiteral("jalali"), today);
		QCOMPARE(converted.year, 1370);
		QCOMPARE(converted.month, 7);
		QCOMPARE(converted.day, 12);
		// The leap day of 1375.
		converted = ConvertBirthday(
			Date("jalali", 1375, 12, 30),
			QStringLiteral("gregorian"),
			today);
		QCOMPARE(converted.year, 1997);
		QCOMPARE(converted.month, 3);
		QCOMPARE(converted.day, 20);
		// Without a year: this year's correspondence, the year stays unset.
		converted = ConvertBirthday(
			Date("jalali", 0, 1, 1),
			QStringLiteral("gregorian"),
			today);
		QCOMPARE(converted.year, 0);
		QCOMPARE(converted.month, 3);
		QVERIFY(converted.day == 20 || converted.day == 21);
		// Esfand 30 without a year in a common year falls back to Esfand 29.
		converted = ConvertBirthday(
			Date("jalali", 0, 12, 30),
			QStringLiteral("gregorian"),
			today);
		QVERIFY(IsValidBirthday(converted, today));
		// February 29 in a year that has none.
		converted = ConvertBirthday(
			Date("gregorian", 0, 2, 29),
			QStringLiteral("jalali"),
			today);
		QVERIFY(IsValidBirthday(converted, today));
		QCOMPARE(converted.month, 12);
		// The same calendar: untouched.
		converted = ConvertBirthday(
			Date("jalali", 1370, 7, 12),
			QStringLiteral("jalali"),
			today);
		QCOMPARE(converted.day, 12);
	}

	void birthdayTexts() {
		QCOMPARE(BirthdayText(Date("jalali", 0, 7, 12)), U("۱۲ مهر"));
		QCOMPARE(BirthdayText(Date("jalali", 1370, 7, 12)), U("۱۲ مهر ۱۳۷۰"));
		QCOMPARE(DefaultCalendar(), QStringLiteral("jalali"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(BirthdayText(Date("gregorian", 1991, 3, 4)), QStringLiteral("March 4, 1991"));
		QCOMPARE(BirthdayText(Date("gregorian", 0, 3, 4)), QStringLiteral("March 4"));
		QCOMPARE(DefaultCalendar(), QStringLiteral("gregorian"));
		Lang::SetCurrent(Lang::Language::Persian);

		QVERIFY(SameBirthday(std::nullopt, std::nullopt));
		QVERIFY(!SameBirthday(Date("jalali", 0, 7, 12), std::nullopt));
		QVERIFY(SameBirthday(Date("jalali", 0, 7, 12), Date("jalali", 0, 7, 12)));
		QVERIFY(!SameBirthday(Date("jalali", 0, 7, 12), Date("jalali", 1370, 7, 12)));
		QVERIFY(!SameBirthday(Date("jalali", 0, 7, 12), Date("gregorian", 0, 7, 12)));
		// An absent calendar name means Jalali (the server's default).
		QVERIFY(SameBirthday(Date("", 0, 7, 12), Date("jalali", 0, 7, 12)));
	}

	// ---- the QR code of the share box

	void qrReedSolomon() {
		// The classic "HELLO WORLD" 1-M example of the standard's tutorials.
		const uchar data[] = {
			32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17,
		};
		const uchar expected[] = { 196, 35, 39, 119, 235, 215, 231, 226, 93, 23 };
		const auto result = Qr::ReedSolomon(
			QByteArray(reinterpret_cast<const char*>(data), sizeof(data)),
			10);
		QCOMPARE(
			result,
			QByteArray(reinterpret_cast<const char*>(expected), sizeof(expected)));
	}

	void qrFormatBits() {
		// Level M from the format information table of the standard.
		QCOMPARE(Qr::FormatBits(0), 0b101010000010010);
		QCOMPARE(Qr::FormatBits(1), 0b101000100100101);
		QCOMPARE(Qr::FormatBits(5), 0b100000011001110);
		QCOMPARE(Qr::FormatBits(2), 0b101111001111100);
		QCOMPARE(Qr::FormatBits(7), 0b100101010100000);
	}

	void qrVersions() {
		QVERIFY(!Qr::Encode(QByteArray()).valid());
		QVERIFY(!Qr::Encode(QByteArray(Qr::kMaxBytes + 1, 'a')).valid());
		const auto capacity = [](int version) {
			static const int kBytes[] = { 14, 26, 42, 62, 84, 106 };
			return kBytes[version - 1];
		};
		for (auto version = 1; version <= 6; ++version) {
			const auto low = (version == 1) ? 1 : (capacity(version - 1) + 1);
			for (const auto length : { low, capacity(version) }) {
				const auto code = Qr::Encode(QByteArray(length, 'x'));
				QVERIFY(code.valid());
				QCOMPARE(code.version, version);
				QCOMPARE(code.size, 17 + 4 * version);
				QCOMPARE(code.modules.size(), code.size * code.size);
				QVERIFY(code.mask >= 0 && code.mask < 8);
			}
		}
	}

	void qrFunctionPatterns() {
		const auto code = Qr::Encode(Profile::ProfileLink(QStringLiteral("ali.r")).toUtf8());
		QVERIFY(code.valid());
		QCOMPARE(code.version, 2);
		const auto size = code.size;
		// The three finders: a dark ring, a light ring, a dark 3x3 core.
		for (const auto &origin : { QPoint(0, 0), QPoint(size - 7, 0), QPoint(0, size - 7) }) {
			for (auto y = 0; y != 7; ++y) {
				for (auto x = 0; x != 7; ++x) {
					const auto ring = std::max(std::abs(x - 3), std::abs(y - 3));
					QCOMPARE(code.dark(origin.x() + x, origin.y() + y), ring != 2);
				}
			}
		}
		// Separators, the timing patterns, the dark module.
		for (auto i = 0; i != 8; ++i) {
			QVERIFY(!code.dark(7, i));
			QVERIFY(!code.dark(i, 7));
		}
		for (auto i = 8; i < size - 8; ++i) {
			QCOMPARE(code.dark(i, 6), (i % 2) == 0);
			QCOMPARE(code.dark(6, i), (i % 2) == 0);
		}
		QVERIFY(code.dark(8, size - 8));
		// The alignment pattern of version 2 is centred at (18, 18).
		QVERIFY(code.dark(18, 18));
		QVERIFY(!code.dark(17, 18));
		QVERIFY(code.dark(16, 18));
		// The format information agrees with the chosen mask (both copies).
		const auto bits = Qr::FormatBits(code.mask);
		for (auto i = 0; i <= 5; ++i) {
			QCOMPARE(code.dark(8, i), ((bits >> i) & 1) != 0);
		}
		for (auto i = 0; i < 8; ++i) {
			QCOMPARE(code.dark(size - 1 - i, 8), ((bits >> i) & 1) != 0);
		}
		const auto image = Qr::Render(code, 4, 2);
		QCOMPARE(image.width(), (size + 4) * 4);
		QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
		QCOMPARE(image.pixelColor(2 * 4 + 1, 2 * 4 + 1), QColor(Qt::black));
	}
};

QTEST_MAIN(TestProfileLogic)
#include "test_profile_logic.moc"
