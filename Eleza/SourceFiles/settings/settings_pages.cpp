// The settings pages that are lists of switches and choices: privacy,
// notifications, messages, region, appearance, about.
#include "settings/settings_pages.h"

#include "api/api_client_config.h"
#include "boxes/about_box.h"
#include "core/core_application.h"
#include "data/data_chats.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "dialogs/dialogs_module.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_logic.h"
#include "support/brand_mark.h"
#include "support/links.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "window/window_title_frame.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QTimeZone>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Settings {
namespace {

[[nodiscard]] Option Everyone() {
	return { u"everyone"_s, tr::lng_settings_audience_everyone(tr::now) };
}
[[nodiscard]] Option Followers() {
	return { u"followers"_s, tr::lng_settings_audience_followers(tr::now) };
}
[[nodiscard]] Option Following() {
	return { u"following"_s, tr::lng_settings_audience_following(tr::now) };
}
[[nodiscard]] Option Nobody() {
	return { u"nobody"_s, tr::lng_settings_audience_nobody(tr::now) };
}

[[nodiscard]] std::vector<Option> MessageAudience() {
	return { Everyone(), Followers(), Following(), Nobody() };
}

// ---- privacy

class PrivacyPage final : public SyncedPage {
public:
	PrivacyPage(QWidget *parent, Window::SessionController *controller)
	: SyncedPage(parent, controller, Section::Privacy) {
		load();
	}

private:
	void build() override {
		const auto mentioned = Option{
			u"mentioned"_s,
			tr::lng_settings_audience_mentioned(tr::now),
		};
		addToggle(
			tr::lng_settings_private_account(tr::now),
			u"privacy.private_account"_s,
			false);
		AddFootnote(content(), tr::lng_settings_private_account_about(tr::now));

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_who_can(tr::now));
		addPicker(
			tr::lng_settings_who_reply(tr::now),
			u"privacy.reply_default"_s,
			{ Everyone(), Followers(), Following(), mentioned },
			u"everyone"_s);
		addPicker(
			tr::lng_settings_who_mention(tr::now),
			u"privacy.mentions"_s,
			{ Everyone(), Followers(), Following() },
			u"everyone"_s);
		addPicker(
			tr::lng_settings_who_message(tr::now),
			u"privacy.messages"_s,
			MessageAudience(),
			u"everyone"_s);
		addPicker(
			tr::lng_settings_who_call(tr::now),
			u"privacy.calls"_s,
			{
				{
					u"thread_contacts"_s,
					tr::lng_settings_audience_chat_people(tr::now),
				},
				Nobody(),
			},
			u"thread_contacts"_s);
		addPicker(
			tr::lng_settings_who_join(tr::now),
			u"privacy.conversation_join"_s,
			{ Everyone(), Followers() },
			u"everyone"_s);

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_who_sees(tr::now));
		addPicker(
			tr::lng_settings_last_seen(tr::now),
			u"privacy.last_seen"_s,
			{
				Everyone(),
				Followers(),
				{
					u"chat_contacts"_s,
					tr::lng_settings_audience_chat_contacts(tr::now),
				},
				Nobody(),
			},
			u"everyone"_s);
		addPicker(
			tr::lng_settings_birthday(tr::now),
			u"privacy.birthday_visibility"_s,
			{ Everyone(), Followers(), Nobody() },
			u"followers"_s);
		addToggle(
			tr::lng_settings_birthday_year(tr::now),
			u"privacy.birthday_show_year"_s,
			false);

		AddDivider(content());
		AddSubsectionTitle(
			content(),
			tr::lng_settings_contacts_phone(tr::now));
		addPicker(
			tr::lng_settings_who_phone(tr::now),
			u"privacy.phone_number"_s,
			{
				Everyone(),
				{ u"contacts"_s, tr::lng_settings_audience_contacts(tr::now) },
				Nobody(),
			},
			u"contacts"_s);
		addToggle(
			tr::lng_settings_find_by_phone(tr::now),
			u"privacy.find_by_phone"_s,
			true);

