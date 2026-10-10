// The screen that covers the whole window when this build is refused: by the
// server (HTTP 426 / api.min_client_version) or by the update manifest
// (min_supported). The mark, «نسخهٔ تازه لازم است», and the update itself: the
// checker downloads the new version whatever the settings say, a line under
// the button tells how far it is, and «به‌روزرسانی» restarts into it. A copy
// that cannot update itself (or a download that failed) gets the button that
// opens the download page instead. Nothing under the screen can be reached.
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
	void refresh();
	void updateGeometries();
	void openPage();

	QString _url;
	Ui::Text::String _title;
	Ui::Text::String _text;
	Ui::Text::String _status;
	Ui::RoundButton *_update = nullptr;
	Ui::LinkButton *_quit = nullptr;
	int _contentTop = 0;
	int _statusTop = 0;

};

} // namespace Window
