// Intro::Widget — the host of the sign-in steps: a centered column with the
// current step, a back button in the leading corner, the theme and language
// switches in the trailing one, and the transitions between steps.
//
// The steps (intro_steps.h) share Intro::State through the widget and move on
// with showStep(). Debug: --debug-route=intro/<step> opens a step directly with
// sample data (welcome, phone, country, code, password, recovery, profile).
#pragma once

#include "api/api_auth.h"
#include "ui/effects/slide_animation.h"
#include "ui/layers/layer_widget.h"
#include "ui/rp_widget.h"

namespace Main {
class Account;
} // namespace Main

namespace Ui {
class BoxContent;
class IconButton;
class RoundButton;
} // namespace Ui

namespace Window {
class MainWindow;
} // namespace Window

namespace Intro {

class Step;

enum class StepType {
	Welcome,
	Phone,
	Code,
	Password,
	Recovery,
	Profile,
};

// What the steps know about the sign-in in progress.
struct State {
	QString countryIso; // of the picker ("" until the phone step set it)
	QString countryCode; // digits
	QString number; // as typed, digits
	QString phone; // E.164, once a code was requested
	bool adultConfirmed = false;

	Api::OtpChallenge challenge;
	qint64 resendAt = 0; // ms, local clock

	Api::TwoStepForm form;
	Api::RecoveryChallenge recovery;
	qint64 recoveryResendAt = 0;

	// A new sign-in registers a fresh device key once.
	bool freshKey = true;
};

class Widget final : public Ui::RpWidget {
public:
	Widget(QWidget *parent, Window::MainWindow *window);
	~Widget();

	[[nodiscard]] State &state() {
		return _state;
	}
	[[nodiscard]] Main::Account &account() const;
	[[nodiscard]] Window::MainWindow *window() const {
		return _window;
	}

	void showStep(StepType type, bool back = false);
	[[nodiscard]] StepType currentStep() const {
		return _type;
	}
	// A sign-in call answered: the session exists. Goes to the profile step
	// for a new account, into the app otherwise.
	void signedIn(const Api::VerifyOutcome &outcome);
	// The login ticket died (or the code expired): start over from the phone.
	void restartExpired();

	void show(Ui::object_ptr<Ui::BoxContent> box);
	void showToast(const QString &text);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;

private:
	class Transition;

	[[nodiscard]] Step *createStep(StepType type);
	void updateStepGeometry();
	void updateCorner();
	[[nodiscard]] QRect columnRect(int height) const;
	void switchLanguage();
#ifdef ELEZA_DEBUG_TOOLS
	void applyDebugRoute();
#endif // ELEZA_DEBUG_TOOLS

	Window::MainWindow *_window = nullptr;
	State _state;
	StepType _type = StepType::Welcome;
	Step *_step = nullptr;
	Ui::IconButton *_back = nullptr;
	Ui::IconButton *_theme = nullptr;
	Ui::IconButton *_language = nullptr;
	// «به‌روزرسانی الزا» in the bottom corner while a new version is ready.
	Ui::RoundButton *_updatePill = nullptr;
	Transition *_transition = nullptr;
	bool _night = true;

};

} // namespace Intro
