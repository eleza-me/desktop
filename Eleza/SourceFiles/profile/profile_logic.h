// The widget-free part of the profile screens: relationship states and the
// buttons they give, the follow toggle, handle validation with its debounced
// availability check, bio / link / birthday helpers and the date picker ranges.
// Compiled into the unit tests on its own (tests/app/test_profile_logic.cpp).
#pragma once

#include "base/timer.h"
#include "data/data_social_types.h"
#include "ui/text/text_entity.h"

#include <QtCore/QDate>
#include <QtCore/QObject>

namespace Profile {

// ---- follow state

enum class FollowUi {
	None, // «دنبال کردن»
	Following, // «دنبال می‌کنی»
	Requested, // «درخواست فرستاده شد»
	FollowsYou, // «تو هم دنبال کن»
};

[[nodiscard]] FollowUi FollowUiOf(const QString &followState, bool followsYou);
[[nodiscard]] FollowUi FollowUiOf(const Data::Profile &profile);
[[nodiscard]] FollowUi FollowUiOf(const Data::GraphState &state);

// How many followers the other side gains when my state goes `from` -> `to`.
[[nodiscard]] int FollowersDelta(FollowUi from, FollowUi to);

struct FollowToggle {
	bool follow = false; // the request to send: follow or unfollow
	FollowUi optimistic = FollowUi::None; // shown until the server answers
	bool confirm = false; // ask «دیگر … را دنبال نکنی؟» first
};
// What a click on the follow button does in `current`.
[[nodiscard]] FollowToggle ToggleFollow(
	FollowUi current,
	bool isPrivate,
	bool followsYou);

enum class FollowLook {
	Filled, // the main action
	Outline, // already following
	Soft, // request pending
};
[[nodiscard]] FollowLook FollowLookOf(FollowUi state);

// ---- the profile header

enum class Action {
	EditProfile,
	ShareProfile,
	Follow,
	Message,
	More,
};

enum class MenuEntry {
	PostNotificationsOn,
	PostNotificationsOff,
	ShareProfile,
	CopyLink,
	Mute,
	Unmute,
	RemoveFollower,
	Report,
	Block,
	Unblock,
};

enum class Content {
	Tabs, // the post lists
	Locked, // a private account I do not follow
	LockedRequested, // … and my request is pending
	Blocked, // I blocked them
	Unblocked, // I just unblocked them: «دیدن نمایه»
	Deleted,
	None, // not loaded
};

struct ViewerState {
	bool loaded = false;
	bool me = false;
	bool deleted = false;
	bool blocked = false; // by me
	bool justUnblocked = false;
	bool isPrivate = false;
	bool followsYou = false;
	bool muted = false;
	bool postNotifications = false;
	bool hasHandle = true;
	FollowUi follow = FollowUi::None;
};

struct ButtonModel {
	QVector<Action> actions;
	QVector<MenuEntry> menu;
	Content content = Content::None;
	bool statsClickable = false; // followers / following open the lists
	bool bookmarksTab = false;
	bool presence = false; // the «آنلاین» line may be shown
	bool conversationCard = false;
	bool mutuals = false;

	[[nodiscard]] bool has(Action action) const {
		return actions.contains(action);
	}
	[[nodiscard]] bool has(MenuEntry entry) const {
		return menu.contains(entry);
	}
};

[[nodiscard]] ButtonModel MakeButtonModel(const ViewerState &state);

// ---- handle

enum class HandleState {
	Idle, // unchanged
	Invalid, // not [a-z0-9._]{3,24}
	Short, // fewer than three letters
	Checking,
	Unknown, // the check failed: saving will tell
	Taken,
	Free,
};

// What the field holds for something typed: trimmed, lower case, without the
// leading "@", Persian / Arabic digits as Latin ones.
[[nodiscard]] QString NormalizeHandle(const QString &typed);
// Idle, Invalid, Short or Checking (= worth asking the server).
[[nodiscard]] HandleState ClassifyHandle(
	const QString &handle,
	const QString &current);
[[nodiscard]] bool HandleBlocksSave(HandleState state);
[[nodiscard]] bool HandleIsError(HandleState state);

// The debounced availability check of the handle field.
//
//   _checker.setRequest([=](const QString &handle, Fn<void(HandleChecker::Answer)> done) { … });
//   _checker.setChanged([=] { refreshHint(_checker.state()); });
//   _checker.setCurrent(me.handle);
//   _checker.setValue(NormalizeHandle(field->text()));
class HandleChecker final : public QObject {
public:
	enum class Answer {
		Free,
		Taken,
		Failed,
	};
	using Request = Fn<void(const QString &handle, Fn<void(Answer)> done)>;

