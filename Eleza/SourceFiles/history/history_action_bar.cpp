#include "history/history_action_bar.h"

#include "data/data_chats.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"
#include "styles/style_layers.h"

#include <QtCore/QPointer>

namespace History {
namespace {

using Data::ActionBar::Kind;
using Data::ActionBar::Outcome;
using Data::ActionBar::Step;

// What the toast says after a choice went (at least partly) through.
[[nodiscard]] QString OutcomeText(
		const Outcome &outcome,
		const QVector<Step> &plan,
		const QString &name) {
	const auto planned = [&](Step step) {
		return plan.contains(step);
	};
	const auto reported = outcome.did(Step::ReportChat) || outcome.did(Step::ReportGroup);
	const auto reportFailed = outcome.failedAt(Step::ReportChat)
		|| outcome.failedAt(Step::ReportGroup);
	const auto who = Lang::Isolated(name);
	if (planned(Step::Leave)) {
		if (outcome.did(Step::Leave)) {
			if (reportFailed) {
				return tr::lng_history_abar_left_not_reported(tr::now);
			} else if (outcome.failedAt(Step::Block)) {
				return tr::lng_history_abar_left_not_blocked(tr::now, lt_name, who);
			}
			return reported
				? tr::lng_history_abar_reported_left(tr::now)
				: tr::lng_groups_left_toast(tr::now);
		}
		return reported ? tr::lng_history_abar_reported_not_left(tr::now) : QString();
	}
	if (outcome.did(Step::Block)) {
		return reported
			? tr::lng_history_abar_reported_blocked(tr::now, lt_name, who)
			: reportFailed
			? tr::lng_history_abar_blocked_not_reported(tr::now)
			: tr::lng_blocked_toast(tr::now);
	} else if (reported) {
		return outcome.failedAt(Step::Block)
			? tr::lng_history_abar_reported_not_blocked(tr::now, lt_name, who)
			: tr::lng_report_sent(tr::now);
	}
	return QString();
}

} // namespace

ActionBar::ActionBar(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &chatId)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _chatId(chatId) {
	_first = Ui::CreateChild<Ui::RoundButton>(
		this,
		QString(),
		st::historyActionBarDanger);
	_second = Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_history_abar_report(tr::now),
		st::historyActionBarButton);
	_close = Ui::CreateChild<Ui::IconButton>(this, st::historyBarClose);
	Ui::InstallTooltip(_close, [] { return tr::lng_history_abar_close(tr::now); });
	_first->setClickedCallback([this] {
		if (_kind == Kind::AddedByStranger) {
			chooseLeave();
		} else {
			chooseBlock();
		}
	});
	_second->setClickedCallback([this] { chooseReport(); });
	_close->setClickedCallback([this] { chooseClose(); });

	auto &data = _session->data();
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](const QString &id) {
		if (id == _chatId) {
			refresh();
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this](const QStringList &ids) {
		if (!_userId.isEmpty() && ids.contains(_userId)) {
			refresh();
		}
	});
	hide();
	refresh();
}

void ActionBar::refresh() {
	const auto chat = _session->data().chats().chat(_chatId);
	const auto kind = (chat && !_acting)
		? Data::ActionBar::For(*chat, _session->userId())
		: Kind::None;
	_userId = (kind == Kind::AddedByStranger)
		? chat->group->addedBy
		: (kind == Kind::Stranger)
		? chat->peerUserId
		: QString();
	_line = (kind == Kind::AddedByStranger)
		? tr::lng_history_abar_added_you(tr::now, lt_name, Lang::Isolated(otherName()))
		: QString();
	_first->setText((kind == Kind::AddedByStranger)
		? tr::lng_groups_leave(tr::now)
		: tr::lng_history_abar_block(tr::now));
	const auto changed = (kind != _kind);
	_kind = kind;
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
	if (changed) {
		setVisible(shown());
		Q_EMIT shownChanged();
	}
}

QString ActionBar::otherName() const {
	if (_kind == Kind::Stranger || _userId.isEmpty()) {
		if (const auto chat = _session->data().chats().chat(_chatId)) {
			if (!chat->isGroup) {
				return Support::ChatTitle(_session, *chat);
			}
		}
	}
	return Support::MemberName(_session, _userId);
}

