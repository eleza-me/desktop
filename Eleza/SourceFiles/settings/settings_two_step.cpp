#include "settings/settings_two_step.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_code_box.h"
#include "settings/settings_common.h"
#include "settings/settings_logic.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QJsonObject>

namespace Settings {
namespace {

using Controller = Window::SessionController;
using Changed = std::function<void()>;

struct Challenge {
	int codeLength = 6;
	int resendAfter = 60;
	QString email; // masked

	[[nodiscard]] static Challenge FromJson(const QJsonObject &json) {
		auto result = Challenge();
		result.codeLength = json.value(u"code_length"_s).toInt(6);
		result.resendAfter = json.value(u"resend_after"_s).toInt(60);
		result.email = json.value(u"email"_s).toString();
		return result;
	}
};

// A masked address is always left-to-right inside a Persian sentence.
[[nodiscard]] QString Isolated(const QString &text) {
	return Lang::Ltr(text);
}

[[nodiscard]] Ui::FlatLabel *AddError(Ui::GenericBox *box) {
	const auto result = box->addRow(
		Ui::object_ptr<Ui::FlatLabel>(box, st::settingsErrorLabel),
		st::settingsErrorPadding);
	result->hide();
	return result;
}

void SetError(Ui::FlatLabel *label, const QString &text) {
	label->setText(text);
	label->setVisible(!text.isEmpty());
}

[[nodiscard]] Ui::PasswordInput *AddPassword(
		Ui::GenericBox *box,
		const QString &placeholder) {
	return box->addRow(
		Ui::object_ptr<Ui::PasswordInput>(
			box,
			st::settingsTwoStepField,
			placeholder),
		st::settingsFieldPadding);
}

[[nodiscard]] Ui::InputField *AddField(
		Ui::GenericBox *box,
		const QString &placeholder,
		int maxLength) {
	const auto result = box->addRow(
		Ui::object_ptr<Ui::InputField>(
			box,
			st::settingsTwoStepField,
			placeholder),
		st::settingsFieldPadding);
	result->setMaxLength(maxLength);
	return result;
}

[[nodiscard]] QString PasswordProblemText(PasswordProblem problem, int min) {
	switch (problem) {
	case PasswordProblem::Empty:
		return tr::lng_settings_two_step_password_empty(tr::now);
	case PasswordProblem::TooShort:
		return tr::lng_settings_two_step_password_short(
			tr::now,
			lt_count,
			min);
	case PasswordProblem::TooLong:
		return tr::lng_settings_two_step_password_long(tr::now);
	case PasswordProblem::Mismatch:
		return tr::lng_settings_two_step_password_mismatch(tr::now);
	case PasswordProblem::None: break;
	}
	return QString();
}

// The mailed code: confirms a setup or a new recovery address.
void ShowConfirmCodeBox(
		Controller *controller,
		const Challenge &challenge,
		const QString &title,
		const QString &doneToast,
		std::function<void(
			std::function<void(int)> done,
			std::function<void(const QString &)> fail)> resend,
		Changed changed,
		Changed restart = nullptr) {
	auto args = CodeBoxArgs();
	args.title = title;
	args.about = tr::lng_intro_recovery_sent_to(
		tr::now,
		lt_email,
		Isolated(challenge.email));
	args.codeLength = challenge.codeLength;
	args.resendAfter = challenge.resendAfter;
	args.submit = [=](
			const QString &code,
			std::function<void()> done,
			std::function<void(const QString &)> fail) {
		controller->session().api().request(
			Api::Post,
			u"/v1/account/two-step/confirm"_s
		).body(QJsonObject{ { u"code"_s, code } })
			.guard(controller)
			.done([=] {
				done();
				controller->showToast(doneToast);
				if (changed) {
					changed();
				}
			})
			.fail([=](const Api::Error &error) {
				fail(Support::ErrorText(error));
			})
			.send();
	};
	args.resend = std::move(resend);
	if (restart) {
		args.extraText = tr::lng_settings_two_step_start_again(tr::now);
		args.extra = std::move(restart);
	}
	ShowCodeBox(controller, std::move(args));
}

// ---- off -> on

void ShowSetupBox(Controller *controller, Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		const auto min = controller->session().account().clientConfig()
			.twoStepMinPasswordLength();
		box->setTitle(tr::lng_settings_two_step(tr::now));
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_settings_two_step_setup_about(tr::now),
			st::boxLabel));
		const auto password = AddPassword(
			box,
			tr::lng_intro_password(tr::now));
		const auto repeat = AddPassword(
			box,
			tr::lng_settings_two_step_repeat(tr::now));
		const auto hint = AddField(
			box,
			tr::lng_settings_two_step_hint(tr::now),
			64);
		const auto email = AddField(
			box,
			tr::lng_settings_two_step_email(tr::now),
			254);
		email->setForcedDirection(Qt::LeftToRight);
		const auto problemLabel = AddError(box);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_two_step_email_about(tr::now),
				st::settingsFootnote),
			st::boxRowPadding);

		const auto busy = std::make_shared<bool>(false);
		const auto button = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			if (*busy) {
				return;
			}
			const auto passwordText = NormalizePassword(password->text());
			const auto hintText = hint->text().trimmed();
			const auto emailText = email->text().trimmed();
			const auto problem = CheckNewPassword(
				passwordText,
				NormalizePassword(repeat->text()),
				min);
			if (problem != PasswordProblem::None) {
				((problem == PasswordProblem::Mismatch)
					? repeat
					: password)->showError();
				((problem == PasswordProblem::Mismatch)
					? repeat
					: password)->setFocusFast();
				SetError(problemLabel, PasswordProblemText(problem, min));
				return;
			} else if (!HintAllowed(hintText, passwordText)) {
				hint->showError();
				hint->setFocusFast();
				SetError(problemLabel, tr::lng_settings_error_hint_invalid(tr::now));
				return;
			} else if (!EmailLooksValid(emailText)) {
				email->showError();
				email->setFocusFast();
				SetError(problemLabel, tr::lng_settings_error_email_invalid(tr::now));
				return;
			}
			SetError(problemLabel, QString());
			auto body = QJsonObject{
				{ u"password"_s, passwordText },
				{ u"recovery_email"_s, emailText },
			};
			if (!hintText.isEmpty()) {
				body.insert(u"hint"_s, hintText);
			}
			const auto request = [=](
					QObject *guard,
					std::function<void(const Challenge &)> done,
					std::function<void(const Api::Error &)> fail) {
				controller->session().api().request(
					Api::Post,
					u"/v1/account/two-step/setup"_s
				).body(body)
					.guard(guard)
					.done([=](const QJsonObject &result) {
						done(Challenge::FromJson(result));
					})
					.fail(fail)
					.send();
			};
			*busy = true;
			if (*button) {
				(*button)->setLoading(true);
			}
			request(box, [=](const Challenge &challenge) {
				box->closeBox();
				ShowConfirmCodeBox(
					controller,
					challenge,
					tr::lng_settings_two_step(tr::now),
					tr::lng_settings_two_step_enabled_toast(tr::now),
					[=](
							std::function<void(int)> done,
							std::function<void(const QString &)> fail) {
						request(controller, [=](const Challenge &again) {
							done(again.resendAfter);
						}, [=](const Api::Error &error) {
							fail(Support::ErrorText(error));
						});
					},
					changed,
					[=] { ShowSetupBox(controller, changed); });
				// The setup is pending now: the account page shows it.
				if (changed) {
					changed();
				}
			}, [=](const Api::Error &error) {
				*busy = false;
				if (*button) {
					(*button)->setLoading(false);
				}
				switch (FieldForError(error.code)) {
				case TwoStepField::Password:
					password->showError();
					password->setFocusFast();
					break;
				case TwoStepField::Hint:
					hint->showError();
					hint->setFocusFast();
					break;
				case TwoStepField::Email:
					email->showError();
					email->setFocusFast();
					break;
				default: break;
				}
				SetError(problemLabel, Support::ErrorText(error));
			});
		};
		QObject::connect(password, &Ui::MaskedInputField::submitted, box, [=] {
			repeat->setFocusFast();
		});
		QObject::connect(repeat, &Ui::MaskedInputField::submitted, box, [=] {
			hint->setFocusFast();
		});
		QObject::connect(hint, &Ui::InputField::submitted, box, [=] {
			email->setFocusFast();
		});
		QObject::connect(email, &Ui::InputField::submitted, box, submit);
		box->setFocusCallback([=] { password->setFocusFast(); });
		*button = box->addButton(
			tr::lng_continue(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- change the password

void ShowChangePasswordBox(Controller *controller, Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		const auto min = controller->session().account().clientConfig()
			.twoStepMinPasswordLength();
		box->setTitle(tr::lng_settings_two_step_change_password(tr::now));
		const auto current = AddPassword(
			box,
			tr::lng_settings_two_step_current(tr::now));
		const auto password = AddPassword(
			box,
			tr::lng_settings_two_step_new(tr::now));
		const auto repeat = AddPassword(
			box,
			tr::lng_settings_two_step_repeat(tr::now));
		const auto hint = AddField(
			box,
			tr::lng_settings_two_step_hint(tr::now),
			64);
		const auto problemLabel = AddError(box);

		const auto busy = std::make_shared<bool>(false);
		const auto button = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			if (*busy) {
				return;
			}
			const auto currentText = NormalizePassword(current->text());
			const auto passwordText = NormalizePassword(password->text());
			const auto hintText = hint->text().trimmed();
			if (currentText.isEmpty()) {
				current->showError();
				current->setFocusFast();
				return;
			}
			const auto problem = CheckNewPassword(
				passwordText,
				NormalizePassword(repeat->text()),
				min);
			if (problem != PasswordProblem::None) {
				const auto field = (problem == PasswordProblem::Mismatch)
					? repeat
					: password;
				field->showError();
				field->setFocusFast();
				SetError(problemLabel, PasswordProblemText(problem, min));
				return;
			} else if (!HintAllowed(hintText, passwordText)) {
				hint->showError();
				hint->setFocusFast();
				SetError(problemLabel, tr::lng_settings_error_hint_invalid(tr::now));
				return;
			}
			SetError(problemLabel, QString());
			auto body = QJsonObject{
				{ u"current_password"_s, currentText },
				{ u"new_password"_s, passwordText },
			};
			if (!hintText.isEmpty()) {
				body.insert(u"hint"_s, hintText);
			}
			*busy = true;
			if (*button) {
				(*button)->setLoading(true);
			}
			controller->session().api().request(
				Api::Post,
				u"/v1/account/two-step/password"_s
			).body(body)
				.guard(box)
				.done([=] {
					box->closeBox();
					controller->showToast(
						tr::lng_settings_two_step_password_changed(tr::now));
					if (changed) {
						changed();
					}
				})
				.fail([=](const Api::Error &error) {
					*busy = false;
					if (*button) {
						(*button)->setLoading(false);
					}
					switch (FieldForError(error.code)) {
					case TwoStepField::Current:
						current->showError();
						current->setFocusFast();
						break;
					case TwoStepField::Password:
						password->showError();
						password->setFocusFast();
						break;
					case TwoStepField::Hint:
						hint->showError();
						hint->setFocusFast();
						break;
					default: break;
					}
					SetError(problemLabel, Support::ErrorText(error));
				})
				.send();
		};
		QObject::connect(current, &Ui::MaskedInputField::submitted, box, [=] {
			password->setFocusFast();
		});
		QObject::connect(password, &Ui::MaskedInputField::submitted, box, [=] {
			repeat->setFocusFast();
		});
		QObject::connect(repeat, &Ui::MaskedInputField::submitted, box, [=] {
			hint->setFocusFast();
		});
		QObject::connect(hint, &Ui::InputField::submitted, box, submit);
		box->setFocusCallback([=] { current->setFocusFast(); });
		*button = box->addButton(
			tr::lng_eli_save(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- change the recovery e-mail

void ShowChangeEmailBox(Controller *controller, Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_two_step_change_email(tr::now));
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_settings_two_step_change_email_about(tr::now),
			st::boxLabel));
		const auto password = AddPassword(
			box,
			tr::lng_intro_password(tr::now));
		const auto email = AddField(
			box,
			tr::lng_settings_two_step_new_email(tr::now),
			254);
		email->setForcedDirection(Qt::LeftToRight);
		const auto problemLabel = AddError(box);

		const auto busy = std::make_shared<bool>(false);
		const auto button = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			if (*busy) {
				return;
			}
			const auto passwordText = NormalizePassword(password->text());
			const auto emailText = email->text().trimmed();
			if (passwordText.isEmpty()) {
				password->showError();
				password->setFocusFast();
				return;
			} else if (!EmailLooksValid(emailText)) {
				email->showError();
				email->setFocusFast();
				SetError(problemLabel, tr::lng_settings_error_email_invalid(tr::now));
				return;
			}
			SetError(problemLabel, QString());
			const auto body = QJsonObject{
				{ u"password"_s, passwordText },
				{ u"recovery_email"_s, emailText },
			};
			const auto request = [=](
					QObject *guard,
					std::function<void(const Challenge &)> done,
					std::function<void(const Api::Error &)> fail) {
				controller->session().api().request(
					Api::Post,
					u"/v1/account/two-step/email"_s
				).body(body)
					.guard(guard)
					.done([=](const QJsonObject &result) {
						done(Challenge::FromJson(result));
					})
					.fail(fail)
					.send();
			};
			*busy = true;
			if (*button) {
				(*button)->setLoading(true);
			}
			request(box, [=](const Challenge &challenge) {
				box->closeBox();
				ShowConfirmCodeBox(
					controller,
					challenge,
					tr::lng_settings_two_step_change_email(tr::now),
					tr::lng_settings_two_step_email_changed(tr::now),
					[=](
							std::function<void(int)> done,
							std::function<void(const QString &)> fail) {
						request(controller, [=](const Challenge &again) {
							done(again.resendAfter);
						}, [=](const Api::Error &error) {
							fail(Support::ErrorText(error));
						});
					},
					changed);
				if (changed) {
					changed();
				}
			}, [=](const Api::Error &error) {
				*busy = false;
				if (*button) {
					(*button)->setLoading(false);
				}
				switch (FieldForError(error.code)) {
				case TwoStepField::Current:
					password->showError();
					password->setFocusFast();
					break;
				case TwoStepField::Email:
					email->showError();
					email->setFocusFast();
					break;
				default: break;
				}
				SetError(problemLabel, Support::ErrorText(error));
			});
		};
		QObject::connect(password, &Ui::MaskedInputField::submitted, box, [=] {
			email->setFocusFast();
		});
		QObject::connect(email, &Ui::InputField::submitted, box, submit);
		box->setFocusCallback([=] { password->setFocusFast(); });
		*button = box->addButton(
			tr::lng_continue(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- on -> off

void ShowDisableBox(Controller *controller, Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_two_step_disable(tr::now));
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_settings_two_step_disable_about(tr::now),
			st::boxLabel));
		const auto password = AddPassword(
			box,
			tr::lng_intro_password(tr::now));
		const auto problemLabel = AddError(box);

		const auto busy = std::make_shared<bool>(false);
		const auto button = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			const auto passwordText = NormalizePassword(password->text());
			if (*busy) {
				return;
			} else if (passwordText.isEmpty()) {
				password->showError();
				password->setFocusFast();
				return;
			}
			SetError(problemLabel, QString());
			*busy = true;
			if (*button) {
				(*button)->setLoading(true);
			}
			controller->session().api().request(
				Api::Post,
				u"/v1/account/two-step/disable"_s
			).body(QJsonObject{ { u"password"_s, passwordText } })
				.guard(box)
				.done([=] {
					box->closeBox();
					controller->showToast(
						tr::lng_settings_two_step_disabled_toast(tr::now));
					if (changed) {
						changed();
					}
				})
				.fail([=](const Api::Error &error) {
					*busy = false;
					if (*button) {
						(*button)->setLoading(false);
					}
					if (FieldForError(error.code) == TwoStepField::Current) {
						password->showError();
						password->setFocusFast();
					}
					SetError(problemLabel, Support::ErrorText(error));
				})
				.send();
		};
		QObject::connect(
			password,
			&Ui::MaskedInputField::submitted,
			box,
			submit);
		box->setFocusCallback([=] { password->setFocusFast(); });
		*button = box->addButton(
			tr::lng_settings_two_step_disable_confirm(tr::now),
			submit,
			st::dangerRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- on: the summary with the actions

class StatusHeader final : public Ui::RpWidget {
public:
	StatusHeader(QWidget *parent, const QString &text)
	: RpWidget(parent)
	, _text(text) {
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &icon = st::settingsStatusIconOn;
		icon.paintInCenter(p, QRect(0, 0, width(), icon.height() + st::spaceSm));
		p.setFont(st::titleFontS);
		p.setPen(st::textPrimary);
		p.drawTextCentered(
			QRect(
				0,
				icon.height() + st::spaceMd,
				width(),
				st::titleFontS->height),
			_text);
	}
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::settingsStatusHeight;
	}

private:
	QString _text;

};

