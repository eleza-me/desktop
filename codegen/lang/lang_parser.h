// Parser of `.strings` files:
//
//   // comment
//   "lng_key" = "value with {tag}";
//   "lng_plural#one" = "{count} message";
//   "lng_plural#other" = "{count} messages";
//
// Escapes inside values: \" \\ \n. Keys are lower_snake_case and start with "lng_".
// A key is plural when it has "#one" / "#other" forms; "#other" is mandatory and the
// value must contain {count}.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QList>
#include <QtCore/QHash>

namespace codegen::lang {

struct Error {
	QString file;
	int line = 0;
	QString text;

	// "file:line: error: text"
	[[nodiscard]] QString format() const;
};

struct Entry {
	QString key; // without the "#form" suffix
	QString value; // singular value or the "#other" form
	QString one; // "#one" form, may be empty
	bool plural = false;
	bool hasOne = false;
	QStringList tags; // in order of first appearance in `value`
	int line = 0;
};

struct File {
	QString path;
	QList<Entry> entries; // in file order
	QHash<QString, int> indexByKey;
};

// The parsed pair: `base` (fa) defines the list and the order of keys,
// `other` (en) must contain exactly the same keys with the same tags.
struct Catalog {
	File base;
	File other;
};

bool ParseContent(
	const QString &path,
	const QString &content,
	File &result,
	Error &error);
bool ParseFile(const QString &path, File &result, Error &error);

// Cross-checks the two files (same keys, same plural kind, same tags).
bool Validate(const Catalog &catalog, Error &error);

[[nodiscard]] QStringList ExtractTags(const QString &value);

} // namespace codegen::lang
