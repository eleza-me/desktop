// Text inputs. All of them paint their own frame and placeholder from a
// style::InputField; the editing itself is done by a frameless native editor inside.
//
//   InputField        single-line or auto-growing multi-line text (messages, bios)
//   MaskedInputField  single-line with value correction (override correctValue)
//   DigitsInput       digits only (accepts Persian / Arabic digits, stores Latin)
//   PasswordInput     hidden text with a show / hide eye button
//   PhoneInput        "+code | number" in one frame, always left-to-right
//   CodeInput         N separate digit boxes for a verification code
//   SearchField       search icon + field + clear button
//
//   const auto name = Ui::CreateChild<Ui::InputField>(this, st::defaultInputField, tr::lng_name(tr::now));
//   connect(name, &Ui::InputField::changed, ...);  connect(name, &Ui::InputField::submitted, ...);
//   name->resizeToWidth(w);  name->showError();  name->setFocusFast();
//
//   const auto message = Ui::CreateChild<Ui::InputField>(
//       this, st::composeField, Ui::InputField::Mode::MultiLine, placeholder);
//   message->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);  // Shift+Enter = new line
//   connect(message, &Ui::RpWidget::heightChanged, ...);                 // grows with the text
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/wrap/fade_wrap.h"
#include "styles/style_widgets.h"

#include <QtCore/QBasicTimer>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>

#include <functional>
#include <optional>

namespace Ui {

class IconButton;
class PopupMenu;

// The rounded frame shared by all inputs. `focused` / `error` are 0..1.
void PaintInputFrame(
	QPainter &p,
	const QRect &rect,
	const style::InputField &st,
	double focused,
	double error);

class InputField : public RpWidget {
	Q_OBJECT

public:
	enum class Mode {
		SingleLine,
		MultiLine,
	};
	enum class SubmitSettings {
		Enter, // Enter submits, Shift+Enter inserts a line break
		CtrlEnter, // Ctrl+Enter submits, Enter inserts a line break
		Both,
		None,
	};

	InputField(
		QWidget *parent,
		const style::InputField &st,
		const QString &placeholder = QString(),
		const QString &value = QString());
	InputField(
		QWidget *parent,
		const style::InputField &st,
		Mode mode,
		const QString &placeholder = QString(),
		const QString &value = QString());
	~InputField();

	[[nodiscard]] QString text() const;
	void setText(const QString &text);
	void clear();
	[[nodiscard]] bool empty() const;
	void insertText(const QString &text);

	void setPlaceholder(const QString &placeholder);
	void setMaxLength(int length);
	void setSubmitSettings(SubmitSettings settings);
	// Fixes the text direction (usernames, links); default: follows the content.
	void setForcedDirection(std::optional<Qt::LayoutDirection> direction);
	void setReadOnly(bool readOnly);

	void setFocusFast();
	[[nodiscard]] bool hasFocus() const;
	void selectAll();
	void setCursorToEnd();
	// Red frame until the next change.
	void showError();
	void hideError();

	[[nodiscard]] QTextEdit *rawTextEdit() const;
	[[nodiscard]] const style::InputField &st() const {
		return _st;
	}
	// Called with the context menu right before it is shown: add own items
	// (formatting) after the standard ones.
	void setContextMenuHook(std::function<void(PopupMenu *menu)> hook) {
		_contextMenuHook = std::move(hook);
	}

Q_SIGNALS:
	void changed();
	void submitted(Qt::KeyboardModifiers modifiers);
	void cancelled();
	void focusedChanged(bool focused);
	// Tab / Backtab in a single-line field.
	void tabbed(bool backward);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	class Inner;
	friend class Inner;

	void innerFocusChanged(bool focused);
	void innerContentsChanged();
	bool innerKeyPress(QKeyEvent *e); // true = handled
	void innerContextMenu(const QPoint &globalPosition);
	void updateInnerGeometry();
	void updateDirection();
	void refreshPalette();
	[[nodiscard]] int contentHeight() const;
	[[nodiscard]] int heightForContent() const;

