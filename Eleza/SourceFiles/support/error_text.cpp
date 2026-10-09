#include "support/error_text.h"

#include "lang/lang_format.h"
#include "lang_auto.h"

#include <QtCore/QRegularExpression>

namespace Support {

QString ErrorPhrase(const QString &code) {
	if (code.isEmpty()) {
		return QString();
	} else if (code == u"otp.invalid" || code == u"two_step.code_invalid") {
		return tr::lng_intro_wrong_code(tr::now);
	} else if (code == u"otp.expired"
		|| code == u"otp.challenge_expired"
		|| code == u"two_step.ticket_invalid"
		|| code == u"two_step.ticket_expired") {
		return tr::lng_intro_code_has_expired_request_new(tr::now);
	} else if (code == u"two_step.password_invalid") {
		return tr::lng_intro_wrong_password(tr::now);
	} else if (code == u"two_step.no_recovery_email") {
		return tr::lng_intro_account_has_no_recovery_e(tr::now);
	} else if (code == u"phone.invalid" || code == u"otp.phone_invalid") {
		return tr::lng_intro_invalid_phone(tr::now);
	} else if (code == u"handle.taken") {
		return tr::lng_intro_username_taken(tr::now);
	} else if (code == u"handle.invalid") {
		return tr::lng_intro_username_rules(tr::now);
	} else if (code == u"conversation.limit_reached") {
		return tr::lng_conv_youve_reached_limit_concurrent_conversations(tr::now);
	} else if (code == u"conversation.not_open") {
		return tr::lng_conv_conversation_no_longer_open(tr::now);
	} else if (code == u"conversation.join_not_allowed") {
		return tr::lng_conversation_join_not_allowed(tr::now);
	} else if (code == u"birthday.change_limit") {
		return tr::lng_support_error_birthday_limit(tr::now);
	} else if (code == u"post.reactions_hidden") {
		return tr::lng_profile_list_not_shown(tr::now);
	} else if (code == u"media.not_ready") {
		return tr::lng_intro_preparing_file_took_too_long(tr::now);
	} else if (code == u"upload.part" || code == u"upload.failed") {
		return tr::lng_chat_upload_failed(tr::now);
	} else if (code == u"too_large" || code == u"upload.too_large") {
		return tr::lng_chat_file_too_large(tr::now);
	} else if (code == u"server.overloaded") {
		return tr::lng_support_error_rate_limited(tr::now);
	}
	return QString();
}

QString ErrorText(const Api::Error &error, const QString &fallback) {
	if (error.offline()) {
		return tr::lng_error_connection(tr::now);
	}
	if (const auto phrase = ErrorPhrase(error.code); !phrase.isEmpty()) {
		return phrase;
	}
	if (error.status == 429) {
		return tr::lng_support_error_rate_limited(tr::now);
	} else if (error.status >= 500) {
		return tr::lng_support_error_server(
			tr::now,
			lt_code,
			Lang::Num(error.status));
	}
	// A message that is only an identifier ("chat.not_found") is not for people.
	static const auto kIdentifier = QRegularExpression(
		QStringLiteral("^[A-Za-z_.]+$"));
	if (!error.message.isEmpty()
		&& error.message != error.code
		&& !kIdentifier.match(error.message).hasMatch()) {
		return error.message;
	}
	const auto detail = error.code.isEmpty()
		? QString::number(error.status)
		: error.code;
	// The detail is Latin text: keep the brackets around it in RTL.
	return fallback
		+ QStringLiteral(" (")
		+ QChar(0x2068) // first strong isolate
		+ detail
		+ QChar(0x2069)
		+ QLatin1Char(')');
}

QString ErrorText(const Api::Error &error) {
	return ErrorText(error, tr::lng_error_generic(tr::now));
}

} // namespace Support
