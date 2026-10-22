// The report / block bar under the header of a chat (Telegram's action
// bar), as plain functions: when it shows, what each choice does and in
// which order, and what a failure means. The contract is the backend's
// docs/report-block-bar.md (direct chats) and docs/groups.md §9.1 (groups):
//
//   direct chat   ChatInfo::actionBar "stranger": somebody I have no tie
//                 with wrote first. «مسدود کردن» (report too, if ticked,
//                 then block), «گزارش هرزنامه» (report, then block), ×.
//   group         Group::actionBar "added_by_stranger" with Group::addedBy:
//                 a stranger added me. «ترک گروه» (report, block who added
//                 me, leave — as ticked), «گزارش هرزنامه» (report, then
//                 leave and block as ticked; «ترک گروه» starts ticked), ×.
//
// A report needs the membership, so it always goes first and a leave last.
// The × (Data::Chats::dismissActionBar) is kept on this device until the
// server has it.
#pragma once

#include "api/api_error.h"
#include "data/data_types.h"

#include <optional>

namespace Data::ActionBar {

enum class Kind {
	None,
	Stranger,        // a direct chat: block / report spam
	AddedByStranger, // a group: leave / report spam («X added you»)
};

// The bar a row shows. The server already holds these rules; they are
// repeated so that a row it has not read again (blocked here a moment ago,
// a request, an ended conversation) shows none.
[[nodiscard]] Kind For(const Chat &chat, const QString &myId);

enum class Step {
	ReportChat,  // POST /v1/reports {target_kind: message, target: <chat>, reason: spam}
	ReportGroup, // POST /v1/reports {target_kind: group, target: <group>, reason: spam}
	Block,       // POST /v1/users/{peer | added_by}/block
	Leave,       // POST /v1/groups/{id}/leave
};

[[nodiscard]] QVector<Step> ChatBlockPlan(bool alsoReport);
[[nodiscard]] QVector<Step> ChatReportPlan();
[[nodiscard]] QVector<Step> GroupLeavePlan(bool block, bool report);
[[nodiscard]] QVector<Step> GroupReportPlan(bool leave, bool block);

// How the boxes of a group's «گزارش هرزنامه» start: «ترک گروه» ticked (as
// Telegram's "report spam and leave"), «مسدود کردن X» not.
struct GroupReportChoice {
	bool leave = true;
	bool block = false;
};
[[nodiscard]] GroupReportChoice GroupReportDefaults();

// A report answered `report.duplicate`: it was there already — done.
[[nodiscard]] bool ReportAlreadyDone(const Api::Error &error);
// The × failed: whether it waits to be sent again (no answer, the server's
// own trouble, too many calls, a session being renewed) rather than be given
// up (not a member any more, or any other refusal that would not change).
[[nodiscard]] bool DismissWaits(const Api::Error &error);

struct Outcome {
	QVector<Step> done;
	QVector<Step> failed;
	std::optional<Api::Error> error; // the first failure

	[[nodiscard]] bool did(Step step) const {
		return done.contains(step);
	}
	[[nodiscard]] bool failedAt(Step step) const {
		return failed.contains(step);
	}
	// Nothing went through: the server still shows the bar, and so do we.
	[[nodiscard]] bool nothingDone() const {
		return done.isEmpty();
	}
};

// One call of a plan; `finished` with the error, or nullopt when it went
// through.
using StepCall = Fn<void(Step step, Fn<void(std::optional<Api::Error>)> finished)>;

// Runs the steps one after the other. A failure never stops the steps after
// it: the report that comes with a block or a leave is best effort, and a
// block that failed does not keep me in a group I chose to leave. A
// duplicate report counts as done. `done` gets what went through.
void Run(QVector<Step> plan, StepCall call, Fn<void(const Outcome &)> done);

} // namespace Data::ActionBar