	const style::InputField &_st;
	Mode _mode = Mode::SingleLine;
	Inner *_inner = nullptr;
	QString _placeholder;
	int _maxLength = 0;
	SubmitSettings _submitSettings = SubmitSettings::Enter;
	std::optional<Qt::LayoutDirection> _forcedDirection;
	int _directionState = -1;
	bool _focused = false;
	bool _error = false;
	bool _hasText = false;
	bool _correcting = false;
	QString _lastText;
	QPointer<PopupMenu> _contextMenu;
	std::function<void(PopupMenu *menu)> _contextMenuHook;

	Animations::Simple _focusAnimation;
	Animations::Simple _errorAnimation;
	Animations::Simple _placeholderAnimation;

};

class MaskedInputField : public RpWidget {
	Q_OBJECT

public:
	MaskedInputField(
		QWidget *parent,
		const style::InputField &st,
		const QString &placeholder = QString(),
		const QString &value = QString());
	~MaskedInputField();

	[[nodiscard]] QString text() const;
	void setText(const QString &text);
	void clear();
	void setPlaceholder(const QString &placeholder);
	void setMaxLength(int length);
	void setEchoMode(QLineEdit::EchoMode mode);
	void setForcedDirection(std::optional<Qt::LayoutDirection> direction);
	// Space kept free at the trailing side (an embedded button).
	void setTrailingPadding(int padding);
	void setReadOnly(bool readOnly);

	void setFocusFast();
	[[nodiscard]] bool hasFocus() const;
	void selectAll();
	void setCursorPosition(int position);
	[[nodiscard]] int cursorPosition() const;
	void showError();
	void hideError();

	[[nodiscard]] QLineEdit *rawLineEdit() const;
	[[nodiscard]] const style::InputField &st() const {
		return _st;
	}

Q_SIGNALS:
	void changed();
	void submitted(Qt::KeyboardModifiers modifiers);
	void cancelled();
	void focusedChanged(bool focused);
	// Backspace pressed with the cursor at the very start.
	void backspaceAtStart();

protected:
	// Called after every edit. Change `now` / `nowCursor` to correct the value.
	virtual void correctValue(
			const QString &was,
			int wasCursor,
			QString &now,
			int &nowCursor) {
		Q_UNUSED(was);
		Q_UNUSED(wasCursor);
		Q_UNUSED(now);
		Q_UNUSED(nowCursor);
	}

	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	class Inner;
	friend class Inner;

	void innerFocusChanged(bool focused);
	void innerTextChanged();
	bool innerKeyPress(QKeyEvent *e);
	void innerContextMenu(const QPoint &globalPosition);
	void updateInnerGeometry();
	void updateDirection();
	void refreshPalette();

	const style::InputField &_st;
	Inner *_inner = nullptr;
	QString _placeholder;
	std::optional<Qt::LayoutDirection> _forcedDirection;
	int _trailingPadding = 0;
	bool _focused = false;
	bool _error = false;
	bool _correcting = false;
	QString _lastText;
	int _lastCursor = 0;
	QPointer<PopupMenu> _contextMenu;

	Animations::Simple _focusAnimation;
	Animations::Simple _errorAnimation;
	Animations::Simple _placeholderAnimation;

};

// Digits only. Persian (۰-۹) and Arabic (٠-٩) digits are stored as Latin ones.
class DigitsInput : public MaskedInputField {
public:
	DigitsInput(
		QWidget *parent,
		const style::InputField &st,
		const QString &placeholder = QString(),
		const QString &value = QString(),
		int maxDigits = 0);

protected:
	void correctValue(
		const QString &was,
		int wasCursor,
		QString &now,
		int &nowCursor) override;

private:
	int _maxDigits = 0;

};

class PasswordInput : public MaskedInputField {
public:
	PasswordInput(
		QWidget *parent,
		const style::InputField &st = st::defaultInputField,
		const QString &placeholder = QString());

