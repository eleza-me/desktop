#include "window/window_session_controller.h"

#include "core/core_application.h"
#include "core/core_settings.h"
#include "data/data_chats.h"
#include "data/data_notifications.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "ui/layers/box_content.h"
#include "ui/layers/generic_box.h"
#include "window/window_main_layout.h"
#include "window/window_main_window.h"
#include "window/window_section_widget.h"

namespace Window {

SessionController::SessionController(MainWindow *window, Main::Session *session)
: QObject(window)
, _window(window)
, _session(session) {
	Q_ASSERT(session != nullptr);
	auto &data = session->data();
	QObject::connect(
		&data.chats(),
		&Data::Chats::unreadChanged,
		this,
		[this] { refreshBadges(); });
	QObject::connect(
		&data.notifications(),
		&Data::Notifications::unreadChanged,
		this,
		[this] { refreshBadges(); });
	// «شمارش گفتگوهای بی‌صدا» changes what the taskbar badge counts.
	QObject::connect(
		&Core::App().settings(),
		&Core::Settings::changed,
		this,
		[this] { refreshBadges(); });
}

SessionController::~SessionController() = default;

Ui::RpWidget *SessionController::bodyWidget() const {
	return _window->bodyWidget();
}

Main::Account &SessionController::account() const {
	return _session->account();
}

void SessionController::setLayout(MainLayout *layout) {
	_layout = layout;
	if (layout) {
		refreshBadges();
	}
}

void SessionController::refreshBadges() {
	auto &data = _session->data();
	const auto chats = data.chats().unreadChats();
	// The window title and the taskbar / tray icon count unread chats; the
	// muted ones only when the user asked for them (a grey badge when there
	// is nothing else).
	const auto muted = Core::App().settings().notifications().countUnreadMuted
		? data.chats().unreadMutedChats()
		: 0;
	_window->setUnreadCount(chats + muted, (chats == 0) && (muted > 0));
	if (_layout) {
		_layout->setTabBadge(Tab::Chats, chats, false);
		_layout->setTabBadge(Tab::Eli, data.notifications().unread(), false);
	}
}

void SessionController::requestLogout() {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_settings_sign_out_sure(tr::now);
	args.text = tr::lng_settings_sign_out_about(tr::now);
	args.confirmText = tr::lng_sign_out(tr::now);
	args.destructive = true;
	const auto account = &_session->account();
	args.confirmed = [account](std::function<void()> close) {
		close();
		// The request is best effort; the account ends the session either way
		// and the application shows the intro.
		account->logout();
	};
	show(Ui::MakeConfirmBox(std::move(args)));
}

void SessionController::showTab(Tab tab) {
	if (_layout) {
		_layout->showTab(tab);
	}
}

void SessionController::activateTab(Tab tab) {
	if (_layout && _layout->currentTab() != tab) {
		_layout->showTab(tab);
	}
}

Tab SessionController::currentTab() const {
	return _layout ? _layout->currentTab() : Tab::Chats;
}

void SessionController::setSideSection(Tab tab, SectionFactory factory) {
	if (_layout) {
		_layout->setSideSection(tab, std::move(factory));
	}
}

SectionWidget *SessionController::sideSection(Tab tab) const {
	return _layout ? _layout->sideSection(tab) : nullptr;
}

void SessionController::setTabBadge(Tab tab, int count, bool muted) {
	if (_layout) {
		_layout->setTabBadge(tab, count, muted);
	}
}

void SessionController::requestSideSubTab(Tab tab, const QString &id) {
	_requestedSideSubTabs[int(tab)] = id;
	Q_EMIT sideSubTabRequested(tab, id);
}

QString SessionController::takeRequestedSideSubTab(Tab tab) {
	return std::exchange(_requestedSideSubTabs[int(tab)], QString());
}

void SessionController::showMainSection(
		SectionFactory factory,
		SectionShow options) {
	if (_layout) {
		_window->layers()->hideLayers();
		_layout->showMainSection(std::move(factory), options);
	}
}

bool SessionController::showBack() {
	return _layout && _layout->showBack();
}

void SessionController::clearMainStack() {
	if (_layout) {
		_layout->clearMainStack(_layout->currentTab());
	}
}

void SessionController::clearMainStack(Tab tab) {
	if (_layout) {
		_layout->clearMainStack(tab);
	}
}

LayoutState SessionController::saveState() const {
	return _layout ? _layout->saveState() : LayoutState();
}

void SessionController::restoreState(const LayoutState &state) {
	if (_layout) {
		_layout->restoreState(state);
	}
}

SectionWidget *SessionController::mainSection() const {
	return _layout ? _layout->mainSection() : nullptr;
}

int SessionController::mainStackDepth() const {
	return _layout ? _layout->mainStackDepth() : 0;
}

bool SessionController::isSingleColumn() const {
	return _layout && _layout->singleColumn();
}

void SessionController::show(
		Ui::object_ptr<Ui::BoxContent> box,
		Ui::LayerOption option) {
	_window->show(std::move(box), option);
}

void SessionController::showSheet(
		Ui::object_ptr<Ui::BoxContent> content,
		Ui::LayerOption option) {
	_window->showSheet(std::move(content), option);
}

void SessionController::hideLayer() {
	_window->layers()->hideTopLayer();
}

void SessionController::hideLayers() {
	_window->layers()->hideLayers();
}

void SessionController::showToast(const QString &text) {
	auto config = Ui::Toast::Config();
	config.text = text;
	showToast(std::move(config));
}

void SessionController::showToast(Ui::Toast::Config config) {
	// Over a box the toast sits at the bottom of the window; otherwise it
	// stays clear of what the visible section keeps at its bottom.
	if (!config.bottomSkip && !_window->layers()->hasLayers()) {
		if (const auto section = mainSection()) {
			config.bottomSkip = section->toastBottomSkip();
		}
	}
	Ui::Toast::Show(_window->bodyWidget(), std::move(config));
}

bool SessionController::openRoute(const QString &route) {
	return HandleRoute(this, route);
}

void SessionController::notifyTabChanged(Tab tab) {
	emit tabChanged(tab);
}

void SessionController::notifyMainSectionChanged() {
	emit mainSectionChanged();
}

void SessionController::notifyColumnsChanged(bool singleColumn) {
	emit columnsChanged(singleColumn);
}

} // namespace Window
