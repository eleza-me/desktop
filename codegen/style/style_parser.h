// Parser of the `.style` language and of `colors.palette`.
//
// Grammar (informal):
//
//   file      := { using | structDef | variable }
//   using     := 'using' STRING ';'
//   structDef := Name '{' { field ':' typeName ';' } '}'
//   variable  := name ':' value ';'        (';' is optional after a struct value's '}')
//   value     := NUMBER 'px' | NUMBER 'ms' | NUMBER | 'true' | 'false' | STRING
//              | '#rrggbb' | '#rrggbbaa' | name
//              | 'font' '(' (NUMBER 'px' | name) { flag | STRING } ')'
//              | 'margins' '(' px ',' px ',' px ',' px ')'
//              | 'size' '(' px ',' px ')' | 'point' '(' px ',' px ')'
//              | 'align' '(' name ')'
//              | 'icon' '{{' STRING ',' color [ ',' px ] '}}'
//              | Name [ '(' parentName ')' ] '{' { field ':' value ';' } '}'
//
// Type names: pixels int double bool string color font margins size point icon align
// duration, or the name of a struct declared in this file or in a `using`-ed one.
//
// A name on the right side refers to an earlier value (this file or a used file) or to a
// palette color. References are resolved while parsing: every value is stored by value.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QList>
#include <QtCore/QHash>

#include <memory>
#include <vector>

namespace codegen::style {

struct Location {
	QString file;
	int line = 0;
};

struct Error {
	Location where;
	QString text;

	// "file:line: error: text"
	[[nodiscard]] QString format() const;
};

enum class TypeKind {
	Invalid,
	Pixels,
	Int,
	Double,
	Bool,
	String,
	Color,
	Font,
	Margins,
	Size,
	Point,
	Icon,
	Align,
	Duration,
	Struct,
};

struct Type {
	TypeKind kind = TypeKind::Invalid;
	QString structName; // for TypeKind::Struct

	friend bool operator==(const Type &a, const Type &b) {
		return (a.kind == b.kind) && (a.structName == b.structName);
	}
	friend bool operator!=(const Type &a, const Type &b) {
		return !(a == b);
	}
};

[[nodiscard]] QString TypeName(const Type &type);

enum FontFlag {
	FontBold = 0x01,
	FontSemibold = 0x02,
	FontMedium = 0x04,
	FontItalic = 0x08,
	FontUnderline = 0x10,
	FontStrikeOut = 0x20,
	FontMonospace = 0x40,
};

struct Value {
	Type type;

	int intValue = 0; // Pixels, Int, Duration, Bool (0/1)
	double doubleValue = 0.;
	QString stringValue; // String, Align (enum name), Icon (name)

	// Color.
	bool colorFromPalette = false;
	QString colorName; // palette name
	quint32 colorRgba = 0; // 0xRRGGBBAA literal

	// Font.
	int fontSize = 0;
	int fontFlags = 0;
	QString fontFamily; // empty = default text family

	// Margins: left, top, right, bottom. Size: w, h. Point: x, y.
	int numbers[4] = { 0, 0, 0, 0 };

	// Icon.
	std::shared_ptr<Value> iconColor;
	int iconSize = 24;

	// Struct.
	std::vector<Value> fields; // same order as StructDef::fields
};

struct StructField {
	QString name;
	Type type;
	Location where;
};

struct StructDef {
	QString name;
	QList<StructField> fields;
	Location where;
};

struct Variable {
	QString name;
	Value value;
	Location where;
};

struct PaletteColor {
	QString name;
	quint32 light = 0; // 0xRRGGBBAA
	quint32 dark = 0;
	QString comment;
	Location where;
};

struct Palette {
	QString path;
	QList<PaletteColor> colors;
	QHash<QString, int> indexByName;
};

struct Module {
	QString path; // absolute
	QString name; // "basic" for ".../basic.style"
	QList<std::shared_ptr<Module>> includes;
	QList<StructDef> structs;
	QList<Variable> variables;

	[[nodiscard]] const StructDef *findStruct(const QString &name) const;
	[[nodiscard]] const Variable *findVariable(const QString &name) const;
};

struct Options {
	QStringList includePaths;
};

// Parses `colors.palette`:
//   name: #rrggbb[aa] | #rrggbb[aa]; // light | dark
//   name: #rrggbb[aa];               // same in both
// Returns false and fills `error` on failure.
bool ParsePalette(const QString &path, Palette &result, Error &error);
bool ParsePaletteContent(
	const QString &path,
	const QString &content,
	Palette &result,
	Error &error);

// Parses a `.style` file with everything it uses. Returns nullptr on failure.
std::shared_ptr<Module> ParseModule(
	const QString &path,
	const Options &options,
	const Palette &palette,
	Error &error);

// For tests: `content` is used instead of reading `path` for the top-level file.
std::shared_ptr<Module> ParseModuleContent(
	const QString &path,
	const QString &content,
	const Options &options,
	const Palette &palette,
	Error &error);

} // namespace codegen::style
