#include "lang/lang_format.h"

#include "lang/lang_instance.h"
#include "lang_auto.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QTextBoundaryFinder>
#include <QtCore/QTimeZone>

#include <algorithm>

namespace Lang {
namespace {

// ---- Jalali calendar arithmetic.
//
// The civil calendar is approximated by the 33-year cycle (8 leap years in
// every 33, at the cycle positions 1, 5, 9, 13, 17, 22, 26 and 30), which
// agrees with the official calendar for the years 1244–1472 SH (1865–2093 CE).
// Days are counted from 1 Farvardin of year 1 and anchored to the Gregorian
// calendar through one well known date: 1 Farvardin 1400 = 21 March 2021.

constexpr auto kDaysIn33Years = 33 * 365 + 8;

// Number of leap years before `year` (in years 1 .. year - 1).
[[nodiscard]] qint64 LeapYearsBefore(qint64 year) {
	// Floor division that also works for year <= 0.
	const auto value = 8 * year + 21;
	return (value >= 0) ? (value / 33) : -((-value + 32) / 33);
}

[[nodiscard]] qint64 DaysBeforeYear(qint64 year) {
	return 365 * (year - 1) + LeapYearsBefore(year);
}

[[nodiscard]] int DaysBeforeMonth(int month) {
	return (month <= 7) ? ((month - 1) * 31) : (186 + (month - 7) * 30);
}

// Day number of a Jalali date: 1 Farvardin 1 -> 0.
[[nodiscard]] qint64 JalaliDayNumber(int year, int month, int day) {
	return DaysBeforeYear(year) + DaysBeforeMonth(month) + (day - 1);
}

// Julian day of 1 Farvardin 1 in this arithmetic.
[[nodiscard]] qint64 JalaliEpoch() {
	static const auto result = QDate(2021, 3, 21).toJulianDay()
		- JalaliDayNumber(1400, 1, 1);
	return result;
}

const char *const kJalaliMonthsFa[12] = {
	"فروردین",
	"اردیبهشت",
	"خرداد",
	"تیر",
	"مرداد",
	"شهریور",
	"مهر",
	"آبان",
	"آذر",
	"دی",
	"بهمن",
	"اسفند",
};

const char *const kJalaliMonthsEn[12] = {
	"Farvardin",
	"Ordibehesht",
	"Khordad",
	"Tir",
	"Mordad",
	"Shahrivar",
	"Mehr",
	"Aban",
	"Azar",
	"Dey",
	"Bahman",
	"Esfand",
};

const char *const kGregorianMonthsEn[12] = {
	"January",
	"February",
	"March",
	"April",
	"May",
	"June",
	"July",
	"August",
	"September",
	"October",
	"November",
	"December",
};

const char *const kGregorianMonthsFa[12] = {
	"ژانویه",
	"فوریه",
	"مارس",
	"آوریل",
	"مه",
	"ژوئن",
	"ژوئیه",
	"اوت",
	"سپتامبر",
	"اکتبر",
	"نوامبر",
	"دسامبر",
};

// Indexed by QDate::dayOfWeek() - 1 (Monday first).
const char *const kWeekdaysFa[7] = {
	"دوشنبه",
	"سه‌شنبه",
	"چهارشنبه",
	"پنجشنبه",
	"جمعه",
	"شنبه",
	"یکشنبه",
};

const char *const kWeekdaysEn[7] = {
	"Monday",
	"Tuesday",
	"Wednesday",
	"Thursday",
	"Friday",
	"Saturday",
	"Sunday",
};

[[nodiscard]] QString TwoDigits(int value) {
	return QString::number(value).rightJustified(2, QLatin1Char('0'));
}

// value / divisor with one digit after the point, halves rounded up
// (1250 / 1000 -> "1.3", 2000 / 1000 -> "2").
[[nodiscard]] QString OneDecimal(qint64 value, qint64 divisor) {
	const auto tenths = (value * 10 + divisor / 2) / divisor;
	const auto whole = tenths / 10;
	const auto rest = tenths % 10;
	return rest
		? (QString::number(whole) + QLatin1Char('.') + QString::number(rest))
		: QString::number(whole);
}

[[nodiscard]] QString Rounded(qint64 value, qint64 divisor) {
	return QString::number((value + divisor / 2) / divisor);
}

// The decimal point follows the digits: «۱٫۲» in Persian.
[[nodiscard]] QString LocalizedDecimal(const QString &latin) {
	if (!IsPersian()) {
		return latin;
	}
	auto result = FaDigits(latin);
	result.replace(QLatin1Char('.'), QChar(0x066B));
	return result;
}

} // namespace

QString FaDigits(const QString &text) {
	auto result = text;
	for (auto &ch : result) {
		const auto code = ch.unicode();
		if (code >= '0' && code <= '9') {
			ch = QChar(ushort(0x06F0 + (code - '0')));
		} else if (code >= 0x0660 && code <= 0x0669) {
			ch = QChar(ushort(0x06F0 + (code - 0x0660)));
		}
	}
	return result;
}

QString Latinize(const QString &text) {
	auto result = text;
	for (auto &ch : result) {
		const auto code = ch.unicode();
		if (code >= 0x06F0 && code <= 0x06F9) {
			ch = QChar(ushort('0' + (code - 0x06F0)));
		} else if (code >= 0x0660 && code <= 0x0669) {
			ch = QChar(ushort('0' + (code - 0x0660)));
		}
	}
	return result;
}

QString Num(const QString &text) {
	return IsPersian() ? FaDigits(text) : text;
}

QString Num(qint64 value) {
	return Num(QString::number(value));
}

bool IsJalaliLeapYear(int year) {
	return (LeapYearsBefore(qint64(year) + 1) - LeapYearsBefore(year)) == 1;
}

int JalaliMonthDays(int year, int month) {
	if (month < 1 || month > 12) {
		return 0;
	} else if (month <= 6) {
		return 31;
	} else if (month <= 11) {
		return 30;
	}
	return IsJalaliLeapYear(year) ? 30 : 29;
}

JalaliDate GregorianToJalali(int year, int month, int day) {
	return ToJalali(QDate(year, month, day));
}

JalaliDate ToJalali(const QDate &date) {
	if (!date.isValid()) {
		return JalaliDate();
	}
	const auto number = date.toJulianDay() - JalaliEpoch();
	// A first guess from the average year length, then exact correction.
	auto year = qint64(1) + (number * 33) / kDaysIn33Years;
	if (number < 0) {
		year = (number * 33 - (kDaysIn33Years - 1)) / kDaysIn33Years + 1;
	}
	while (DaysBeforeYear(year) > number) {
		--year;
	}
	while (DaysBeforeYear(year + 1) <= number) {
		++year;
	}
	const auto dayOfYear = int(number - DaysBeforeYear(year)); // 0-based
	auto result = JalaliDate();
	result.year = int(year);
	if (dayOfYear < 186) {
		result.month = 1 + dayOfYear / 31;
		result.day = 1 + dayOfYear % 31;
	} else {
		result.month = 7 + (dayOfYear - 186) / 30;
		result.day = 1 + (dayOfYear - 186) % 30;
	}
	return result;
}

QDate JalaliToGregorian(int year, int month, int day) {
	if (month < 1
		|| month > 12
		|| day < 1
		|| day > JalaliMonthDays(year, month)) {
		return QDate();
	}
	return QDate::fromJulianDay(
		JalaliEpoch() + JalaliDayNumber(year, month, day));
}

QString JalaliMonthName(int month) {
	if (month < 1 || month > 12) {
		return QString();
	}
	return IsPersian()
		? QString::fromUtf8(kJalaliMonthsFa[month - 1])
		: QString::fromLatin1(kJalaliMonthsEn[month - 1]);
}

QString GregorianMonthName(int month) {
	if (month < 1 || month > 12) {
		return QString();
	}
	return IsPersian()
		? QString::fromUtf8(kGregorianMonthsFa[month - 1])
		: QString::fromLatin1(kGregorianMonthsEn[month - 1]);
}

QString Clock(const QTime &time) {
	return Num(TwoDigits(time.hour())
		+ QLatin1Char(':')
		+ TwoDigits(time.minute()));
}

QString Clock(const QDateTime &dateTime) {
	return Clock(dateTime.time());
}

QString SolarDate(const QDate &date, bool includeYear, const QDate &today) {
	if (!date.isValid()) {
		return QString();
	}
	if (IsPersian()) {
		const auto jalali = ToJalali(date);
		const auto withYear = includeYear
			|| (jalali.year != ToJalali(today).year);
		auto result = Num(jalali.day)
			+ QLatin1Char(' ')
			+ JalaliMonthName(jalali.month);
		if (withYear) {
			result += QLatin1Char(' ') + Num(jalali.year);
		}
		return result;
	}
	const auto withYear = includeYear || (date.year() != today.year());
	auto result = GregorianMonthName(date.month())
		+ QLatin1Char(' ')
		+ QString::number(date.day());
	if (withYear) {
		result += QStringLiteral(", ") + QString::number(date.year());
	}
	return result;
}

QString Weekday(const QDate &date) {
	if (!date.isValid()) {
		return QString();
	}
	const auto index = date.dayOfWeek() - 1;
	return IsPersian()
		? QString::fromUtf8(kWeekdaysFa[index])
		: QString::fromLatin1(kWeekdaysEn[index]);
}

QString ListTime(const QDateTime &dateTime, const QDateTime &now) {
	const auto date = dateTime.date();
	const auto today = now.date();
	if (date == today) {
		return Clock(dateTime);
	} else if (date == today.addDays(-1)) {
		return tr::lng_yesterday(tr::now);
	} else if (dateTime <= now
		&& dateTime.msecsTo(now) < qint64(6) * 86'400'000) {
		return Weekday(date);
	}
	return SolarDate(date, false, today);
}

QString DayLabel(const QDate &date, const QDate &today) {
	if (date == today) {
		return tr::lng_today(tr::now);
	} else if (date == today.addDays(-1)) {
		return tr::lng_yesterday(tr::now);
	}
	return SolarDate(date, false, today);
}

QString RelativeTime(const QDateTime &dateTime, const QDateTime &now) {
	const auto diff = std::max(dateTime.msecsTo(now), qint64(0));
	if (diff < 60'000) {
		return tr::lng_now(tr::now);
	} else if (diff < 3'600'000) {
		return tr::lng_minutes_short(tr::now, lt_count, double(diff / 60'000));
	} else if (diff < 86'400'000) {
		return tr::lng_hours_short(tr::now, lt_count, double(diff / 3'600'000));
	} else if (diff < 2 * qint64(86'400'000)) {
		return tr::lng_yesterday(tr::now);
	}
	return SolarDate(dateTime.date(), false, now.date());
}

QString Duration(int seconds) {
	const auto total = std::max(seconds, 0);
	const auto hours = total / 3600;
	const auto minutes = (total / 60) % 60;
	const auto rest = total % 60;
	const auto text = hours
		? (QString::number(hours)
			+ QLatin1Char(':')
			+ TwoDigits(minutes)
			+ QLatin1Char(':')
			+ TwoDigits(rest))
		: (QString::number(total / 60) + QLatin1Char(':') + TwoDigits(rest));
	return Num(text);
}

QString Bytes(qint64 bytes) {
	const auto kilo = qint64(1024);
	if (bytes < kilo) {
		return Num(bytes) + QStringLiteral(" B");
	} else if (bytes < kilo * kilo) {
		return Num(Rounded(bytes, kilo)) + QStringLiteral(" KB");
	} else if (bytes < kilo * kilo * kilo) {
		return LocalizedDecimal(OneDecimal(bytes, kilo * kilo))
			+ QStringLiteral(" MB");
	}
	const auto hundredths = (bytes * 100 + (kilo * kilo * kilo) / 2)
		/ (kilo * kilo * kilo);
	return LocalizedDecimal(QString::number(hundredths / 100)
		+ QLatin1Char('.')
		+ TwoDigits(int(hundredths % 100))) + QStringLiteral(" GB");
}

QString CompactCount(qint64 value) {
	if (value < 1000) {
		return Num(value);
	}
	const auto million = qint64(1'000'000);
	const auto thousands = (value < million);
	const auto number = thousands
		? ((value < 10'000) ? OneDecimal(value, 1000) : Rounded(value, 1000))
		: OneDecimal(value, million);
	const auto suffix = thousands
		? tr::lng_count_thousand(tr::now)
		: tr::lng_count_million(tr::now);
	// «۱٫۲ هزار» with a space, "1.2K" without.
	return LocalizedDecimal(number)
		+ (IsPersian() ? QStringLiteral(" ") : QString())
		+ suffix;
}

QString LastSeen(
		const QString &status,
		qint64 lastSeenAt,
		const QDateTime &now) {
	if (status == QStringLiteral("online")) {
		return tr::lng_status_online(tr::now);
	} else if (status == QStringLiteral("recently")) {
		return tr::lng_status_last_seen_recently(tr::now);
	} else if (status == QStringLiteral("within_week")
		|| status == QStringLiteral("week")) {
		return tr::lng_status_last_seen_week(tr::now);
	} else if (status == QStringLiteral("within_month")
		|| status == QStringLiteral("month")) {
		return tr::lng_status_last_seen_month(tr::now);
	} else if (status == QStringLiteral("long_ago")) {
		return tr::lng_status_last_seen_long_ago(tr::now);
	} else if (lastSeenAt <= 0) {
		return status.isEmpty()
			? QString()
			: tr::lng_status_last_seen_recently(tr::now);
	}
	const auto seen = QDateTime::fromSecsSinceEpoch(lastSeenAt).toTimeZone(
		now.timeZone());
	if (seen.msecsTo(now) < 60'000) {
		return tr::lng_status_last_seen_now(tr::now);
	}
	const auto today = now.date();
	if (seen.date() == today) {
		return tr::lng_status_last_seen(tr::now, lt_time, Clock(seen));
	} else if (seen.date() == today.addDays(-1)) {
		return tr::lng_status_last_seen(
			tr::now,
			lt_time,
			tr::lng_yesterday(tr::now) + QLatin1Char(' ') + Clock(seen));
	}
	return tr::lng_status_last_seen(
		tr::now,
		lt_time,
		SolarDate(seen.date(), false, today) + QLatin1Char(' ') + Clock(seen));
}

QString Initial(const QString &name) {
	const auto trimmed = name.trimmed();
	if (trimmed.isEmpty()) {
		return QString(QChar(0x061F)); // «؟»
	}
	auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, trimmed);
	const auto next = finder.toNextBoundary();
	return trimmed.left((next > 0) ? next : 1);
}

QString E164(const QString &input, const QString &defaultCode) {
	static const auto strip = QRegularExpression(QStringLiteral("[\\s\\-()]"));
	static const auto valid = QRegularExpression(
		QStringLiteral("^\\+[1-9][0-9]{6,14}$"));
	auto result = Latinize(input);
	result.remove(strip);
	if (result.startsWith(QStringLiteral("00"))) {
		result = QLatin1Char('+') + result.mid(2);
	}
	if (!result.startsWith(QLatin1Char('+'))) {
		if (result.startsWith(QLatin1Char('0'))) {
			result.remove(0, 1);
		}
		result = QLatin1Char('+') + defaultCode + result;
	}
	return valid.match(result).hasMatch() ? result : QString();
}

bool IsRtlText(const QString &text) {
	for (const auto ch : text) {
		switch (ch.direction()) {
		case QChar::DirL: return false;
		case QChar::DirR:
		case QChar::DirAL: return true;
		default: break;
		}
	}
	return IsPersian();
}

} // namespace Lang
