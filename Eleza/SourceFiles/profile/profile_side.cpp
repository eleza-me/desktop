#include "profile/profile_side.h"

#include "data/data_notifications.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "profile/profile_boxes.h"
#include "profile/profile_edit.h"
#include "profile/profile_follow_list.h"
#include "profile/profile_logic.h"
#include "profile/profile_widget.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

namespace Profile {

// Userpic, name, handle and the three counters; a click opens the profile.
class SideWidget::Summary final : public Ui::RippleButton {
public:
	Summary(QWidget *parent, Main::Session *session)
	: RippleButton(parent, st::defaultRipple)
	, _userpic(Ui::CreateChild<Support::UserpicButton>(
		this,
		session,
		QString(),
		st::profileSideUserpic)) {
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	void setProfile(const Data::Profile &profile) {
		_profile = profile;
		_userpic->setUserId(profile.id);
		_name.setText(st::profileSideNameStyle, profile.displayName);
		const auto item = [](qint64 value, const QString &label) {
			return Lang::CompactCount(value) + QLatin1Char(' ') + label;
		};
		_stats = item(profile.posts, tr::lng_profile_posts(tr::now))
			+ u" · "_s
			+ item(profile.followers, tr::lng_profile_followers(tr::now))
			+ u" · "_s
			+ item(profile.following, tr::lng_profile_following(tr::now));
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::profileSidePadding;
		_userpic->moveToLeft(padding.left(), padding.top(), newWidth);
		return padding.top() + st::profileSideUserpic + padding.bottom();
	}

	void onStateChanged(State was, StateChangeSource source) override {
		RippleButton::onStateChanged(was, source);
		update();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		if (isOver()) {
			p.fillRect(rect(), st::stateHover);
		}
		paintRipple(p, 0, 0);
		const auto outer = width();
		const auto &padding = st::profileSidePadding;
		const auto left = padding.left()
			+ st::profileSideUserpic
			+ st::profilePersonUserpicSkip;
		const auto available = std::max(outer - left - padding.right(), 0);
		const auto &nameFont = st::profileSideNameStyle.font;
		const auto &lineFont = st::profilePersonStatusFont;
		const auto hasHandle = !_profile.handle.isEmpty();
		const auto total = nameFont->height
			+ (hasHandle ? (lineFont->height + st::space2xs) : 0)
			+ lineFont->height
			+ st::spaceXs;
		auto y = padding.top() + (int(st::profileSideUserpic) - total) / 2;

		const auto badge = _profile.verified
			? (st::profileRowVerifiedIcon.width() + st::profileBadgeSkip)
			: 0;
		const auto nameWidth = std::min(
			_name.maxWidth(),
			std::max(available - badge, 0));
		p.setPen(st::textPrimary);
		_name.drawLeftElided(p, left, y, nameWidth, outer);
		if (_profile.verified) {
			const auto &icon = st::profileRowVerifiedIcon;
			icon.paint(
				p,
				left + nameWidth + st::profileBadgeSkip,
				y + (nameFont->height - icon.height()) / 2,
				outer);
		}
		y += nameFont->height;
		p.setFont(lineFont->f);
		if (hasHandle) {
			y += st::space2xs;
			p.setPen(st::textSecondary);
			const auto handle = LtrText(QLatin1Char('@') + _profile.handle);
			p.drawTextLeft(left, y, outer, lineFont->elided(handle, available));
			y += lineFont->height;
		}
		y += st::spaceXs;
		p.setPen(st::textTertiary);
		p.drawTextLeft(left, y, outer, lineFont->elided(_stats, available));
	}

private:
	Support::UserpicButton *_userpic = nullptr;
	Data::Profile _profile;
	Ui::Text::String _name;
	QString _stats;

};

SideWidget::SideWidget(QWidget *parent, Window::SessionController *controller)
: SectionWidget(parent, controller)
, _bar(Ui::CreateChild<Ui::TopBar>(this))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this)) {
	setTopBar(_bar);
	_bar->setTitle(tr::lng_tab_me(tr::now));
	const auto settings = _bar->addAction(st::topBarSettingsButton);
	settings->setClickedCallback([=] {
		controller->openRoute(Window::Routes::Settings());
	});
	Ui::InstallTooltip(settings, [] { return tr::lng_settings(tr::now); });

	_layout = _scroll->setOwnedWidget(
		Ui::object_ptr<Ui::VerticalLayout>(_scroll));
	_summary = _layout->add(
		Ui::object_ptr<Summary>(_layout, &controller->session()));
	_summary->setClickedCallback([this] { openProfile(); });
	_layout->add(Ui::object_ptr<Ui::Divider>(_layout, st::sectionDivider));

	const auto row = [&](
			const QString &text,
			const style::icon *icon,
			Fn<void()> callback) {
		const auto button = _layout->add(Ui::object_ptr<Ui::SettingsButton>(
			_layout,
			text,
			st::profileSideButton));
		button->setIcon(icon);
		button->setClickedCallback(std::move(callback));
		return button;
	};
	row(
		tr::lng_profile_my_profile(tr::now),
		&st::profileSideProfileIcon,
		[this] { openProfile(); });
	row(tr::lng_profile_edit_profile(tr::now), &st::profileSideEditIcon, [=] {
		controller->showMainSection(EditFactory());
	});
	row(
		tr::lng_eli_bookmarks_title(tr::now),
		&st::profileSideBookmarksIcon,
		[this] { openBookmarks(); });
	_requests = row(
		tr::lng_eli_follow_requests(tr::now),
		&st::profileRequestsIcon,
		[=] { controller->showMainSection(FollowRequestsFactory()); });
	row(tr::lng_profile_share_profile(tr::now), &st::profileSideShareIcon, [=] {
		ShowShareBox(controller, controller->session().data().users().me());
	});
	_layout->add(Ui::object_ptr<Ui::Divider>(_layout, st::sectionDivider));
	row(tr::lng_settings(tr::now), &st::profileSideSettingsIcon, [=] {
		controller->openRoute(Window::Routes::Settings());
	});

	auto &data = controller->session().data();
	connect(&data.users(), &Data::Users::meChanged, this, [this] { refresh(); });
	connect(
		&data.notifications(),
		&Data::Notifications::unreadChanged,
		this,
		[this] { refresh(); });
	connect(
		controller,
		&Window::SessionController::columnsChanged,
		this,
		[this] { ensureMainProfile(); });
	connect(
		controller,
		&Window::SessionController::tabChanged,
		this,
		[this] { ensureMainProfile(); });
	refresh();
	Ui::PostponeCall(this, [this] { ensureMainProfile(); });
}

