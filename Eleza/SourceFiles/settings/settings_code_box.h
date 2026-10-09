// A box that asks for a code that was just sent (by e-mail for two-step
// verification, by SMS for the data export): the boxes of digits, the error
// line and «ارسال دوباره» with its countdown.
#pragma once

#include <QtCore/QString>

#include <functional>

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

struct CodeBoxArgs {
	QString title;
	QString about; // «کد به … فرستاده شد.»
	int codeLength = 6;
	int resendAfter = 60; // seconds until «ارسال دوباره» is offered
	// Check the code: call `done` (the box closes) or `fail` with the words
	// for the user (the code is marked wrong and can be typed again).
	std::function<void(
		const QString &code,
		std::function<void()> done,
		std::function<void(const QString &error)> fail)> submit;
	// Send a new code: `done(resendAfter)` or `fail(text)`. Null: no link.
	std::function<void(
		std::function<void(int resendAfter)> done,
		std::function<void(const QString &error)> fail)> resend;
	// An optional text button on the leading side («شروع دوباره»).
	QString extraText;
	std::function<void()> extra;
};

void ShowCodeBox(Window::SessionController *controller, CodeBoxArgs args);

} // namespace Settings
