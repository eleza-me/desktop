#include "settings/settings_widget.h"

#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_pages.h"
#include "core/core_application.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "updater/updater_checker.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_settings_update.h"

namespace Settings {

// The signed-in user: userpic, name, @handle.
class Widget::ProfileRow final : public Ui::RippleButton {
public:
	ProfileRow(QWidget *parent, Main::Session *session)
	: RippleButton(parent, st::defaultRipple)
	, _session(session)
	, _userpic(new Support::UserpicButton(
		this,
		session,
		session->userId(),
		st::settingsProfileUserpic)) {
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		connect(
			&session->data().users(),
			&Data::Users::meChanged,
			this,
			[this] { update(); });
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_userpic->moveToLeft(
			st::settingsProfilePadding.left(),
			(st::settingsProfileHeight - st::settingsProfileUserpic) / 2,
			newWidth);
		return st::settingsProfileHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		if (isOver()) {
			p.fillRect(rect(), st::stateHover);
		}
		paintRipple(p, 0, 0);
		const auto &me = _session->data().users().me();
		const auto &padding = st::settingsProfilePadding;
		const auto left = padding.left()
			+ st::settingsProfileUserpic
			+ st::settingsProfileSkip;
		const auto &chevron = st::settingsProfileChevron;
		chevron.paint(
			p,
			width() - padding.right() - chevron.width(),
			(height() - chevron.height()) / 2,
			width());
		const auto available = std::max(
			width() - left - padding.right() - chevron.width() - st::spaceSm,
			0);
		const auto badge = me.verified
			? (st::settingsProfileVerified.width()
				+ st::settingsProfileVerifiedSkip)
			: 0;
		const auto name = st::settingsProfileNameFont->elided(
			me.displayName,
			std::max(available - badge, 0));
		const auto nameWidth = st::settingsProfileNameFont->width(name);
		const auto single = me.handle.isEmpty();
		const auto nameTop = single
			? ((height() - st::settingsProfileNameFont->height) / 2)
			: int(st::settingsProfileNameTop);
		p.setFont(st::settingsProfileNameFont);
		p.setPen(st::settingsProfileNameFg);
		p.drawTextLeft(left, nameTop, width(), name, nameWidth);
		if (me.verified) {
			st::settingsProfileVerified.paint(
				p,
				left + nameWidth + st::settingsProfileVerifiedSkip,
				nameTop
					+ (st::settingsProfileNameFont->height
						- st::settingsProfileVerified.height()) / 2,
				width());
		}
		if (!single) {
			p.setFont(st::settingsProfileStatusFont);
			p.setPen(st::settingsProfileStatusFg);
			p.drawTextLeft(
				left,
				st::settingsProfileStatusTop,
				width(),
				st::settingsProfileStatusFont->elided(
					Lang::Handle(me.handle),
					available));
		}
	}

private:
	Main::Session *_session = nullptr;
	Support::UserpicButton *_userpic = nullptr;

};

Widget::Widget(QWidget *parent, Window::SessionController *controller)
: SectionWidget(parent, controller)
, _bar(new Ui::TopBar(this))
, _scroll(new Ui::ScrollArea(this)) {
	_bar->setTitle(tr::lng_settings(tr::now));
	_content = _scroll->setOwnedWidget(
		Ui::object_ptr<Ui::VerticalLayout>(_scroll));
	setFocusPolicy(Qt::StrongFocus);
	build();

	connect(
		controller,
		&Window::SessionController::mainSectionChanged,
		this,
		[this] { refreshActive(); });
	connect(
		controller,
		&Window::SessionController::tabChanged,
		this,
		[this] { refreshActive(); });
	connect(
		controller,
		&Window::SessionController::columnsChanged,
		this,
		[this] { refreshActive(); });
	refreshActive();

	// «به‌روزرسانی» says when a new version waits there.
	if (const auto updater = Core::App().updater()) {
		connect(updater, &Updater::Checker::stateChanged, this, [this] {
			refreshUpdateValue();
		});
		refreshUpdateValue();
	}

	// The values next to «حریم خصوصی» and «منطقهٔ الی».
	auto &graph = controller->session().data().graph();
	connect(&graph, &Data::Graph::settingsChanged, this, [this] {
		refreshValues();
	});
	refreshValues();
	if (!graph.settingsLoaded()) {
		graph.loadSettings(false, this, [this](const QJsonObject &) {
			refreshValues();
		}, [](const Api::Error &) {
			// The rows simply stay without values.
		});
	}
}

QString Widget::sectionKey() const {
	return Window::Routes::Settings();
}

void Widget::setInnerFocus() {
	setFocus();
}

Ui::SettingsButton *Widget::addSection(
		Section section,
		const style::icon *icon) {
	const auto window = controller();
	const auto button = _content->add(Ui::object_ptr<Ui::SettingsButton>(
		_content,
		SectionTitle(section),
		st::settingsSideButton));
	button->setIcon(icon);
	button->setClickedCallback([=] {
		window->openRoute(Window::Routes::Settings(SectionId(section)));
	});
	_sections.emplace_back(section, button);
	return button;
}

void Widget::build() {
	const auto window = controller();
	const auto profile = _content->add(Ui::object_ptr<ProfileRow>(
		_content,
		&window->session()));
	profile->setClickedCallback([=] {
		window->openRoute(Window::Routes::Me());
	});
	AddDivider(_content);
	addSection(Section::Account, &st::settingsIconAccount);
	addSection(Section::Privacy, &st::settingsIconPrivacy);
	addSection(Section::Notifications, &st::settingsIconNotifications);
	addSection(Section::Messages, &st::settingsIconMessages);
	if (HasCallsPage()) {
		addSection(Section::Calls, &st::settingsIconPhone);
	}
	addSection(Section::Filters, &st::settingsIconMuted);
	addSection(Section::Region, &st::settingsIconRegion);
	AddDivider(_content);
	addSection(Section::Appearance, &st::settingsIconAppearance);
	addSection(Section::Devices, &st::settingsIconDevices);
	addSection(Section::Storage, &st::settingsIconStorage);
	AddDivider(_content);
	addSection(Section::Update, &st::settingsIconUpdate);
	addSection(Section::About, &st::settingsIconAbout);
	const auto logout = _content->add(Ui::object_ptr<Ui::SettingsButton>(
		_content,
		tr::lng_sign_out(tr::now),
		st::settingsSideButton));
	logout->setIcon(&st::settingsIconLogout);
	logout->setDestructive(true);
	logout->setClickedCallback([=] { window->requestLogout(); });
	_content->add(
		Ui::object_ptr<Ui::FlatLabel>(
			_content,
			tr::lng_settings_version(
				tr::now,
				lt_version,
				Lang::Num(QString::fromLatin1(ELEZA_VERSION_STRING))),
			st::settingsVersionLabel),
		st::settingsVersionPadding);
}

void Widget::refreshActive() {
	auto active = std::optional<Section>();
	if (controller()->currentTab() == Window::Tab::Settings
		&& !controller()->isSingleColumn()) {
		if (const auto section = controller()->mainSection()) {
			const auto route = Window::ParseRoute(section->sectionKey());
			if (route.type == Window::RouteType::Settings) {
				active = SectionFromId(route.id);
			}
		}
	}
	for (const auto &[section, button] : _sections) {
		button->setForceRippled(active && (*active == section));
	}
}

void Widget::refreshUpdateValue() {
	const auto updater = Core::App().updater();
	if (!updater) {
		return;
	}
	const auto state = updater->state();
	const auto offered = (state == Updater::State::Available)
		|| (state == Updater::State::Downloading)
		|| (state == Updater::State::Preparing)
		|| (state == Updater::State::Ready);
	for (const auto &[section, button] : _sections) {
		if (section == Section::Update) {
			button->setValueText(offered
				? Lang::Num(updater->info().version)
				: QString());
		}
	}
}

void Widget::refreshValues() {
	auto &graph = controller()->session().data().graph();
	if (!graph.settingsLoaded()) {
		return;
	}
	for (const auto &[section, button] : _sections) {
		if (section == Section::Privacy) {
			button->setValueText(
				graph.settingValue(u"privacy.private_account"_s).toBool()
					? tr::lng_settings_private_account(tr::now)
					: tr::lng_settings_public_account(tr::now));
		} else if (section == Section::Region) {
			const auto mode = graph.settingValue(
				u"discovery.region_mode"_s).toString();
			const auto title = ProvinceTitle(graph.settingValue(
				u"discovery.province_code"_s).toString());
			button->setValueText((mode.isEmpty() || mode == u"off")
				? tr::lng_off(tr::now)
				: title);
		}
	}
}

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_scroll->setGeometry(
		0,
		_bar->height(),
		width(),
		std::max(height() - _bar->height(), 0));
}

} // namespace Settings