		AddDivider(content());
		addToggle(
			tr::lng_settings_show_in_suggestions(tr::now),
			u"privacy.show_in_suggestions"_s,
			true);
		addToggle(
			tr::lng_settings_show_conversations(tr::now),
			u"privacy.show_conversations_on_profile"_s,
			true);
	}

};

// ---- notifications

class NotificationsPage final : public SyncedPage {
public:
	NotificationsPage(QWidget *parent, Window::SessionController *controller)
	: SyncedPage(parent, controller, Section::Notifications) {
		buildLocal();
		showContent();
		loadInline();
	}

private:
	using Field = bool Core::NotificationSettings::*;

	void addLocal(const QString &text, Field field) {
		AddToggle(
			content(),
			text,
			Core::App().settings().notifications().*field,
			[=](bool checked) {
				auto settings = Core::App().settings().notifications();
				settings.*field = checked;
				Core::App().settings().setNotifications(settings);
			});
	}

	// What this computer does; kept on this computer.
	void buildLocal() {
		AddSubsectionTitle(
			content(),
			tr::lng_settings_notify_this_device(tr::now));
		addLocal(
			tr::lng_settings_notify_desktop(tr::now),
			&Core::NotificationSettings::desktopEnabled);
		addLocal(
			tr::lng_settings_notify_sender(tr::now),
			&Core::NotificationSettings::showSender);
		addLocal(
			tr::lng_settings_notify_preview(tr::now),
			&Core::NotificationSettings::showPreview);
		addLocal(
			tr::lng_settings_notify_sound(tr::now),
			&Core::NotificationSettings::soundEnabled);
		addLocal(
			tr::lng_settings_notify_only_inactive(tr::now),
			&Core::NotificationSettings::onlyWhenInactive);
		addLocal(
			tr::lng_settings_notify_flash(tr::now),
			&Core::NotificationSettings::flashTaskbar);
		addLocal(
			tr::lng_settings_notify_count_muted(tr::now),
			&Core::NotificationSettings::countUnreadMuted);
		AddToggle(
			content(),
			tr::lng_settings_close_to_tray(tr::now),
			Core::App().settings().closeToTray(),
			[](bool checked) {
				Core::App().settings().setCloseToTray(checked);
			});
		AddDivider(content());
	}

	void build() override {
		AddSubsectionTitle(content(), tr::lng_tab_talk(tr::now));
		addToggle(
			tr::lng_settings_notify_conversations(tr::now),
			u"notifications.conversations.enabled"_s,
			true);
		addToggle(
			tr::lng_settings_notify_joined(tr::now),
			u"notifications.conversations.joined"_s,
			true);
		addToggle(
			tr::lng_settings_notify_new_message(tr::now),
			u"notifications.conversations.new_message"_s,
			true);
		addToggle(
			tr::lng_settings_notify_last_hour(tr::now),
			u"notifications.conversations.one_hour_left"_s,
			true);

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_tab_eli(tr::now));
		addToggle(
			tr::lng_settings_notify_likes(tr::now),
			u"notifications.eli.likes"_s,
			true);
		addToggle(
			tr::lng_settings_notify_replies(tr::now),
			u"notifications.eli.replies_mentions"_s,
			true);
		addToggle(
			tr::lng_settings_notify_reposts(tr::now),
			u"notifications.eli.reposts_quotes"_s,
			true);
		addToggle(
			tr::lng_settings_notify_followers(tr::now),
			u"notifications.eli.new_followers"_s,
			true);
		addToggle(
			tr::lng_settings_notify_subscribed(tr::now),
			u"notifications.eli.subscribed_posts"_s,
			true);
		addToggle(
			tr::lng_settings_notify_contacts(tr::now),
			u"notifications.contacts.joined"_s,
			true);

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_notify_how(tr::now));
		addToggle(
			tr::lng_settings_notify_lock_screen(tr::now),
			u"notifications.general.lock_screen_preview"_s,
			true);
		// Quiet hours need the time zone they are counted in.
		const auto path = u"notifications.quiet_hours.enabled"_s;
		const auto quiet = AddToggle(
			content(),
			tr::lng_settings_quiet_hours(tr::now),
			boolean(path, false),
			[this](bool checked) {
				savePatch(QJsonObject{ {
					u"notifications"_s,
					QJsonObject{ {
						u"quiet_hours"_s,
						QJsonObject{
							{ u"enabled"_s, checked },
							{
								u"tz"_s,
								QString::fromLatin1(
									QTimeZone::systemTimeZoneId()),
							},
						},
					} },
				} });
			});
		onRefresh([=, this] {
			const auto now = boolean(path, false);
			if (quiet->toggled() != now) {
				quiet->setToggle(now, true);
			}
		});
		AddFootnote(content(), tr::lng_settings_quiet_hours_about(tr::now));
	}

};

