// The screen that covers the whole window when the server refuses this build
// (HTTP 426 / api.min_client_version): the mark, «نسخهٔ تازه لازم است» and a button
// that opens the update page. Nothing under it can be reached.
#pragma once

#include "ui/rp_widget.h"
#include "ui/text/text.h"

namespace Ui {
class LinkButton;
class RoundButton;
} // namespace Ui

namespace Window {

class UpgradeBlocker final : public Ui::RpWidget {
public:
	UpgradeBlocker(QWidget *parent, const QString &updateUrl);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] int contentHeight(int textWidth) const;
	[[nodiscard]] int textWidth() const;

	QString _url;
	Ui::Text::String _title;
	Ui::Text::String _text;
	Ui::RoundButton *_update = nullptr;
	Ui::LinkButton *_quit = nullptr;
	int _contentTop = 0;

};

} // namespace Window
