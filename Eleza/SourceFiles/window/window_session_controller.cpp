#include "window/window_session_controller.h"

#include "ui/layers/box_content.h"
#include "window/window_main_layout.h"
#include "window/window_main_window.h"
#include "window/window_section_widget.h"

namespace Window {

SessionController::SessionController(MainWindow *window, Main::Session *session)
: QObject(window)
, _window(window)
, _session(session) {
}

SessionController::~SessionController() = default;

Ui::RpWidget *SessionController::bodyWidget() const {
	return _window->bodyWidget();
}

void SessionController::setLayout(MainLayout *layout) {
	_layout = layout;
}

void SessionController::showTab(Tab tab) {
	if (_layout) {
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
	_window->showToast(text);
}

void SessionController::showToast(Ui::Toast::Config config) {
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
