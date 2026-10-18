// «حساب کاربری» (phone and its change, handle, devices, two-step
// verification, data export, sign out, delete account) and «دستگاه‌ها» (the
// device sessions, «این من نبودم»).
//
// A phone change and an account deletion are confirmed by an SMS code
// (`POST /v1/account/otp` with the purpose, then the confirmation with the
// challenge); with two-step verification on, the server answers the
// confirmation with `two_step.password_required` and the same request is
// sent again with the password — that answer does not spend the code.
#include "settings/settings_pages.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "core/core_application.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_countries.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_code_box.h"
#include "settings/settings_common.h"
#include "settings/settings_logic.h"
#include "settings/settings_two_step.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "ui/ui_utility.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>

namespace Settings {
namespace {

using Controller = Window::SessionController;

[[nodiscard]] QString DateTimeText(qint64 ms) {
	const auto time = QDateTime::fromMSecsSinceEpoch(ms);
	return Lang::SolarDate(time.date())
		+ QLatin1Char(' ')
		+ Lang::Clock(time);
}

// ---- data export

void RequestExportCode(
	Controller *controller,
	QObject *guard,
	std::function<void(const QJsonObject &)> done,
	std::function<void(const Api::Error &)> fail) {
	controller->session().data().graph().requestExportCode(
		controller->session().account().deviceId(),
		guard,
		std::move(done),
		std::move(fail));
}

void ShowEmailRequiredBox(Controller *controller) {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_settings_data_export(tr::now);
	args.text = tr::lng_settings_export_email_required(tr::now);
	const auto twoStep = controller->session().account().clientConfig()
		.twoStepEnabled();
	if (twoStep) {
		args.confirmText = tr::lng_settings_two_step(tr::now);
		args.confirmed = [=](std::function<void()> close) {
			close();
			LoadTwoStepState(controller, controller, [=](
					const TwoStepState &state) {
				ShowTwoStepBox(controller, state, nullptr);
			}, [=](const Api::Error &error) {
				Support::ShowError(controller, error);
			});
		};
	} else {
		args.inform = true;
	}
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void StartExportRequest(Controller *controller) {
	RequestExportCode(controller, controller, [=](const QJsonObject &result) {
		const auto challenge = std::make_shared<QString>(
			result.value(u"challenge_id"_s).toString());
		auto args = CodeBoxArgs();
		args.title = tr::lng_settings_data_export(tr::now);
		args.about = tr::lng_settings_export_code_sent(tr::now);
		args.codeLength = result.value(u"code_length"_s).toInt(6);
		args.resendAfter = result.value(u"resend_after"_s).toInt(60);
		args.submit = [=](
				const QString &code,
				std::function<void()> done,
				std::function<void(const QString &)> fail) {
			controller->session().data().graph().requestExport(
				*challenge,
				code,
				controller->session().account().deviceId(),
				controller,
				[=](const QJsonObject &) {
					done();
					controller->showToast(
						tr::lng_settings_export_requested(tr::now));
				},
				[=](const Api::Error &error) {
					fail(Support::ErrorText(error));
				});
		};
		args.resend = [=](
				std::function<void(int)> done,
				std::function<void(const QString &)> fail) {
			RequestExportCode(controller, controller, [=](
					const QJsonObject &again) {
				*challenge = again.value(u"challenge_id"_s).toString();
				done(again.value(u"resend_after"_s).toInt(60));
			}, [=](const Api::Error &error) {
				fail(Support::ErrorText(error));
			});
		};
		ShowCodeBox(controller, std::move(args));
	}, [=](const Api::Error &error) {
		if (error.code == u"export.email_required") {
			ShowEmailRequiredBox(controller);
		} else {
			Support::ShowError(controller, error);
		}
	});
}

void DownloadExport(Controller *controller, const QString &id) {
	Support::AskSavePath(
		controller->window(),
		u"eleza-data-export.json"_s,
		[=](const QString &path) {
			controller->showToast(tr::lng_settings_export_downloading(tr::now));
			controller->session().api().download(
				u"/v1/me/exports/%1/download"_s.arg(
					QString::fromUtf8(QUrl::toPercentEncoding(id)))
			).toFile(path).guard(controller).done([=](
					const Api::DownloadResult &) {
				controller->showToast(tr::lng_settings_export_saved(tr::now));
			}).fail([=](const Api::Error &error) {
				Support::ShowError(controller, error);
			}).send();
		});
}

void ShowExportStatusBox(Controller *controller, const QJsonObject &status) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_data_export(tr::now));
		const auto state = status.value(u"state"_s).toString(
			status.value(u"status"_s).toString());
		const auto id = status.value(u"id"_s).toString();
		auto lines = QStringList();
		if (status.isEmpty() || state.isEmpty()) {
			lines.push_back(tr::lng_settings_export_none(tr::now));
		} else {
			lines.push_back(tr::lng_settings_export_state(
				tr::now,
				lt_state,
				ExportStateText(state)));
			const auto number = [&](const QString &key) {
				return base::SafeInt64(status.value(key).toDouble());
			};
			if (const auto at = number(u"requested_at"_s)) {
				lines.push_back(tr::lng_settings_export_requested_at(
					tr::now,
					lt_date,
					DateTimeText(at)));
			}
			if (state == u"ready") {
				if (const auto size = number(u"size_bytes"_s)) {
					lines.push_back(tr::lng_settings_export_size(
						tr::now,
						lt_size,
						Lang::Bytes(size)));
				}
				if (const auto until = number(u"expires_at"_s)) {
					lines.push_back(tr::lng_settings_export_expires(
						tr::now,
						lt_date,
						DateTimeText(until)));
				}
				if (status.value(u"email_failed"_s).toBool()) {
					lines.push_back(tr::lng_settings_export_email_failed(tr::now));
				} else if (number(u"emailed_at"_s)) {
					lines.push_back(tr::lng_settings_export_emailed(tr::now));
				}
			}
		}
		box->addRow(Ui::object_ptr<Ui::FlatLabel>(
			box,
			lines.join(QLatin1Char('\n')),
			st::boxLabel));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_export_about(tr::now),
				st::settingsFootnote),
			st::boxRowPadding);

		const auto busy = (state == u"pending") || (state == u"running");
		const auto canDownload = (state == u"ready")
			&& !id.isEmpty()
			&& (!status.contains(u"downloads_left"_s)
				|| status.value(u"downloads_left"_s).toInt() > 0);
		if (canDownload) {
			box->addButton(tr::lng_settings_export_download(tr::now), [=] {
				box->closeBox();
				DownloadExport(controller, id);
			}, st::defaultRoundButton);
		}
		if (!busy) {
			box->addButton(
				tr::lng_settings_export_request(tr::now),
				[=] {
					box->closeBox();
					StartExportRequest(controller);
				},
				canDownload ? st::defaultBoxButton : st::defaultRoundButton);
		}
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- account

