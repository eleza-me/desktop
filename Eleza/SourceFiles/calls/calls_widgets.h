// Small widgets of the call screens.
//
//   Calls::Button     a round button on the night surface: a disc with an icon
//                     (mute, end, devices…); setActive(true) is the "on" look of
//                     a toggle. Styles: st::callsBarMute, st::callsViewEnd, …
//   Calls::LabeledButton  the same with a caption below (the expanded view).
#pragma once

#include "ui/widgets/buttons.h"
#include "styles/style_calls.h"

namespace Calls {

class Button final : public Ui::RippleButton {
public:
	Button(QWidget *parent, const style::CallButton &st);

	void setActive(bool active);
	[[nodiscard]] bool active() const {
		return _active;
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;
	QImage prepareRippleMask() const override;

private:
	const style::CallButton &_st;
	bool _active = false;

};

class LabeledButton final : public Ui::RpWidget {
public:
	LabeledButton(
		QWidget *parent,
		const style::CallButton &st,
		const QString &label);

	[[nodiscard]] Button *button() const {
		return _button;
	}
	void setLabel(const QString &label);
	void setClickedCallback(std::function<void()> callback);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	Button *_button = nullptr;
	QString _label;

};

} // namespace Calls
