#include "lang/lang_parser.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>

namespace codegen::lang {
namespace {

[[nodiscard]] bool IsGoodKey(const QString &key) {
	if (!key.startsWith(QStringLiteral("lng_")) || key.size() < 5) {
		return false;
	}
	for (const auto ch : key) {
		const auto code = ch.unicode();
		if (!((code >= 'a' && code <= 'z')
			|| (code >= '0' && code <= '9')
			|| code == '_')) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsGoodTag(const QString &tag) {
	if (tag.isEmpty() || tag[0].isDigit()) {
		return false;
	}
	for (const auto ch : tag) {
		const auto code = ch.unicode();
		if (!((code >= 'a' && code <= 'z')
			|| (code >= '0' && code <= '9')
			|| code == '_')) {
			return false;
		}
	}
	return true;
}

} // namespace

QString Error::format() const {
	return QStringLiteral("%1:%2: error: %3").arg(file).arg(line).arg(text);
}

QStringList ExtractTags(const QString &value) {
	auto result = QStringList();
	auto from = 0;
	while (true) {
		const auto open = value.indexOf(QLatin1Char('{'), from);
		if (open < 0) {
			break;
		}
		const auto close = value.indexOf(QLatin1Char('}'), open + 1);
		if (close < 0) {
			break;
		}
		const auto tag = value.mid(open + 1, close - open - 1);
		if (IsGoodTag(tag)) {
			if (!result.contains(tag)) {
				result.push_back(tag);
			}
			from = int(close + 1);
		} else {
			from = int(open + 1);
		}
	}
	return result;
}

bool ParseContent(
		const QString &path,
		const QString &content,
		File &result,
		Error &error) {
	result = File();
	result.path = path;

	auto pos = 0;
	auto line = 1;
	const auto size = int(content.size());
	const auto fail = [&](const QString &text, int atLine) {
		error.file = path;
		error.line = atLine;
		error.text = text;
		return false;
	};
	const auto skip = [&]() -> bool {
		while (pos < size) {
			const auto ch = content[pos];
			if (ch == QLatin1Char('\n')) {
				++line;
				++pos;
			} else if (ch.isSpace() || ch == QChar(0xFEFF)) {
				++pos;
			} else if (ch == QLatin1Char('/')
				&& pos + 1 < size
				&& content[pos + 1] == QLatin1Char('/')) {
				while (pos < size && content[pos] != QLatin1Char('\n')) {
					++pos;
				}
			} else if (ch == QLatin1Char('/')
				&& pos + 1 < size
				&& content[pos + 1] == QLatin1Char('*')) {
				const auto end = content.indexOf(QStringLiteral("*/"), pos + 2);
				if (end < 0) {
					return fail(QStringLiteral("unterminated comment"), line);
				}
				line += content.mid(pos, end - pos).count(QLatin1Char('\n'));
				pos = int(end + 2);
			} else {
				break;
			}
		}
		return true;
	};
	const auto readString = [&](QString &to, const char *what) -> bool {
		if (pos >= size || content[pos] != QLatin1Char('"')) {
			return fail(QStringLiteral("expected %1 in double quotes").arg(
				QString::fromLatin1(what)), line);
		}
		++pos;
		to = QString();
		while (true) {
			if (pos >= size) {
				return fail(QStringLiteral("unterminated string"), line);
			}
			const auto ch = content[pos++];
			if (ch == QLatin1Char('"')) {
				return true;
			} else if (ch == QLatin1Char('\n')) {
				return fail(QStringLiteral(
					"unterminated string (use \\n for a line break)"), line);
			} else if (ch == QLatin1Char('\\')) {
				if (pos >= size) {
					return fail(QStringLiteral("unterminated string"), line);
				}
				const auto escaped = content[pos++];
				if (escaped == QLatin1Char('n')) {
					to.append(QLatin1Char('\n'));
				} else if (escaped == QLatin1Char('"')
					|| escaped == QLatin1Char('\\')) {
					to.append(escaped);
				} else {
					return fail(QStringLiteral(
						"unknown escape '\\%1'").arg(escaped), line);
				}
			} else {
				to.append(ch);
			}
		}
	};

	auto seenForms = QSet<QString>();
	while (true) {
		if (!skip()) {
			return false;
		} else if (pos >= size) {
			break;
		}
		const auto entryLine = line;
		auto fullKey = QString();
		auto value = QString();
		if (!readString(fullKey, "a key") || !skip()) {
			return false;
		} else if (pos >= size || content[pos] != QLatin1Char('=')) {
			return fail(QStringLiteral(
				"expected '=' after \"%1\"").arg(fullKey), line);
		}
		++pos;
		if (!skip() || !readString(value, "a value") || !skip()) {
			return false;
		} else if (pos >= size || content[pos] != QLatin1Char(';')) {
			return fail(QStringLiteral(
				"expected ';' after the value of \"%1\"").arg(fullKey), line);
		}
		++pos;

		const auto hash = fullKey.indexOf(QLatin1Char('#'));
		const auto key = (hash < 0) ? fullKey : fullKey.left(hash);
		const auto form = (hash < 0) ? QString() : fullKey.mid(hash + 1);
		if (!IsGoodKey(key)) {
			return fail(QStringLiteral(
				"bad key \"%1\": keys start with lng_ and use a-z, 0-9, _"
			).arg(key), entryLine);
		} else if (hash >= 0
			&& form != QStringLiteral("one")
			&& form != QStringLiteral("other")) {
			return fail(QStringLiteral(
				"bad plural form \"#%1\" (use #one and #other)").arg(form),
				entryLine);
		} else if (seenForms.contains(fullKey)) {
			return fail(QStringLiteral(
				"key \"%1\" is defined twice").arg(fullKey), entryLine);
		}
		seenForms.insert(fullKey);

		auto index = result.indexByKey.value(key, -1);
		if (index < 0) {
			index = int(result.entries.size());
			result.indexByKey.insert(key, index);
			auto entry = Entry();
			entry.key = key;
			entry.plural = (hash >= 0);
			entry.line = entryLine;
			result.entries.push_back(entry);
		}
		auto &entry = result.entries[index];
		if (entry.plural != (hash >= 0)) {
			return fail(QStringLiteral(
				"key \"%1\" is used both with and without plural forms"
			).arg(key), entryLine);
		}
		if (form == QStringLiteral("one")) {
			entry.one = value;
			entry.hasOne = true;
		} else {
			entry.value = value;
			entry.tags = ExtractTags(value);
		}
	}
	for (const auto &entry : result.entries) {
		if (!entry.plural) {
			continue;
		} else if (!seenForms.contains(entry.key + QStringLiteral("#other"))) {
			return fail(QStringLiteral(
				"plural key \"%1\" has no \"#other\" form").arg(entry.key),
				entry.line);
		} else if (!entry.tags.contains(QStringLiteral("count"))) {
			return fail(QStringLiteral(
				"plural key \"%1\" must contain {count} in \"#other\""
			).arg(entry.key), entry.line);
		}
		for (const auto &tag : ExtractTags(entry.one)) {
			if (!entry.tags.contains(tag)) {
				return fail(QStringLiteral(
					"\"%1#one\" uses {%2} which is missing in \"#other\""
				).arg(entry.key, tag), entry.line);
			}
		}
	}
	return true;
}

bool ParseFile(const QString &path, File &result, Error &error) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		error.file = path;
		error.line = 0;
		error.text = QStringLiteral("could not read the file");
		return false;
	}
	return ParseContent(
		QFileInfo(path).absoluteFilePath(),
		QString::fromUtf8(file.readAll()),
		result,
		error);
}

bool Validate(const Catalog &catalog, Error &error) {
	const auto fail = [&](const File &file, int line, const QString &text) {
		error.file = file.path;
		error.line = line;
		error.text = text;
		return false;
	};
	for (const auto &entry : catalog.base.entries) {
		const auto index = catalog.other.indexByKey.value(entry.key, -1);
		if (index < 0) {
			return fail(catalog.base, entry.line, QStringLiteral(
				"key \"%1\" is missing in %2"
			).arg(entry.key, QFileInfo(catalog.other.path).fileName()));
		}
		const auto &other = catalog.other.entries[index];
		if (other.plural != entry.plural) {
			return fail(catalog.other, other.line, QStringLiteral(
				"key \"%1\" must be %2 like in %3").arg(
					entry.key,
					entry.plural
						? QStringLiteral("plural (#one / #other)")
						: QStringLiteral("non-plural"),
					QFileInfo(catalog.base.path).fileName()));
		}
		auto mine = entry.tags;
		auto theirs = other.tags;
		mine.sort();
		theirs.sort();
		if (mine != theirs) {
			return fail(catalog.other, other.line, QStringLiteral(
				"key \"%1\" has tags {%2} here but {%3} in %4").arg(
					entry.key,
					other.tags.join(QStringLiteral("}, {")),
					entry.tags.join(QStringLiteral("}, {")),
					QFileInfo(catalog.base.path).fileName()));
		}
	}
	for (const auto &entry : catalog.other.entries) {
		if (!catalog.base.indexByKey.contains(entry.key)) {
			return fail(catalog.other, entry.line, QStringLiteral(
				"key \"%1\" does not exist in %2"
			).arg(entry.key, QFileInfo(catalog.base.path).fileName()));
		}
	}
	return true;
}

} // namespace codegen::lang