// ---- confirmations with an SMS code (and the two-step password)

using ConfirmRequest = std::function<void(
	const QString &password,
	std::function<void()> done,
	std::function<void(const Api::Error &)> fail)>;

[[nodiscard]] bool PasswordRequired(const Api::Error &error) {
	return error.code == u"two_step.password_required";
}

[[nodiscard]] bool PasswordWrong(const Api::Error &error) {
	return error.code == u"two_step.password_invalid";
}

// «رمز دومرحله‌ای»: the same request again, with the password.
void ShowConfirmPasswordBox(
		Controller *controller,
		ConfirmRequest request,
		std::function<void()> succeeded) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_password_title(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_password_about(tr::now),
				st::boxLabel),
			st::boxRowPadding);
		const auto password = box->addRow(
			Ui::object_ptr<Ui::PasswordInput>(
				box,
				st::settingsTwoStepField,
				tr::lng_intro_password(tr::now)),
			st::settingsFieldPadding);
		const auto error = box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, st::settingsErrorLabel),
			st::settingsErrorPadding);
		error->hide();
		const auto busy = std::make_shared<bool>(false);
		const auto button = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			const auto text = NormalizePassword(password->text());
			if (*busy) {
				return;
			} else if (text.isEmpty()) {
				password->showError();
				password->setFocusFast();
				return;
			}
			*busy = true;
			if (*button) {
				(*button)->setLoading(true);
			}
			error->hide();
			request(text, [=] {
				box->closeBox();
				succeeded();
			}, [=](const Api::Error &failure) {
				*busy = false;
				if (*button) {
					(*button)->setLoading(false);
				}
				password->showError();
				password->setFocusFast();
				error->setText(PasswordWrong(failure)
					? tr::lng_settings_password_wrong(tr::now)
					: Support::ErrorText(failure));
				error->show();
			});
		};
		QObject::connect(
			password,
			&Ui::MaskedInputField::submitted,
			box,
			submit);
		box->setFocusCallback([=] { password->setFocusFast(); });
		*button = box->addButton(
			tr::lng_continue(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

struct AccountCodeArgs {
	QString title;
	QString about;
	QString purpose;  // delete_account | phone_change
	QString newPhone; // phone_change
	// The confirmation with the challenge, the code and the password ("").
	std::function<void(
		const QString &challenge,
		const QString &code,
		const QString &password,
		std::function<void()> done,
		std::function<void(const Api::Error &)> fail)> confirm;
	std::function<void()> succeeded;
	// The code was sent (the code box is shown next).
	std::function<void()> requested;
	// The code could not be requested (the box was not shown).
	std::function<void(const Api::Error &)> failed;
};

void ShowAccountCodeBox(Controller *controller, AccountCodeArgs args) {
	const auto session = &controller->session();
	const auto deviceId = session->account().deviceId();
	const auto shared = std::make_shared<AccountCodeArgs>(std::move(args));
	session->data().graph().requestAccountCode(
		shared->purpose,
		shared->newPhone,
		deviceId,
		controller,
		[=](const QJsonObject &result) {
			const auto challenge = std::make_shared<QString>(
				result.value(u"challenge_id"_s).toString());
			auto code = CodeBoxArgs();
			code.title = shared->title;
			code.about = shared->about;
			code.codeLength = result.value(u"code_length"_s).toInt(6);
			code.resendAfter = result.value(u"resend_after"_s).toInt(60);
			code.submit = [=](
					const QString &typed,
					std::function<void()> done,
					std::function<void(const QString &)> fail) {
				shared->confirm(*challenge, typed, QString(), [=] {
					done();
					shared->succeeded();
				}, [=](const Api::Error &error) {
					if (!PasswordRequired(error)) {
						fail(Support::ErrorText(error));
						return;
					}
					// The code stays valid: ask for the password and
					// send the same confirmation with it.
					done();
					ShowConfirmPasswordBox(controller, [=](
							const QString &password,
							std::function<void()> passed,
							std::function<void(const Api::Error &)> refused) {
						shared->confirm(*challenge, typed, password, passed, refused);
					}, shared->succeeded);
				});
			};
			code.resend = [=](
					std::function<void(int)> done,
					std::function<void(const QString &)> fail) {
				session->data().graph().requestAccountCode(
					shared->purpose,
					shared->newPhone,
					deviceId,
					controller,
					[=](const QJsonObject &again) {
						*challenge = again.value(u"challenge_id"_s).toString();
						done(again.value(u"resend_after"_s).toInt(60));
					},
					[=](const Api::Error &error) {
						fail(Support::ErrorText(error));
					});
			};
			if (shared->requested) {
				shared->requested();
			}
			ShowCodeBox(controller, std::move(code));
		},
		[=](const Api::Error &error) {
			if (shared->failed) {
				shared->failed(error);
			} else {
				Support::ShowError(controller, error);
			}
		});
}

// ---- delete account

void ShowDeleteAccountBox(Controller *controller) {
	const auto session = &controller->session();
	const auto handle = session->data().users().me().handle;
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_delete_account(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_delete_title(tr::now),
				st::settingsDeleteTitle),
			st::boxRowPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_delete_body(tr::now),
				st::settingsDeleteText),
			st::boxRowPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_delete_after(tr::now),
				st::settingsSubsectionTitle),
			st::settingsDeleteHeaderPadding);
		const auto points = QStringList{
			tr::lng_settings_delete_point_posts(tr::now),
			tr::lng_settings_delete_point_messages(tr::now),
			handle.isEmpty()
				? tr::lng_settings_delete_point_handle_generic(tr::now)
				: tr::lng_settings_delete_point_handle(
					tr::now,
					lt_handle,
					Lang::Handle(handle)),
			tr::lng_settings_delete_point_anon(tr::now),
		};
		for (const auto &point : points) {
			box->addRow(
				Ui::object_ptr<Ui::FlatLabel>(
					box,
					QString(QChar(0x2022)) + QChar(' ') + point,
					st::settingsDeletePoint),
				st::settingsDeletePointPadding);
		}
		box->addButton(tr::lng_settings_delete_continue(tr::now), [=] {
			box->closeBox();
			auto args = AccountCodeArgs();
			args.title = tr::lng_settings_delete_account(tr::now);
			args.about = tr::lng_settings_delete_code_about(tr::now);
			args.purpose = u"delete_account"_s;
			args.confirm = [=](
					const QString &challenge,
					const QString &code,
					const QString &password,
					std::function<void()> done,
					std::function<void(const Api::Error &)> fail) {
				session->data().graph().deleteAccount(
					challenge,
					code,
					password,
					session->account().deviceId(),
					controller,
					std::move(done),
					std::move(fail));
			};
			const auto account = &session->account();
			args.succeeded = [=] {
				// The local session and its data go now; signing in within
				// the 30 days cancels the deletion. Later: this runs from the
				// box that the session's end destroys.
				QMetaObject::invokeMethod(account, [=] {
					account->endSession(Main::SessionEndReason::SignedOut);
					if (const auto window = Core::App().window()) {
						window->showToast(tr::lng_settings_delete_done(tr::now));
					}
				}, Qt::QueuedConnection);
			};
			ShowAccountCodeBox(controller, std::move(args));
		}, st::dangerRoundButton);
		box->addButton(tr::lng_settings_delete_cancel(tr::now), [=] {
			box->closeBox();
		});
	}));
}

