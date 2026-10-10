#include "style/style_parser.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QSet>

namespace codegen::style {
namespace {

enum class TokenType {
	End,
	Ident,
	Number,
	String,
	Color,
	Colon,
	Semicolon,
	Comma,
	Pipe,
	LeftBrace,
	RightBrace,
	LeftParen,
	RightParen,
	DoubleLeftBrace,
	DoubleRightBrace,
	Invalid,
};

struct Token {
	TokenType type = TokenType::End;
	QString text; // Ident, String (unescaped), Color (hex digits), Invalid (message)
	double number = 0.;
	bool isInteger = false;
	QString unit; // "px", "ms" or empty
	int line = 0;
};

class Lexer {
public:
	Lexer(QString content) : _content(std::move(content)) {
	}

	[[nodiscard]] Token next() {
		skipSpaceAndComments();
		auto result = Token();
		result.line = _line;
		if (_failed) {
			result.type = TokenType::Invalid;
			result.text = _failure;
			return result;
		} else if (_pos >= _content.size()) {
			result.type = TokenType::End;
			return result;
		}
		const auto ch = _content[_pos];
		const auto simple = [&](TokenType type, int length = 1) {
			_pos += length;
			result.type = type;
			return result;
		};
		if (ch == QLatin1Char('{')) {
			return (peek(1) == QLatin1Char('{'))
				? simple(TokenType::DoubleLeftBrace, 2)
				: simple(TokenType::LeftBrace);
		} else if (ch == QLatin1Char('}')) {
			return (peek(1) == QLatin1Char('}'))
				? simple(TokenType::DoubleRightBrace, 2)
				: simple(TokenType::RightBrace);
		} else if (ch == QLatin1Char('(')) {
			return simple(TokenType::LeftParen);
		} else if (ch == QLatin1Char(')')) {
			return simple(TokenType::RightParen);
		} else if (ch == QLatin1Char(':')) {
			return simple(TokenType::Colon);
		} else if (ch == QLatin1Char(';')) {
			return simple(TokenType::Semicolon);
		} else if (ch == QLatin1Char(',')) {
			return simple(TokenType::Comma);
		} else if (ch == QLatin1Char('|')) {
			return simple(TokenType::Pipe);
		} else if (ch == QLatin1Char('"')) {
			return readString(result);
		} else if (ch == QLatin1Char('#')) {
			return readColor(result);
		} else if (ch.isDigit()
			|| ((ch == QLatin1Char('-') || ch == QLatin1Char('.'))
				&& peek(1).isDigit())) {
			return readNumber(result);
		} else if (ch.isLetter() || ch == QLatin1Char('_')) {
			const auto from = _pos;
			while (_pos < _content.size()
				&& (_content[_pos].isLetterOrNumber()
					|| _content[_pos] == QLatin1Char('_'))) {
				++_pos;
			}
			result.type = TokenType::Ident;
			result.text = _content.mid(from, _pos - from);
			return result;
		}
		result.type = TokenType::Invalid;
		result.text = QStringLiteral("unexpected character '%1'").arg(ch);
		++_pos;
		return result;
	}

private:
	[[nodiscard]] QChar peek(int offset) const {
		return (_pos + offset < _content.size())
			? _content[_pos + offset]
			: QChar();
	}

	void skipSpaceAndComments() {
		while (_pos < _content.size()) {
			const auto ch = _content[_pos];
			if (ch == QLatin1Char('\n')) {
				++_line;
				++_pos;
			} else if (ch.isSpace() || ch == QChar(0xFEFF)) {
				++_pos;
			} else if (ch == QLatin1Char('/') && peek(1) == QLatin1Char('/')) {
				while (_pos < _content.size()
					&& _content[_pos] != QLatin1Char('\n')) {
					++_pos;
				}
			} else if (ch == QLatin1Char('/') && peek(1) == QLatin1Char('*')) {
				const auto startLine = _line;
				_pos += 2;
				auto closed = false;
				while (_pos < _content.size()) {
					if (_content[_pos] == QLatin1Char('*')
						&& peek(1) == QLatin1Char('/')) {
						_pos += 2;
						closed = true;
						break;
					} else if (_content[_pos] == QLatin1Char('\n')) {
						++_line;
					}
					++_pos;
				}
				if (!closed) {
					_failed = true;
					_failure = QStringLiteral("unterminated comment");
					_line = startLine;
					return;
				}
			} else {
				break;
			}
		}
	}

