#include "api/api_auth.h"

#include "api/api_client.h"
#include "base/platform_file.h"
#include "base/unixtime.h"
#include "main/main_account.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace Api {
namespace {

[[nodiscard]] int IntOr(const QJsonObject &object, const QString &key, int fallback) {
	const auto value = object.value(key);
	return value.isDouble() ? base::SafeInt(value.toDouble()) : fallback;
}

[[nodiscard]] QString Encoded(const QString &segment) {
	return QString::fromLatin1(QUrl::toPercentEncoding(segment));
}

} // namespace

Auth::Auth(Main::Account *account) : _account(account) {
}

bool Auth::IsValidHandle(const QString &handle) {
	static const auto kHandle = QRegularExpression(u"^[a-z0-9_.]{3,24}$"_s);
	return kHandle.match(handle).hasMatch();
}

QString Auth::Latinize(const QString &text) {
	auto result = text;
	for (auto &ch : result) {
		const auto code = ch.unicode();
		if (code >= 0x06F0 && code <= 0x06F9) {
			ch = QChar(ushort('0' + (code - 0x06F0)));
		} else if (code >= 0x0660 && code <= 0x0669) {
			ch = QChar(ushort('0' + (code - 0x0660)));
		}
	}
	return result;
}

void Auth::requestCode(
		const QString &phone,
		QObject *guard,
		Fn<void(const OtpChallenge &)> done,
		Fn<void(const Error &)> fail) {
	_account->api().request(Post, u"/v1/auth/otp/request"_s)
		.unauthenticated()
		.body(QJsonObject{
			{ u"phone"_s, Latinize(phone) },
			{ u"device_id"_s, _account->deviceId() },
			{ u"platform"_s, kPlatform },
		})
		.guard(guard)
		.done([=](const QJsonObject &result) {
			auto challenge = OtpChallenge();
			challenge.challengeId = result.value(u"challenge_id"_s).toString();
			challenge.codeLength = IntOr(result, u"code_length"_s, 6);
			challenge.resendAfter = qMax(1, IntOr(result, u"resend_after"_s, 60));
			challenge.expiresIn = IntOr(result, u"expires_in"_s, 300);
			done(challenge);
		})
		.fail(std::move(fail))
		.send();
}

void Auth::verifyCode(
		const QString &challengeId,
		const QString &code,
		bool freshKey,
		QObject *guard,
		Fn<void(const VerifyOutcome &)> done,
		Fn<void(const Error &)> fail) {
	if (freshKey) {
		_account->resetDeviceKey();
	}
	const auto &key = _account->deviceKey();
	const auto alive = base::Guard(guard);
	_account->api().request(Post, u"/v1/auth/otp/verify"_s)
		.unauthenticated()
		.body(QJsonObject{
			{ u"challenge_id"_s, challengeId },
			{ u"code"_s, Latinize(code) },
			{ u"device_id"_s, _account->deviceId() },
			{ u"device_name"_s, base::platform::DeviceName() },
			{ u"platform"_s, kPlatform },
			{ u"app_version"_s, _account->config().version },
			{ u"device_key"_s, QJsonObject{
				{ u"type"_s, Crypto::DeviceKey::Type() },
				{ u"key"_s, key.publicKeyBase64() },
			} },
		})
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			if (result.value(u"two_step_required"_s).toBool()) {
				if (!alive.alive()) {
					return;
				}
				auto outcome = VerifyOutcome();
				outcome.kind = VerifyOutcome::Kind::TwoStep;
				outcome.form.ticket = result.value(u"ticket"_s).toString();
				outcome.form.hint = result.value(u"hint"_s).toString();
				outcome.form.recoveryEmail = result.value(u"recovery_email"_s).toString();
				outcome.form.expiresAt = base::unixtime::nowMs()
					+ qint64(qMax(1, IntOr(result, u"expires_in"_s, 300))) * 1000;
				done(outcome);
				return;
			}
			// The tokens are stored even if the screen that asked is gone.
			finishSignIn(result, alive, done);
		})
		.fail(base::guard(alive, std::move(fail)))
		.send();
}

void Auth::signInWithPassword(
		const TwoStepForm &form,
		const QString &password,
		QObject *guard,
		Fn<void(const VerifyOutcome &)> done,
		Fn<void(const Error &)> fail) {
	const auto alive = base::Guard(guard);
	_account->api().request(Post, u"/v1/auth/two-step"_s)
		.unauthenticated()
		.body(QJsonObject{
			{ u"ticket"_s, form.ticket },
			{ u"password"_s, password.normalized(QString::NormalizationForm_C) },
			{ u"device_id"_s, _account->deviceId() },
		})
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			finishSignIn(result, alive, done);
		})
		.fail(base::guard(alive, std::move(fail)))
		.send();
}

