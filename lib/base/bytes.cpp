#include "base/bytes.h"

namespace base::bytes {

QByteArray concat(std::initializer_list<QByteArray> parts) {
	auto size = qsizetype(0);
	for (const auto &part : parts) {
		size += part.size();
	}
	auto result = QByteArray();
	result.reserve(size);
	for (const auto &part : parts) {
		result.append(part);
	}
	return result;
}

QString toHex(const QByteArray &bytes) {
	return QString::fromLatin1(bytes.toHex());
}

QByteArray fromHex(const QString &hex) {
	const auto text = hex.trimmed().toLatin1();
	if (text.size() % 2) {
		return QByteArray();
	}
	for (const auto ch : text) {
		const auto ok = (ch >= '0' && ch <= '9')
			|| (ch >= 'a' && ch <= 'f')
			|| (ch >= 'A' && ch <= 'F');
		if (!ok) {
			return QByteArray();
		}
	}
	auto result = QByteArray::fromHex(text);
	if (result.isNull()) {
		result = QByteArray("", 0);
	}
	return result;
}

QString toBase64(const QByteArray &bytes) {
	return QString::fromLatin1(bytes.toBase64());
}

QString toBase64Url(const QByteArray &bytes) {
	return QString::fromLatin1(bytes.toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray fromBase64(const QString &text) {
	auto clean = QByteArray();
	clean.reserve(text.size());
	for (const auto ch : text) {
		const auto c = ch.unicode();
		if (c == '-') {
			clean.append('+');
		} else if (c == '_') {
			clean.append('/');
		} else if ((c >= 'A' && c <= 'Z')
			|| (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9')
			|| c == '+'
			|| c == '/') {
			clean.append(char(c));
		}
	}
	return QByteArray::fromBase64(clean, QByteArray::Base64Encoding);
}

void wipe(QByteArray &bytes) {
	if (!bytes.isEmpty()) {
		volatile char *data = bytes.data();
		for (auto i = qsizetype(0), count = bytes.size(); i != count; ++i) {
			data[i] = 0;
		}
	}
	bytes.clear();
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
	if (a.size() != b.size()) {
		return false;
	}
	auto diff = uchar(0);
	for (auto i = qsizetype(0), count = a.size(); i != count; ++i) {
		diff |= uchar(a[i]) ^ uchar(b[i]);
	}
	return (diff == 0);
}

} // namespace base::bytes