	Token readString(Token result) {
		++_pos;
		auto text = QString();
		while (true) {
			if (_pos >= _content.size()
				|| _content[_pos] == QLatin1Char('\n')) {
				result.type = TokenType::Invalid;
				result.text = QStringLiteral("unterminated string");
				return result;
			}
			const auto ch = _content[_pos++];
			if (ch == QLatin1Char('"')) {
				break;
			} else if (ch == QLatin1Char('\\') && _pos < _content.size()) {
				const auto escaped = _content[_pos++];
				if (escaped == QLatin1Char('n')) {
					text.append(QLatin1Char('\n'));
				} else if (escaped == QLatin1Char('t')) {
					text.append(QLatin1Char('\t'));
				} else {
					text.append(escaped);
				}
			} else {
				text.append(ch);
			}
		}
		result.type = TokenType::String;
		result.text = text;
		return result;
	}

	Token readColor(Token result) {
		++_pos;
		const auto from = _pos;
		while (_pos < _content.size() && _content[_pos].isLetterOrNumber()) {
			++_pos;
		}
		const auto hex = _content.mid(from, _pos - from);
		auto good = (hex.size() == 6 || hex.size() == 8);
		for (const auto ch : hex) {
			const auto code = ch.toLower().unicode();
			if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) {
				good = false;
			}
		}
		if (!good) {
			result.type = TokenType::Invalid;
			result.text = QStringLiteral(
				"bad color '#%1', expected #rrggbb or #rrggbbaa").arg(hex);
			return result;
		}
		result.type = TokenType::Color;
		result.text = hex;
		return result;
	}

	Token readNumber(Token result) {
		const auto from = _pos;
		if (_content[_pos] == QLatin1Char('-')) {
			++_pos;
		}
		auto dot = false;
		while (_pos < _content.size()
			&& (_content[_pos].isDigit()
				|| (!dot && _content[_pos] == QLatin1Char('.')))) {
			if (_content[_pos] == QLatin1Char('.')) {
				dot = true;
			}
			++_pos;
		}
		result.number = _content.mid(from, _pos - from).toDouble();
		result.isInteger = !dot;
		const auto unitFrom = _pos;
		while (_pos < _content.size() && _content[_pos].isLetter()) {
			++_pos;
		}
		result.unit = _content.mid(unitFrom, _pos - unitFrom);
		if (!result.unit.isEmpty()
			&& result.unit != QStringLiteral("px")
			&& result.unit != QStringLiteral("ms")) {
			result.type = TokenType::Invalid;
			result.text = QStringLiteral(
				"unknown unit '%1', expected 'px' or 'ms'").arg(result.unit);
			return result;
		}
		result.type = TokenType::Number;
		return result;
	}

	QString _content;
	int _pos = 0;
	int _line = 1;
	bool _failed = false;
	QString _failure;

};

[[nodiscard]] quint32 ColorFromHex(const QString &hex) {
	auto ok = false;
	const auto value = hex.toUInt(&ok, 16);
	return (hex.size() == 6) ? ((value << 8) | 0xFFU) : value;
}

[[nodiscard]] Type BuiltinType(const QString &name) {
	static const auto map = QHash<QString, TypeKind>{
		{ QStringLiteral("pixels"), TypeKind::Pixels },
		{ QStringLiteral("int"), TypeKind::Int },
		{ QStringLiteral("double"), TypeKind::Double },
		{ QStringLiteral("bool"), TypeKind::Bool },
		{ QStringLiteral("string"), TypeKind::String },
		{ QStringLiteral("color"), TypeKind::Color },
		{ QStringLiteral("font"), TypeKind::Font },
		{ QStringLiteral("margins"), TypeKind::Margins },
		{ QStringLiteral("size"), TypeKind::Size },
		{ QStringLiteral("point"), TypeKind::Point },
		{ QStringLiteral("icon"), TypeKind::Icon },
		{ QStringLiteral("align"), TypeKind::Align },
		{ QStringLiteral("duration"), TypeKind::Duration },
	};
	return Type{ map.value(name, TypeKind::Invalid), QString() };
}

[[nodiscard]] bool IsReservedName(const QString &name) {
	static const auto set = QSet<QString>{
		QStringLiteral("using"),
		QStringLiteral("true"),
		QStringLiteral("false"),
		QStringLiteral("pixels"),
		QStringLiteral("int"),
		QStringLiteral("double"),
		QStringLiteral("bool"),
		QStringLiteral("string"),
		QStringLiteral("color"),
		QStringLiteral("font"),
		QStringLiteral("margins"),
		QStringLiteral("size"),
		QStringLiteral("point"),
		QStringLiteral("icon"),
		QStringLiteral("align"),
		QStringLiteral("duration"),
	};
	return set.contains(name);
}

struct Shared {
	const Options *options = nullptr;
	const Palette *palette = nullptr;
	QHash<QString, std::shared_ptr<Module>> parsed; // by absolute path
	QStringList stack; // files being parsed, for cycle detection
	Error error;
	bool failed = false;
};

class Parser {
public:
	Parser(QString path, QString content, Shared &shared)
	: _path(std::move(path))
	, _lexer(std::move(content))
	, _shared(shared) {
		advance();
	}