QString SideWidget::sectionKey() const {
	return u"profile/side"_s;
}

void SideWidget::refresh() {
	const auto &data = controller()->session().data();
	_summary->setProfile(data.users().me());
	const auto count = data.notifications().kind(u"follow_request"_s);
	_requests->setValueText((count > 0) ? Lang::Num(count) : QString());
}

// With two columns the main one shows my full profile next to the links.
void SideWidget::ensureMainProfile() {
	const auto controller = this->controller();
	if (controller->isSingleColumn()
		|| controller->currentTab() != Window::Tab::Me
		|| controller->mainStackDepth() > 0) {
		return;
	}
	auto options = Window::SectionShow();
	options.stack = false;
	options.animated = anim::type::instant;
	options.tab = Window::Tab::Me;
	controller->showMainSection(Factory(Source::Me()), options);
}

void SideWidget::openProfile() {
	auto options = Window::SectionShow();
	options.stack = false;
	options.tab = Window::Tab::Me;
	controller()->showMainSection(Factory(Source::Me()), options);
}

void SideWidget::openBookmarks() {
	const auto controller = this->controller();
	const auto current = controller->mainSection();
	if (current && current->sectionKey() == u"profile/me") {
		static_cast<Widget*>(current)->showTab(PostsTab::Bookmarks);
		return;
	}
	auto options = Window::SectionShow();
	options.stack = false;
	options.tab = Window::Tab::Me;
	controller->showMainSection(
		Factory(Source::Me(), PostsTab::Bookmarks),
		options);
}

void SideWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_scroll->setGeometry(
		0,
		_bar->height(),
		width(),
		std::max(height() - _bar->height(), 0));
}

} // namespace Profile
