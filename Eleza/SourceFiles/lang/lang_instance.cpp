#include "lang/lang_instance.h"

#include "lang/lang_format.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>

#include <cmath>
#include <vector>

namespace Lang {
namespace {

Language CurrentLanguage = Language::Persian;

[[nodiscard]] Notifier *NotifierInstance() {
	static const auto result = new Notifier(QCoreApplication::instance());
	return result;
}

struct Cache {
	std::vector<QString> values[2];
	std::vector<QString> ones[2];
	std::vector<bool> filled[2];
};

[[nodiscard]] Cache &Values() {
	static auto result = Cache();
	return result;
}

[[nodiscard]] int Index(Language language) {
	return (language == Language::English) ? 1 : 0;
}

void Ensure(int language, int key) {
	auto &cache = Values();
	const auto catalog = internal::GeneratedCatalog();
	if (int(cache.filled[language].size()) != catalog.count) {
		cache.values[language].assign(catalog.count, QString());
		cache.ones[language].assign(catalog.count, QString());
		cache.filled[language].assign(catalog.count, false);
	}
	if (!cache.filled[language][key]) {
		const auto &data = catalog.keys[key];
		cache.values[language][key] = QString::fromUtf8(data.values[language]);
		if (data.ones[language]) {
			cache.ones[language][key] = QString::fromUtf8(data.ones[language]);
		}
		cache.filled[language][key] = true;
	}
}

} // namespace

Language Current() {
	return CurrentLanguage;
}

void SetCurrent(Language language) {
	if (CurrentLanguage == language) {
		return;
	}
	CurrentLanguage = language;
	emit NotifierInstance()->changed();
}

bool RightToLeft() {
	return CurrentLanguage == Language::Persian;
}

QString Id(Language language) {
	return (language == Language::English)
		? QStringLiteral("en")
		: QStringLiteral("fa");
}

Language FromId(const QString &id, Language fallback) {
	const auto lower = id.toLower();
	if (lower.startsWith(QStringLiteral("fa"))) {
		return Language::Persian;
	} else if (lower.startsWith(QStringLiteral("en"))) {
		return Language::English;
	}
	return fallback;
}

QString NativeName(Language language) {
	return (language == Language::English)
		? QStringLiteral("English")
		: QString::fromUtf8("فارسی");
}

QString HttpAcceptLanguage() {
	return (CurrentLanguage == Language::Persian)
		? QStringLiteral("fa-IR,fa;q=0.9,en;q=0.5")
		: QStringLiteral("en-US,en;q=0.9,fa;q=0.5");
}

Notifier *Updated() {
	return NotifierInstance();
}

void OnUpdated(QObject *context, std::function<void()> callback) {
	QObject::connect(
		NotifierInstance(),
		&Notifier::changed,
		context,
		std::move(callback));
}

QString GetValue(Key key) {
	const auto index = int(key);
	const auto catalog = internal::GeneratedCatalog();
	if (index < 0 || index >= catalog.count) {
		return QString();
	}
	const auto language = Index(CurrentLanguage);
	Ensure(language, index);
	return Values().values[language][index];
}

QString GetPlural(Key key, double count) {
	const auto index = int(key);
	const auto catalog = internal::GeneratedCatalog();
	if (index < 0 || index >= catalog.count) {
		return QString();
	}
	const auto language = Index(CurrentLanguage);
	Ensure(language, index);
	const auto &cache = Values();
	const auto integral = (std::floor(count) == count);
	const auto useOne = integral
		&& (std::abs(count) == 1.)
		&& !cache.ones[language][index].isEmpty();
	auto result = useOne
		? cache.ones[language][index]
		: cache.values[language][index];
	const auto number = integral
		? QString::number(qint64(count))
		: QString::number(count, 'f', 1);
	internal::ReplaceTag(result, "count", Num(number));
	return result;
}

QString GetValueByName(const QString &name) {
	static auto map = QHash<QString, int>();
	const auto catalog = internal::GeneratedCatalog();
	if (map.isEmpty()) {
		for (auto i = 0; i != catalog.count; ++i) {
			map.insert(QString::fromLatin1(catalog.keys[i].name), i);
		}
	}
	const auto i = map.constFind(name);
	return (i == map.cend()) ? QString() : GetValue(Key(i.value()));
}

namespace internal {

void ReplaceTag(QString &text, const char *tag, const QString &value) {
	const auto pattern = QLatin1Char('{')
		+ QString::fromLatin1(tag)
		+ QLatin1Char('}');
	text.replace(pattern, value);
}

} // namespace internal
} // namespace Lang
