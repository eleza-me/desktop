// Sign-in with an SMS code bound to this device's key, the two-step password
// step, password recovery, and the profile step of onboarding. A successful
// sign-in stores the tokens in Main::Account, which brings Main::Session up.
//
// The platform string sent to the API is "desktop".
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <QtCore/QObject>

namespace Main {
class Account;
} // namespace Main

namespace Api {

struct OtpChallenge {
	QString challengeId;
	int codeLength = 6;
	int resendAfter = 60; // seconds
	int expiresIn = 300;  // seconds
};

// The account has a two-step password: sign in with it (or recover by email).
struct TwoStepForm {
	QString ticket;
	QString hint;
	QString recoveryEmail; // masked ("h***@gmail.com"); empty when none is set
	qint64 expiresAt = 0;  // ms
};

struct VerifyOutcome {
	enum class Kind {
		SignedIn,
		TwoStep,
	};
	Kind kind = Kind::SignedIn;
	bool needsProfile = false; // SignedIn: the account has no handle yet
	QString userId;            // SignedIn
	TwoStepForm form;          // TwoStep
};

struct RecoveryChallenge {
	int codeLength = 6;
	int resendAfter = 60;
	// Lifetime of the mailed code in seconds. The login ticket lives as long
	// (the server extends it): restart its countdown from this.
	int expiresIn = 600;
	QString email;
};

class Auth final : public QObject {
public:
	explicit Auth(Main::Account *account);

	static inline const QString kPlatform = QStringLiteral("desktop");

	// `POST /v1/auth/otp/request`. `phone` in E.164 ("+98912...").
	void requestCode(
		const QString &phone,
		QObject *guard,
		Fn<void(const OtpChallenge &)> done,
		Fn<void(const Error &)> fail);

	// `POST /v1/auth/otp/verify`. With `freshKey` a new device key is
	// generated first (a new sign-in after the old session ended).
	void verifyCode(
		const QString &challengeId,
		const QString &code,
		bool freshKey,
		QObject *guard,
		Fn<void(const VerifyOutcome &)> done,
		Fn<void(const Error &)> fail);

	// `POST /v1/auth/two-step`.
	void signInWithPassword(
		const TwoStepForm &form,
		const QString &password,
		QObject *guard,
		Fn<void(const VerifyOutcome &)> done,
		Fn<void(const Error &)> fail);

	// `POST /v1/auth/two-step/recover`: mails a code to the recovery address.
	void requestRecovery(
		const TwoStepForm &form,
		QObject *guard,
		Fn<void(const RecoveryChallenge &)> done,
		Fn<void(const Error &)> fail);

	// `POST /v1/auth/two-step/recover/confirm`.
	void confirmRecovery(
		const TwoStepForm &form,
		const QString &code,
		QObject *guard,
		Fn<void(const VerifyOutcome &)> done,
		Fn<void(const Error &)> fail);

	// `GET /v1/handles/{handle}/availability`.
	void isHandleAvailable(
		const QString &handle,
		QObject *guard,
		Fn<void(bool)> done,
		Fn<void(const Error &)> fail);

	// `PATCH /v1/me/profile` then `PUT /v1/me/handle`.
	void completeProfile(
		const QString &name,
		const QString &handle,
		const QString &avatarId,
		QObject *guard,
		Fn<void(const Data::Profile &)> done,
		Fn<void(const Error &)> fail);

	// `GET /v1/me`.
	void me(
		QObject *guard,
		Fn<void(const Data::Profile &)> done,
		Fn<void(const Error &)> fail);

	// `POST /v1/auth/logout` (best effort), then the local session is dropped.
	void logout(QObject *guard = nullptr, Fn<void()> done = nullptr);

	// ^[a-z0-9_.]{3,24}$
	[[nodiscard]] static bool IsValidHandle(const QString &handle);
	// Persian / Arabic-Indic digits -> ASCII digits (codes typed on a Persian keyboard).
	[[nodiscard]] static QString Latinize(const QString &text);

private:
	void finishSignIn(
		const QJsonObject &tokens,
		base::Guard guard,
		Fn<void(const VerifyOutcome &)> done);

	Main::Account *_account = nullptr;

};

} // namespace Api
