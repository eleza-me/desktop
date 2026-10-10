#include "data/data_json.h"

#include <cmath>

namespace Data::Json {

QString String(const QJsonObject &object, QLatin1StringView key) {
	const auto value = object.value(key);
	if (value.isString()) {
		return value.toString();
	} else if (value.isDouble()) {
		const auto number = value.toDouble();
		return (number == std::trunc(number))
			? QString::number(base::SafeInt64(number))
			: QString::number(number);
	}
	return QString();
}

QString CleanName(const QString &name) {
	const auto invisible = [](char16_t code) {
		switch (code) {
		case 0x200B: case 0xFEFF: case 0x200E: case 0x200F:
		case 0x202A: case 0x202B: case 0x202C: case 0x202D: case 0x202E:
		case 0x2066: case 0x2067: case 0x2068: case 0x2069: case 0x00AD:
		case 0x061C: case 0x2028: case 0x2029: case 0x2060:
			return true;
		}
		return false;
	};
	auto clean = true;
	for (const auto ch : name) {
		if (invisible(ch.unicode()) || ch.category() == QChar::Other_Control) {
			clean = false;
			break;
		}
	}
	if (clean) {
		return name;
	}
	auto result = QString();
	result.reserve(name.size());
	for (const auto ch : name) {
		if (ch.category() == QChar::Other_Control) {
			result.append(u' ');
		} else if (!invisible(ch.unicode())) {
			result.append(ch);
		}
	}
	return result.simplified();
}

QString Name(const QJsonObject &object, QLatin1StringView key) {
	return CleanName(String(object, key));
}

qint64 Int64(const QJsonValue &value, qint64 fallback) {
	if (value.isDouble()) {
		return base::SafeInt64(value.toDouble());
	} else if (value.isString()) {
		auto ok = false;
		const auto text = value.toString();
		const auto result = text.toLongLong(&ok);
		if (ok) {
			return result;
		}
		const auto real = text.toDouble(&ok);
		return ok ? base::SafeInt64(real) : fallback;
	} else if (value.isBool()) {
		return value.toBool() ? 1 : 0;
	}
	return fallback;
}

qint64 Int64(const QJsonObject &object, QLatin1StringView key, qint64 fallback) {
	return Int64(object.value(key), fallback);
}

int Int(const QJsonObject &object, QLatin1StringView key, int fallback) {
	return base::SafeInt(Int64(object.value(key), fallback));
}

double Double(const QJsonObject &object, QLatin1StringView key, double fallback) {
	const auto value = object.value(key);
	if (value.isDouble()) {
		return value.toDouble();
	} else if (value.isString()) {
		auto ok = false;
		const auto result = value.toString().toDouble(&ok);
		return ok ? result : fallback;
	}
	return fallback;
}

bool Bool(const QJsonObject &object, QLatin1StringView key, bool fallback) {
	const auto value = object.value(key);
	if (value.isBool()) {
		return value.toBool();
	} else if (value.isDouble()) {
		return value.toDouble() != 0.;
	} else if (value.isString()) {
		const auto text = value.toString();
		return (text == u"true" || text == u"1");
	}
	return fallback;
}

bool Has(const QJsonObject &object, QLatin1StringView key) {
	const auto value = object.value(key);
	return !value.isUndefined() && !value.isNull();
}

QStringList Strings(const QJsonValue &value) {
	auto result = QStringList();
	for (const auto &item : value.toArray()) {
		if (item.isString()) {
			result.push_back(item.toString());
		}
	}
	return result;
}

QVector<qint64> Int64s(const QJsonValue &value) {
	auto result = QVector<qint64>();
	for (const auto &item : value.toArray()) {
		result.push_back(Int64(item));
	}
	return result;
}

QMap<QString, qint64> CountMap(const QJsonValue &value) {
	auto result = QMap<QString, qint64>();
	const auto object = value.toObject();
	for (auto i = object.begin(); i != object.end(); ++i) {
		result.insert(i.key(), Int64(i.value()));
	}
	return result;
}

QMap<QString, QString> StringMap(const QJsonValue &value) {
	auto result = QMap<QString, QString>();
	const auto object = value.toObject();
	for (auto i = object.begin(); i != object.end(); ++i) {
		result.insert(i.key(), i.value().toString());
	}
	return result;
}

qint64 ToMs(qint64 value) {
	return (value > 0 && value < 100000000000LL) ? value * 1000 : value;
}

void Put(QJsonObject &object, QLatin1StringView key, const QString &value) {
	if (!value.isEmpty()) {
		object.insert(key, value);
	}
}

void Put(QJsonObject &object, QLatin1StringView key, qint64 value) {
	if (value != 0) {
		object.insert(key, double(value));
	}
}

void Put(QJsonObject &object, QLatin1StringView key, int value) {
	if (value != 0) {
		object.insert(key, value);
	}
}

void Put(QJsonObject &object, QLatin1StringView key, double value) {
	if (value != 0.) {
		object.insert(key, value);
	}
}

void Put(QJsonObject &object, QLatin1StringView key, bool value) {
	if (value) {
		object.insert(key, true);
	}
}

void Put(QJsonObject &object, QLatin1StringView key, const QJsonObject &value) {
	if (!value.isEmpty()) {
		object.insert(key, value);
	}
}

void Put(QJsonObject &object, QLatin1StringView key, const QJsonArray &value) {
	if (!value.isEmpty()) {
		object.insert(key, value);
	}
}

QJsonArray Array(const QStringList &values) {
	auto result = QJsonArray();
	for (const auto &value : values) {
		result.push_back(value);
	}
	return result;
}

QJsonArray Array(const QVector<qint64> &values) {
	auto result = QJsonArray();
	for (const auto value : values) {
		result.push_back(double(value));
	}
	return result;
}

QJsonObject Object(const QMap<QString, qint64> &values) {
	auto result = QJsonObject();
	for (auto i = values.begin(); i != values.end(); ++i) {
		result.insert(i.key(), double(i.value()));
	}
	return result;
}

QJsonObject Object(const QMap<QString, QString> &values) {
	auto result = QJsonObject();
	for (auto i = values.begin(); i != values.end(); ++i) {
		result.insert(i.key(), i.value());
	}
	return result;
}

} // namespace Data::Json
