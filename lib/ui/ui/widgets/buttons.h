// Buttons. All of them are custom painted from a style struct (ui/widgets/widgets.style).
//
//   const auto send = Ui::CreateChild<Ui::RoundButton>(this, tr::lng_send(tr::now), st::defaultRoundButton);
//   send->setClickedCallback([=] { ... });
//   send->resizeToNaturalWidth(width());   // or setFullWidth(w)
//
//   AbstractButton   state machine: over / down / disabled, click callback + signal
//   RippleButton     + press ripple (override prepareRippleMask for the shape)
//   RoundButton      text button: st::defaultRoundButton (filled), lightRoundButton,
//                    dangerRoundButton, dangerLightButton, outlineRoundButton; loading state
//   FlatButton       RoundButton without a background (st::defaultFlatButton, box buttons)
//   IconButton       an icon with a circular hover / ripple
//   LinkButton       inline text link
//   SettingsButton   settings row: icon, label, value / chevron / toggle on the trailing side
//   PillButton       a chip (see also Ui::ChipsRow in ui/widgets/chips.h)
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/effects/ripple_animation.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>

#include <functional>
#include <memory>
#include <optional>

namespace Ui {

class InfiniteRadialAnimation;
class ToggleView;

class AbstractButton : public RpWidget {
	Q_OBJECT

public:
	explicit AbstractButton(QWidget *parent);

	void setClickedCallback(std::function<void()> callback) {
		_clickedCallback = std::move(callback);
	}

	[[nodiscard]] bool isOver() const {
		return (_state & StateFlag::Over) != 0;
	}
	[[nodiscard]] bool isDown() const {
		return (_state & StateFlag::Down) != 0;
	}
	[[nodiscard]] bool isDisabled() const {
		return (_state & StateFlag::Disabled) != 0;
	}
	void setDisabled(bool disabled = true);

	void setPointerCursor(bool enablePointerCursor);
	// Also react to the right mouse button.
	void setAcceptBoth(bool acceptBoth = true);
	[[nodiscard]] Qt::KeyboardModifiers clickModifiers() const {
		return _modifiers;
	}

	// Programmatic click (keyboard, tests).
	void clicked(Qt::KeyboardModifiers modifiers, Qt::MouseButton button);
	void click() {
		clicked(Qt::NoModifier, Qt::LeftButton);
	}

	void clearState();

Q_SIGNALS:
	void clickedSignal(Qt::KeyboardModifiers modifiers, Qt::MouseButton button);

protected:
	enum StateFlag : uint {
		None = 0,
		Over = 0x01,
		Down = 0x02,
		Disabled = 0x04,
	};
	using State = uint;
	enum class StateChangeSource {
		ByUser,
		ByPress,
		ByHover,
	};

	[[nodiscard]] State state() const {
		return _state;
	}
	void setOver(bool over, StateChangeSource source = StateChangeSource::ByUser);
	virtual void onStateChanged(State was, StateChangeSource source) {
		Q_UNUSED(was);
		Q_UNUSED(source);
	}

	void enterEvent(QEnterEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void updateCursor();

	State _state = StateFlag::None;
	Qt::KeyboardModifiers _modifiers;
	bool _enablePointerCursor = true;
	bool _acceptBoth = false;
	std::function<void()> _clickedCallback;

};

class RippleButton : public AbstractButton {
public:
	RippleButton(QWidget *parent, const style::RippleAnimation &st);
	~RippleButton();

	// Keeps a ripple shown without a press (an opened menu, a selected row).
	void setForceRippled(bool rippled);
	[[nodiscard]] bool forceRippled() const {
		return _forceRippled;
	}

	void finishAnimating();

protected:
	void paintRipple(
		QPainter &p,
		int x,
		int y,
		const QColor *colorOverride = nullptr);

	void onStateChanged(State was, StateChangeSource source) override;
	void resizeEvent(QResizeEvent *e) override;

	// The ripple shape, the whole rect by default.
	[[nodiscard]] virtual QImage prepareRippleMask() const;
	[[nodiscard]] virtual QPoint prepareRippleStartPosition() const;
	// Position of the mask inside the button.
	[[nodiscard]] virtual QPoint rippleOffset() const {
		return QPoint();
	}
	void resetRipple();

private:
	void ensureRipple();

	const style::RippleAnimation &_st;
	std::unique_ptr<RippleAnimation> _ripple;
	bool _forceRippled = false;

};

class RoundButton : public RippleButton {
public:
	RoundButton(
		QWidget *parent,
		const QString &text,
		const style::RoundButton &st);
	~RoundButton();

