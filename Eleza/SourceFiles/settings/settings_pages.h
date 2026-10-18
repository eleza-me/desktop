// The pages of the settings (main column) by section.
#pragma once

#include "settings/settings_logic.h"
#include "window/window_registry.h"

namespace Settings {

// The page of a section; null for an unknown one.
[[nodiscard]] Window::SectionFactory PageFactory(Section section);

// «تماس‌ها» belongs to the calls module (calls/calls_settings.cpp), which
// registers its page at startup. In a build without calls nobody does: the
// section is not listed and its route opens nothing.
void RegisterCallsPage(Window::SectionFactory factory);
[[nodiscard]] bool HasCallsPage();

// settings_account.cpp
[[nodiscard]] Window::SectionFactory AccountPageFactory();
[[nodiscard]] Window::SectionFactory DevicesPageFactory();
// settings_filters.cpp
[[nodiscard]] Window::SectionFactory FiltersPageFactory();
// «همیشه نشان بده به» / «هرگز نشان نده به» (kind always | never).
void ShowLastSeenExceptionsBox(
	Window::SessionController *controller,
	const QString &kind,
	std::function<void()> changed = nullptr);
// «درخواست‌های فرستاده‌شده»: the follow requests that wait, with «لغو».
void ShowSentRequestsBox(Window::SessionController *controller);
// settings_storage.cpp
[[nodiscard]] Window::SectionFactory StoragePageFactory();
// settings_update.cpp
[[nodiscard]] Window::SectionFactory UpdatePageFactory();

// «خروجی داده‌ها»: the state of the latest export, a new request (confirmed by
// an SMS code; needs the recovery e-mail of two-step verification) and the
// download of a finished one.
void ShowExportBox(Window::SessionController *controller);

} // namespace Settings