	explicit HandleChecker(QObject *parent = nullptr, int delayMs = kDelayMs);

	void setRequest(Request request);
	void setChanged(Fn<void()> changed);
	// The handle the account has now (typing it again is Idle).
	void setCurrent(const QString &handle);
	void setValue(const QString &handle);
	// Saving said the handle is taken.
	void markTaken();

	[[nodiscard]] const QString &value() const {
		return _value;
	}
	[[nodiscard]] HandleState state() const {
		return _state;
	}

	static constexpr auto kDelayMs = 400;

private:
	void setState(HandleState state);
	void check();

	Request _request;
	Fn<void()> _changed;
	QString _current;
	QString _value;
	HandleState _state = HandleState::Idle;
	base::Timer _timer;
	int _delay = 0;
	int _generation = 0;

};

// ---- bio, link, text

// The bio with its @mentions, #tags and links as clickable ranges. A direction
// mark is put in front of every "@" / "#" so the sign stays with its word in a
// line of the other direction (the returned text is longer by those marks).
[[nodiscard]] Ui::Text::TextWithEntities ParseBio(const QString &bio);

// "sara.photo" -> "https://sara.photo"; empty stays empty.
[[nodiscard]] QString NormalizeLink(const QString &typed);
[[nodiscard]] bool IsValidLink(const QString &typed);
// Without the scheme and the trailing slash.
[[nodiscard]] QString DisplayLink(const QString &link);
// Handles and addresses read left-to-right also inside a Persian line: the
// text between two left-to-right marks (plain drawText does not keep isolates).
[[nodiscard]] QString LtrText(const QString &text);
// https://eleza.me/u/<handle>
[[nodiscard]] QString ProfileLink(const QString &handle);

// «سارا، علی و ۳ نفر دیگر که می‌شناسی دنبالش می‌کنند».
[[nodiscard]] QString MutualsText(const QStringList &names, int total);
// «تهران» for "IR-07"; the code itself when unknown.
[[nodiscard]] QString ProvinceTitle(const QString &code);
// «۲۳ ساعت مانده» / «۴۵ دقیقه مانده» / «تمام شد».
[[nodiscard]] QString RemainingText(qint64 expiresAtSeconds, qint64 nowMs);

// ---- birthday

[[nodiscard]] bool IsJalali(const QString &calendar);
// The calendar a new birthday is picked in: Jalali in Persian, Gregorian otherwise.
[[nodiscard]] QString DefaultCalendar();
[[nodiscard]] QString MonthName(const QString &calendar, int month);
// «۱۲ مهر ۱۳۷۰» / "March 4, 1991" (the year only when it is shared).
[[nodiscard]] QString BirthdayText(const Data::Birthday &birthday);
[[nodiscard]] bool SameBirthday(
	const std::optional<Data::Birthday> &a,
	const std::optional<Data::Birthday> &b);

// Days of a month; with `year` 0 (not shared) the longest the month can be.
[[nodiscard]] int MonthDays(const QString &calendar, int year, int month);

struct YearRange {
	int from = 0; // the newest year offered
	int till = 0; // the oldest
	[[nodiscard]] int count() const {
		return from - till + 1;
	}
};
inline constexpr auto kBirthdayMinAge = 13;
inline constexpr auto kBirthdayMaxAge = 110;
[[nodiscard]] YearRange BirthYears(const QString &calendar, const QDate &today);

// The same date with the day clamped to its month.
[[nodiscard]] Data::Birthday ClampBirthday(Data::Birthday birthday);
// A date that exists (and, with a year, lies inside BirthYears and not in the future).
[[nodiscard]] bool IsValidBirthday(
	const Data::Birthday &birthday,
	const QDate &today);
// The same day in the other calendar. Without a year the conversion uses
// `today`'s year (the two calendars drift by a day between years).
[[nodiscard]] Data::Birthday ConvertBirthday(
	const Data::Birthday &birthday,
	const QString &calendar,
	const QDate &today);

} // namespace Profile
