// What the window shows of the in-app updates (the checker itself is
// Core::App().updater(), updater/updater_checker.h):
//
//   UpdateRailButton   at the bottom of the nav rail, above «تنظیمات», when a
//                      new version is unpacked and ready: «به‌روزرسانی» — one
//                      click restarts into it. For a version that is only
//                      announced (automatic updates off, a copy that cannot
//                      update itself) the softer «نسخهٔ تازه», which opens the
//                      settings page with the details.
//   UpdateBanner       the same as a slim bar over the single column layout.
//   CreateUpdatePill   «به‌روزرسانی الزا» for the corner of the signed-out
//                      screen.
//
// All of them follow the checker and show / hide themselves.
#pragma once

#include "ui/widgets/buttons.h"

namespace Window {

enum class UpdateOffer {
	None,
	Ready,     // unpacked: a click installs it
	Announced, // known, not downloaded: a click shows the details
};
// What there is to offer right now.
[[nodiscard]] UpdateOffer CurrentUpdateOffer();
// Does what a click on the offer does. `showDetails` opens the settings page
// of the updates (the signed-out screen has none and passes null).
void ActivateUpdateOffer(std::function<void()> showDetails);

class UpdateRailButton final : public Ui::RippleButton {
public:
	UpdateRailButton(QWidget *parent, std::function<void()> showDetails);

	[[nodiscard]] UpdateOffer offer() const {
		return _offer;
	}
	// Called when offer() changed (the owner lays the rail out again).
	void setOfferChangedCallback(std::function<void()> callback) {
		_offerChanged = std::move(callback);
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;
	QPoint rippleOffset() const override;

private:
	void refresh();
	[[nodiscard]] QRect pillRect() const;

	UpdateOffer _offer = UpdateOffer::None;
	std::function<void()> _offerChanged;

};

class UpdateBanner final : public Ui::RippleButton {
public:
	UpdateBanner(QWidget *parent, std::function<void()> showDetails);

	[[nodiscard]] UpdateOffer offer() const {
		return _offer;
	}
	void setOfferChangedCallback(std::function<void()> callback) {
		_offerChanged = std::move(callback);
	}

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void refresh();

	UpdateOffer _offer = UpdateOffer::None;
	QString _text;
	QString _action;
	std::function<void()> _offerChanged;

};

// A small filled button, visible only while a version is ready to install.
// `changed` is called when it appears, disappears or changes its width.
[[nodiscard]] Ui::RoundButton *CreateUpdatePill(
	QWidget *parent,
	std::function<void()> changed);

} // namespace Window
