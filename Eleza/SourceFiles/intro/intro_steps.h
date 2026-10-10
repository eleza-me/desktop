// The steps of the intro. Each one is a column of rows (a title, a description,
// fields, a hint / error line, the main button) laid out by a VerticalLayout;
// Intro::Widget centers it in the window.
#pragma once

#include "intro/intro_widget.h"
#include "ui/text/text.h"
#include "ui/wrap/vertical_layout.h"

namespace Intro {

// One line under a field: a hint, an error or a confirmation. Keeps the height
// of a line while empty so the layout does not jump.
class HintLine final : public Ui::RpWidget {
public:
	enum class Kind {
		Hint,
		Error,
		Ok,
	};

	explicit HintLine(QWidget *parent, style::align align = style::al_top);

	void setText(const QString &text, Kind kind = Kind::Hint);
	void setError(const QString &text) {
		setText(text, Kind::Error);
	}
	void clear() {
		setText(QString());
	}
	[[nodiscard]] bool empty() const {
		return _text.isEmpty();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	Ui::Text::String _text;
	Kind _kind = Kind::Hint;
	style::align _align = style::al_top;

};

class Step : public Ui::RpWidget {
public:
	Step(QWidget *parent, Widget *intro);

	// Where typing goes when the step is shown.
	virtual void setInnerFocus() {
		setFocus();
	}
	[[nodiscard]] virtual bool hasBack() const {
		return true;
	}
	// The back button / Esc.
	virtual void goBack() {
	}
	// Enter anywhere in the step.
	virtual void submit() {
	}
	// The welcome step lies on the night background.
	[[nodiscard]] virtual bool night() const {
		return false;
	}

protected:
	[[nodiscard]] Widget *intro() const {
		return _intro;
	}
	[[nodiscard]] State &state() const {
		return _intro->state();
	}
	[[nodiscard]] Ui::VerticalLayout *content() const {
		return _content;
	}
	// Rows used by every step.
	void addTitle(const QString &title, const QString &about = QString());

	int resizeGetHeight(int newWidth) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	Widget *_intro = nullptr;
	Ui::VerticalLayout *_content = nullptr;

};

[[nodiscard]] Step *CreateStep(StepType type, QWidget *parent, Widget *intro);

// Shows the country list over the phone step (also the "intro/country" debug route).
void ShowCountryBox(Widget *intro, std::function<void(const QString &iso)> chosen);

} // namespace Intro
