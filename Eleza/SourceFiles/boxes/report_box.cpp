#include "boxes/report_box.h"

#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"

namespace Boxes {
namespace {

constexpr auto kNoteMaxChars = 500;

[[nodiscard]] QString ReasonLabel(const QString &slug) {
	if (slug == u"spam") {
		return tr::lng_eli_reason_spam(tr::now);
	} else if (slug == u"harassment") {
		return tr::lng_eli_reason_harassment(tr::now);
	} else if (slug == u"hate") {
		return tr::lng_eli_reason_hate(tr::now);
	} else if (slug == u"violence") {
		return tr::lng_eli_reason_violence(tr::now);
	} else if (slug == u"sexual") {
		return tr::lng_eli_reason_sexual(tr::now);
	} else if (slug == u"self_harm") {
		return tr::lng_eli_reason_self_harm(tr::now);
	} else if (slug == u"misinformation") {
		return tr::lng_eli_reason_misinformation(tr::now);
	} else if (slug == u"impersonation") {
		return tr::lng_eli_reason_impersonation(tr::now);
	} else if (slug == u"other") {
		return tr::lng_eli_reason_other(tr::now);
	}
	// A reason this build has no wording for: its slug, readable.
	auto readable = slug;
	readable.replace(QChar('_'), QChar(' '));
	return readable;
}

// The reasons every server knows, for when the list could not be read.
[[nodiscard]] QStringList DefaultReasons() {
	return {
		u"spam"_s,
		u"harassment"_s,
		u"hate"_s,
		u"violence"_s,
		u"sexual"_s,
		u"self_harm"_s,
		u"misinformation"_s,
		u"impersonation"_s,
		u"other"_s,
	};
}

[[nodiscard]] QString Question(const QString &kind) {
	if (kind == u"reply") {
		return tr::lng_eli_report_q_reply(tr::now);
	} else if (kind == u"account") {
		return tr::lng_eli_report_q_account(tr::now);
	} else if (kind == u"conversation") {
		return tr::lng_eli_report_q_conversation(tr::now);
	}
	return tr::lng_eli_report_q_post(tr::now);
}

void Show(
		Window::SessionController *controller,
		const ReportArgs &args,
		const QStringList &reasons) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_report(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, Question(args.kind), st::boxLabel),
			st::boxRowPadding);
		const auto group = std::make_shared<Ui::RadiobuttonGroup>();
		for (auto i = 0; i != int(reasons.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(
					box,
					group,
					i,
					ReasonLabel(reasons[i])),
				st::boxCheckboxPadding);
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_eli_report_anonymous(tr::now),
				st::captionLabel),
			st::boxRowPadding);
		const auto note = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_report_details(tr::now)));
		note->setMaxLength(kNoteMaxChars);
		note->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
		const auto offerBlock = !args.offerBlockUserId.isEmpty()
			&& (args.kind != u"conversation")
			&& (args.offerBlockUserId != controller->session().userId());
		const auto block = offerBlock
			? box->addRow(
				Ui::object_ptr<Ui::Checkbox>(
					box,
					tr::lng_report_also_block(tr::now)),
				st::boxCheckboxPadding)
			: nullptr;
		const auto sending = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_report_send(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setDisabled(true);
		group->setChangedCallback([=](int) {
			submit->setDisabled(false);
		});
		const auto send = [=] {
			if (!group->hasValue() || *sending) {
				return;
			}
			*sending = true;
			submit->setLoading(true);
			const auto alsoBlock = block && block->checked();
			controller->session().data().feed().sendReport(
				args.kind,
				args.id,
				reasons.value(group->value()),
				note->text().trimmed(),
				base::Guard(box),
				[=] {
					if (alsoBlock) {
						const auto blocked = args.blocked;
						controller->session().data().graph().block(
							args.offerBlockUserId,
							controller,
							[=](const Data::GraphState &) {
								if (blocked) {
									blocked();
								}
							},
							[=](const Api::Error &error) {
								Support::ShowError(controller, error);
							});
					}
					if (args.sent) {
						args.sent();
					}
					controller->showToast(tr::lng_report_sent(tr::now));
					box->closeBox();
				},
				[=](const Api::Error &error) {
					*sending = false;
					submit->setLoading(false);
					Support::ShowError(
						controller,
						error,
						tr::lng_eli_report_failed(tr::now));
				});
		};
		submit->setClickedCallback(send);
		QObject::connect(note, &Ui::InputField::submitted, box, send);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

} // namespace

void ShowReportBox(Window::SessionController *controller, ReportArgs args) {
	controller->session().data().feed().reportReasons(
		base::Guard(controller),
		[=](const Data::ReportReasons &all) {
			const auto &list = (args.kind == u"reply")
				? all.reply
				: (args.kind == u"account")
				? all.account
				: (args.kind == u"conversation")
				? all.conversation
				: all.post;
			Show(controller, args, list.isEmpty() ? DefaultReasons() : list);
		},
		[=](const Api::Error &) {
			// The form still works with the reasons every server knows.
			Show(controller, args, DefaultReasons());
		});
}

} // namespace Boxes
