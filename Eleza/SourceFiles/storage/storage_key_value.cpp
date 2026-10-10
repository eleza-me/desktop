#include "storage/storage_key_value.h"

#include "base/platform_file.h"

#include <QtCore/QJsonDocument>

namespace Storage {

KeyValue::KeyValue(const QString &path, QObject *parent)
: QObject(parent)
, _path(path)
, _saveTimer([this] { flush(); }) {
	if (const auto bytes = base::platform::ReadFile(_path)) {
		_data = QJsonDocument::fromJson(*bytes).object();
	}
}

KeyValue::~KeyValue() {
	flush();
}

bool KeyValue::contains(const QString &key) const {
	return _data.contains(key);
}

QJsonValue KeyValue::value(const QString &key) const {
	return _data.value(key);
}

QString KeyValue::string(const QString &key, const QString &fallback) const {
	const auto found = _data.value(key);
	return found.isString() ? found.toString() : fallback;
}

qint64 KeyValue::integer(const QString &key, qint64 fallback) const {
	const auto found = _data.value(key);
	return found.isDouble() ? base::SafeInt64(found.toDouble()) : fallback;
}

bool KeyValue::boolean(const QString &key, bool fallback) const {
	const auto found = _data.value(key);
	return found.isBool() ? found.toBool() : fallback;
}

QJsonObject KeyValue::object(const QString &key) const {
	return _data.value(key).toObject();
}

void KeyValue::set(const QString &key, const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) {
		remove(key);
		return;
	} else if (_data.value(key) == value) {
		return;
	}
	_data.insert(key, value);
	scheduleSave();
}

void KeyValue::remove(const QString &key) {
	if (_data.contains(key)) {
		_data.remove(key);
		scheduleSave();
	}
}

void KeyValue::removePrefix(const QString &prefix) {
	auto changed = false;
	for (const auto &key : _data.keys()) {
		if (key.startsWith(prefix)) {
			_data.remove(key);
			changed = true;
		}
	}
	if (changed) {
		scheduleSave();
	}
}

void KeyValue::removeSuffix(const QString &suffix) {
	auto changed = false;
	for (const auto &key : _data.keys()) {
		if (key.endsWith(suffix)) {
			_data.remove(key);
			changed = true;
		}
	}
	if (changed) {
		scheduleSave();
	}
}

void KeyValue::scheduleSave() {
	_dirty = true;
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(0);
	}
}

void KeyValue::flush() {
	_saveTimer.cancel();
	if (!_dirty) {
		return;
	}
	_dirty = false;
	const auto bytes = QJsonDocument(_data).toJson(QJsonDocument::Compact);
	if (!base::platform::WriteFileAtomic(_path, bytes, true)) {
		// Not the path: it has the name of the user's home folder in it.
		qWarning("Storage: could not save a settings file.");
	}
}

} // namespace Storage