// ---- change phone

void ShowChangePhoneBox(Controller *controller, std::function<void()> changed) {
	const auto session = &controller->session();
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_settings_phone_new(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_settings_phone_new_about(tr::now),
				st::boxLabel),
			st::boxRowPadding);
		const auto phone = box->addRow(
			Ui::object_ptr<Ui::PhoneInput>(
				box,
				st::settingsTwoStepField,
				u"98"_s),
			st::settingsFieldPadding);
		phone->setNumberPlaceholder(tr::lng_intro_phone_placeholder(tr::now));
		const auto error = box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, st::settingsErrorLabel),
			st::settingsErrorPadding);
		error->hide();
		QObject::connect(
			phone,
			&Ui::PhoneInput::internationalNumberEntered,
			box,
			[=](const QString &digits) {
				if (const auto found = Lang::CountryByPhone(digits)) {
					phone->setCountryCode(found->code);
					phone->setNumber(digits.mid(found->code.size()));
				}
			});
		QObject::connect(phone, &Ui::PhoneInput::changed, box, [=] {
			error->hide();
		});
		const auto busy = std::make_shared<bool>(false);
		const auto button = std::make_shared<QPointer<Ui::RoundButton>>();
		const auto submit = [=] {
			const auto code = phone->countryCode();
			const auto number = code.isEmpty()
				? QString()
				: Lang::E164(phone->number(), code);
			if (*busy) {
				return;
			} else if (number.isEmpty()) {
				phone->showError();
				error->setText(tr::lng_intro_invalid_phone(tr::now));
				error->show();
				return;
			}
			*busy = true;
			if (*button) {
				(*button)->setLoading(true);
			}
			const auto weak = QPointer<Ui::GenericBox>(box);
			auto args = AccountCodeArgs();
			args.title = tr::lng_settings_phone_change(tr::now);
			args.about = tr::lng_settings_phone_code_about(
				tr::now,
				lt_phone,
				Lang::Phone(number));
			args.purpose = u"phone_change"_s;
			args.newPhone = number;
			args.confirm = [=](
					const QString &challenge,
					const QString &typed,
					const QString &password,
					std::function<void()> done,
					std::function<void(const Api::Error &)> fail) {
				session->data().graph().changePhone(
					challenge,
					typed,
					password,
					session->account().deviceId(),
					controller,
					std::move(done),
					std::move(fail));
			};
			args.succeeded = [=] {
				controller->showToast(tr::lng_settings_phone_changed(tr::now));
				if (changed) {
					changed();
				}
			};
			args.failed = [=](const Api::Error &failure) {
				if (!weak) {
					Support::ShowError(controller, failure);
					return;
				}
				*busy = false;
				if (*button) {
					(*button)->setLoading(false);
				}
				phone->showError();
				error->setText(Support::ErrorText(failure));
				error->show();
			};
			// The box closes once the code is on its way (the code box
			// takes its place); it stays to show why it was not.
			args.requested = [=] {
				if (weak) {
					weak->closeBox();
				}
			};
			ShowAccountCodeBox(controller, std::move(args));
		};
		QObject::connect(phone, &Ui::PhoneInput::submitted, box, submit);
		box->setFocusCallback([=] { phone->setFocusFast(); });
		*button = box->addButton(
			tr::lng_settings_phone_send_code(tr::now),
			submit,
			st::defaultRoundButton);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

