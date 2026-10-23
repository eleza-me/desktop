#include "base/text_direction.h"

#include <QtCore/QChar>

namespace base {

std::optional<bool> FirstStrongIsRtl(QStringView text) {
	auto isolates = 0;
	const auto size = text.size();
	for (auto i = qsizetype(0); i != size; ++i) {
		auto code = char32_t(text[i].unicode());
		if (QChar::isHighSurrogate(code)
			&& i + 1 != size
			&& text[i + 1].isLowSurrogate()) {
			code = QChar::surrogateToUcs4(text[i].unicode(), text[i + 1].unicode());
			++i;
		}
		switch (QChar::direction(code)) {
		case QChar::DirLRI:
		case QChar::DirRLI:
		case QChar::DirFSI:
			++isolates;
			break;
		case QChar::DirPDI:
			// An unmatched PDI is ignored.
			if (isolates > 0) {
				--isolates;
			}
			break;
		case QChar::DirL:
			if (!isolates) {
				return false;
			}
			break;
		case QChar::DirR:
		case QChar::DirAL:
			if (!isolates) {
				return true;
			}
			break;
		default: break;
		}
	}
	return std::nullopt;
}

} // namespace base