// ---- messages

class MessagesPage final : public SyncedPage {
public:
	MessagesPage(QWidget *parent, Window::SessionController *controller)
	: SyncedPage(parent, controller, Section::Messages) {
		load();
	}

private:
	void build() override {
		const auto window = controller();
		auto &session = window->session();
		addPicker(
			tr::lng_settings_who_can_message(tr::now),
			u"privacy.messages"_s,
			MessageAudience(),
			u"everyone"_s);
		const auto requests = AddLinkButton(
			content(),
			tr::lng_settings_message_requests(tr::now),
			nullptr,
			[=] { Dialogs::ShowRequests(window); });
		const auto chats = &session.data().chats();
		const auto refreshRequests = [=] {
			const auto count = chats->count(Data::Chats::Section::Requests);
			requests->setValueText(count ? Lang::Num(count) : QString());
		};
		refreshRequests();
		connect(chats, &Data::Chats::listChanged, requests, refreshRequests);
		addToggle(
			tr::lng_settings_read_receipts(tr::now),
			u"privacy.read_receipts_and_online"_s,
			true);
		AddFootnote(content(), tr::lng_settings_read_receipts_about(tr::now));

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_tab_talk(tr::now));
		auto &config = session.account().clientConfig();
		auto durations = std::vector<Option>();
		const auto list = config.conversationDurations();
		for (const auto &duration : list) {
			durations.push_back({ duration, DurationTitle(duration) });
		}
		if (!durations.empty()) {
			auto fallback = config.conversationDefaultDuration();
			if (!list.contains(fallback)) {
				fallback = list.back();
			}
			addPicker(
				tr::lng_settings_default_duration(tr::now),
				u"messages.default_expiry"_s,
				durations,
				fallback);
		}
		addToggle(
			tr::lng_settings_masked_join(tr::now),
			u"privacy.conversation_masked_join_allowed"_s,
			true);

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_media(tr::now));
		addToggle(
			tr::lng_settings_autodownload_wifi(tr::now),
			u"media.autodownload_wifi_only"_s,
			false);
		addToggle(
			tr::lng_settings_autoplay_notes(tr::now),
			u"media.autoplay_video_notes"_s,
			true);
		addPicker(
			tr::lng_settings_autoplay_videos(tr::now),
			u"media.autoplay_videos"_s,
			{
				{ u"always"_s, tr::lng_settings_autoplay_always(tr::now) },
				{ u"wifi"_s, tr::lng_settings_autoplay_wifi(tr::now) },
				{ u"never"_s, tr::lng_settings_autoplay_never(tr::now) },
			},
			u"always"_s);
	}

};

// ---- region

