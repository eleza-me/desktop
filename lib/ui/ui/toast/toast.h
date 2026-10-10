// Toasts: short notices at the bottom centre of a window.
//
//   Ui::Toast::Show(window->bodyWidget(), tr::lng_copied(tr::now));
//   Ui::Toast::Show(parent, { .text = ..., .duration = 4000, .icon = &st::toastIconCheck });
//
// One toast is visible per parent; the next ones wait in a queue.
// (Feature code: controller->showToast(text).)
#pragma once

#include "ui/rp_widget.h"
#include "styles/style_widgets.h"

namespace Ui::Toast {

struct Config {
	QString text;
	int duration = 0; // ms, 0 = by the text length
	const style::icon *icon = nullptr;
	// Extra space kept under the toast (a bottom bar).
	int bottomSkip = 0;
};

void Show(QWidget *parent, const QString &text);
void Show(QWidget *parent, Config config);
// Hides the visible toast of the parent and drops the queue.
void HideAll(QWidget *parent);

} // namespace Ui::Toast
