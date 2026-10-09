// The UI gallery: every widget of lib/ui on a few pages, switchable between Persian /
// English and light / dark at runtime. Opened with `Eleza --gallery[=<page>]`.
// It is the visual test bed of the library and a source of usage examples.
#pragma once

#include "ui/rp_widget.h"

#include <QtCore/QStringList>

namespace Ui {
class IconButton;
class ScrollArea;
class TabBar;
class TopBar;
class VerticalLayout;
} // namespace Ui

namespace Window {
class MainWindow;
} // namespace Window

namespace Gallery {

class Widget final : public Ui::RpWidget {
	Q_OBJECT

public:
	// `page`: buttons, inputs, text, lists, layers, misc.
	// `overlay`: something to open right away on the layers page
	// (box, confirm, sheet, toast, menu, dropdown).
	Widget(
		QWidget *parent,
		Window::MainWindow *window,
		const QString &page,
		const QString &overlay);

	[[nodiscard]] static QStringList PageIds();

Q_SIGNALS:
	void pageChanged(QString page);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void showPage(int index);
	void updateThemeButton();

	Window::MainWindow *_window = nullptr;
	Ui::TopBar *_bar = nullptr;
	Ui::IconButton *_theme = nullptr;
	Ui::IconButton *_language = nullptr;
	Ui::TabBar *_tabs = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	int _page = 0;
	QString _overlay;

};

} // namespace Gallery
