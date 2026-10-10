// Pages of the UI gallery. Each function fills a vertical layout with examples.
#pragma once

#include "ui/wrap/vertical_layout.h"

namespace Window {
class MainWindow;
} // namespace Window

namespace Gallery {

struct Context {
	Window::MainWindow *window = nullptr;
	// An overlay to open once the page is shown (the layers page).
	QString overlay;
};

void FillButtonsPage(Ui::VerticalLayout *content, const Context &context);
void FillInputsPage(Ui::VerticalLayout *content, const Context &context);
void FillTextPage(Ui::VerticalLayout *content, const Context &context);
void FillListsPage(Ui::VerticalLayout *content, const Context &context);
void FillLayersPage(Ui::VerticalLayout *content, const Context &context);
void FillMiscPage(Ui::VerticalLayout *content, const Context &context);

} // namespace Gallery
