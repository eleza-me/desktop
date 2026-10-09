// «حساب کاربری» (phone, handle, devices, two-step verification, data export,
// sign out, delete account) and «دستگاه‌ها» (the device sessions).
#include "settings/settings_pages.h"

#include "api/api_client.h"
#include "api/api_client_config.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_code_box.h"
#include "settings/settings_common.h"
#include "settings/settings_two_step.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/tooltip.h"
#include "ui/ui_utility.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>

namespace Settings {
namespace {

using Controller = Window::SessionController;

[[nodiscard]] QString Isolated(const QString &text) {
	return QChar(0x2066) + text + QChar(0x2069);
}

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
	controller->session().api().request(Api::Post, u"/v1/account/otp"_s)
		.body(QJsonObject{
			{ u"purpose"_s, u"data_export"_s },
			{ u"device_id"_s, controller->session().account().deviceId() },
		})
		.guard(guard)
		.done(std::move(done))
		.fail(std::move(fail))
		.send();
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
			controller->session().api().request(
				Api::Post,
				u"/v1/me/exports"_s
			).body(QJsonObject{
				{ u"challenge_id"_s, *challenge },
				{ u"code"_s, code },
				{
					u"device_id"_s,
					controller->session().account().deviceId(),
				},
			}).guard(controller).done([=](const QJsonObject &) {
				done();
				controller->showToast(
					tr::lng_settings_export_requested(tr::now));
			}).fail([=](const Api::Error &error) {
				fail(Support::ErrorText(error));
			}).send();
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
				return qint64(status.value(key).toDouble());
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

		const auto phone = AddInfoRow(
			content(),
			tr::lng_settings_phone(tr::now),
			QString(QChar(0x2026)),
			&st::settingsIconPhone);
		session.data().graph().accountPhone(this, [=](const QString &value) {
			phone->setValueText(value.isEmpty()
				? QString(QChar(0x2014))
				: Isolated(Lang::Num(value)));
		});

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
				: Isolated(QLatin1Char('@') + value));
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
			[=] {
				window->show(Ui::MakeInformBox(
					tr::lng_settings_delete_account_app_only(tr::now),
					tr::lng_settings_delete_account(tr::now)));
			});
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
		std::function<void()> terminate)
	: RpWidget(parent)
	, _session(session)
	, _subtitle(SessionSubtitle(
		session,
		QDateTime::currentMSecsSinceEpoch())) {
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

private:
	Data::DeviceSession _session;
	QString _subtitle;
	Ui::IconButton *_terminate = nullptr;

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
						[=, this] { terminate(copy); }));
				}
			}
			AddFootnote(content(), tr::lng_settings_sessions_about(tr::now));
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
