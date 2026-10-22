// Language runtime: generated strings, digits, the Jalali calendar, formatting.
#include "lang/lang_countries.h"
#include "lang/lang_format.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"

#include <QtTest/QtTest>

#include <limits>

using namespace Lang;

class TestLangFormat : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

private Q_SLOTS:
	void init() {
		SetCurrent(Language::Persian);
	}

	void isolates() {
		const auto lri = QChar(0x2066);
		const auto fsi = QChar(0x2068);
		const auto pdi = QChar(0x2069);
		QCOMPARE(Ltr(QStringLiteral("a@b.c")), lri + QStringLiteral("a@b.c") + pdi);
		QVERIFY(Ltr(QString()).isEmpty());
		QCOMPARE(Isolated(QStringLiteral("Sara")), fsi + QStringLiteral("Sara") + pdi);
		QCOMPARE(Handle(QStringLiteral("sara")), lri + QStringLiteral("@sara") + pdi);
		QCOMPARE(Handle(QStringLiteral("@sara")), lri + QStringLiteral("@sara") + pdi);
		QVERIFY(Handle(QString()).isEmpty());

		QCOMPARE(PhoneGroups(QStringLiteral("+989121234567")), U("+۹۸ ۹۱۲ ۱۲۳ ۴۵۶۷"));
		QCOMPARE(PhoneGroups(QStringLiteral("989121234567")), U("+۹۸ ۹۱۲ ۱۲۳ ۴۵۶۷"));
		QCOMPARE(
			Phone(QStringLiteral("+989121234567")),
			lri + U("+۹۸ ۹۱۲ ۱۲۳ ۴۵۶۷") + pdi);
		SetCurrent(Language::English);
		QCOMPARE(PhoneGroups(QStringLiteral("+989121234567")), QStringLiteral("+98 912 123 4567"));
		QCOMPARE(PhoneGroups(QStringLiteral("+14155550123")), QStringLiteral("+1 415 555 0123"));
		QCOMPARE(PhoneGroups(QStringLiteral("+442079460958")), QStringLiteral("+44 207 946 0958"));
		// Not a number: untouched.
		QCOMPARE(PhoneGroups(QStringLiteral("+98 *** 4567")), QStringLiteral("+98 *** 4567"));
		QCOMPARE(PhoneGroups(QStringLiteral("123")), QStringLiteral("123"));
		QVERIFY(Phone(QString()).isEmpty());
	}

	void rtlText() {
		const auto lri = QChar(0x2066);
		const auto rli = QChar(0x2067);
		const auto fsi = QChar(0x2068);
		const auto pdi = QChar(0x2069);
		const auto fa = U("سلام");
		QVERIFY(IsRtlText(fa));
		QVERIFY(!IsRtlText(QStringLiteral("hello ") + fa));
		// An isolated handle in front does not decide the paragraph (UAX #9 P2).
		QVERIFY(IsRtlText(Handle(QStringLiteral("sara")) + QChar(' ') + fa));
		QVERIFY(IsRtlText(fsi + QStringLiteral("Sara") + pdi + fa));
		QVERIFY(!IsRtlText(rli + fa + pdi + QStringLiteral(" hi")));
		// Nested isolates close at the matching PDI only.
		QVERIFY(IsRtlText(lri + QStringLiteral("a") + fsi + QStringLiteral("b") + pdi
			+ QStringLiteral("c") + pdi + fa));
		// An unmatched PDI is ignored; an unclosed isolate hides the rest.
		QVERIFY(!IsRtlText(pdi + QStringLiteral("abc")));
		SetCurrent(Language::English);
		QVERIFY(!IsRtlText(lri + fa));
		QVERIFY(!IsRtlText(QStringLiteral("123")));
		SetCurrent(Language::Persian);
		QVERIFY(IsRtlText(lri + QStringLiteral("abc")));
		QVERIFY(IsRtlText(QStringLiteral("123")));
		// Strong characters outside the BMP count: U+10900 is Phoenician (R).
		QVERIFY(IsRtlText(QString::fromUcs4(U"\U00010900 abc")));
		QVERIFY(!IsRtlText(QString::fromUcs4(U"\U0001D400 ") + fa)); // bold A (L)
	}

	void strings() {
		QCOMPARE(tr::lng_app_name(tr::now), U("الزا"));
		QCOMPARE(
			tr::lng_intro_code_sent_to(tr::now, lt_phone, QStringLiteral("+98")),
			U("به +98 فرستاده شد"));
		QCOMPARE(tr::lng_eli_new_posts(tr::now, lt_count, 12), U("۱۲ پست تازه"));
		// A fraction has the Persian decimal separator, as in sizes and counts.
		QCOMPARE(tr::lng_eli_new_posts(tr::now, lt_count, 1.5), U("۱٫۵ پست تازه"));
		QCOMPARE(Decimal(QStringLiteral("2.5")), U("۲٫۵"));
		QCOMPARE(GetValueByName(QStringLiteral("lng_cancel")), U("انصراف"));
		QVERIFY(GetValueByName(QStringLiteral("lng_no_such_key")).isEmpty());
		QVERIFY(RightToLeft());

		auto notified = 0;
		auto context = QObject();
		OnUpdated(&context, [&] { ++notified; });
		SetCurrent(Language::English);
		QCOMPARE(notified, 1);
		QVERIFY(!RightToLeft());
		QCOMPARE(tr::lng_app_name(tr::now), QStringLiteral("Eleza"));
		QCOMPARE(
			tr::lng_intro_code_sent_to(tr::now, lt_phone, QStringLiteral("+98")),
			QStringLiteral("Sent to +98"));
		QCOMPARE(tr::lng_eli_new_posts(tr::now, lt_count, 1), QStringLiteral("1 new post"));
		QCOMPARE(tr::lng_eli_new_posts(tr::now, lt_count, 1.5), QStringLiteral("1.5 new posts"));
		QCOMPARE(tr::lng_eli_new_posts(tr::now, lt_count, 2), QStringLiteral("2 new posts"));
		SetCurrent(Language::English);
		QCOMPARE(notified, 1); // no change, no signal
		QCOMPARE(FromId(QStringLiteral("fa-IR"), Language::English), Language::Persian);
		QCOMPARE(FromId(QStringLiteral("xx"), Language::English), Language::English);
		QCOMPARE(CurrentId(), QStringLiteral("en"));
	}

	void tagsInOnePass() {
		// A value that looks like a later tag (somebody's name) stays as it is.
		QCOMPARE(
			tr::lng_action_member(
				tr::now,
				lt_name,
				QStringLiteral("{action}"),
				lt_action,
				QStringLiteral("joined")),
			QStringLiteral("{action} joined"));
		auto text = QStringLiteral("{a}-{b}-{a}-{c}-{b");
		const auto a = QStringLiteral("{b}");
		const auto b = QStringLiteral("{a}");
		Lang::internal::ReplaceTags(text, { { "a", a }, { "b", b } });
		QCOMPARE(text, QStringLiteral("{b}-{a}-{b}-{c}-{b"));
		auto plain = QStringLiteral("no tags {here");
		Lang::internal::ReplaceTag(plain, "here", a);
		QCOMPARE(plain, QStringLiteral("no tags {here"));
		auto nested = QStringLiteral("{{a}}");
		Lang::internal::ReplaceTag(nested, "a", QStringLiteral("x"));
		QCOMPARE(nested, QStringLiteral("{x}"));
	}

	void digits() {
		QCOMPARE(FaDigits(QStringLiteral("0123456789")), U("۰۱۲۳۴۵۶۷۸۹"));
		QCOMPARE(FaDigits(U("٣ نفر، 2 پیام")), U("۳ نفر، ۲ پیام"));
		QCOMPARE(Latinize(U("۰۹۱۲ ٣٤٥ 6789")), QStringLiteral("0912 345 6789"));
		QCOMPARE(Latinize(U("کد: ۱۲۳۴۵")), U("کد: 12345"));
		QCOMPARE(Num(1404), U("۱۴۰۴"));
		SetCurrent(Language::English);
		QCOMPARE(Num(1404), QStringLiteral("1404"));
	}

	void jalaliKnownDates() {
		struct Pair {
			int gy, gm, gd, jy, jm, jd;
		};
		const Pair pairs[] = {
			{ 2021, 3, 21, 1400, 1, 1 }, // Nowruz 1400
			{ 2021, 3, 20, 1399, 12, 30 }, // the last day of a leap year
			{ 2024, 3, 20, 1403, 1, 1 },
			{ 2025, 3, 20, 1403, 12, 30 }, // 1403 is leap
			{ 2025, 3, 21, 1404, 1, 1 },
			{ 2026, 3, 20, 1404, 12, 29 }, // 1404 is not
			{ 2026, 3, 21, 1405, 1, 1 },
			{ 2025, 10, 9, 1404, 7, 17 },
			{ 2024, 10, 6, 1403, 7, 15 },
			{ 1979, 2, 11, 1357, 11, 22 },
			{ 2000, 1, 1, 1378, 10, 11 },
			{ 1970, 1, 1, 1348, 10, 11 }, // the unix epoch
			{ 2017, 3, 21, 1396, 1, 1 },
			{ 2020, 3, 20, 1399, 1, 1 },
			{ 2023, 9, 23, 1402, 7, 1 }, // six months of 31 days end here
			{ 2023, 9, 22, 1402, 6, 31 },
			{ 2038, 1, 19, 1416, 10, 30 },
		};
		for (const auto &pair : pairs) {
			const auto jalali = GregorianToJalali(pair.gy, pair.gm, pair.gd);
			const auto label = QStringLiteral("%1-%2-%3").arg(pair.gy).arg(pair.gm).arg(
				pair.gd);
			QVERIFY2(
				(jalali == JalaliDate{ pair.jy, pair.jm, pair.jd }),
				qPrintable(label + QStringLiteral(" -> %1/%2/%3").arg(jalali.year).arg(
					jalali.month).arg(jalali.day)));
			QCOMPARE(
				JalaliToGregorian(pair.jy, pair.jm, pair.jd),
				QDate(pair.gy, pair.gm, pair.gd));
		}
	}

	void jalaliRoundTrip() {
		// Every day of 150 years converts back to itself, and consecutive days
		// are consecutive dates.
		auto date = QDate(1925, 1, 1);
		auto previous = ToJalali(date.addDays(-1));
		const auto end = QDate(2075, 1, 1);
		for (; date < end; date = date.addDays(1)) {
			const auto jalali = ToJalali(date);
			QVERIFY(jalali.month >= 1 && jalali.month <= 12);
			QVERIFY(jalali.day >= 1
				&& jalali.day <= JalaliMonthDays(jalali.year, jalali.month));
			QCOMPARE(JalaliToGregorian(jalali.year, jalali.month, jalali.day), date);
			const auto nextDay = (jalali.day == previous.day + 1
				&& jalali.month == previous.month
				&& jalali.year == previous.year);
			const auto nextMonth = (jalali.day == 1
				&& previous.day == JalaliMonthDays(previous.year, previous.month)
				&& ((jalali.month == previous.month + 1
					&& jalali.year == previous.year)
					|| (jalali.month == 1
						&& previous.month == 12
						&& jalali.year == previous.year + 1)));
			QVERIFY2(nextDay || nextMonth, qPrintable(date.toString(Qt::ISODate)));
			previous = jalali;
		}
	}

	void jalaliLeapYears() {
		// Leap years of the official calendar around the present.
		for (const auto year : { 1370, 1375, 1379, 1383, 1387, 1391, 1395, 1399, 1403, 1408, 1412 }) {
			QVERIFY2(IsJalaliLeapYear(year), qPrintable(QString::number(year)));
			QCOMPARE(JalaliMonthDays(year, 12), 30);
		}
		for (const auto year : { 1400, 1401, 1402, 1404, 1405, 1406, 1407 }) {
			QVERIFY2(!IsJalaliLeapYear(year), qPrintable(QString::number(year)));
			QCOMPARE(JalaliMonthDays(year, 12), 29);
		}
		QCOMPARE(JalaliMonthDays(1404, 1), 31);
		QCOMPARE(JalaliMonthDays(1404, 7), 30);
		QVERIFY(!JalaliToGregorian(1404, 12, 30).isValid());
		QVERIFY(!JalaliToGregorian(1404, 13, 1).isValid());
	}

	void dates() {
		const auto today = QDate(2025, 10, 9);
		QCOMPARE(SolarDate(today, false, today), U("۱۷ مهر"));
		QCOMPARE(SolarDate(today, true, today), U("۱۷ مهر ۱۴۰۴"));
		QCOMPARE(SolarDate(QDate(2024, 10, 6), false, today), U("۱۵ مهر ۱۴۰۳"));
		QCOMPARE(Weekday(today), U("پنجشنبه"));
		QCOMPARE(Clock(QTime(9, 5)), U("۰۹:۰۵"));
		QCOMPARE(DayLabel(today, today), U("امروز"));
		QCOMPARE(DayLabel(today.addDays(-1), today), U("دیروز"));
		QCOMPARE(DayLabel(today.addDays(-2), today), U("۱۵ مهر"));

		const auto now = QDateTime(today, QTime(21, 41));
		QCOMPARE(ListTime(now.addSecs(-3600), now), U("۲۰:۴۱"));
		QCOMPARE(ListTime(now.addDays(-1), now), U("دیروز"));
		QCOMPARE(ListTime(now.addDays(-3), now), U("دوشنبه"));
		QCOMPARE(ListTime(now.addDays(-20), now), U("۲۸ شهریور"));
		QCOMPARE(RelativeTime(now.addSecs(-20), now), U("همین حالا"));
		QCOMPARE(RelativeTime(now.addSecs(-25 * 60), now), U("۲۵ دقیقه"));
		QCOMPARE(RelativeTime(now.addSecs(-5 * 3600), now), U("۵ ساعت"));
		QCOMPARE(RelativeTime(now.addSecs(-30 * 3600), now), U("دیروز"));

		SetCurrent(Language::English);
		QCOMPARE(SolarDate(today, false, today), QStringLiteral("October 9"));
		QCOMPARE(SolarDate(QDate(2024, 10, 6), false, today), QStringLiteral("October 6, 2024"));
		QCOMPARE(Weekday(today), QStringLiteral("Thursday"));
		QCOMPARE(Clock(QTime(21, 41)), QStringLiteral("21:41"));
		QCOMPARE(RelativeTime(now.addSecs(-25 * 60), now), QStringLiteral("25 min"));
	}

	void lastSeen() {
		const auto now = QDateTime(QDate(2025, 10, 9), QTime(21, 41));
		const auto at = [&](qint64 secondsAgo) {
			return now.toSecsSinceEpoch() - secondsAgo;
		};
		QCOMPARE(LastSeen(QStringLiteral("online"), 0, now), U("آنلاین"));
		QCOMPARE(LastSeen(QStringLiteral("recently"), 0, now), U("آخرین بازدید اخیراً"));
		QCOMPARE(LastSeen(QStringLiteral("within_week"), 0, now), U("آخرین بازدید این هفته"));
		QCOMPARE(LastSeen(QString(), 0, now), QString());
		QCOMPARE(LastSeen(QString(), at(30), now), U("آخرین بازدید همین حالا"));
		QCOMPARE(LastSeen(QString(), at(3600), now), U("آخرین بازدید ۲۰:۴۱"));
		QCOMPARE(LastSeen(QString(), at(86400), now), U("آخرین بازدید دیروز ۲۱:۴۱"));
		QCOMPARE(LastSeen(QString(), at(3 * 86400), now), U("آخرین بازدید ۱۴ مهر ۲۱:۴۱"));
	}

	void counts() {
		QCOMPARE(CompactCount(999), U("۹۹۹"));
		QCOMPARE(CompactCount(1000), U("۱ هزار"));
		QCOMPARE(CompactCount(1200), U("۱٫۲ هزار"));
		QCOMPARE(CompactCount(1250), U("۱٫۳ هزار"));
		QCOMPARE(CompactCount(48300), U("۴۸ هزار"));
		QCOMPARE(CompactCount(3400000), U("۳٫۴ میلیون"));
		QCOMPARE(Duration(42), U("۰:۴۲"));
		QCOMPARE(Duration(3723), U("۱:۰۲:۰۳"));
		QCOMPARE(Bytes(512), U("۵۱۲ B"));
		QCOMPARE(Bytes(2048), U("۲ KB"));
		SetCurrent(Language::English);
		QCOMPARE(CompactCount(1200), QStringLiteral("1.2K"));
		QCOMPARE(CompactCount(3400000), QStringLiteral("3.4M"));
		QCOMPARE(Bytes(3565158), QStringLiteral("3.4 MB"));
		QCOMPARE(Bytes(qint64(5) * 1024 * 1024 * 1024 / 2), QStringLiteral("2.50 GB"));
		// The numbers are the sender's: the top of the range must not overflow.
		QCOMPARE(Bytes(std::numeric_limits<qint64>::max()), QStringLiteral("8589934592.00 GB"));
		QCOMPARE(Bytes(qint64(1024) * 1024 * 1024 - 1), QStringLiteral("1024 MB"));
		QCOMPARE(Bytes(1535), QStringLiteral("1 KB"));
		QCOMPARE(Bytes(1536), QStringLiteral("2 KB"));
		QCOMPARE(CompactCount(1999999), QStringLiteral("2M"));
		QVERIFY(CompactCount(std::numeric_limits<qint64>::max()).startsWith(QStringLiteral("9223372036854.8")));
	}

	void phonesAndNames() {
		QCOMPARE(E164(U("۰۹۱۲ ۳۴۵ ۶۷۸۹")), QStringLiteral("+989123456789"));
		QCOMPARE(E164(QStringLiteral("0912-345-6789")), QStringLiteral("+989123456789"));
		QCOMPARE(E164(QStringLiteral("+1 (415) 555 2671")), QStringLiteral("+14155552671"));
		QCOMPARE(E164(QStringLiteral("0044 20 7946 0958")), QStringLiteral("+442079460958"));
		QCOMPARE(E164(QStringLiteral("5551234"), QStringLiteral("1")), QStringLiteral("+15551234"));
		QVERIFY(E164(QStringLiteral("123")).isEmpty());
		QVERIFY(E164(QStringLiteral("abc")).isEmpty());

		QCOMPARE(Initial(U("  سارا")), U("س"));
		QCOMPARE(Initial(QStringLiteral("eleza")), QStringLiteral("e"));
		QCOMPARE(Initial(QString()), U("؟"));
		// A whole emoji, not half of a surrogate pair.
		QCOMPARE(Initial(U("🦊 Fox")), U("🦊"));

		QVERIFY(IsRtlText(U("سلام world")));
		QVERIFY(!IsRtlText(U("hello دنیا")));
		QVERIFY(IsRtlText(U("123 ...")));
		SetCurrent(Language::English);
		QVERIFY(!IsRtlText(U("123 ...")));
	}

	void countries() {
		QVERIFY(Countries().size() > 70);
		QCOMPARE(Countries().front().iso, QStringLiteral("IR"));
		QCOMPARE(CountryByIso(QStringLiteral("de"))->code, QStringLiteral("49"));
		QCOMPARE(CountryByCode(QStringLiteral("1"))->iso, QStringLiteral("US"));
		QCOMPARE(CountryByCode(QStringLiteral("7"))->iso, QStringLiteral("KZ"));
		QCOMPARE(CountryByPhone(QStringLiteral("+989121234567"))->iso, QStringLiteral("IR"));
		QCOMPARE(CountryByPhone(U("۹۷۱۵۰۱۲۳۴۵۶۷"))->iso, QStringLiteral("AE"));
		QVERIFY(CountryByPhone(QStringLiteral("000")) == nullptr);
		QCOMPARE(CountryByIso(QStringLiteral("IR"))->name(), U("ایران"));
		QCOMPARE(SearchCountries(U("آلم")).size(), size_t(1));
		QCOMPARE(SearchCountries(QStringLiteral("+98")).front()->iso, QStringLiteral("IR"));
		QCOMPARE(FlagEmoji(QStringLiteral("ir")).size(), 4); // two surrogate pairs
		SetCurrent(Language::English);
		QCOMPARE(CountryByIso(QStringLiteral("IR"))->name(), QStringLiteral("Iran"));
		QCOMPARE(SearchCountries(QStringLiteral("united")).size(), size_t(3));
	}

};

QTEST_GUILESS_MAIN(TestLangFormat)
#include "test_lang_format.moc"