	std::shared_ptr<Module> parse() {
		_module = std::make_shared<Module>();
		_module->path = _path;
		_module->name = QFileInfo(_path).completeBaseName();
		while (!_shared.failed && _token.type != TokenType::End) {
			if (!parseTopLevel()) {
				break;
			}
		}
		return _shared.failed ? nullptr : _module;
	}

private:
	bool fail(const QString &text, int line = -1) {
		if (!_shared.failed) {
			_shared.failed = true;
			_shared.error.where.file = _path;
			_shared.error.where.line = (line >= 0) ? line : _token.line;
			_shared.error.text = text;
		}
		return false;
	}

	void advance() {
		_lastWasStructEnd = (_token.type == TokenType::RightBrace);
		_token = _lexer.next();
		if (_token.type == TokenType::Invalid) {
			fail(_token.text);
		}
	}

	[[nodiscard]] QString describe(const Token &token) const {
		switch (token.type) {
		case TokenType::End: return QStringLiteral("end of file");
		case TokenType::Ident: return QStringLiteral("'%1'").arg(token.text);
		case TokenType::Number: return QStringLiteral("number");
		case TokenType::String: return QStringLiteral("string");
		case TokenType::Color: return QStringLiteral("color");
		case TokenType::Colon: return QStringLiteral("':'");
		case TokenType::Semicolon: return QStringLiteral("';'");
		case TokenType::Comma: return QStringLiteral("','");
		case TokenType::Pipe: return QStringLiteral("'|'");
		case TokenType::LeftBrace: return QStringLiteral("'{'");
		case TokenType::RightBrace: return QStringLiteral("'}'");
		case TokenType::LeftParen: return QStringLiteral("'('");
		case TokenType::RightParen: return QStringLiteral("')'");
		case TokenType::DoubleLeftBrace: return QStringLiteral("'{{'");
		case TokenType::DoubleRightBrace: return QStringLiteral("'}}'");
		case TokenType::Invalid: return token.text;
		}
		return QString();
	}

	bool expect(TokenType type, const char *what) {
		if (_shared.failed) {
			return false;
		} else if (_token.type != type) {
			return fail(QStringLiteral("expected %1, got %2").arg(
				QString::fromLatin1(what),
				describe(_token)));
		}
		advance();
		return !_shared.failed;
	}

	// ';' ends a value; it is optional right after the '}' of a struct value.
	bool expectValueEnd() {
		if (_shared.failed) {
			return false;
		} else if (_token.type == TokenType::Semicolon) {
			advance();
			return !_shared.failed;
		} else if (_lastWasStructEnd) {
			return true;
		}
		return expect(TokenType::Semicolon, "';'");
	}

	[[nodiscard]] const StructDef *findStruct(const QString &name) const {
		return _module->findStruct(name);
	}

	[[nodiscard]] const Variable *findVariable(const QString &name) const {
		return _module->findVariable(name);
	}

	bool parseTopLevel() {
		if (_token.type != TokenType::Ident) {
			return fail(QStringLiteral("expected a name, got %1").arg(
				describe(_token)));
		}
		const auto name = _token.text;
		const auto line = _token.line;
		if (name == QStringLiteral("using")) {
			advance();
			return parseUsing(line);
		}
		advance();
		if (_shared.failed) {
			return false;
		} else if (_token.type == TokenType::LeftBrace) {
			return parseStructDef(name, line);
		} else if (_token.type == TokenType::Colon) {
			advance();
			return parseVariable(name, line);
		}
		return fail(QStringLiteral(
			"expected ':' (value) or '{' (struct definition) after '%1', got %2"
		).arg(name, describe(_token)));
	}

	bool parseUsing(int line) {
		if (_token.type != TokenType::String) {
			return fail(QStringLiteral(
				"expected a file name in quotes after 'using'"));
		}
		const auto relative = _token.text;
		advance();
		if (!expect(TokenType::Semicolon, "';'")) {
			return false;
		}
		auto candidates = QStringList();
		candidates.push_back(QFileInfo(_path).absoluteDir().filePath(relative));
		for (const auto &dir : _shared.options->includePaths) {
			candidates.push_back(QDir(dir).filePath(relative));
		}
		auto found = QString();
		for (const auto &candidate : candidates) {
			if (QFileInfo(candidate).isFile()) {
				found = QFileInfo(candidate).absoluteFilePath();
				break;
			}
		}
		if (found.isEmpty()) {
			return fail(QStringLiteral(
				"could not find \"%1\" (searched next to this file and in "
				"%2 include path(s))"
			).arg(relative).arg(_shared.options->includePaths.size()), line);
		}
		for (const auto &included : _module->includes) {
			if (included->path == found) {
				return true;
			}
		}
		if (_shared.stack.contains(found)) {
			return fail(QStringLiteral(
				"circular 'using' of \"%1\"").arg(relative), line);
		}
		auto module = _shared.parsed.value(found);
		if (!module) {
			auto file = QFile(found);
			if (!file.open(QIODevice::ReadOnly)) {
				return fail(QStringLiteral(
					"could not read \"%1\"").arg(found), line);
			}
			const auto content = QString::fromUtf8(file.readAll());
			_shared.stack.push_back(found);
			auto parser = Parser(found, content, _shared);
			module = parser.parse();
			_shared.stack.pop_back();
			if (!module) {
				return false;
			}
			_shared.parsed.insert(found, module);
		}
		_module->includes.push_back(module);
		return true;
	}