class RegionPage final : public SyncedPage {
public:
	RegionPage(QWidget *parent, Window::SessionController *controller)
	: SyncedPage(parent, controller, Section::Region) {
		load();
	}

private:
	void build() override {
		const auto modePath = u"discovery.region_mode"_s;
		const auto codePath = u"discovery.province_code"_s;
		addPicker(
			tr::lng_settings_region(tr::now),
			modePath,
			{
				{ u"auto"_s, tr::lng_theme_automatic(tr::now) },
				{ u"manual"_s, tr::lng_settings_region_manual(tr::now) },
				{ u"off"_s, tr::lng_off(tr::now) },
			},
			u"off"_s);
		auto provinces = std::vector<Option>();
		for (const auto &province : Provinces()) {
			provinces.push_back({ province.code, province.title });
		}
		// Chosen by hand in the manual mode, found by the server otherwise.
		const auto picker = addPicker(
			tr::lng_settings_province(tr::now),
			codePath,
			provinces,
			QString());
		const auto info = AddInfoRow(
			content(),
			tr::lng_settings_province(tr::now),
			QString());
		onRefresh([=, this] {
			const auto mode = string(modePath, u"off"_s);
			const auto manual = (mode == u"manual");
			picker->setVisible(manual);
			info->setVisible(!manual);
			const auto title = ProvinceTitle(string(codePath, QString()));
			if (manual && title.isEmpty()) {
				picker->setValueText(tr::lng_settings_province_choose(tr::now));
			}
			info->setValueText((mode == u"off")
				? QString(QChar(0x2014))
				: title.isEmpty()
				? tr::lng_settings_province_unknown(tr::now)
				: title);
		});
		addToggle(
			tr::lng_settings_province_on_profile(tr::now),
			u"discovery.show_province_on_profile"_s,
			false);
		AddFootnote(content(), tr::lng_settings_region_about(tr::now));
	}

};

// ---- appearance (this computer only)

class AppearancePage final : public Page {
public:
	AppearancePage(QWidget *parent, Window::SessionController *controller)
	: Page(parent, controller, Section::Appearance) {
		build();
		showContent();
	}

private:
	template <typename Value>
	void addRadios(
			const std::vector<std::pair<Value, QString>> &options,
			Value current,
			std::function<void(Value)> chosen) {
		auto selected = 0;
		for (auto i = 0, count = int(options.size()); i != count; ++i) {
			if (options[i].first == current) {
				selected = i;
			}
		}
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(selected);
		for (auto i = 0, count = int(options.size()); i != count; ++i) {
			content()->add(
				Ui::object_ptr<Ui::Radiobutton>(
					content(),
					group,
					i,
					options[i].second),
				st::settingsRadioPadding);
		}
		group->setChangedCallback([=](int index) {
			if (index >= 0 && index < int(options.size())) {
				chosen(options[index].first);
			}
		});
	}

	// The window frame is built when the window is created: the choice
	// takes effect with the next start.
	void addTitleBarOption() {
		if (!Window::CustomTitleBarSupported()) {
			return;
		}
		AddToggle(
			content(),
			tr::lng_title_use_system(tr::now),
			Core::App().settings().systemTitleBar(),
			[](bool checked) {
				Core::App().settings().setSystemTitleBar(checked);
			});
		AddFootnote(content(), tr::lng_title_use_system_about(tr::now));
	}

	void build() {
		AddSubsectionTitle(content(), tr::lng_settings_theme(tr::now));
		addRadios<style::ThemeMode>(
			{
				{
					style::ThemeMode::System,
					tr::lng_settings_theme_system(tr::now),
				},
				{ style::ThemeMode::Light, tr::lng_theme_light(tr::now) },
				{ style::ThemeMode::Dark, tr::lng_theme_dark(tr::now) },
			},
			Core::App().settings().themeMode(),
			[](style::ThemeMode mode) { Core::App().setThemeMode(mode); });
		addTitleBarOption();

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_language(tr::now));
		addRadios<Lang::Language>(
			{
				{ Lang::Language::Persian, tr::lng_language_fa(tr::now) },
				{ Lang::Language::English, tr::lng_language_en(tr::now) },
			},
			Core::App().settings().language(),
			[this](Lang::Language language) {
				// The whole window is rebuilt for the new direction: this
				// page included, so nothing may follow the call.
				Ui::CallDelayed(st::motionFast, this, [=] {
					Core::App().setLanguage(language);
				});
			});

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_send_key(tr::now));
		addRadios<bool>(
			{
				{ false, tr::lng_settings_send_enter(tr::now) },
				{ true, tr::lng_settings_send_ctrl_enter(tr::now) },
			},
			Core::App().settings().sendByCtrlEnter(),
			[](bool ctrl) { Core::App().settings().setSendByCtrlEnter(ctrl); });
		AddFootnote(content(), tr::lng_settings_send_key_about(tr::now));
	}

};