void Auth::requestRecovery(
		const TwoStepForm &form,
		QObject *guard,
		Fn<void(const RecoveryChallenge &)> done,
		Fn<void(const Error &)> fail) {
	_account->api().request(Post, u"/v1/auth/two-step/recover"_s)
		.unauthenticated()
		.body(QJsonObject{
			{ u"ticket"_s, form.ticket },
			{ u"device_id"_s, _account->deviceId() },
		})
		.guard(guard)
		.done([=](const QJsonObject &result) {
			auto challenge = RecoveryChallenge();
			challenge.codeLength = IntOr(result, u"code_length"_s, 6);
			challenge.resendAfter = IntOr(result, u"resend_after"_s, 60);
			challenge.expiresIn = qMax(1, IntOr(result, u"expires_in"_s, 600));
			challenge.email = result.value(u"email"_s).isString()
				? result.value(u"email"_s).toString()
				: form.recoveryEmail;
			done(challenge);
		})
		.fail(std::move(fail))
		.send();
}

void Auth::confirmRecovery(
		const TwoStepForm &form,
		const QString &code,
		QObject *guard,
		Fn<void(const VerifyOutcome &)> done,
		Fn<void(const Error &)> fail) {
	const auto alive = base::Guard(guard);
	_account->api().request(Post, u"/v1/auth/two-step/recover/confirm"_s)
		.unauthenticated()
		.body(QJsonObject{
			{ u"ticket"_s, form.ticket },
			{ u"code"_s, Latinize(code) },
			{ u"device_id"_s, _account->deviceId() },
		})
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			finishSignIn(result, alive, done);
		})
		.fail(base::guard(alive, std::move(fail)))
		.send();
}

void Auth::finishSignIn(
		const QJsonObject &tokens,
		base::Guard guard,
		Fn<void(const VerifyOutcome &)> done) {
	_account->storeTokens(tokens);
	auto outcome = VerifyOutcome();
	outcome.kind = VerifyOutcome::Kind::SignedIn;
	outcome.needsProfile = tokens.value(u"is_new_user"_s).toBool();
	outcome.userId = _account->userId();
	if (outcome.userId.isEmpty()) {
		outcome.userId = tokens.value(u"user_id"_s).toString();
	}
	if (outcome.needsProfile) {
		if (guard.alive()) {
			done(outcome);
		}
		return;
	}
	// An existing account may still lack its handle (onboarding was interrupted).
	_account->api().request(Get, u"/v1/me"_s)
		.guard(this)
		.done([=](const QJsonObject &me) mutable {
			outcome.needsProfile = me.value(u"handle"_s).toString().isEmpty();
			if (const auto id = me.value(u"id"_s).toString(); !id.isEmpty()) {
				outcome.userId = id;
			}
			if (guard.alive()) {
				done(outcome);
			}
		})
		.fail([=](const Error &) {
			// Offline right after sign-in: the shell retries.
			if (guard.alive()) {
				done(outcome);
			}
		})
		.send();
}

void Auth::isHandleAvailable(
		const QString &handle,
		QObject *guard,
		Fn<void(bool)> done,
		Fn<void(const Error &)> fail) {
	_account->api().request(Get, u"/v1/handles/%1/availability"_s.arg(Encoded(handle)))
		.guard(guard)
		.done([=](const QJsonObject &result) {
			done(result.value(u"available"_s).toBool());
		})
		.fail(std::move(fail))
		.send();
}

void Auth::completeProfile(
		const QString &name,
		const QString &handle,
		const QString &avatarId,
		QObject *guard,
		Fn<void(const Data::Profile &)> done,
		Fn<void(const Error &)> fail) {
	auto body = QJsonObject{ { u"display_name"_s, name } };
	if (!avatarId.isEmpty()) {
		body.insert(u"avatar_id"_s, avatarId);
	}
	const auto alive = base::Guard(guard);
	_account->api().request(Patch, u"/v1/me/profile"_s)
		.body(body)
		.guard(this)
		.done([=, this] {
			_account->api().request(Put, u"/v1/me/handle"_s)
				.body(QJsonObject{ { u"handle"_s, handle } })
				.guard(this)
				.done([=](const QJsonObject &result) {
					if (alive.alive()) {
						done(Data::Profile::FromJson(result));
					}
				})
				.fail(base::guard(alive, fail))
				.send();
		})
		.fail(base::guard(alive, fail))
		.send();
}

void Auth::me(
		QObject *guard,
		Fn<void(const Data::Profile &)> done,
		Fn<void(const Error &)> fail) {
	_account->api().request(Get, u"/v1/me"_s)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			done(Data::Profile::FromJson(result));
		})
		.fail(std::move(fail))
		.send();
}

void Auth::logout(QObject *guard, Fn<void()> done) {
	_account->logout(guard, std::move(done));
}

} // namespace Api