	void setText(const QString &text);
	[[nodiscard]] const QString &text() const {
		return _text;
	}
	// An optional icon on the leading side of the text.
	void setIcon(const style::icon *icon);

	// Fixed width instead of the natural one (0 = natural again).
	void setFullWidth(int newFullWidth);
	[[nodiscard]] int naturalWidth() const override;
	[[nodiscard]] int contentWidth() const;

	// Shows a spinner instead of the text and ignores clicks.
	void setLoading(bool loading);
	[[nodiscard]] bool loading() const {
		return _loading;
	}

	// Paints the over state as if it was down (for default buttons of a box).
	void setBrandOverride(bool active);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void onStateChanged(State was, StateChangeSource source) override;

	QImage prepareRippleMask() const override;

private:
	void refreshSize();

	const style::RoundButton &_st;
	QString _text;
	int _textWidth = 0;
	int _fullWidth = 0;
	const style::icon *_icon = nullptr;
	bool _loading = false;
	std::unique_ptr<InfiniteRadialAnimation> _loadingAnimation;
	Animations::Simple _overAnimation;

};

class FlatButton : public RoundButton {
public:
	FlatButton(QWidget *parent, const QString &text);
	FlatButton(
		QWidget *parent,
		const QString &text,
		const style::RoundButton &st);

};

class IconButton : public RippleButton {
public:
	IconButton(QWidget *parent, const style::IconButton &st);

	// Replaces the style's icon(s); nullptr restores them.
	void setIconOverride(
		const style::icon *iconOverride,
		const style::icon *iconOverOverride = nullptr);
	// Tints the icon with a specific color.
	void setIconColorOverride(std::optional<QColor> color);
	// An accent dot on the top trailing corner (unread marker).
	void setDotShown(bool shown);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;
	QPoint rippleOffset() const override;

private:
	const style::IconButton &_st;
	const style::icon *_iconOverride = nullptr;
	const style::icon *_iconOverOverride = nullptr;
	std::optional<QColor> _colorOverride;
	bool _dotShown = false;
	Animations::Simple _overAnimation;

};

class LinkButton : public AbstractButton {
public:
	LinkButton(
		QWidget *parent,
		const QString &text,
		const style::LinkButton &st = st::defaultLinkButton);

	void setText(const QString &text);
	void setColorOverride(std::optional<QColor> color);
	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;
	int resizeGetHeight(int newWidth) override;

private:
	const style::LinkButton &_st;
	QString _text;
	int _textWidth = 0;
	std::optional<QColor> _colorOverride;

};

// A settings row. The trailing side shows, in this order of priority, a toggle
// (setToggle), a value text (setValueText) and / or a chevron (setChevron).
class SettingsButton : public RippleButton {
	Q_OBJECT

public:
	SettingsButton(
		QWidget *parent,
		const QString &text,
		const style::SettingsButton &st = st::defaultSettingsButton);
	~SettingsButton();

	void setText(const QString &text);
	void setIcon(const style::icon *icon);
	void setValueText(const QString &value);
	void setChevron(bool shown);
	void setDestructive(bool destructive);

	// Turns the row into a switch row. Clicking toggles it.
	void setToggle(bool checked, bool animated = false);
	[[nodiscard]] bool toggled() const;
	[[nodiscard]] bool hasToggle() const {
		return _toggle != nullptr;
	}

Q_SIGNALS:
	void toggledChanged(bool checked);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	const style::SettingsButton &_st;
	QString _text;
	QString _value;
	const style::icon *_icon = nullptr;
	bool _chevron = false;
	bool _destructive = false;
	std::unique_ptr<ToggleView> _toggle;

};

// A chip: outlined when idle, filled (bgNight) when active.
class PillButton : public RippleButton {
public:
	PillButton(
		QWidget *parent,
		const QString &text,
		const style::PillButton &st = st::defaultPillButton);

	void setText(const QString &text);
	void setIcon(const style::icon *icon);
	void setActive(bool active, bool animated = true);
	[[nodiscard]] bool active() const {
		return _active;
	}
	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	[[nodiscard]] QPainterPath shape(const QRectF &rect) const;

	const style::PillButton &_st;
	QString _text;
	int _textWidth = 0;
	const style::icon *_icon = nullptr;
	bool _active = false;
	Animations::Simple _activeAnimation;

};

} // namespace Ui
