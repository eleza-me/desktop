// The language runtime: Persian (fa, RTL, the source language) and English (en).
//
//   tr::lng_settings(tr::now)                          -> QString in the current language
//   tr::lng_status_last_seen(tr::now, lt_time, text)   -> with a {time} tag replaced
//   tr::lng_eli_new_posts(tr::now, lt_count, 5)        -> plural form + localized digits
//
// (the tr::lng_* functions are generated from Resources/langs/lang.strings into
// lang_auto.h — include "lang_auto.h" to use them.)
//
//   Lang::SetCurrent(Lang::Language::English);         // switches at runtime
//   Lang::OnUpdated(this, [=] { refreshTexts(); });    // re-read strings on change
//
// The interface direction follows the language (Lang::RightToLeft()); the application
// forwards it to style::SetRightToLeft and rebuilds the window content on a switch.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <functional>
#include <initializer_list>

namespace Lang {

enum class Language {
	Persian,
	English,
};

// Defined in the generated lang_auto.h.
enum class Key : ushort;

class Notifier final : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;

Q_SIGNALS:
	void changed();

};

[[nodiscard]] Language Current();
void SetCurrent(Language language);
[[nodiscard]] bool RightToLeft();
[[nodiscard]] inline bool IsPersian() {
	return Current() == Language::Persian;
}

// "fa" / "en".
[[nodiscard]] QString Id(Language language);
[[nodiscard]] inline QString CurrentId() {
	return Id(Current());
}
[[nodiscard]] Language FromId(const QString &id, Language fallback);
// The language's own name: «فارسی», "English".
[[nodiscard]] QString NativeName(Language language);
// Value for the Accept-Language header.
[[nodiscard]] QString HttpAcceptLanguage();

[[nodiscard]] Notifier *Updated();
void OnUpdated(QObject *context, std::function<void()> callback);

[[nodiscard]] QString GetValue(Key key);
// Chooses #one / #other by the count and substitutes {count}.
[[nodiscard]] QString GetPlural(Key key, double count);
// Looks a string up by its key name ("lng_settings"); empty if there is none.
[[nodiscard]] QString GetValueByName(const QString &name);

namespace internal {

struct KeyData {
	const char *name;
	bool plural;
	const char *values[2]; // by Language; "#other" for plural keys
	const char *ones[2]; // "#one" or nullptr
};

struct Catalog {
	const KeyData *keys = nullptr;
	int count = 0;
};

// Defined in the generated lang_auto.cpp.
[[nodiscard]] Catalog GeneratedCatalog();

struct TagValue {
	const char *tag;
	const QString &value;
};

// Puts the values in place of their {tag}s in one pass: a value that has
// "{tag}" in it (somebody's name) is left as it is.
void ReplaceTags(QString &text, std::initializer_list<TagValue> tags);
void ReplaceTag(QString &text, const char *tag, const QString &value);

} // namespace internal
} // namespace Lang

namespace tr {

struct now_t {
};
inline constexpr now_t now = {};

} // namespace tr