void ShowManageBox(
		Controller *controller,
		const TwoStepState &state,
		Changed changed) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_two_step(tr::now));
		const auto layout = box->verticalLayout();
		layout->add(Ui::object_ptr<StatusHeader>(
			layout,
			tr::lng_settings_two_step_is_on(tr::now)));
		if (!state.recoveryEmail.isEmpty()) {
			const auto row = AddInfoRow(
				layout,
				tr::lng_settings_two_step_email(tr::now),
				Isolated(state.recoveryEmail));
			Q_UNUSED(row);
		}
		if (!state.hint.isEmpty()) {
			AddInfoRow(
				layout,
				tr::lng_settings_two_step_hint_row(tr::now),
				state.hint);
		}
		AddDivider(layout);
		const auto then = [=](std::function<void()> open) {
			return [=] {
				box->closeBox();
				open();
			};
		};
		AddLinkButton(
			layout,
			tr::lng_settings_two_step_change_password(tr::now),
			nullptr,
			then([=] { ShowChangePasswordBox(controller, changed); }));
		AddLinkButton(
			layout,
			tr::lng_settings_two_step_change_email(tr::now),
			nullptr,
			then([=] { ShowChangeEmailBox(controller, changed); }));
		if (state.pending && !state.pendingEmail.isEmpty()) {
			// A new address is waiting for its code.
			auto challenge = Challenge();
			challenge.email = state.pendingEmail;
			AddLinkButton(
				layout,
				tr::lng_settings_two_step_enter_code(tr::now),
				nullptr,
				then([=] {
					ShowConfirmCodeBox(
						controller,
						challenge,
						tr::lng_settings_two_step_change_email(tr::now),
						tr::lng_settings_two_step_email_changed(tr::now),
						nullptr,
						changed,
						[=] { ShowChangeEmailBox(controller, changed); });
				}));
		}
		const auto disable = AddButton(
			layout,
			tr::lng_settings_two_step_disable(tr::now),
			nullptr,
			then([=] { ShowDisableBox(controller, changed); }));
		disable->setDestructive(true);
		AddFootnote(layout, tr::lng_settings_two_step_email_about(tr::now));
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

} // namespace