	bool checkNewName(const QString &name, int line) {
		if (IsReservedName(name)) {
			return fail(QStringLiteral(
				"'%1' is a reserved word").arg(name), line);
		} else if (const auto already = findVariable(name)) {
			return fail(QStringLiteral(
				"'%1' is already defined at %2:%3"
			).arg(name, already->where.file).arg(already->where.line), line);
		} else if (const auto already = findStruct(name)) {
			return fail(QStringLiteral(
				"'%1' is already defined as a struct at %2:%3"
			).arg(name, already->where.file).arg(already->where.line), line);
		} else if (_shared.palette->indexByName.contains(name)) {
			return fail(QStringLiteral(
				"'%1' is already a palette color").arg(name), line);
		}
		return true;
	}

	bool parseTypeName(Type &result) {
		if (_token.type != TokenType::Ident) {
			return fail(QStringLiteral("expected a type name, got %1").arg(
				describe(_token)));
		}
		result = BuiltinType(_token.text);
		if (result.kind == TypeKind::Invalid) {
			if (!findStruct(_token.text)) {
				return fail(QStringLiteral("unknown type '%1'").arg(
					_token.text));
			}
			result.kind = TypeKind::Struct;
			result.structName = _token.text;
		}
		advance();
		return !_shared.failed;
	}

	bool parseStructDef(const QString &name, int line) {
		if (!checkNewName(name, line)) {
			return false;
		}
		advance(); // '{'
		auto def = StructDef();
		def.name = name;
		def.where = Location{ _path, line };
		while (!_shared.failed && _token.type != TokenType::RightBrace) {
			if (_token.type != TokenType::Ident) {
				return fail(QStringLiteral(
					"expected a field name in struct '%1', got %2"
				).arg(name, describe(_token)));
			}
			auto field = StructField();
			field.name = _token.text;
			field.where = Location{ _path, _token.line };
			for (const auto &existing : def.fields) {
				if (existing.name == field.name) {
					return fail(QStringLiteral(
						"duplicate field '%1' in struct '%2'"
					).arg(field.name, name));
				}
			}
			advance();
			if (!expect(TokenType::Colon, "':'")
				|| !parseTypeName(field.type)
				|| !expect(TokenType::Semicolon, "';'")) {
				return false;
			}
			def.fields.push_back(field);
		}
		if (_shared.failed) {
			return false;
		} else if (def.fields.isEmpty()) {
			return fail(QStringLiteral(
				"struct '%1' has no fields").arg(name), line);
		}
		advance(); // '}'
		_module->structs.push_back(def);
		return !_shared.failed;
	}

	bool parseVariable(const QString &name, int line) {
		if (!checkNewName(name, line)) {
			return false;
		}
		auto variable = Variable();
		variable.name = name;
		variable.where = Location{ _path, line };
		if (!parseValue(variable.value, nullptr) || !expectValueEnd()) {
			return false;
		}
		_module->variables.push_back(variable);
		return true;
	}

	bool parsePixels(int &result, const char *what) {
		if (_token.type == TokenType::Ident) {
			const auto variable = findVariable(_token.text);
			if (variable && variable->value.type.kind == TypeKind::Pixels) {
				result = variable->value.intValue;
				advance();
				return !_shared.failed;
			}
		}
		if (_token.type != TokenType::Number
			|| !_token.isInteger
			|| (_token.unit != QStringLiteral("px")
				&& !(_token.unit.isEmpty() && _token.number == 0.))) {
			return fail(QStringLiteral(
				"expected a pixels value like '12px' for %1, got %2"
			).arg(QString::fromLatin1(what), describe(_token)));
		}
		result = int(_token.number);
		advance();
		return !_shared.failed;
	}