class AccountPage final : public Page {
public:
	AccountPage(QWidget *parent, Controller *controller)
	: Page(parent, controller, Section::Account) {
		build();
		showContent();
	}

private:
	void build() {
		const auto window = controller();
		auto &session = window->session();

		const auto phone = std::make_shared<QPointer<Ui::SettingsButton>>();
		const auto loadPhone = Guarded(this, [=, this] {
			window->session().data().graph().accountPhone(this, [=](
					const QString &value) {
				if (*phone) {
					(*phone)->setValueText(value.isEmpty()
						? QString(QChar(0x2014))
						: Lang::Phone(value));
				}
			});
		});
		*phone = AddLinkButton(
			content(),
			tr::lng_settings_phone(tr::now),
			&st::settingsIconPhone,
			[=] { ShowChangePhoneBox(window, loadPhone); });
		(*phone)->setValueText(QString(QChar(0x2026)));
		loadPhone();

		const auto handle = AddLinkButton(
			content(),
			tr::lng_settings_handle(tr::now),
			&st::settingsIconHandle,
			[=] { window->openRoute(Window::Routes::Me()); });
		const auto users = &session.data().users();
		const auto refreshHandle = [=] {
			const auto value = users->me().handle;
			handle->setValueText(value.isEmpty()
				? QString(QChar(0x2014))
				: Lang::Handle(value));
		};
		refreshHandle();
		connect(users, &Data::Users::meChanged, handle, refreshHandle);

		AddLinkButton(
			content(),
			tr::lng_settings_devices(tr::now),
			&st::settingsIconDevices,
			[=] {
				window->openRoute(Window::Routes::Settings(
					SectionId(Section::Devices)));
			});

		if (session.account().clientConfig().twoStepEnabled()) {
			_twoStep = AddLinkButton(
				content(),
				tr::lng_settings_two_step(tr::now),
				&st::settingsIconTwoStep,
				[this] { openTwoStep(); });
			loadTwoStep(false);
		}

		AddLinkButton(
			content(),
			tr::lng_settings_data_export(tr::now),
			&st::settingsIconExport,
			[=] { ShowExportBox(window); });

		AddDivider(content());
		const auto logout = AddButton(
			content(),
			tr::lng_sign_out(tr::now),
			&st::settingsIconLogout,
			[=] { window->requestLogout(); });
		logout->setDestructive(true);
		const auto remove = AddButton(
			content(),
			tr::lng_settings_delete_account(tr::now),
			&st::settingsIconDelete,
			[=] { ShowDeleteAccountBox(window); });
		remove->setDestructive(true);
		AddFootnote(content(), tr::lng_settings_delete_account_about(tr::now));
	}