int ActionBar::resizeGetHeight(int newWidth) {
	const auto group = (_kind == Kind::AddedByStranger);
	const auto height = group
		? int(st::historyActionBarGroupHeight)
		: int(st::historyBarHeight);
	_close->moveToRight(
		st::historyActionBarClosePosition.x(),
		group
			? st::historyActionBarClosePosition.y()
			: (height - _close->height()) / 2,
		newWidth);
	// Two halves of the width the × leaves, a button in the middle of each.
	const auto skip = _close->width() + st::historyActionBarClosePosition.x();
	const auto half = std::max((newWidth - 2 * skip) / 2, 1);
	const auto buttonsTop = group
		? (height - st::historyActionBarButtonsBottom - _first->height())
		: (height - _first->height()) / 2;
	const auto place = [&](Ui::RoundButton *button, int index) {
		button->setFullWidth(0);
		const auto natural = button->naturalWidth();
		if (natural > half) {
			button->setFullWidth(half);
		}
		const auto width = std::min(natural, half);
		button->moveToLeft(skip + index * half + (half - width) / 2, buttonsTop, newWidth);
	};
	place(_first, 0);
	place(_second, 1);
	return height;
}

void ActionBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::historyBarBg);
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::historyBarLineFg);
	if (_line.isEmpty()) {
		return;
	}
	const auto &font = st::historyActionBarLineFont;
	const auto skip = _close->width() + st::historyActionBarClosePosition.x();
	const auto available = std::max(width() - 2 * skip, 1);
	p.setFont(font);
	p.setPen(st::historyActionBarLineFg);
	p.drawText(
		QRect(skip, st::historyActionBarLineTop, available, font->height),
		Qt::AlignCenter,
		font->elided(_line, available));
}

