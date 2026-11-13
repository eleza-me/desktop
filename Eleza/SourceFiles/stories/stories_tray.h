// The strip of story circles on top of the feed: «استوری تو» first (my face
// with a «+»; a ring when I have stories; the posting progress), then the
// people I follow in the server's order (unseen, seen, muted). A click opens
// the viewer, a right click mutes; the wheel scrolls it sideways.
// Keyboard: a Tab stop; Left / Right (Home / End) move between the circles,
// Space / Enter open one, the menu key (Shift+F10) shows its menu. Screen
// readers see a list.
#pragma once

#include "ui/accessibility.h"
#include "ui/rp_widget.h"

#include <memory>

namespace Window {
class SessionController;
} // namespace Window

namespace Support {
class UserpicPainter;
} // namespace Support

namespace Stories {

class Tray final
	: public Ui::RpWidget
	, public Ui::Accessibility::ItemsSource {
public:
	Tray(QWidget *parent, Window::SessionController *controller);
	~Tray();

	// The feed came back on screen: read the tray again (debounced).
	void refresh();

	int accessibilityItemsCount() const override;
	Ui::Accessibility::Item accessibilityItem(int index) const override;
	void accessibilityItemPress(int index) override;
	int accessibilityFocusedItem() const override;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;

private:
	enum class Part {
		None,
		Item,
		Plus,
		ArrowBack,
		ArrowForward,
	};
	struct Hit {
		Part part = Part::None;
		int index = -1; // 0 = me
		friend bool operator==(const Hit &, const Hit &) = default;
	};

	[[nodiscard]] int count() const;
	[[nodiscard]] QString userIdAt(int index) const;
	[[nodiscard]] QRect itemRect(int index) const;   // widget coordinates
	[[nodiscard]] QRect userpicRect(int index) const;
	[[nodiscard]] QRect plusRect() const;
	[[nodiscard]] QRect arrowRect(bool forward) const;
	[[nodiscard]] int contentWidth() const;
	[[nodiscard]] int maxScroll() const;
	[[nodiscard]] Hit hitAt(QPoint point) const;
	void setScroll(int value);
	void activate(const Hit &hit);
	void showMenu(int index, QPoint global);
	void openMine();
	void updateVisibility();
	[[nodiscard]] QString nameAt(int index) const;
	void setKeyboardIndex(int index);

	Window::SessionController *_controller = nullptr;
	std::unique_ptr<Support::UserpicPainter> _userpics;
	int _scroll = 0;
	Hit _over;
	Hit _pressed;
	qint64 _lastRefresh = 0;
	int _keyboardIndex = -1; // the circle the keyboard is on
	bool _keyboardFocus = false;

};

} // namespace Stories