	void loadTwoStep(bool thenOpen) {
		if (_twoStepLoading) {
			_openWhenLoaded = _openWhenLoaded || thenOpen;
			return;
		}
		_twoStepLoading = true;
		_openWhenLoaded = thenOpen;
		LoadTwoStepState(controller(), this, [this](const TwoStepState &state) {
			_twoStepLoading = false;
			_state = state;
			_stateKnown = true;
			if (_twoStep) {
				_twoStep->setValueText(state.enabled
					? tr::lng_settings_on(tr::now)
					: state.pending
					? tr::lng_settings_two_step_pending(tr::now)
					: tr::lng_off(tr::now));
			}
			if (std::exchange(_openWhenLoaded, false)) {
				openTwoStep();
			}
		}, [this](const Api::Error &error) {
			_twoStepLoading = false;
			if (error.notFound()) {
				// The server has the feature switched off.
				if (_twoStep) {
					_twoStep->hide();
				}
			} else if (std::exchange(_openWhenLoaded, false)) {
				Support::ShowError(controller(), error);
			}
		});
	}

	void openTwoStep() {
		if (!_stateKnown) {
			loadTwoStep(true);
			return;
		}
		ShowTwoStepBox(controller(), _state, Guarded(this, [this] {
			_stateKnown = false;
			loadTwoStep(false);
		}));
	}

