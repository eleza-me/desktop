// Support::ErrorText / ErrorPhrase: what the user reads when a request failed.
#include "api/api_error.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "support/error_text.h"

#include <QtTest/QtTest>

class TestErrorText : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}
	static Api::Error Problem(int status, const char *code, const char *message = "") {
		return Api::Error::Local(
			status,
			QString::fromLatin1(code),
			QString::fromUtf8(message));
	}

private Q_SLOTS:
	void init() {
		Lang::SetCurrent(Lang::Language::Persian);
	}

	void offline() {
		QCOMPARE(
			Support::ErrorText(Api::Error::Offline(), U("نشد")),
			tr::lng_error_connection(tr::now));
	}

	void knownCodes() {
		QCOMPARE(
			Support::ErrorText(Problem(400, "otp.invalid", "whatever")),
			tr::lng_intro_wrong_code(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(409, "handle.taken")),
			tr::lng_intro_username_taken(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(403, "two_step.password_invalid")),
			tr::lng_intro_wrong_password(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(410, "two_step.ticket_invalid")),
			tr::lng_intro_code_has_expired_request_new(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(409, "conversation.limit_reached")),
			tr::lng_conv_youve_reached_limit_concurrent_conversations(tr::now));
		QVERIFY(Support::ErrorPhrase(QStringLiteral("no.such_code")).isEmpty());
		QVERIFY(Support::ErrorPhrase(QString()).isEmpty());
	}

	void settingsCodes() {
		// Two-step verification, sensitive actions and the data export.
		QCOMPARE(
			Support::ErrorText(Problem(400, "two_step.password_weak")),
			tr::lng_settings_error_password_weak(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(400, "two_step.code_expired")),
			tr::lng_intro_code_has_expired_request_new(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(403, "account.sensitive_cooldown", "x")),
			tr::lng_settings_error_sensitive_cooldown(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(422, "export.email_required")),
			tr::lng_settings_export_email_required(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(409, "two_step.no_pending")),
			tr::lng_settings_error_no_pending(tr::now));
	}

	void statuses() {
		QCOMPARE(
			Support::ErrorText(Problem(429, "rate.limited", "slow down")),
			tr::lng_support_error_rate_limited(tr::now));
		QCOMPARE(
			Support::ErrorText(Problem(503, "server.unavailable")),
			U("خطای سرور (۵۰۳)"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(
			Support::ErrorText(Problem(500, "")),
			QStringLiteral("Server error (500)"));
	}

	void serverMessage() {
		// A localized sentence from the server is shown as is…
		QCOMPARE(
			Support::ErrorText(Problem(400, "chat.closed", "این گفتگو بسته شده"), U("نشد")),
			U("این گفتگو بسته شده"));
		// …an identifier is not.
		const auto text = Support::ErrorText(
			Problem(404, "chat.not_found", "chat.not_found"),
			U("نشد"));
		QVERIFY(text.startsWith(U("نشد (")));
		QVERIFY(text.contains(QStringLiteral("chat.not_found")));
		const auto bare = Support::ErrorText(Problem(418, ""), U("نشد"));
		QVERIFY(bare.contains(QStringLiteral("418")));
	}

	void genericFallback() {
		const auto text = Support::ErrorText(Problem(400, "x.y"));
		QVERIFY(text.startsWith(tr::lng_error_generic(tr::now)));
	}
};

QTEST_GUILESS_MAIN(TestErrorText)
#include "test_error_text.moc"
