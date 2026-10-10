#include "profile/profile_logic.h"

#include "lang/lang_format.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"

#include <QtCore/QRegularExpression>

namespace Profile {
namespace {

constexpr auto kJalali = "jalali"_L1;
constexpr auto kGregorian = "gregorian"_L1;

[[nodiscard]] bool IsGregorianLeap(int year) {
	return QDate::isLeapYear(year);
}

} // namespace

FollowUi FollowUiOf(const QString &followState, bool followsYou) {
	if (followState == u"following") {
		return FollowUi::Following;
	} else if (followState == u"requested") {
		return FollowUi::Requested;
	}
	return followsYou ? FollowUi::FollowsYou : FollowUi::None;
}

FollowUi FollowUiOf(const Data::Profile &profile) {
	return FollowUiOf(profile.followState, profile.followsYou);
}

FollowUi FollowUiOf(const Data::GraphState &state) {
	return FollowUiOf(state.follow, state.followsYou);
}

int FollowersDelta(FollowUi from, FollowUi to) {
	return ((to == FollowUi::Following) ? 1 : 0)
		- ((from == FollowUi::Following) ? 1 : 0);
}

FollowToggle ToggleFollow(FollowUi current, bool isPrivate, bool followsYou) {
	auto result = FollowToggle();
	const auto idle = followsYou ? FollowUi::FollowsYou : FollowUi::None;
	switch (current) {
	case FollowUi::None:
	case FollowUi::FollowsYou:
		result.follow = true;
		result.optimistic = isPrivate
			? FollowUi::Requested
			: FollowUi::Following;
		break;
	case FollowUi::Following:
		result.follow = false;
		result.optimistic = idle;
		result.confirm = true;
		break;
	case FollowUi::Requested:
		// Withdrawing a request needs no question.
		result.follow = false;
		result.optimistic = idle;
		break;
	}
	return result;
}

FollowLook FollowLookOf(FollowUi state) {
	switch (state) {
	case FollowUi::Following: return FollowLook::Outline;
	case FollowUi::Requested: return FollowLook::Soft;
	case FollowUi::None:
	case FollowUi::FollowsYou: break;
	}
	return FollowLook::Filled;
}

ButtonModel MakeButtonModel(const ViewerState &state) {
	auto result = ButtonModel();
	if (!state.loaded) {
		return result;
	} else if (state.deleted) {
		result.content = Content::Deleted;
		return result;
	} else if (state.me) {
		result.actions = { Action::EditProfile, Action::ShareProfile };
		result.content = Content::Tabs;
		result.statsClickable = true;
		result.bookmarksTab = true;
		return result;
	}
	const auto locked = state.isPrivate
		&& (state.follow != FollowUi::Following);
	if (state.blocked) {
		result.actions = { Action::More };
		result.menu = { MenuEntry::Report, MenuEntry::Unblock };
		result.content = Content::Blocked;
		return result;
	}
	result.actions = { Action::Follow, Action::Message, Action::More };
	result.menu.push_back(state.postNotifications
		? MenuEntry::PostNotificationsOff
		: MenuEntry::PostNotificationsOn);
	if (state.hasHandle) {
		result.menu.push_back(MenuEntry::ShareProfile);
		result.menu.push_back(MenuEntry::CopyLink);
	}
	result.menu.push_back(state.muted ? MenuEntry::Unmute : MenuEntry::Mute);
	if (state.followsYou) {
		result.menu.push_back(MenuEntry::RemoveFollower);
	}
	result.menu.push_back(MenuEntry::Report);
	result.menu.push_back(MenuEntry::Block);
	result.content = state.justUnblocked
		? Content::Unblocked
		: !locked
		? Content::Tabs
		: (state.follow == FollowUi::Requested)
		? Content::LockedRequested
		: Content::Locked;
	result.statsClickable = !locked;
	result.presence = !locked;
	result.conversationCard = true;
	result.mutuals = true;
	return result;
}

QString NormalizeHandle(const QString &typed) {
	auto result = Lang::Latinize(typed.trimmed()).toLower();
	while (result.startsWith(QLatin1Char('@'))) {
		result.remove(0, 1);
	}
	return result;
}

HandleState ClassifyHandle(const QString &handle, const QString &current) {
	static const auto kValid = QRegularExpression(u"^[a-z0-9._]{3,24}$"_s);
	if (handle == current) {
		return HandleState::Idle;
	} else if (!kValid.match(handle).hasMatch()) {
		return HandleState::Invalid;
	}
	auto letters = 0;
	for (const auto ch : handle) {
		if (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) {
			++letters;
		}
	}
	return (letters < 3) ? HandleState::Short : HandleState::Checking;
}

bool HandleBlocksSave(HandleState state) {
	switch (state) {
	case HandleState::Invalid:
	case HandleState::Short:
	case HandleState::Taken:
	case HandleState::Checking: return true;
	case HandleState::Idle:
	case HandleState::Unknown:
	case HandleState::Free: break;
	}
	return false;
}

bool HandleIsError(HandleState state) {
	return (state == HandleState::Invalid)
		|| (state == HandleState::Short)
		|| (state == HandleState::Taken);
}

HandleChecker::HandleChecker(QObject *parent, int delayMs)
: QObject(parent)
, _timer([this] { check(); })
, _delay(delayMs) {
}

void HandleChecker::setRequest(Request request) {
	_request = std::move(request);
}

void HandleChecker::setChanged(Fn<void()> changed) {
	_changed = std::move(changed);
}

void HandleChecker::setCurrent(const QString &handle) {
	_current = handle;
	if (_value.isEmpty()) {
		_value = handle;
	}
}

void HandleChecker::setValue(const QString &handle) {
	if (_value == handle) {
		return;
	}
	_value = handle;
	++_generation;
	_timer.cancel();
	const auto state = ClassifyHandle(handle, _current);
	setState(state);
	if (state == HandleState::Checking) {
		_timer.callOnce(_delay);
	}
}

void HandleChecker::markTaken() {
	++_generation;
	_timer.cancel();
	setState(HandleState::Taken);
}

void HandleChecker::setState(HandleState state) {
	if (_state == state) {
		return;
	}
	_state = state;
	if (_changed) {
		_changed();
	}
}

void HandleChecker::check() {
	if (_state != HandleState::Checking) {
		return;
	} else if (!_request) {
		setState(HandleState::Unknown);
		return;
	}
	const auto generation = _generation;
	const auto weak = QPointer<HandleChecker>(this);
	_request(_value, [=](Answer answer) {
		// An answer for something that was typed over is dropped.
		if (!weak || weak->_generation != generation) {
			return;
		}
		weak->setState((answer == Answer::Free)
			? HandleState::Free
			: (answer == Answer::Taken)
			? HandleState::Taken
			: HandleState::Unknown);
	});
}

Ui::Text::TextWithEntities ParseBio(const QString &bio) {
	using namespace Ui::Text;
	auto result = ParseEntities(bio, ParseAll);
	// Handles may contain dots and underscores ("@ali.r"): the generic mention
	// parser stops at the first dot, so the range is extended here.
	const auto &text = result.text;
	const auto allowed = [](QChar ch) {
		return (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
			|| (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
			|| (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
			|| (ch == QLatin1Char('_'))
			|| (ch == QLatin1Char('.'));
	};
	for (auto i = 0; i != result.entities.size(); ++i) {
		auto &entity = result.entities[i];
		if (entity.type != EntityType::Mention) {
			continue;
		}
		auto till = entity.offset + entity.length;
		const auto limit = (i + 1 < result.entities.size())
			? std::min(int(text.size()), result.entities[i + 1].offset)
			: int(text.size());
		while (till < limit && allowed(text[till])) {
			++till;
		}
		// A sentence may end right after the handle: "… @sara."
		while (till > entity.offset + entity.length
			&& text[till - 1] == QLatin1Char('.')) {
			--till;
		}
		entity.length = till - entity.offset;
	}
	// "@" and "#" are neutral characters: inside a line of the other direction
	// they would jump to the far side of their word ("sara@"). A direction
	// mark in front of each ties the sign to its word.
	auto marked = QString();
	marked.reserve(text.size() + result.entities.size());
	auto from = 0;
	auto shift = 0;
	for (auto &entity : result.entities) {
		const auto offset = entity.offset;
		const auto sign = (entity.type == EntityType::Mention)
			|| (entity.type == EntityType::Hashtag);
		if (sign && offset >= from && entity.length > 1) {
			marked += QStringView(text).mid(from, offset - from);
			const auto rtl = (text[offset + 1].direction() == QChar::DirR)
				|| (text[offset + 1].direction() == QChar::DirAL);
			marked += QChar(rtl ? 0x200F : 0x200E);
			from = offset;
			++shift;
		}
		entity.offset += shift;
	}
	marked += QStringView(text).mid(from);
	result.text = marked;
	return result;
}

QString NormalizeLink(const QString &typed) {
	const auto value = typed.trimmed();
	if (value.isEmpty()) {
		return QString();
	}
	static const auto kScheme = QRegularExpression(
		u"^https?://"_s,
		QRegularExpression::CaseInsensitiveOption);
	return kScheme.match(value).hasMatch() ? value : (u"https://"_s + value);
}

bool IsValidLink(const QString &typed) {
	const auto value = typed.trimmed();
	if (value.isEmpty()) {
		return true;
	}
	for (const auto ch : value) {
		if (ch.isSpace()) {
			return false;
		}
	}
	static const auto kScheme = QRegularExpression(
		u"^https?://"_s,
		QRegularExpression::CaseInsensitiveOption);
	auto host = value;
	host.remove(kScheme);
	if (host.contains(u"://"_s)) {
		return false; // another scheme
	}
	host = host.section(QLatin1Char('/'), 0, 0);
	static const auto kHost = QRegularExpression(u"^[^.]+\\.[^.]+"_s);
	return kHost.match(host).hasMatch();
}

QString DisplayLink(const QString &link) {
	static const auto kScheme = QRegularExpression(
		u"^https?://"_s,
		QRegularExpression::CaseInsensitiveOption);
	auto result = link.trimmed();
	result.remove(kScheme);
	while (result.endsWith(QLatin1Char('/'))) {
		result.chop(1);
	}
	return result;
}

QString LtrText(const QString &text) {
	return QString(QChar(0x200E)) + text + QChar(0x200E);
}

QString ProfileLink(const QString &handle) {
	return u"https://eleza.me/u/"_s + handle;
}

QString MutualsText(const QStringList &names, int total) {
	if (names.isEmpty()) {
		return QString();
	}
	const auto count = std::max(total, int(names.size()));
	if (names.size() >= 2 && count > 2) {
		return tr::lng_profile_mutuals_more(
			tr::now,
			lt_a,
			names[0],
			lt_b,
			names[1],
			lt_n,
			Lang::Num(count - 2));
	} else if (names.size() >= 2) {
		return tr::lng_profile_mutuals_two(
			tr::now,
			lt_a,
			names[0],
			lt_b,
			names[1]);
	}
	return tr::lng_profile_mutuals_one(tr::now, lt_a, names[0]);
}

QString ProvinceTitle(const QString &code) {
	using Phrase = QString(*)(tr::now_t);
	static const Phrase kTitles[] = {
		tr::lng_profile_province_01,
		tr::lng_profile_province_02,
		tr::lng_profile_province_03,
		tr::lng_profile_province_04,
		tr::lng_profile_province_05,
		tr::lng_profile_province_06,
		tr::lng_profile_province_07,
		tr::lng_profile_province_08,
		tr::lng_profile_province_09,
		tr::lng_profile_province_10,
		tr::lng_profile_province_11,
		tr::lng_profile_province_12,
		tr::lng_profile_province_13,
		tr::lng_profile_province_14,
		tr::lng_profile_province_15,
		tr::lng_profile_province_16,
		tr::lng_profile_province_17,
		tr::lng_profile_province_18,
		tr::lng_profile_province_19,
		tr::lng_profile_province_20,
		tr::lng_profile_province_21,
		tr::lng_profile_province_22,
		tr::lng_profile_province_23,
		tr::lng_profile_province_24,
		tr::lng_profile_province_25,
		tr::lng_profile_province_26,
		tr::lng_profile_province_27,
		tr::lng_profile_province_28,
		tr::lng_profile_province_29,
		tr::lng_profile_province_30,
		tr::lng_profile_province_31,
	};
	if (code.size() == 5 && code.startsWith(u"IR-"_s)) {
		auto ok = false;
		const auto index = code.mid(3).toInt(&ok);
		if (ok && index >= 1 && index <= 31) {
			return kTitles[index - 1](tr::now);
		}
	}
	return code;
}

QString RemainingText(qint64 expiresAtSeconds, qint64 nowMs) {
	const auto left = expiresAtSeconds * 1000 - nowMs;
	if (left <= 0) {
		return tr::lng_ended(tr::now);
	}
	const auto minutes = (left + 59'999) / 60'000;
	if (minutes >= 60 * 24 * 2) {
		return tr::lng_conv_days_left(
			tr::now,
			lt_count,
			double(minutes / (60 * 24)));
	} else if (minutes >= 60) {
		return tr::lng_conv_hours_left(tr::now, lt_count, double(minutes / 60));
	}
	return tr::lng_conv_minutes_left(tr::now, lt_count, double(minutes));
}

bool IsJalali(const QString &calendar) {
	return calendar != kGregorian;
}

QString DefaultCalendar() {
	return Lang::IsPersian() ? QString(kJalali) : QString(kGregorian);
}

QString MonthName(const QString &calendar, int month) {
	return IsJalali(calendar)
		? Lang::JalaliMonthName(month)
		: Lang::GregorianMonthName(month);
}

QString BirthdayText(const Data::Birthday &birthday) {
	const auto month = MonthName(birthday.calendar, birthday.month);
	const auto day = Lang::Num(birthday.day);
	if (Lang::IsPersian()) {
		auto result = day + QLatin1Char(' ') + month;
		if (birthday.year > 0) {
			result += QLatin1Char(' ') + Lang::Num(birthday.year);
		}
		return result;
	}
	auto result = month + QLatin1Char(' ') + day;
	if (birthday.year > 0) {
		result += u", "_s + Lang::Num(birthday.year);
	}
	return result;
}

bool SameBirthday(
		const std::optional<Data::Birthday> &a,
		const std::optional<Data::Birthday> &b) {
	if (!a || !b) {
		return !a && !b;
	}
	return (IsJalali(a->calendar) == IsJalali(b->calendar))
		&& (a->day == b->day)
		&& (a->month == b->month)
		&& (a->year == b->year);
}

int MonthDays(const QString &calendar, int year, int month) {
	if (month < 1 || month > 12) {
		return 0;
	}
	if (IsJalali(calendar)) {
		if (month <= 6) {
			return 31;
		} else if (month <= 11) {
			return 30;
		}
		return (year <= 0 || Lang::IsJalaliLeapYear(year)) ? 30 : 29;
	}
	static constexpr int kDays[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	if (month == 2) {
		return (year <= 0 || IsGregorianLeap(year)) ? 29 : 28;
	}
	return kDays[month - 1];
}

YearRange BirthYears(const QString &calendar, const QDate &today) {
	const auto current = IsJalali(calendar)
		? Lang::ToJalali(today).year
		: today.year();
	return { current - kBirthdayMinAge, current - kBirthdayMaxAge };
}

Data::Birthday ClampBirthday(Data::Birthday birthday) {
	birthday.month = std::clamp(birthday.month, 1, 12);
	birthday.day = std::clamp(
		birthday.day,
		1,
		MonthDays(birthday.calendar, birthday.year, birthday.month));
	return birthday;
}

bool IsValidBirthday(const Data::Birthday &birthday, const QDate &today) {
	if (birthday.month < 1
		|| birthday.month > 12
		|| birthday.day < 1
		|| birthday.day > MonthDays(
			birthday.calendar,
			birthday.year,
			birthday.month)) {
		return false;
	} else if (birthday.year <= 0) {
		return birthday.year == 0;
	}
	const auto range = BirthYears(birthday.calendar, today);
	return (birthday.year <= range.from) && (birthday.year >= range.till);
}

Data::Birthday ConvertBirthday(
		const Data::Birthday &birthday,
		const QString &calendar,
		const QDate &today) {
	auto result = birthday;
	result.calendar = calendar;
	if (IsJalali(birthday.calendar) == IsJalali(calendar)) {
		return result;
	}
	const auto withYear = (birthday.year > 0);
	if (IsJalali(calendar)) {
		const auto year = withYear ? birthday.year : today.year();
		auto date = QDate(year, birthday.month, birthday.day);
		if (!date.isValid()) {
			// February 29 of a year that has none.
			date = QDate(year, birthday.month, 1).addMonths(1).addDays(-1);
		}
		const auto jalali = Lang::ToJalali(date);
		result.month = jalali.month;
		result.day = jalali.day;
		result.year = withYear ? jalali.year : 0;
	} else {
		const auto year = withYear
			? birthday.year
			: Lang::ToJalali(today).year;
		auto date = Lang::JalaliToGregorian(year, birthday.month, birthday.day);
		if (!date.isValid()) {
			// Esfand 30 of a common year.
			date = Lang::JalaliToGregorian(year, birthday.month, 29);
		}
		result.month = date.month();
		result.day = date.day();
		result.year = withYear ? date.year() : 0;
	}
	return result;
}

} // namespace Profile