	Ui::SettingsButton *_twoStep = nullptr;
	TwoStepState _state;
	bool _stateKnown = false;
	bool _twoStepLoading = false;
	bool _openWhenLoaded = false;

};

// ---- devices

class SessionRow final : public Ui::RpWidget {
public:
	SessionRow(
		QWidget *parent,
		const Data::DeviceSession &session,
		std::function<void()> terminate,
		std::function<void()> menu = nullptr)
	: RpWidget(parent)
	, _session(session)
	, _subtitle(SessionSubtitle(
		session,
		QDateTime::currentMSecsSinceEpoch()))
	, _menu(std::move(menu)) {
		if (_menu) {
			setCursor(style::cur_pointer);
		}
		if (!session.current && terminate) {
			_terminate = new Ui::IconButton(this, st::settingsSessionTerminate);
			_terminate->setClickedCallback(std::move(terminate));
			Ui::InstallTooltip(_terminate, [] {
				return tr::lng_settings_session_terminate(tr::now);
			});
		}
	}

	[[nodiscard]] QString sessionId() const {
		return _session.id;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_terminate) {
			_terminate->moveToRight(
				st::settingsSessionPadding.right(),
				(st::settingsSessionHeight - _terminate->height()) / 2,
				newWidth);
		}
		return st::settingsSessionHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::settingsSessionPadding;
		const auto size = int(st::settingsSessionIconSize);
		const auto circle = style::rtlrect(
			padding.left(),
			(height() - size) / 2,
			size,
			size,
			width());
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::settingsSessionIconBg);
			p.drawEllipse(circle);
		}
		const auto &platform = _session.platform;
		if (platform == u"ios" || platform == u"android") {
			// A handset: there is no such glyph in the icon set.
			auto hq = PainterHighQualityEnabler(p);
			const auto body = QRectF(
				circle.center().x() + 0.5 - 6.,
				circle.center().y() + 0.5 - 9.,
				12.,
				18.);
			auto pen = QPen(st::textSecondary->c, 1.75);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(body, 2.5, 2.5);
			p.drawLine(
				QPointF(body.center().x() - 1.5, body.bottom() - 3.),
				QPointF(body.center().x() + 1.5, body.bottom() - 3.));
		} else {
			((platform == u"web")
				? st::settingsSessionWeb
				: st::settingsSessionDesktop).paintInCenter(p, circle);
		}

		const auto left = padding.left() + size + st::settingsSessionIconSkip;
		const auto right = padding.right()
			+ (_terminate
				? (_terminate->width() + st::spaceXs)
				: st::spaceMd);
		const auto available = std::max(width() - left - right, 0);
		const auto name = _session.deviceName.isEmpty()
			? PlatformTitle(_session.platform)
			: _session.deviceName;
		p.setFont(st::settingsSessionNameFont);
		p.setPen(st::settingsSessionNameFg);
		p.drawTextLeft(
			left,
			st::settingsSessionNameTop,
			width(),
			st::settingsSessionNameFont->elided(name, available));
		p.setFont(st::settingsSessionInfoFont);
		p.setPen(_session.current
			? st::settingsSessionCurrentFg
			: st::settingsSessionInfoFg);
		p.drawTextLeft(
			left,
			st::settingsSessionInfoTop,
			width(),
			st::settingsSessionInfoFont->elided(_subtitle, available));
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (_menu
			&& (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)
			&& rect().contains(e->pos())) {
			const auto callback = _menu;
			callback();
		}
	}

