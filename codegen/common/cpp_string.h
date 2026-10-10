// Helpers shared by the code generators.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace codegen::common {

// A C++ narrow string literal holding `value` as UTF-8, safe for any compiler
// (non-ASCII bytes are written as three-digit octal escapes).
[[nodiscard]] inline QByteArray CppUtf8Literal(const QString &value) {
	const auto utf8 = value.toUtf8();
	auto result = QByteArray("\"");
	for (const auto ch : utf8) {
		const auto code = uchar(ch);
		if (code == '"' || code == '\\') {
			result.append('\\').append(ch);
		} else if (code == '\n') {
			result.append("\\n");
		} else if (code == '\t') {
			result.append("\\t");
		} else if (code == '?') {
			result.append("\\?"); // no trigraphs
		} else if (code < 0x20 || code >= 0x7F) {
			result.append('\\');
			result.append(char('0' + ((code >> 6) & 0x07)));
			result.append(char('0' + ((code >> 3) & 0x07)));
			result.append(char('0' + (code & 0x07)));
		} else {
			result.append(ch);
		}
	}
	result.append('"');
	return result;
}

// QString::fromUtf8("...") expression.
[[nodiscard]] inline QByteArray CppQString(const QString &value) {
	return "QString::fromUtf8(" + CppUtf8Literal(value) + ")";
}

} // namespace codegen::common
