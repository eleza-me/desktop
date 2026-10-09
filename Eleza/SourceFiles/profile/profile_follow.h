// The follow button and the follow / unfollow action behind it, shared by the
// profile header and the people rows of follow lists.
//
//   const auto button = Ui::CreateChild<Profile::FollowButton>(this);
//   button->setState(Profile::FollowUiOf(profile));
//   button->setClickedCallback([=] {
//       Profile::PerformFollowToggle(controller, this, target, [=](Profile::FollowUi state, bool busy) { … });
//   });
#pragma once

#include "profile/profile_logic.h"
#include "ui/widgets/buttons.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Profile {

struct FollowTarget {
	QString id;
	QString name;
	bool isPrivate = false;
	bool followsYou = false;
	FollowUi state = FollowUi::None;
};

// One click on a follow button: the confirmation before unfollowing, the
// optimistic state, the request and its answer (or the way back with a toast).
// `update(state, busy)` is called for every visible change while `guard` lives.
void PerformFollowToggle(
	Window::SessionController *controller,
	QObject *guard,
	const FollowTarget &target,
	Fn<void(FollowUi state, bool busy)> update);

[[nodiscard]] QString FollowText(FollowUi state);
[[nodiscard]] const style::RoundButton &FollowStyle(FollowUi state, bool small);
[[nodiscard]] const style::icon *FollowIcon(FollowUi state);
// The width of the pill for a state (text, paddings; the icon when not small).
[[nodiscard]] int FollowPillWidth(FollowUi state, bool small);
// Paints a round button of the given style inside custom painted rows.
void PaintPill(
	QPainter &p,
	const QRect &rect,
	const style::RoundButton &st,
	const QString &text,
	bool over,
	bool down,
	const style::icon *icon = nullptr,
	double opacity = 1.);

class FollowButton final : public Ui::RippleButton {
public:
	explicit FollowButton(QWidget *parent);

	void setState(FollowUi state);
	[[nodiscard]] FollowUi state() const {
		return _state;
	}
	// Dimmed and not clickable while a request runs.
	void setBusy(bool busy);

	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	FollowUi _state = FollowUi::None;
	bool _busy = false;

};

} // namespace Profile
