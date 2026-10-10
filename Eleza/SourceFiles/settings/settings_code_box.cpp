#include "settings/settings_code_box.h"

#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QTimer>

namespace Settings {

void ShowCodeBox(Window::SessionController *controller, CodeBoxArgs args) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		struct State {
			qint64 resendAt = 0; // Ui::Now() milliseconds
			bool busy = false;
			bool resending = false;
		};
		const auto state = std::make_shared<State>();
		state->resendAt = Ui::Now() + qint64(args.resendAfter) * 1000;

		box->setTitle(args.title);
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			args.about,
			st::boxLabel));
		const auto code = box->addRow(
			Ui::object_ptr<Ui::CodeInput>(box, std::max(args.codeLength, 4)),
			st::settingsCodePadding,
			style::al_top);
		const auto error = box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, st::settingsErrorLabel),
			st::settingsErrorPadding);
		error->hide();
		const auto resend = args.resend
			? box->addRow(
				Ui::object_ptr<Ui::LinkButton>(
					box,
					tr::lng_intro_resend(tr::now)),
				st::boxRowPadding,
				style::al_left)
			: nullptr;

		const auto showError = [=](const QString &text) {
			error->setText(text);
			error->setVisible(!text.isEmpty());
		};
		const auto refreshResend = [=] {
			if (!resend) {
				return;
			}
			const auto left = (state->resendAt - Ui::Now() + 999) / 1000;
			const auto waiting = (left > 0);
			resend->setText(waiting
				? tr::lng_intro_resend_in(
					tr::now,
					lt_time,
					Lang::Duration(int(left)))
				: tr::lng_intro_resend(tr::now));
			resend->setDisabled(waiting || state->resending);
			resend->setColorOverride((waiting || state->resending)
				? std::optional<QColor>(st::settingsFootnote.textFg->c)
				: std::nullopt);
			resend->resizeToNaturalWidth(box->width());
		};
		if (resend) {
			const auto timer = new QTimer(box);
			timer->setInterval(500);
			QObject::connect(timer, &QTimer::timeout, box, refreshResend);
			timer->start();
			refreshResend();
			resend->setClickedCallback([=] {
				if (state->resending || state->resendAt > Ui::Now()) {
					return;
				}
				state->resending = true;
				showError(QString());
				refreshResend();
				const auto weak = QPointer<Ui::GenericBox>(box);
				args.resend([=](int resendAfter) {
					if (!weak) {
						return;
					}
					state->resending = false;
					state->resendAt = Ui::Now()
						+ qint64(std::max(resendAfter, 1)) * 1000;
					code->clear();
					code->setFocusFast();
					refreshResend();
					controller->showToast(
						tr::lng_intro_new_code_was_sent(tr::now));
				}, [=](const QString &text) {
					if (!weak) {
						return;
					}
					state->resending = false;
					showError(text);
					refreshResend();
				});
			});
		}

		const auto confirm = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			const auto value = code->code();
			if (state->busy) {
				return;
			} else if (value.size() < args.codeLength) {
				code->showError();
				return;
			}
			state->busy = true;
			showError(QString());
			if (*confirm) {
				(*confirm)->setLoading(true);
			}
			const auto weak = QPointer<Ui::GenericBox>(box);
			args.submit(value, [=] {
				if (weak) {
					weak->closeBox();
				}
			}, [=](const QString &text) {
				if (!weak) {
					return;
				}
				state->busy = false;
				if (*confirm) {
					(*confirm)->setLoading(false);
				}
				code->clear();
				code->showError();
				code->setFocusFast();
				showError(text);
			});
		};
		QObject::connect(code, &Ui::CodeInput::finished, box, [=] {
			submit();
		});
		QObject::connect(code, &Ui::CodeInput::codeChanged, box, [=](
				const QString &value) {
			if (!value.isEmpty()) {
				showError(QString());
			}
		});
		QObject::connect(code, &Ui::CodeInput::cancelled, box, [=] {
			box->closeBox();
		});
		box->setFocusCallback([=] { code->setFocusFast(); });

		*confirm = box->addButton(
			tr::lng_settings_confirm(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		if (args.extra && !args.extraText.isEmpty()) {
			const auto extra = args.extra;
			box->addLeftButton(args.extraText, [=] {
				box->closeBox();
				extra();
			});
		}
	}));
}

} // namespace Settings