// ---- about

class AboutHeader final : public Ui::AbstractButton {
public:
	explicit AboutHeader(QWidget *parent) : AbstractButton(parent) {
		resize(width(), st::settingsAboutHeight);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto mark = QSize(st::settingsAboutMarkSize);
		auto top = st::spaceLg;
		Support::PaintBrandMark(p, QRect(
			(width() - mark.width()) / 2,
			top,
			mark.width(),
			mark.height()));
		top += mark.height() + st::spaceSm;
		p.setFont(st::settingsAboutNameFont);
		p.setPen(st::settingsAboutNameFg);
		p.drawTextCentered(
			QRect(0, top, width(), st::settingsAboutNameFont->height),
			tr::lng_settings_app_name(tr::now));
		top += st::settingsAboutNameFont->height + st::space2xs;
		p.setFont(st::settingsAboutVersionFont);
		p.setPen(st::settingsAboutVersionFg);
		p.drawTextCentered(
			QRect(0, top, width(), st::settingsAboutVersionFont->height),
			tr::lng_boxes_about_version(
				tr::now,
				lt_version,
				Lang::Num(QString::fromLatin1(ELEZA_VERSION_STRING))));
	}
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::settingsAboutHeight;
	}

};

class AboutPage final : public Page {
public:
	AboutPage(QWidget *parent, Window::SessionController *controller)
	: Page(parent, controller, Section::About) {
		const auto window = controller;
		const auto open = [=](const QString &link) {
			return [=] { Support::OpenLink(window, link); };
		};
		const auto header = content()->add(
			Ui::object_ptr<AboutHeader>(content()));
		header->setClickedCallback([=] {
			window->show(Ui::Box<Boxes::AboutBox>());
		});
		AddDivider(content());
		AddLinkButton(
			content(),
			tr::lng_settings_about_help(tr::now),
			&st::settingsIconHelp,
			open(u"mailto:support@eleza.me"_s));
		AddLinkButton(
			content(),
			tr::lng_settings_about_report(tr::now),
			&st::settingsIconReport,
			open(u"mailto:bugs@eleza.me"_s));
		AddDivider(content());
		AddLinkButton(
			content(),
			tr::lng_boxes_about_terms(tr::now),
			&st::settingsIconExternal,
			open(u"https://eleza.me/terms"_s));
		AddLinkButton(
			content(),
			tr::lng_boxes_about_privacy(tr::now),
			&st::settingsIconExternal,
			open(u"https://eleza.me/privacy"_s));
		AddLinkButton(
			content(),
			tr::lng_boxes_about_guidelines(tr::now),
			&st::settingsIconExternal,
			open(u"https://eleza.me/guidelines"_s));
		AddDivider(content());
		AddLinkButton(
			content(),
			tr::lng_settings_about_this_version(tr::now),
			&st::settingsIconAbout,
			[=] { window->show(Ui::Box<Boxes::AboutBox>()); });
		AddLinkButton(
			content(),
			tr::lng_settings_about_licenses(tr::now),
			&st::settingsIconExport,
			[=] {
				window->show(Ui::MakeInformBox(
					tr::lng_settings_about_licenses_text(tr::now),
					tr::lng_settings_about_licenses(tr::now)));
			});
		showContent();
	}

};

template <typename PageType>
[[nodiscard]] Window::SectionFactory Make() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new PageType(parent, controller));
	};
}

} // namespace

Window::SectionFactory PageFactory(Section section) {
	switch (section) {
	case Section::Account: return AccountPageFactory();
	case Section::Privacy: return Make<PrivacyPage>();
	case Section::Notifications: return Make<NotificationsPage>();
	case Section::Messages: return Make<MessagesPage>();
	case Section::Filters: return FiltersPageFactory();
	case Section::Region: return Make<RegionPage>();
	case Section::Appearance: return Make<AppearancePage>();
	case Section::Devices: return DevicesPageFactory();
	case Section::Storage: return StoragePageFactory();
	case Section::About: return Make<AboutPage>();
	}
	return nullptr;
}

} // namespace Settings
