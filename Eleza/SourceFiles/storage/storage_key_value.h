// A small JSON key-value file for things that exist before / outside a
// signed-in account's database: the device id, the cached realtime keys,
// the cached client-config, UI preferences.
//
// Reads are in-memory. Writes are coalesced and saved atomically on the next
// event loop turn (and on destruction / flush()).
#pragma once

#include "base/timer.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace Storage {

class KeyValue final : public QObject {
public:
	explicit KeyValue(const QString &path, QObject *parent = nullptr);
	~KeyValue();

	[[nodiscard]] bool contains(const QString &key) const;
	[[nodiscard]] QJsonValue value(const QString &key) const;
	[[nodiscard]] QString string(const QString &key, const QString &fallback = QString()) const;
	[[nodiscard]] qint64 integer(const QString &key, qint64 fallback = 0) const;
	[[nodiscard]] bool boolean(const QString &key, bool fallback = false) const;
	[[nodiscard]] QJsonObject object(const QString &key) const;

	void set(const QString &key, const QJsonValue &value);
	void remove(const QString &key);
	// Removes every key that starts with `prefix`.
	void removePrefix(const QString &prefix);
	// Removes every key that ends with `suffix` ("<name>.<user id>").
	void removeSuffix(const QString &suffix);

	// Writes pending changes now.
	void flush();

private:
	void scheduleSave();

	QString _path;
	QJsonObject _data;
	base::Timer _saveTimer;
	bool _dirty = false;

};

} // namespace Storage