	void setRevealed(bool revealed);
	[[nodiscard]] bool revealed() const {
		return _revealed;
	}

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	IconButton *_toggle = nullptr;
	bool _revealed = false;

};

// "+98 | 912 345 6789": the country code and the number, always LTR.
class PhoneInput : public RpWidget {
	Q_OBJECT

public:
	PhoneInput(
		QWidget *parent,
		const style::InputField &st = st::defaultInputField,
		const QString &countryCode = QStringLiteral("98"));

	void setCountryCode(const QString &code); // digits without "+"
	[[nodiscard]] QString countryCode() const;
	void setNumber(const QString &number);
	[[nodiscard]] QString number() const; // digits only
	// "+989123456789", empty if there is no number yet.
	[[nodiscard]] QString fullNumber() const;
	void setNumberPlaceholder(const QString &placeholder);

	void setFocusFast();
	void showError();

Q_SIGNALS:
	void changed();
	void countryCodeChanged(QString code);
	// A number with its international prefix ("+98912…", "0098912…") was pasted
	// into the number part: all its digits. The owner knows the calling codes:
	// it calls setCountryCode() and setNumber() with the two halves.
	void internationalNumberEntered(QString digits);
	void submitted();
	void cancelled();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	class CodePart;
	class NumberPart;

	void setFocused(bool focused);

	const style::InputField &_st;
	CodePart *_code = nullptr;
	NumberPart *_number = nullptr;
	bool _focused = false;
	bool _error = false;
	Animations::Simple _focusAnimation;
	Animations::Simple _errorAnimation;

};

// A verification code as separate boxes. Typing advances, Backspace goes back,
// pasting fills everything; Persian / Arabic digits are accepted.
class CodeInput : public RpWidget {
	Q_OBJECT

public:
	CodeInput(
		QWidget *parent,
		int digits,
		const style::CodeInput &st = st::defaultCodeInput);

	void setDigitsCount(int digits);
	[[nodiscard]] QString code() const; // Latin digits
	void setCode(const QString &code);
	void clear();
	void showError(); // red boxes and a shake, cleared on the next key
	void setDisabled(bool disabled);
	void setFocusFast() {
		setFocus();
	}
	[[nodiscard]] int naturalWidth() const override;

Q_SIGNALS:
	void codeChanged(QString code);
	// All the boxes are filled.
	void finished(QString code);
	void cancelled();

protected:
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void timerEvent(QTimerEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void inputMethodEvent(QInputMethodEvent *e) override;
	QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] QRect boxRect(int index) const;
	void insertDigits(const QString &text);
	void paste();
	void changedNotify();

	const style::CodeInput &_st;
	int _count = 0;
	QString _digits; // Latin digits, up to _count
	int _cursor = 0;
	bool _error = false;
	bool _disabled = false;
	bool _caretShown = false;
	QBasicTimer _caretTimer;
	Animations::Simple _shakeAnimation;
	Animations::Simple _errorAnimation;

};

class SearchField : public RpWidget {
	Q_OBJECT

public:
	SearchField(
		QWidget *parent,
		const QString &placeholder = QString(),
		const style::SearchField &st = st::defaultSearchField);

	[[nodiscard]] QString text() const;
	void setText(const QString &text);
	void clear();
	void setFocusFast();
	[[nodiscard]] InputField *field() const {
		return _field;
	}

Q_SIGNALS:
	void changed(QString text);
	void submitted();
	void cancelled();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const style::SearchField &_st;
	InputField *_field = nullptr;
	FadeWrap<IconButton> *_clear = nullptr;

};

// "۱۲۳" / "١٢٣" -> "123". Other characters are kept.
[[nodiscard]] QString LatinizeDigits(const QString &text);

} // namespace Ui