	bool parseColor(Value &result) {
		result.type = Type{ TypeKind::Color, QString() };
		if (_token.type == TokenType::Color) {
			result.colorFromPalette = false;
			result.colorRgba = ColorFromHex(_token.text);
			advance();
			return !_shared.failed;
		} else if (_token.type == TokenType::Ident) {
			const auto name = _token.text;
			if (const auto variable = findVariable(name)) {
				if (variable->value.type.kind != TypeKind::Color) {
					return fail(QStringLiteral(
						"'%1' is %2, expected a color"
					).arg(name, TypeName(variable->value.type)));
				}
				result = variable->value;
				advance();
				return !_shared.failed;
			} else if (_shared.palette->indexByName.contains(name)) {
				result.colorFromPalette = true;
				result.colorName = name;
				advance();
				return !_shared.failed;
			}
			return fail(QStringLiteral(
				"unknown color '%1' (not a palette color and not a value "
				"defined above)").arg(name));
		}
		return fail(QStringLiteral("expected a color, got %1").arg(
			describe(_token)));
	}

	bool parseFont(Value &result) {
		// after 'font'
		result.type = Type{ TypeKind::Font, QString() };
		if (!expect(TokenType::LeftParen, "'(' after 'font'")) {
			return false;
		}
		if (_token.type == TokenType::Ident) {
			const auto variable = findVariable(_token.text);
			if (!variable || variable->value.type.kind != TypeKind::Font) {
				return fail(QStringLiteral(
					"expected a size like '14px' or a font value, got '%1'"
				).arg(_token.text));
			}
			result = variable->value;
			advance();
		} else {
			if (!parsePixels(result.fontSize, "the font size")) {
				return false;
			} else if (result.fontSize <= 0) {
				return fail(QStringLiteral("font size must be positive"));
			}
		}
		while (!_shared.failed && _token.type != TokenType::RightParen) {
			if (_token.type == TokenType::String) {
				result.fontFamily = _token.text;
			} else if (_token.type == TokenType::Ident) {
				static const auto flags = QHash<QString, int>{
					{ QStringLiteral("bold"), FontBold },
					{ QStringLiteral("semibold"), FontSemibold },
					{ QStringLiteral("medium"), FontMedium },
					{ QStringLiteral("italic"), FontItalic },
					{ QStringLiteral("underline"), FontUnderline },
					{ QStringLiteral("strikeout"), FontStrikeOut },
					{ QStringLiteral("monospace"), FontMonospace },
				};
				const auto weights = FontBold | FontSemibold | FontMedium;
				if (_token.text == QStringLiteral("regular")) {
					result.fontFlags &= ~weights;
				} else if (const auto flag = flags.value(_token.text)) {
					if (flag & weights) {
						result.fontFlags &= ~weights;
					}
					result.fontFlags |= flag;
				} else {
					return fail(QStringLiteral(
						"unknown font flag '%1' (use regular, medium, semibold, "
						"bold, italic, underline, strikeout, monospace or a "
						"\"Family Name\")").arg(_token.text));
				}
			} else {
				return fail(QStringLiteral(
					"expected a font flag or ')', got %1").arg(describe(_token)));
			}
			advance();
		}
		return expect(TokenType::RightParen, "')'");
	}

	bool parseNumbers(Value &result, int count, const char *what) {
		if (!expect(TokenType::LeftParen, "'('")) {
			return false;
		}
		for (auto i = 0; i != count; ++i) {
			if (i > 0 && !expect(TokenType::Comma, "','")) {
				return false;
			} else if (!parsePixels(result.numbers[i], what)) {
				return false;
			}
		}
		return expect(TokenType::RightParen, "')'");
	}

	bool parseAlign(Value &result) {
		result.type = Type{ TypeKind::Align, QString() };
		if (!expect(TokenType::LeftParen, "'(' after 'align'")) {
			return false;
		}
		static const auto names = QSet<QString>{
			QStringLiteral("topleft"),
			QStringLiteral("top"),
			QStringLiteral("topright"),
			QStringLiteral("left"),
			QStringLiteral("center"),
			QStringLiteral("right"),
			QStringLiteral("bottomleft"),
			QStringLiteral("bottom"),
			QStringLiteral("bottomright"),
		};
		if (_token.type != TokenType::Ident || !names.contains(_token.text)) {
			return fail(QStringLiteral(
				"expected one of topleft, top, topright, left, center, right, "
				"bottomleft, bottom, bottomright; got %1").arg(describe(_token)));
		}
		result.stringValue = _token.text;
		advance();
		return expect(TokenType::RightParen, "')'");
	}

	bool parseIcon(Value &result) {
		result.type = Type{ TypeKind::Icon, QString() };
		if (!expect(TokenType::DoubleLeftBrace, "'{{' after 'icon'")) {
			return false;
		} else if (_token.type != TokenType::String) {
			return fail(QStringLiteral(
				"expected the icon name in quotes, got %1").arg(describe(_token)));
		}
		result.stringValue = _token.text;
		if (result.stringValue.isEmpty()) {
			return fail(QStringLiteral("icon name is empty"));
		}
		advance();
		if (!expect(TokenType::Comma, "',' and the icon color")) {
			return false;
		}
		auto color = Value();
		if (!parseColor(color)) {
			return false;
		}
		result.iconColor = std::make_shared<Value>(color);
		result.iconSize = 24;
		if (_token.type == TokenType::Comma) {
			advance();
			if (!parsePixels(result.iconSize, "the icon size")) {
				return false;
			} else if (result.iconSize <= 0) {
				return fail(QStringLiteral("icon size must be positive"));
			}
		}
		return expect(TokenType::DoubleRightBrace, "'}}'");
	}