void ActionBar::chooseBlock() {
	const auto name = otherName();
	const auto report = std::make_shared<bool>(false);
	const auto weak = QPointer<ActionBar>(this);
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_history_abar_block_title(tr::now, lt_name, Lang::Isolated(name));
	args.text = tr::lng_history_abar_block_text(tr::now, lt_name, Lang::Isolated(name));
	args.confirmText = tr::lng_history_abar_block_confirm(tr::now);
	args.destructive = true;
	args.checkboxText = tr::lng_history_abar_also_report(tr::now);
	args.checkboxChecked = false;
	args.checkboxChanged = [=](bool checked) {
		*report = checked;
	};
	args.confirmed = [=](std::function<void()> close) {
		close();
		if (weak) {
			weak->run(Data::ActionBar::ChatBlockPlan(*report));
		}
	};
	_controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ActionBar::chooseReport() {
	const auto weak = QPointer<ActionBar>(this);
	if (_kind == Kind::Stranger) {
		auto args = Ui::ConfirmBoxArgs();
		args.title = tr::lng_history_abar_report_title(tr::now);
		args.text = tr::lng_history_abar_report_chat_text(
			tr::now,
			lt_name,
			Lang::Isolated(otherName()));
		args.confirmText = tr::lng_history_abar_report_chat_confirm(tr::now);
		args.destructive = true;
		args.confirmed = [=](std::function<void()> close) {
			close();
			if (weak) {
				weak->run(Data::ActionBar::ChatReportPlan());
			}
		};
		_controller->show(Ui::MakeConfirmBox(std::move(args)));
		return;
	}
	// A group: «ترک گروه» comes ticked (report spam and leave).
	const auto name = otherName();
	const auto defaults = Data::ActionBar::GroupReportDefaults();
	_controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_history_abar_report_title(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_history_abar_report_group_text(tr::now),
				st::boxLabel));
		const auto leave = box->addRow(
			Ui::object_ptr<Ui::Checkbox>(
				box,
				tr::lng_groups_leave(tr::now),
				defaults.leave),
			st::boxCheckboxPadding);
		const auto block = box->addRow(
			Ui::object_ptr<Ui::Checkbox>(
				box,
				tr::lng_history_abar_also_block(tr::now, lt_name, Lang::Isolated(name)),
				defaults.block),
			st::boxCheckboxPadding);
		box->addButton(tr::lng_history_abar_report_confirm(tr::now), [=] {
			const auto plan = Data::ActionBar::GroupReportPlan(
				leave->checked(),
				block->checked());
			box->closeBox();
			if (weak) {
				weak->run(plan);
			}
		}, st::dangerRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ActionBar::chooseLeave() {
	const auto weak = QPointer<ActionBar>(this);
	const auto name = otherName();
	_controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_history_abar_leave_title(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_history_abar_leave_text(tr::now),
				st::boxLabel));
		const auto block = box->addRow(
			Ui::object_ptr<Ui::Checkbox>(
				box,
				tr::lng_history_abar_also_block(tr::now, lt_name, Lang::Isolated(name)),
				false),
			st::boxCheckboxPadding);
		const auto report = box->addRow(
			Ui::object_ptr<Ui::Checkbox>(
				box,
				tr::lng_history_abar_report(tr::now),
				false),
			st::boxCheckboxPadding);
		box->addButton(tr::lng_groups_leave(tr::now), [=] {
			const auto plan = Data::ActionBar::GroupLeavePlan(
				block->checked(),
				report->checked());
			box->closeBox();
			if (weak) {
				weak->run(plan);
			}
		}, st::dangerRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ActionBar::chooseClose() {
	// Kept on this device until the server has it (Data::Chats).
	_session->data().chats().dismissActionBar(_chatId);
}

// The bar goes at once; it comes back only when nothing went through (the
// server still shows it then). Whatever went through, the server lowered the
// bar by itself. The calls outlive this screen (a leave closes it): they are
// guarded by the window, which says how it went.
void ActionBar::run(QVector<Step> plan) {
	const auto session = _session;
	const auto window = _controller;
	const auto chatId = _chatId;
	const auto userId = _userId;
	const auto name = otherName();
	const auto group = (_kind == Kind::AddedByStranger);
	if (plan.contains(Step::Leave)) {
		Q_EMIT leaving(true);
	}
	_acting = true;
	refresh();
	const auto weak = QPointer<ActionBar>(this);
	const auto guard = QPointer<Window::SessionController>(window);
	Data::ActionBar::Run(plan, [=](
			Step step,
			Fn<void(std::optional<Api::Error>)> finished) {
		if (!guard) {
			return;
		}
		auto &data = session->data();
		const auto ok = [=] { finished(std::nullopt); };
		const auto fail = [=](const Api::Error &error) { finished(error); };
		switch (step) {
		case Step::ReportChat:
		case Step::ReportGroup:
			data.feed().sendReport(
				(step == Step::ReportGroup) ? u"group"_s : u"message"_s,
				chatId,
				u"spam"_s,
				QString(),
				window,
				ok,
				fail);
			break;
		case Step::Block:
			data.graph().block(userId, window, [=](const Data::GraphState &) {
				ok();
			}, fail);
			break;
		case Step::Leave:
			data.groups().leave(chatId, window, ok, fail);
			break;
		}
	}, [=](const Outcome &outcome) {
		if (!guard) {
			return;
		}
		if (weak && plan.contains(Step::Leave) && !outcome.did(Step::Leave)) {
			Q_EMIT weak->leaving(false);
		}
		if (outcome.nothingDone()) {
			// The server still has the bar: so do we, and the header is read
			// again in case it changed meanwhile.
			if (weak) {
				weak->_acting = false;
				weak->refresh();
			}
			if (group) {
				session->data().groups().refreshHeader(chatId);
			}
			if (outcome.error) {
				Support::ShowError(window, *outcome.error);
			}
			return;
		}
		session->data().chats().forgetActionBar(chatId);
		if (weak) {
			weak->_acting = false;
			weak->refresh();
		}
		const auto text = OutcomeText(outcome, plan, name);
		if (!text.isEmpty()) {
			window->showToast(text);
		} else if (outcome.error) {
			Support::ShowError(window, *outcome.error);
		}
	});
}

} // namespace History
