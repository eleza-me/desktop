// Calling codes for the sign-in country picker: code, ISO, Persian name, English name.
// A port of the web client's core/countries.ts.
#pragma once

#include <QtCore/QString>

#include <vector>

namespace Lang {

struct Country {
	QString code; // calling code digits, "98"
	QString iso; // "IR"
	QString nameFa;
	QString nameEn;

	// The name in the current language.
	[[nodiscard]] QString name() const;
};

// In the picker's order (Iran and its neighbours first).
[[nodiscard]] const std::vector<Country> &Countries();

[[nodiscard]] const Country *CountryByIso(const QString &iso);
// Exact calling code; Iran / the United States win shared codes.
[[nodiscard]] const Country *CountryByCode(const QString &code);
// The country whose code is the longest prefix of the digits ("989121234567").
[[nodiscard]] const Country *CountryByPhone(const QString &digits);
// Countries whose name (either language), ISO or code contains the query.
[[nodiscard]] std::vector<const Country*> SearchCountries(const QString &query);

// Two regional-indicator symbols. Windows has no flag glyphs: check
// FlagEmojiSupported() and show the ISO code instead.
[[nodiscard]] QString FlagEmoji(const QString &iso);
[[nodiscard]] bool FlagEmojiSupported();
// The flag, or the ISO code where flags do not render.
[[nodiscard]] QString FlagOrIso(const QString &iso);

} // namespace Lang