	bool parseStruct(Value &result, const StructDef &def, int line) {
		// after the struct name
		result.type = Type{ TypeKind::Struct, def.name };
		auto filled = QList<bool>(def.fields.size(), false);
		result.fields.clear();
		result.fields.resize(def.fields.size());
		if (_token.type == TokenType::LeftParen) {
			advance();
			if (_token.type != TokenType::Ident) {
				return fail(QStringLiteral(
					"expected a parent value name, got %1").arg(describe(_token)));
			}
			const auto parent = findVariable(_token.text);
			if (!parent) {
				return fail(QStringLiteral(
					"unknown parent value '%1'").arg(_token.text));
			} else if (parent->value.type != result.type) {
				return fail(QStringLiteral(
					"parent '%1' is %2, expected %3"
				).arg(_token.text, TypeName(parent->value.type), def.name));
			}
			result.fields = parent->value.fields;
			filled.fill(true);
			advance();
			if (!expect(TokenType::RightParen, "')'")) {
				return false;
			}
		}
		if (!expect(TokenType::LeftBrace, "'{'")) {
			return false;
		}
		auto assigned = QSet<QString>();
		while (!_shared.failed && _token.type != TokenType::RightBrace) {
			if (_token.type != TokenType::Ident) {
				return fail(QStringLiteral(
					"expected a field name of '%1', got %2"
				).arg(def.name, describe(_token)));
			}
			const auto fieldName = _token.text;
			auto index = -1;
			for (auto i = 0; i != def.fields.size(); ++i) {
				if (def.fields[i].name == fieldName) {
					index = i;
					break;
				}
			}
			if (index < 0) {
				return fail(QStringLiteral(
					"struct '%1' has no field '%2'").arg(def.name, fieldName));
			} else if (assigned.contains(fieldName)) {
				return fail(QStringLiteral(
					"field '%1' is assigned twice").arg(fieldName));
			}
			assigned.insert(fieldName);
			advance();
			if (!expect(TokenType::Colon, "':'")) {
				return false;
			}
			auto value = Value();
			if (!parseValue(value, &def.fields[index].type)
				|| !expectValueEnd()) {
				return false;
			}
			result.fields[index] = value;
			filled[index] = true;
		}
		if (_shared.failed) {
			return false;
		}
		auto missing = QStringList();
		for (auto i = 0; i != def.fields.size(); ++i) {
			if (!filled[i]) {
				missing.push_back(def.fields[i].name);
			}
		}
		if (!missing.isEmpty()) {
			return fail(QStringLiteral(
				"missing field(s) of '%1': %2 (set them or inherit: %1(parent) "
				"{ ... })").arg(def.name, missing.join(QStringLiteral(", "))),
				line);
		}
		advance(); // '}'
		return !_shared.failed;
	}

