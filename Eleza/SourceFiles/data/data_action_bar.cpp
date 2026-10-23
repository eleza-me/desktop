#include "data/data_action_bar.h"

#include <memory>

namespace Data::ActionBar {
namespace {

struct RunState {
	QVector<Step> plan;
	StepCall call;
	Fn<void(const Outcome &)> done;
	Outcome outcome;
	int next = 0;
};

[[nodiscard]] bool IsReport(Step step) {
	return (step == Step::ReportChat) || (step == Step::ReportGroup);
}

void RunNext(const std::shared_ptr<RunState> &state) {
	if (state->next >= state->plan.size()) {
		if (state->done) {
			state->done(state->outcome);
		}
		return;
	}
	const auto step = state->plan[state->next++];
	state->call(step, [=](std::optional<Api::Error> error) {
		if (!error || (IsReport(step) && ReportAlreadyDone(*error))) {
			state->outcome.done.push_back(step);
		} else {
			state->outcome.failed.push_back(step);
			if (!state->outcome.error) {
				state->outcome.error = std::move(error);
			}
		}
		RunNext(state);
	});
}

} // namespace

Kind For(const Chat &chat, const QString &myId) {
	if (chat.isGroup) {
		if (!chat.group) {
			return Kind::None;
		}
		const auto &group = *chat.group;
		return (group.actionBar == u"added_by_stranger"
			&& !group.addedBy.isEmpty()
			&& group.addedBy != myId
			&& group.isMember())
			? Kind::AddedByStranger
			: Kind::None;
	}
	const auto state = chat.info.state.isEmpty() ? chat.state : chat.info.state;
	return (chat.info.actionBar == u"stranger"
		&& (state.isEmpty() || state == u"active")
		&& chat.request.isEmpty()
		&& chat.info.request.isEmpty()
		&& !chat.isConversation
		&& !chat.info.conversation
		&& !chat.saved
		&& !chat.isBot
		&& !chat.peerDeleted
		&& !chat.peerUserId.isEmpty()
		&& chat.peerUserId != myId)
		? Kind::Stranger
		: Kind::None;
}

QVector<Step> ChatBlockPlan(bool alsoReport) {
	auto result = QVector<Step>();
	if (alsoReport) {
		result.push_back(Step::ReportChat);
	}
	result.push_back(Step::Block);
	return result;
}

QVector<Step> ChatReportPlan() {
	return { Step::ReportChat, Step::Block };
}

QVector<Step> GroupLeavePlan(bool block, bool report) {
	auto result = QVector<Step>();
	if (report) {
		result.push_back(Step::ReportGroup);
	}
	if (block) {
		result.push_back(Step::Block);
	}
	result.push_back(Step::Leave);
	return result;
}

QVector<Step> GroupReportPlan(bool leave, bool block) {
	auto result = QVector<Step>{ Step::ReportGroup };
	if (block) {
		result.push_back(Step::Block);
	}
	if (leave) {
		result.push_back(Step::Leave);
	}
	return result;
}

GroupReportChoice GroupReportDefaults() {
	return GroupReportChoice();
}

bool ReportAlreadyDone(const Api::Error &error) {
	return (error.code == u"report.duplicate")
		|| (error.status == 409 && (error.code.isEmpty() || error.code.startsWith(u"report.")));
}

bool DismissWaits(const Api::Error &error) {
	return error.offline()
		|| error.status == 401
		|| error.status == 408
		|| error.rateLimited()
		|| error.serverSide();
}

void Run(QVector<Step> plan, StepCall call, Fn<void(const Outcome &)> done) {
	auto state = std::make_shared<RunState>();
	state->plan = std::move(plan);
	state->call = std::move(call);
	state->done = std::move(done);
	RunNext(state);
}

} // namespace Data::ActionBar
