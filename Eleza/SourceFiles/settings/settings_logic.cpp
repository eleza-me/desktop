#include "settings/settings_logic.h"

#include "api/api_client_config.h"
#include "lang/lang_format.h"
#include "lang_auto.h"

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <array>

namespace Settings {
namespace {

struct SectionInfo {
	Section section;
	const char *id;
};

constexpr auto kSections = std::array<SectionInfo, 10>{ {
	{ Section::Account, "account" },
	{ Section::Privacy, "privacy" },
	{ Section::Notifications, "notifications" },
	{ Section::Messages, "messages" },
	{ Section::Filters, "filters" },
	{ Section::Region, "region" },
	{ Section::Appearance, "appearance" },
	{ Section::Devices, "devices" },
	{ Section::Storage, "storage" },
	{ Section::About, "about" },
} };

constexpr auto kMaxPasswordLength = 256;
constexpr auto kMaxHintLength = 64;
constexpr auto kMaxEmailLength = 254;
constexpr auto kOnlineWindowMs = qint64(3) * 60 * 1000;

[[nodiscard]] QString ProvinceName(int index) {
	switch (index) {
	case 1: return tr::lng_settings_province_01(tr::now);
	case 2: return tr::lng_settings_province_02(tr::now);
	case 3: return tr::lng_settings_province_03(tr::now);
	case 4: return tr::lng_settings_province_04(tr::now);
	case 5: return tr::lng_settings_province_05(tr::now);
	case 6: return tr::lng_settings_province_06(tr::now);
	case 7: return tr::lng_settings_province_07(tr::now);
	case 8: return tr::lng_settings_province_08(tr::now);
	case 9: return tr::lng_settings_province_09(tr::now);
	case 10: return tr::lng_settings_province_10(tr::now);
	case 11: return tr::lng_settings_province_11(tr::now);
	case 12: return tr::lng_settings_province_12(tr::now);
	case 13: return tr::lng_settings_province_13(tr::now);
	case 14: return tr::lng_settings_province_14(tr::now);
	case 15: return tr::lng_settings_province_15(tr::now);
	case 16: return tr::lng_settings_province_16(tr::now);
	case 17: return tr::lng_settings_province_17(tr::now);
	case 18: return tr::lng_settings_province_18(tr::now);
	case 19: return tr::lng_settings_province_19(tr::now);
	case 20: return tr::lng_settings_province_20(tr::now);
	case 21: return tr::lng_settings_province_21(tr::now);
	case 22: return tr::lng_settings_province_22(tr::now);
	case 23: return tr::lng_settings_province_23(tr::now);
	case 24: return tr::lng_settings_province_24(tr::now);
	case 25: return tr::lng_settings_province_25(tr::now);
	case 26: return tr::lng_settings_province_26(tr::now);
	case 27: return tr::lng_settings_province_27(tr::now);
	case 28: return tr::lng_settings_province_28(tr::now);
	case 29: return tr::lng_settings_province_29(tr::now);
	case 30: return tr::lng_settings_province_30(tr::now);
	case 31: return tr::lng_settings_province_31(tr::now);
	}
	return QString();
}

} // namespace

QString SectionId(Section section) {
	for (const auto &info : kSections) {
		if (info.section == section) {
			return QString::fromLatin1(info.id);
		}
	}
	return QString();
}

std::optional<Section> SectionFromId(const QString &id) {
	for (const auto &info : kSections) {
		if (id == QLatin1String(info.id)) {
			return info.section;
		}
	}
	return std::nullopt;
}

QString SectionTitle(Section section) {
	switch (section) {
	case Section::Account: return tr::lng_settings_account(tr::now);
	case Section::Privacy: return tr::lng_settings_privacy(tr::now);
	case Section::Notifications:
		return tr::lng_settings_notifications(tr::now);
	case Section::Messages:
		return tr::lng_settings_messages_conversations(tr::now);
	case Section::Filters: return tr::lng_settings_muted_blocked(tr::now);
	case Section::Region: return tr::lng_settings_eli_region(tr::now);
	case Section::Appearance:
		return tr::lng_settings_appearance_language(tr::now);
	case Section::Devices: return tr::lng_settings_devices(tr::now);
	case Section::Storage: return tr::lng_settings_storage(tr::now);
	case Section::About: return tr::lng_settings_about_eleza(tr::now);
	}
	return QString();
}

QVector<Province> Provinces() {
	auto result = QVector<Province>();
	result.reserve(31);
	for (auto i = 1; i <= 31; ++i) {
		result.push_back({
			QStringLiteral("IR-%1").arg(i, 2, 10, QLatin1Char('0')),
			ProvinceName(i),
		});
	}
	return result;
}

QString ProvinceTitle(const QString &code) {
	if (code.isEmpty()) {
		return QString();
	}
	static const auto kPattern = QRegularExpression(
		QStringLiteral("^IR-(\\d{2})$"));
	const auto match = kPattern.match(code);
	if (match.hasMatch()) {
		const auto name = ProvinceName(match.captured(1).toInt());
		if (!name.isEmpty()) {
			return name;
		}
	}
	return code;
}

PasswordProblem CheckNewPassword(
		const QString &password,
		const QString &repeat,
		int minLength) {
	// The server counts characters (code points), not UTF-16 units.
	const auto length = int(password.toUcs4().size());
	if (password.isEmpty()) {
		return PasswordProblem::Empty;
	} else if (length < minLength) {
		return PasswordProblem::TooShort;
	} else if (length > kMaxPasswordLength) {
		return PasswordProblem::TooLong;
	} else if (password != repeat) {
		return PasswordProblem::Mismatch;
	}
	return PasswordProblem::None;
}

bool HintAllowed(const QString &hint, const QString &password) {
	if (hint.isEmpty()) {
		return true;
	} else if (int(hint.toUcs4().size()) > kMaxHintLength
		|| hint.contains(QLatin1Char('\n'))
		|| hint.contains(QLatin1Char('\r'))) {
		return false;
	}
	return password.isEmpty()
		|| !hint.contains(password, Qt::CaseInsensitive);
}

bool EmailLooksValid(const QString &email) {
	if (email.isEmpty() || email.size() > kMaxEmailLength) {
		return false;
	}
	for (const auto ch : email) {
		if (ch.unicode() <= 0x20 || ch.unicode() >= 0x7F) {
			return false;
		}
	}
	static const auto kPattern = QRegularExpression(QStringLiteral(
		"^[^@\\s<>()\\[\\],;:\"]+@[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?"
		"(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)*\\.[A-Za-z]{2,}$"));
	return kPattern.match(email).hasMatch();
}

QString NormalizePassword(const QString &password) {
	return password.normalized(QString::NormalizationForm_C);
}

TwoStepField FieldForError(const QString &code) {
	if (code == u"two_step.password_invalid") {
		return TwoStepField::Current;
	} else if (code == u"two_step.password_weak") {
		return TwoStepField::Password;
	} else if (code == u"two_step.hint_invalid") {
		return TwoStepField::Hint;
	} else if (code == u"two_step.email_invalid") {
		return TwoStepField::Email;
	} else if (code == u"two_step.code_invalid"
		|| code == u"two_step.code_expired"
		|| code == u"otp.invalid"
		|| code == u"otp.expired"
		|| code == u"otp.already_used") {
		return TwoStepField::Code;
	}
	return TwoStepField::None;
}

QString DurationTitle(const QString &duration) {
	const auto seconds = Api::ClientConfig::ParseDuration(duration);
	if (seconds <= 0) {
		return duration;
	} else if (seconds % 86400 == 0) {
		return tr::lng_settings_duration_days(
			tr::now,
			lt_count,
			int(seconds / 86400));
	} else if (seconds % 3600 == 0) {
		return tr::lng_settings_duration_hours(
			tr::now,
			lt_count,
			int(seconds / 3600));
	}
	return tr::lng_settings_duration_minutes(
		tr::now,
		lt_count,
		int(std::max(seconds / 60, qint64(1))));
}

QString ExportStateText(const QString &state) {
	if (state == u"ready") {
		return tr::lng_settings_export_ready(tr::now);
	} else if (state == u"failed") {
		return tr::lng_settings_export_failed(tr::now);
	} else if (state == u"expired") {
		return tr::lng_settings_export_expired(tr::now);
	}
	return tr::lng_settings_export_preparing(tr::now);
}

QString PlatformTitle(const QString &platform) {
	if (platform == u"ios") {
		return u"iOS"_s;
	} else if (platform == u"android") {
		return u"Android"_s;
	} else if (platform == u"web") {
		return tr::lng_settings_platform_web(tr::now);
	} else if (platform == u"desktop") {
		return tr::lng_settings_platform_desktop(tr::now);
	}
	return platform;
}

QString SessionSubtitle(const Data::DeviceSession &session, qint64 nowMs) {
	auto parts = QStringList();
	const auto platform = PlatformTitle(session.platform);
	if (!platform.isEmpty()) {
		parts.push_back(session.appVersion.isEmpty()
			? platform
			: (platform + QLatin1Char(' ') + Lang::Num(session.appVersion)));
	}
	if (!session.city.isEmpty()) {
		parts.push_back(session.city);
	}
	if (session.current
		|| (session.lastActiveAt > 0
			&& nowMs - session.lastActiveAt < kOnlineWindowMs)) {
		parts.push_back(tr::lng_status_online(tr::now));
	} else if (session.lastActiveAt > 0) {
		parts.push_back(Lang::ListTime(
			QDateTime::fromMSecsSinceEpoch(session.lastActiveAt),
			QDateTime::fromMSecsSinceEpoch(nowMs)));
	}
	// Latin device data inside a Persian line: every part keeps its own
	// direction (first-strong isolates), the parts follow the interface.
	for (auto &part : parts) {
		part = QChar(0x2068) + part + QChar(0x2069);
	}
	return parts.join(QStringLiteral(" · "));
}

std::vector<qint64> CacheLimitSteps() {
	const auto mb = qint64(1) << 20;
	return {
		128 * mb,
		256 * mb,
		512 * mb,
		1024 * mb,
		2048 * mb,
		5120 * mb,
	};
}

int CacheLimitIndex(qint64 bytes) {
	const auto steps = CacheLimitSteps();
	auto best = 0;
	for (auto i = 1, count = int(steps.size()); i != count; ++i) {
		if (std::abs(steps[i] - bytes) < std::abs(steps[best] - bytes)) {
			best = i;
		}
	}
	return best;
}

} // namespace Settings
