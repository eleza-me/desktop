// Two-state controls.
//
//   const auto check = Ui::CreateChild<Ui::Checkbox>(this, text, false);
//   connect(check, &Ui::Checkbox::checkedChanged, ...);
//
//   const auto toggle = Ui::CreateChild<Ui::Toggle>(this, true);
//
//   const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
//   Ui::CreateChild<Ui::Radiobutton>(this, group, 0, textA);
//   Ui::CreateChild<Ui::Radiobutton>(this, group, 1, textB);
//   group->setChangedCallback([=](int value) { ... });
#pragma once

#include "ui/widgets/buttons.h"
#include "ui/text/text.h"

#include <functional>
#include <memory>
#include <vector>

namespace Ui {

// The animated views; usable on their own inside custom rows.
class ToggleView final {
public:
	ToggleView(
		const style::Toggle &st,
		bool checked,
		std::function<void()> update);

	void setChecked(bool checked, bool animated);
	[[nodiscard]] bool checked() const {
		return _checked;
	}
	[[nodiscard]] QSize size() const;
	// `left` from the leading side of `outerw`.
	void paint(QPainter &p, int left, int top, int outerw);
	void setDisabled(bool disabled) {
		_disabled = disabled;
	}

private:
	const style::Toggle &_st;
	bool _checked = false;
	bool _disabled = false;
	std::function<void()> _update;
	Animations::Simple _animation;

};

class CheckView final {
public:
	CheckView(
		const style::Check &st,
		bool checked,
		std::function<void()> update);

	void setChecked(bool checked, bool animated);
	[[nodiscard]] bool checked() const {
		return _checked;
	}
	[[nodiscard]] QSize size() const;
	void paint(QPainter &p, int left, int top, int outerw);

private:
	const style::Check &_st;
	bool _checked = false;
	std::function<void()> _update;
	Animations::Simple _animation;

};

class RadioView final {
public:
	RadioView(
		const style::Radio &st,
		bool checked,
		std::function<void()> update);

	void setChecked(bool checked, bool animated);
	[[nodiscard]] bool checked() const {
		return _checked;
	}
	[[nodiscard]] QSize size() const;
	void paint(QPainter &p, int left, int top, int outerw);

private:
	const style::Radio &_st;
	bool _checked = false;
	std::function<void()> _update;
	Animations::Simple _animation;

};

// A check box with a (possibly multi-line) label.
class Checkbox : public RippleButton {
	Q_OBJECT

public:
	Checkbox(
		QWidget *parent,
		const QString &text,
		bool checked = false,
		const style::Checkbox &st = st::defaultCheckbox);
	~Checkbox();

	void setText(const QString &text);
	[[nodiscard]] bool checked() const;
	void setChecked(bool checked, bool animated = true);

	[[nodiscard]] int naturalWidth() const override;

Q_SIGNALS:
	void checkedChanged(bool checked);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	const style::Checkbox &_st;
	std::unique_ptr<CheckView> _check;
	Text::String _text;

};

// A standalone switch.
class Toggle : public AbstractButton {
	Q_OBJECT

public:
	Toggle(
		QWidget *parent,
		bool checked = false,
		const style::Toggle &st = st::defaultToggle);

	[[nodiscard]] bool checked() const;
	void setChecked(bool checked, bool animated = true);

Q_SIGNALS:
	void checkedChanged(bool checked);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	ToggleView _view;

};

class Radiobutton;

// Shared value of a set of radio buttons.
class RadiobuttonGroup final {
public:
	RadiobuttonGroup() = default;
	explicit RadiobuttonGroup(int value) : _value(value), _hasValue(true) {
	}

	void setChangedCallback(std::function<void(int value)> callback) {
		_changedCallback = std::move(callback);
	}
	[[nodiscard]] bool hasValue() const {
		return _hasValue;
	}
	[[nodiscard]] int value() const {
		return _value;
	}
	void setValue(int value);

private:
	friend class Radiobutton;

	void registerButton(Radiobutton *button);
	void unregisterButton(Radiobutton *button);

	int _value = 0;
	bool _hasValue = false;
	std::function<void(int value)> _changedCallback;
	std::vector<Radiobutton*> _buttons;

};

class Radiobutton : public RippleButton {
public:
	Radiobutton(
		QWidget *parent,
		const std::shared_ptr<RadiobuttonGroup> &group,
		int value,
		const QString &text,
		const style::Checkbox &st = st::defaultCheckbox,
		const style::Radio &radioSt = st::defaultRadio);
	~Radiobutton();

	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	friend class RadiobuttonGroup;

	void handleNewGroupValue(int value);

	const style::Checkbox &_st;
	std::shared_ptr<RadiobuttonGroup> _group;
	int _value = 0;
	RadioView _radio;
	Text::String _text;

};

} // namespace Ui
