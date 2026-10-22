// The panel that opens from the composer's emoji button: an «emoji» tab (a
// categorised grid with recents and search by name) and a «stickers» tab
// (recent stickers, the installed packs, adding / removing packs).
//
//   _panel = Ui::CreateChild<TabbedPanel>(section, &session);
//   connect(_panel, &TabbedPanel::emojiChosen, ...);   // the panel stays open
//   connect(_panel, &TabbedPanel::stickerChosen, ...); // the owner hides it
//   _panel->toggle(anchorButton);
#pragma once

#include "data/data_types.h"
#include "ui/effects/animations.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class ScrollArea;
class TabBar;
} // namespace Ui

namespace History {

class TabbedPanel final : public Ui::RpWidget {
	Q_OBJECT

public:
	TabbedPanel(QWidget *parent, Main::Session *session);
	~TabbedPanel();

	// Shows above `anchor` (a widget inside the same parent tree), its
	// trailing edge aligned with the anchor's; hides when shown.
	void toggle(QWidget *anchor);
	void showAbove(QWidget *anchor);
	void hideAnimated();
	void hideFast();
	[[nodiscard]] bool shown() const {
		return _shown;
	}
	// Opens on the stickers tab (the debug hooks and «add stickers» links).
	void showStickers();
	// Keeps the panel above the anchor after the parent was resized.
	void updatePosition();

Q_SIGNALS:
	void emojiChosen(const QString &emoji);
	void stickerChosen(const Data::Sticker &sticker);
	void panelShownChanged(bool shown);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	class EmojiPane;
	class StickersPane;

	[[nodiscard]] QRect bodyRect() const;
	void switchTab(int index);
	void place();

	Main::Session *_session = nullptr;
	Ui::TabBar *_tabs = nullptr;
	EmojiPane *_emoji = nullptr;
	StickersPane *_stickers = nullptr;
	QPointer<QWidget> _anchor;
	bool _shown = false;
	bool _stickersAllowed = true;
	Ui::Animations::Simple _showAnimation;

};

} // namespace History
