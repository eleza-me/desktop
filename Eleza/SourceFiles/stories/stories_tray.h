// The strip of story circles on top of the feed: «استوری تو» first (my face
// with a «+»; a ring when I have stories; the posting progress), then the
// people I follow in the server's order (unseen, seen, muted). A click opens
// the viewer, a right click mutes; the wheel scrolls it sideways.
#pragma once

#include "ui/rp_widget.h"

#include <memory>

namespace Window {
class SessionController;
} // namespace Window

namespace Support {
class UserpicPainter;
} // namespace Support

namespace Stories {

class Tray final : public Ui::RpWidget {
public:
	Tray(QWidget *parent, Window::SessionController *controller);
	~Tray();

	// The feed came back on screen: read the tray again (debounced).
	void refresh();

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

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

	Window::SessionController *_controller = nullptr;
	std::unique_ptr<Support::UserpicPainter> _userpics;
	int _scroll = 0;
	Hit _over;
	Hit _pressed;
	qint64 _lastRefresh = 0;

};

} // namespace Stories
