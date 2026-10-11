// «@» in the composer of a group: the members whose name or @handle matches
// what is typed after it, in a small list right above the field.
//
// The field keeps the keyboard: the composer passes Up / Down / Enter / Tab /
// Esc here while the list is shown, the mouse picks directly.
//
//   _mentions = Ui::CreateChild<MentionsDropdown>(panelParent, session);
//   _mentions->setChosenCallback([=](const Data::User &user) { insertMention(user); });
//   _mentions->showFor(users);          // an empty list hides it
//   _mentions->placeAbove(fieldRect);   // in the parent's coordinates
//
// What a mention is (where it starts, how it is written into the text, how
// its offsets go out) is decided by Data::MentionRules.
#pragma once

#include "data/data_types.h"
#include "support/media_image.h"
#include "ui/rp_widget.h"

#include <optional>

namespace Main {
class Session;
} // namespace Main

namespace History {

class MentionsDropdown final : public Ui::RpWidget {
public:
	MentionsDropdown(QWidget *parent, Main::Session *session);

	void showFor(const QVector<Data::User> &users);
	void hideFast();
	[[nodiscard]] bool shown() const {
		return !isHidden();
	}
	void selectSkip(int direction);
	[[nodiscard]] std::optional<Data::User> selected() const;
	void setChosenCallback(Fn<void(const Data::User &user)> callback) {
		_chosen = std::move(callback);
	}
	// Right above `anchor`, as wide as it (both in the parent's coordinates).
	void placeAbove(const QRect &anchor);

	static constexpr auto kMaxRows = 6;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	[[nodiscard]] int rowAt(QPoint position) const;

	Support::UserpicPainter _userpics;
	QVector<Data::User> _users;
	Fn<void(const Data::User &user)> _chosen;
	QRect _anchor;
	int _selected = 0;
	int _pressed = -1;

};

} // namespace History
