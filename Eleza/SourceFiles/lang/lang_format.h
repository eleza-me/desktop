// Number, date and name formatting that follows the interface language:
// Persian digits and the Solar Hijri (Jalali) calendar in Persian, Latin digits and
// the Gregorian calendar in English. A port of the web client's core/format.ts.
#pragma once

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QString>

namespace Lang {

// ---- digits
// "123" -> "۱۲۳" (also converts Arabic-Indic digits).
[[nodiscard]] QString FaDigits(const QString &text);
// "۱۲۳" / "١٢٣" -> "123": codes and phone numbers typed with a Persian keyboard.
[[nodiscard]] QString Latinize(const QString &text);
// Digits of the current language.
[[nodiscard]] QString Num(const QString &text);
[[nodiscard]] QString Num(qint64 value);
// A number with a fraction: digits and the decimal separator of the current
// language ("1.5" -> "۱٫۵").
[[nodiscard]] QString Decimal(const QString &latin);

// ---- the Jalali calendar
struct JalaliDate {
	int year = 0;
	int month = 0; // 1..12
	int day = 0; // 1..31

	friend bool operator==(const JalaliDate &a, const JalaliDate &b) {
		return (a.year == b.year) && (a.month == b.month) && (a.day == b.day);
	}
};
[[nodiscard]] JalaliDate GregorianToJalali(int year, int month, int day);
[[nodiscard]] JalaliDate ToJalali(const QDate &date);
// Invalid QDate for an impossible Jalali date.
[[nodiscard]] QDate JalaliToGregorian(int year, int month, int day);
[[nodiscard]] bool IsJalaliLeapYear(int year);
[[nodiscard]] int JalaliMonthDays(int year, int month);
// «فروردین» … «اسفند» in Persian, "Farvardin" … "Esfand" otherwise.
[[nodiscard]] QString JalaliMonthName(int month);
[[nodiscard]] QString GregorianMonthName(int month);

// ---- dates and times
// «۲۱:۴۱» / "21:41".
[[nodiscard]] QString Clock(const QTime &time);
[[nodiscard]] QString Clock(const QDateTime &dateTime);
// «۱۵ مهر» / "October 7"; with the year when asked or when it is not this year's:
// «۱۵ مهر ۱۴۰۳» / "October 7, 2024".
[[nodiscard]] QString SolarDate(
	const QDate &date,
	bool includeYear = false,
	const QDate &today = QDate::currentDate());
// «شنبه» / "Saturday".
[[nodiscard]] QString Weekday(const QDate &date);
// Chat list time: the clock today, «دیروز», the weekday within the last week,
// else the date.
[[nodiscard]] QString ListTime(
	const QDateTime &dateTime,
	const QDateTime &now = QDateTime::currentDateTime());
// Day separator in a chat: «امروز», «دیروز», else the date.
[[nodiscard]] QString DayLabel(
	const QDate &date,
	const QDate &today = QDate::currentDate());
// Feed time: «همین حالا», «۵ دقیقه», «۳ ساعت», «دیروز», else the date.
[[nodiscard]] QString RelativeTime(
	const QDateTime &dateTime,
	const QDateTime &now = QDateTime::currentDateTime());
// «۰:۴۲», «۱:۰۲:۰۳» for voice / video lengths.
[[nodiscard]] QString Duration(int seconds);
// "12 KB", "3.4 MB" with localized digits.
[[nodiscard]] QString Bytes(qint64 bytes);
// «۹۹۹», «۱٫۲ هزار», «۳٫۴ میلیون» / "999", "1.2K", "3.4M".
[[nodiscard]] QString CompactCount(qint64 value);

// Presence wording. `status`: "online", "recently", "within_week",
// "within_month", "long_ago" or empty; `lastSeenAt`: unix seconds, 0 if unknown.
[[nodiscard]] QString LastSeen(
	const QString &status,
	qint64 lastSeenAt,
	const QDateTime &now = QDateTime::currentDateTime());

// ---- text helpers
// The first letter of a name for an avatar («؟» for an empty name).
[[nodiscard]] QString Initial(const QString &name);
// E.164 from what the user typed ("0912…" with a default country code, "+1…" as is).
// Empty when the result is not a valid number.
[[nodiscard]] QString E164(
	const QString &input,
	const QString &defaultCode = QStringLiteral("98"));
// Direction by the first strong character outside direction isolates (UAX #9 P2;
// the interface direction if there is none).
[[nodiscard]] bool IsRtlText(const QString &text);

// ---- direction isolates
// Tokens that always read left to right (phone numbers, @handles, addresses,
// codes, coordinates) inside a line of either direction: wrapped in
// U+2066 … U+2069, so a leading "+" / "@" stays in front. Empty stays empty.
[[nodiscard]] QString Ltr(const QString &text);
// A name or a phrase of unknown direction inside a sentence (U+2068 … U+2069):
// it keeps its own direction and does not reorder its neighbours.
[[nodiscard]] QString Isolated(const QString &text);
// "@sara", left-to-right isolated ("" for an empty handle; a leading "@" is not doubled).
[[nodiscard]] QString Handle(const QString &handle);
// "+98 912 123 4567": grouped, digits of the interface language, left-to-right
// isolated. Accepts E.164 with or without the plus; other text is only isolated.
[[nodiscard]] QString Phone(const QString &phone);
// The same without the isolate characters (for the clipboard and for tests).
[[nodiscard]] QString PhoneGroups(const QString &phone);

} // namespace Lang
