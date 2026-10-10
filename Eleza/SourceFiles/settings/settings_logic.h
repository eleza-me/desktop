// The widget-free parts of the settings: the sections and their route ids, the
// province list of «منطقهٔ الی», the client-side checks of the two-step
// verification forms and the wording of durations, exports and device sessions.
// Covered by tests/app/test_settings_logic.cpp.
#pragma once

#include "data/data_social_types.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>
#include <vector>

namespace Settings {

enum class Section {
	Account,
	Privacy,
	Notifications,
	Messages,
	Filters,
	Region,
	Appearance,
	Devices,
	Storage,
	Update,
	About,
};

// "account", "privacy", … — the <section> of the settings/<section> route.
[[nodiscard]] QString SectionId(Section section);
[[nodiscard]] std::optional<Section> SectionFromId(const QString &id);
[[nodiscard]] QString SectionTitle(Section section);

// ---- provinces (ISO 3166-2:IR)
struct Province {
	QString code;
	QString title;
};
[[nodiscard]] QVector<Province> Provinces();
// The title for a code; the code itself when it is unknown, "" for "".
[[nodiscard]] QString ProvinceTitle(const QString &code);

// ---- two-step verification forms
enum class PasswordProblem {
	None,
	Empty,
	TooShort,
	TooLong,
	Mismatch,
};
[[nodiscard]] PasswordProblem CheckNewPassword(
	const QString &password,
	const QString &repeat,
	int minLength);
// At most 64 characters, one line, and not the password itself.
[[nodiscard]] bool HintAllowed(const QString &hint, const QString &password);
// A plain ASCII local@domain.tld of at most 254 characters.
[[nodiscard]] bool EmailLooksValid(const QString &email);
// The server hashes the bytes as they come: always send NFC.
[[nodiscard]] QString NormalizePassword(const QString &password);

enum class TwoStepField {
	None,     // not about one field: show it under the form
	Current,  // the current password is wrong
	Password,
	Hint,
	Email,
	Code,
};
// Which field of a form a problem code is about.
[[nodiscard]] TwoStepField FieldForError(const QString &code);

// ---- wording
// "1h" -> «۱ ساعت», "24h" -> «۱ روز», "30m" -> «۳۰ دقیقه».
[[nodiscard]] QString DurationTitle(const QString &duration);
// pending / running -> «در حال آماده‌سازی», ready, failed, expired.
[[nodiscard]] QString ExportStateText(const QString &state);
// «iOS · 1.4.0 · Tehran · آنلاین» / «… · دیروز».
[[nodiscard]] QString SessionSubtitle(
	const Data::DeviceSession &session,
	qint64 nowMs);
[[nodiscard]] QString PlatformTitle(const QString &platform);

// ---- the media cache limit slider
[[nodiscard]] std::vector<qint64> CacheLimitSteps(); // bytes, ascending
[[nodiscard]] int CacheLimitIndex(qint64 bytes);     // the nearest step

} // namespace Settings