void LoadTwoStepState(
		Controller *controller,
		QObject *guard,
		std::function<void(const TwoStepState &)> done,
		std::function<void(const Api::Error &)> fail) {
	controller->session().api().request(
		Api::Get,
		u"/v1/account/two-step"_s
	).guard(guard)
		.done([=](const QJsonObject &result) {
			auto state = TwoStepState();
			state.enabled = result.value(u"enabled"_s).toBool();
			state.pending = result.value(u"pending"_s).toBool();
			state.hint = result.value(u"hint"_s).toString();
			state.recoveryEmail = result.value(u"recovery_email"_s).toString();
			state.pendingEmail = result.value(u"pending_email"_s).toString();
			done(state);
		})
		.fail(std::move(fail))
		.send();
}

void ShowTwoStepBox(
		Controller *controller,
		const TwoStepState &state,
		Changed changed) {
	if (state.enabled) {
		ShowManageBox(controller, state, std::move(changed));
	} else if (state.pending) {
		// The setup waits for the mailed code.
		auto challenge = Challenge();
		challenge.email = state.pendingEmail;
		ShowConfirmCodeBox(
			controller,
			challenge,
			tr::lng_settings_two_step(tr::now),
			tr::lng_settings_two_step_enabled_toast(tr::now),
			nullptr,
			changed,
			[=] { ShowSetupBox(controller, changed); });
	} else {
		ShowSetupBox(controller, std::move(changed));
	}
}

} // namespace Settings