	// `expected` is null for a top-level value (type is inferred).
	bool parseValue(Value &result, const Type *expected) {
		if (_shared.failed) {
			return false;
		}
		const auto line = _token.line;
		const auto mismatch = [&](const Type &got) {
			return fail(QStringLiteral("expected %1, got %2").arg(
				TypeName(*expected),
				TypeName(got)), line);
		};
		const auto check = [&] {
			if (!expected || result.type == *expected) {
				return true;
			}
			// Small conveniences.
			if (expected->kind == TypeKind::Double
				&& result.type.kind == TypeKind::Int) {
				result.type = *expected;
				result.doubleValue = result.intValue;
				return true;
			} else if ((expected->kind == TypeKind::Pixels
				|| expected->kind == TypeKind::Duration)
				&& result.type.kind == TypeKind::Int
				&& result.intValue == 0) {
				result.type = *expected;
				return true;
			} else if (expected->kind == TypeKind::Duration
				&& result.type.kind == TypeKind::Int) {
				result.type = *expected;
				return true;
			}
			return mismatch(result.type);
		};
		switch (_token.type) {
		case TokenType::Number: {
			if (_token.unit == QStringLiteral("px")) {
				if (!_token.isInteger) {
					return fail(QStringLiteral(
						"pixels values must be integers"));
				}
				result.type = Type{ TypeKind::Pixels, QString() };
				result.intValue = int(_token.number);
			} else if (_token.unit == QStringLiteral("ms")) {
				if (!_token.isInteger || _token.number < 0) {
					return fail(QStringLiteral(
						"durations must be non-negative integers"));
				}
				result.type = Type{ TypeKind::Duration, QString() };
				result.intValue = int(_token.number);
			} else if (_token.isInteger) {
				result.type = Type{ TypeKind::Int, QString() };
				result.intValue = int(_token.number);
			} else {
				result.type = Type{ TypeKind::Double, QString() };
				result.doubleValue = _token.number;
			}
			advance();
			return !_shared.failed && check();
		}
		case TokenType::String: {
			result.type = Type{ TypeKind::String, QString() };
			result.stringValue = _token.text;
			advance();
			return !_shared.failed && check();
		}
		case TokenType::Color: {
			return parseColor(result) && check();
		}
		case TokenType::Ident: {
			const auto name = _token.text;
			if (name == QStringLiteral("true")
				|| name == QStringLiteral("false")) {
				result.type = Type{ TypeKind::Bool, QString() };
				result.intValue = (name == QStringLiteral("true")) ? 1 : 0;
				advance();
				return !_shared.failed && check();
			} else if (name == QStringLiteral("font")) {
				advance();
				return parseFont(result) && check();
			} else if (name == QStringLiteral("margins")) {
				advance();
				result.type = Type{ TypeKind::Margins, QString() };
				return parseNumbers(result, 4, "margins(left, top, right, bottom)")
					&& check();
			} else if (name == QStringLiteral("size")) {
				advance();
				result.type = Type{ TypeKind::Size, QString() };
				return parseNumbers(result, 2, "size(width, height)") && check();
			} else if (name == QStringLiteral("point")) {
				advance();
				result.type = Type{ TypeKind::Point, QString() };
				return parseNumbers(result, 2, "point(x, y)") && check();
			} else if (name == QStringLiteral("align")) {
				advance();
				return parseAlign(result) && check();
			} else if (name == QStringLiteral("icon")) {
				advance();
				return parseIcon(result) && check();
			} else if (const auto def = findStruct(name)) {
				advance();
				return parseStruct(result, *def, line) && check();
			} else if (const auto variable = findVariable(name)) {
				result = variable->value;
				advance();
				return !_shared.failed && check();
			} else if (_shared.palette->indexByName.contains(name)) {
				return parseColor(result) && check();
			}
			return fail(expected
				? QStringLiteral(
					"unknown name '%1' (expected %2)").arg(name, TypeName(*expected))
				: QStringLiteral(
					"unknown name '%1' (not a value defined above, not a struct "
					"and not a palette color)").arg(name));
		}
		default: break;
		}
		return fail(QStringLiteral("expected a value, got %1").arg(
			describe(_token)));
	}

	QString _path;
	Lexer _lexer;
	Shared &_shared;
	Token _token;
	bool _lastWasStructEnd = false;
	std::shared_ptr<Module> _module;

};

[[nodiscard]] bool ReadFile(const QString &path, QString &content) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	content = QString::fromUtf8(file.readAll());
	return true;
}

template <typename Visit>
bool VisitModules(
		const Module &module,
		QSet<const Module*> &visited,
		Visit &&visit) {
	if (visited.contains(&module)) {
		return false;
	}
	visited.insert(&module);
	if (visit(module)) {
		return true;
	}
	for (const auto &included : module.includes) {
		if (VisitModules(*included, visited, visit)) {
			return true;
		}
	}
	return false;
}

} // namespace

QString Error::format() const {
	return QStringLiteral("%1:%2: error: %3").arg(where.file).arg(where.line).arg(
		text);
}

QString TypeName(const Type &type) {
	switch (type.kind) {
	case TypeKind::Invalid: return QStringLiteral("invalid");
	case TypeKind::Pixels: return QStringLiteral("pixels");
	case TypeKind::Int: return QStringLiteral("int");
	case TypeKind::Double: return QStringLiteral("double");
	case TypeKind::Bool: return QStringLiteral("bool");
	case TypeKind::String: return QStringLiteral("string");
	case TypeKind::Color: return QStringLiteral("color");
	case TypeKind::Font: return QStringLiteral("font");
	case TypeKind::Margins: return QStringLiteral("margins");
	case TypeKind::Size: return QStringLiteral("size");
	case TypeKind::Point: return QStringLiteral("point");
	case TypeKind::Icon: return QStringLiteral("icon");
	case TypeKind::Align: return QStringLiteral("align");
	case TypeKind::Duration: return QStringLiteral("duration");
	case TypeKind::Struct: return type.structName;
	}
	return QString();
}

const StructDef *Module::findStruct(const QString &name) const {
	const StructDef *result = nullptr;
	auto visited = QSet<const Module*>();
	VisitModules(*this, visited, [&](const Module &module) {
		for (const auto &def : module.structs) {
			if (def.name == name) {
				result = &def;
				return true;
			}
		}
		return false;
	});
	return result;
}

