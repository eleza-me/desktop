// Notes over the chat list (Data::Notes; backend docs/notes.md), the way the
// web app shows them on a desktop:
//
//  * NotesRow — the row on top of the list, scrolling away with it: «نوت تو»
//    first (my face, my note or «نوت بذار» in its bubble, a «+» without one),
//    then my friends' notes in the server's order. A click on mine opens «نوت
//    جدید», on a friend's their note with a reply field; a right click has the
//    rest (a message, the profile, hiding the row). Sideways: the arrows at
//    its ends, a horizontal wheel or Shift + wheel.
//  * NotesFaces — the first three friends' faces next to the title «گفتگو»
//    while the row is out of sight (Telegram's collapsed stories): a click
//    brings the row back and opens that friend's note.
//
// The boxes are in dialogs_notes_box.h.
#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"

#include <QtCore/QStringList>

#include <memory>

namespace Window {
class SessionController;
} // namespace Window

namespace Support {
class UserpicPainter;
} // namespace Support

namespace Dialogs {

class NotesRow final : public Ui::RpWidget {
public:
	NotesRow(QWidget *parent, Window::SessionController *controller);
	~NotesRow();

	// «پنهان کردن نوت‌ها» in the menu.
	void setHideCallback(Fn<void()> callback);

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
		ArrowBack,
		ArrowForward,
	};
	struct Hit {
		Part part = Part::None;
		int index = -1; // 0 = mine
		friend bool operator==(const Hit &, const Hit &) = default;
	};

	[[nodiscard]] int count() const;
	[[nodiscard]] QRect itemRect(int index) const;
	[[nodiscard]] QRect userpicRect(int index) const;
	[[nodiscard]] QRect arrowRect(bool forward) const;
	[[nodiscard]] int maxScroll() const;
	[[nodiscard]] Hit hitAt(QPoint point) const;
	void setScroll(int value);
	void activate(const Hit &hit);
	void showMenu(int index, QPoint global);

	Window::SessionController *_controller = nullptr;
	std::unique_ptr<Support::UserpicPainter> _userpics;
	Fn<void()> _hide;
	int _scroll = 0;
	Hit _over;
	Hit _pressed;

};

class NotesFaces final : public Ui::RpWidget {
public:
	NotesFaces(QWidget *parent, Window::SessionController *controller);
	~NotesFaces();

	// The friends whose faces show (at most three are taken).
	void setUsers(const QStringList &userIds);
	[[nodiscard]] bool empty() const {
		return _users.isEmpty();
	}
	void setClickedCallback(Fn<void(const QString &userId)> callback);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	[[nodiscard]] QRect faceRect(int index) const;
	[[nodiscard]] int faceAt(QPoint point) const;

	Window::SessionController *_controller = nullptr;
	std::unique_ptr<Support::UserpicPainter> _userpics;
	Fn<void(const QString &)> _clicked;
	QStringList _users;
	int _over = -1;
	int _pressed = -1;

};

// The first friends' faces for NotesFaces, in the row's order.
inline constexpr auto kNotesFacesMax = 3;

// The speech bubble of a note: a rounded box with two dots under it, toward
// the face below. A close-friends note has a green rim.
void PaintNoteBubble(
	QPainter &p,
	const QRect &rect,
	int radius,
	bool closeFriends);

} // namespace Dialogs