private:
	Data::DeviceSession _session;
	QString _subtitle;
	Ui::IconButton *_terminate = nullptr;
	std::function<void()> _menu;

};

class DevicesPage final : public Page {
public:
	DevicesPage(QWidget *parent, Controller *controller)
	: Page(parent, controller, Section::Devices) {
		load();
	}

private:
	void load() {
		showLoading();
		controller()->session().data().graph().sessions(this, [this](
				const QVector<Data::DeviceSession> &list) {
			_sessions = list;
			rebuild();
		}, [this](const Api::Error &error) {
			showError(error, [this] { load(); });
		});
	}

	void rebuild() {
		clearContent();
		auto others = 0;
		AddSubsectionTitle(content(), tr::lng_settings_this_device_title(tr::now));
		for (const auto &session : _sessions) {
			if (session.current) {
				content()->add(Ui::object_ptr<SessionRow>(
					content(),
					session,
					nullptr));
			} else {
				++others;
			}
		}
		if (others > 0) {
			const auto all = AddButton(
				content(),
				tr::lng_settings_sessions_terminate_others(tr::now),
				nullptr,
				[this] { terminateOthers(); });
			all->setDestructive(true);
			AddFootnote(
				content(),
				tr::lng_settings_sessions_terminate_others_about(tr::now));
			AddDivider(content());
			AddSubsectionTitle(
				content(),
				tr::lng_settings_other_sessions(tr::now));
			for (const auto &session : _sessions) {
				if (!session.current) {
					const auto copy = session;
					content()->add(Ui::object_ptr<SessionRow>(
						content(),
						session,
						[=, this] { terminate(copy); },
						[=, this] { showMenu(copy); }));
				}
			}
			AddFootnote(content(), tr::lng_settings_sessions_about(tr::now));
			AddFootnote(
				content(),
				tr::lng_settings_sessions_not_me_about(tr::now));
		} else {
			AddFootnote(content(), tr::lng_settings_sessions_only_this(tr::now));
		}
		showContent();
	}

