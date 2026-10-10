// Tooltips in our own style.
//
//   Ui::InstallTooltip(button, [] { return tr::lng_settings(tr::now); });  // on hover
//   Ui::Tooltip::Show(anchorWidget, text);  Ui::Tooltip::Hide();           // manual
#pragma once

#include "ui/rp_widget.h"
#include "styles/style_widgets.h"

#include <functional>

namespace Ui {

class Tooltip final : public RpWidget {
public:
	// Shows below (or above, if there is no room) the anchor widget.
	static void Show(QWidget *anchor, const QString &text);
	// Shows near a global point (the cursor).
	static void ShowAt(const QPoint &globalPosition, const QString &text);
	static void Hide();
	[[nodiscard]] static bool IsShown();

	// The tooltip bubble as an image (previews, tests).
	[[nodiscard]] static QPixmap Render(const QString &text, double ratio);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	Tooltip();

	void setTextAndResize(const QString &text);
	void place(const QRect &globalAnchor);

	QString _text;
	QStringList _lines;

};

// Shows `text()` after a short hover over `widget`; hides on leave / press.
void InstallTooltip(QWidget *widget, std::function<QString()> text);

// Hover tooltips are not shown while suppressed (scripted screenshot runs: the
// pointer of a headless platform rests at the window origin and would raise
// the tooltip of whatever is there). Manual Tooltip::Show() is not affected.
void SetHoverTooltipsSuppressed(bool suppressed);
[[nodiscard]] bool HoverTooltipsSuppressed();

} // namespace Ui
