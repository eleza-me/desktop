// Ui::TopBar — the header of a section: back button, title + subtitle, actions.
//
//   const auto bar = Ui::CreateChild<Ui::TopBar>(this);
//   bar->setTitle(name);  bar->setSubtitle(tr::lng_online(tr::now), true /*accent*/);
//   bar->setBackShown(true);  connect(bar, &Ui::TopBar::backRequested, ...);
//   const auto more = bar->addAction(st::topBarMoreButton);     // trailing side, in order
//   bar->setLeadingWidget(Ui::object_ptr<Ui::UserpicWidget>(...));  // between back and title
//   bar->resizeToWidth(width());                                 // height = st::topBarHeight
#pragma once

#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/effects/animations.h"
#include "styles/style_widgets.h"

#include <vector>

namespace Ui {

class IconButton;

class TopBar : public RpWidget {
	Q_OBJECT

public:
	explicit TopBar(QWidget *parent, const style::TopBar &st = st::defaultTopBar);

	void setTitle(const QString &title);
	// `accent` paints the subtitle in the brand color (online, typing…).
	void setSubtitle(const QString &subtitle, bool accent = false);
	void setBackShown(bool shown);
	// The title area becomes clickable (opens a profile).
	void setTitleClickable(bool clickable);
	// Title in the middle instead of the leading side.
	void setCentered(bool centered);

	IconButton *addAction(const style::IconButton &st);
	void clearActions();
	template <typename Widget>
	Widget *setLeadingWidget(object_ptr<Widget> widget) {
		const auto result = widget.data();
		doSetLeadingWidget(Owned<RpWidget>(widget.release()));
		return result;
	}
	[[nodiscard]] IconButton *backButton() const {
		return _back;
	}

Q_SIGNALS:
	void backRequested();
	void titleClicked();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void doSetLeadingWidget(object_ptr<RpWidget> widget);
	void updateControls();
	[[nodiscard]] QRect titleRect() const; // logical (leading-side) coordinates

	const style::TopBar &_st;
	IconButton *_back = nullptr;
	bool _backShown = false;
	object_ptr<RpWidget> _leading;
	std::vector<IconButton*> _actions;
	Text::String _title;
	Text::String _subtitle;
	bool _subtitleAccent = false;
	bool _titleClickable = false;
	bool _centered = false;
	bool _titlePressed = false;
	int _titleLeft = 0;
	int _titleRight = 0;

};

} // namespace Ui