	void terminate(const Data::DeviceSession &session) {
		const auto window = controller();
		const auto id = session.id;
		const auto name = session.deviceName.isEmpty()
			? PlatformTitle(session.platform)
			: session.deviceName;
		auto args = Ui::ConfirmBoxArgs();
		args.title = tr::lng_settings_session_terminate(tr::now);
		args.text = tr::lng_settings_session_terminate_sure(
			tr::now,
			lt_device,
			name);
		args.confirmText = tr::lng_settings_session_terminate(tr::now);
		args.destructive = true;
		args.confirmed = Guarded(this, [=, this](std::function<void()> close) {
			close();
			window->session().data().graph().revokeSession(id, this, [=, this] {
				_sessions.erase(
					std::remove_if(
						_sessions.begin(),
						_sessions.end(),
						[&](const Data::DeviceSession &session) {
							return session.id == id;
						}),
					_sessions.end());
				rebuild();
				window->showToast(
					tr::lng_settings_session_terminated(tr::now));
			}, [=](const Api::Error &error) {
				Support::ShowError(window, error);
			});
		});
		window->show(Ui::MakeConfirmBox(std::move(args)));
	}

	void showMenu(const Data::DeviceSession &session) {
		const auto menu = new Ui::PopupMenu(this);
		menu->addAction(tr::lng_settings_session_terminate(tr::now), [=, this] {
			terminate(session);
		}, &st::menuIconLogout);
		auto report = Ui::MenuItem();
		report.text = tr::lng_settings_session_not_me(tr::now);
		report.icon = &st::menuIconReport;
		report.destructive = true;
		report.callback = [=, this] {
			reportUnauthorized(session);
		};
		menu->addItem(std::move(report));
		menu->popup(QCursor::pos());
	}

	void reportUnauthorized(const Data::DeviceSession &session) {
		const auto window = controller();
		const auto id = session.id;
		const auto name = session.deviceName.isEmpty()
			? PlatformTitle(session.platform)
			: session.deviceName;
		auto args = Ui::ConfirmBoxArgs();
		args.title = name.isEmpty()
			? tr::lng_settings_session_not_me_title(tr::now)
			: tr::lng_settings_session_not_me_title_named(
				tr::now,
				lt_device,
				Lang::Isolated(name));
		args.text = tr::lng_settings_session_not_me_text(tr::now);
		args.confirmText = tr::lng_settings_session_not_me_confirm(tr::now);
		args.destructive = true;
		args.confirmed = Guarded(this, [=, this](std::function<void()> close) {
			close();
			window->session().data().graph().reportUnauthorizedSession(id, this, [=, this] {
				// The server also ends the sessions that came after it
				// (they re-verify): read the list again.
				load();
				window->showToast(tr::lng_settings_session_reported(tr::now));
			}, [=](const Api::Error &error) {
				Support::ShowError(window, error);
			});
		});
		window->show(Ui::MakeConfirmBox(std::move(args)));
	}

	void terminateOthers() {
		const auto window = controller();
		auto args = Ui::ConfirmBoxArgs();
		args.title = tr::lng_settings_sessions_terminate_others(tr::now);
		args.text = tr::lng_settings_sessions_terminate_others_sure(tr::now);
		args.confirmText = tr::lng_settings_session_terminate(tr::now);
		args.destructive = true;
		args.confirmed = Guarded(this, [=, this](std::function<void()> close) {
			close();
			window->session().data().graph().revokeOtherSessions(this, [=, this] {
				_sessions.erase(
					std::remove_if(
						_sessions.begin(),
						_sessions.end(),
						[](const Data::DeviceSession &session) {
							return !session.current;
						}),
					_sessions.end());
				rebuild();
				window->showToast(
					tr::lng_settings_sessions_terminated(tr::now));
			}, [=](const Api::Error &error) {
				Support::ShowError(window, error);
			});
		});
		window->show(Ui::MakeConfirmBox(std::move(args)));
	}

	QVector<Data::DeviceSession> _sessions;

};

} // namespace

void ShowExportBox(Window::SessionController *controller) {
	controller->session().data().graph().latestExport(controller, [=](
			const QJsonObject &status) {
		ShowExportStatusBox(controller, status);
	}, [=](const Api::Error &error) {
		Support::ShowError(controller, error);
	});
}

Window::SectionFactory AccountPageFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new AccountPage(parent, controller));
	};
}

Window::SectionFactory DevicesPageFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new DevicesPage(parent, controller));
	};
}

} // namespace Settings