const Variable *Module::findVariable(const QString &name) const {
	const Variable *result = nullptr;
	auto visited = QSet<const Module*>();
	VisitModules(*this, visited, [&](const Module &module) {
		for (const auto &variable : module.variables) {
			if (variable.name == name) {
				result = &variable;
				return true;
			}
		}
		return false;
	});
	return result;
}

bool ParsePaletteContent(
		const QString &path,
		const QString &content,
		Palette &result,
		Error &error) {
	result = Palette();
	result.path = path;
	auto lexer = Lexer(content);
	auto token = lexer.next();
	const auto fail = [&](const QString &text, int line) {
		error.where = Location{ path, line };
		error.text = text;
		return false;
	};
	const auto next = [&] {
		token = lexer.next();
		return token.type != TokenType::Invalid;
	};
	while (token.type != TokenType::End) {
		if (token.type == TokenType::Invalid) {
			return fail(token.text, token.line);
		} else if (token.type != TokenType::Ident) {
			return fail(QStringLiteral("expected a color name"), token.line);
		}
		auto color = PaletteColor();
		color.name = token.text;
		color.where = Location{ path, token.line };
		if (IsReservedName(color.name)) {
			return fail(QStringLiteral(
				"'%1' is a reserved word").arg(color.name), token.line);
		} else if (result.indexByName.contains(color.name)) {
			return fail(QStringLiteral(
				"color '%1' is defined twice").arg(color.name), token.line);
		}
		if (!next()) {
			return fail(token.text, token.line);
		} else if (token.type != TokenType::Colon) {
			return fail(QStringLiteral(
				"expected ':' after '%1'").arg(color.name), token.line);
		}
		const auto readColor = [&](quint32 &value) {
			if (!next()) {
				return fail(token.text, token.line);
			} else if (token.type == TokenType::Color) {
				value = ColorFromHex(token.text);
				return true;
			} else if (token.type == TokenType::Ident) {
				const auto i = result.indexByName.constFind(token.text);
				if (i == result.indexByName.cend()) {
					return fail(QStringLiteral(
						"unknown color '%1'").arg(token.text), token.line);
				}
				value = 0; // filled by the caller through `copyFrom`
				return true;
			}
			return fail(QStringLiteral(
				"expected #rrggbb, #rrggbbaa or a color name"), token.line);
		};
		auto copyLight = -1;
		if (!readColor(color.light)) {
			return false;
		} else if (token.type == TokenType::Ident) {
			copyLight = result.indexByName.value(token.text);
			color.light = result.colors[copyLight].light;
			color.dark = result.colors[copyLight].dark;
		} else {
			color.dark = color.light;
		}
		if (!next()) {
			return fail(token.text, token.line);
		}
		if (token.type == TokenType::Pipe) {
			if (!readColor(color.dark)) {
				return false;
			} else if (token.type == TokenType::Ident) {
				color.dark = result.colors[
					result.indexByName.value(token.text)].dark;
			}
			if (!next()) {
				return fail(token.text, token.line);
			}
		}
		if (token.type != TokenType::Semicolon) {
			return fail(QStringLiteral(
				"expected ';' after color '%1'").arg(color.name), token.line);
		}
		result.indexByName.insert(color.name, int(result.colors.size()));
		result.colors.push_back(color);
		if (!next()) {
			return fail(token.text, token.line);
		}
	}
	if (result.colors.isEmpty()) {
		return fail(QStringLiteral("the palette is empty"), 1);
	}
	return true;
}

bool ParsePalette(const QString &path, Palette &result, Error &error) {
	auto content = QString();
	if (!ReadFile(path, content)) {
		error.where = Location{ path, 0 };
		error.text = QStringLiteral("could not read the palette file");
		return false;
	}
	return ParsePaletteContent(
		QFileInfo(path).absoluteFilePath(),
		content,
		result,
		error);
}

std::shared_ptr<Module> ParseModuleContent(
		const QString &path,
		const QString &content,
		const Options &options,
		const Palette &palette,
		Error &error) {
	auto shared = Shared();
	shared.options = &options;
	shared.palette = &palette;
	const auto absolute = QFileInfo(path).absoluteFilePath();
	shared.stack.push_back(absolute);
	auto parser = Parser(absolute, content, shared);
	auto result = parser.parse();
	if (!result) {
		error = shared.error;
	}
	return result;
}

std::shared_ptr<Module> ParseModule(
		const QString &path,
		const Options &options,
		const Palette &palette,
		Error &error) {
	auto content = QString();
	if (!ReadFile(path, content)) {
		error.where = Location{ path, 0 };
		error.text = QStringLiteral("could not read the file");
		return nullptr;
	}
	return ParseModuleContent(path, content, options, palette, error);
}

} // namespace codegen::style
