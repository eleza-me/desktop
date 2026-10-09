#include "search/search_recent.h"

#include <QtCore/QJsonArray>

#include <algorithm>

namespace Search {

RecentList::RecentList(int limit)
: _limit(std::max(limit, 1)) {
}

QString RecentList::Normalize(const QString &query) {
	auto result = query.simplified();
	if (result.size() > kMaxLength) {
		result = result.left(kMaxLength).trimmed();
	}
	if (result == u"@" || result == u"#") {
		return QString();
	}
	return result;
}

int RecentList::indexOf(const QString &normalized) const {
	for (auto i = 0; i != int(_list.size()); ++i) {
		if (_list[i].compare(normalized, Qt::CaseInsensitive) == 0) {
			return i;
		}
	}
	return -1;
}

bool RecentList::add(const QString &query) {
	const auto normalized = Normalize(query);
	if (normalized.isEmpty()) {
		return false;
	}
	const auto index = indexOf(normalized);
	if (index == 0 && _list.front() == normalized) {
		return false;
	} else if (index >= 0) {
		_list.removeAt(index);
	}
	_list.push_front(normalized);
	while (_list.size() > _limit) {
		_list.removeLast();
	}
	return true;
}

bool RecentList::remove(const QString &query) {
	const auto index = indexOf(Normalize(query));
	if (index < 0) {
		return false;
	}
	_list.removeAt(index);
	return true;
}

bool RecentList::clear() {
	if (_list.isEmpty()) {
		return false;
	}
	_list.clear();
	return true;
}

QJsonValue RecentList::toJson() const {
	return QJsonArray::fromStringList(_list);
}

void RecentList::fromJson(const QJsonValue &value) {
	_list.clear();
	for (const auto &item : value.toArray()) {
		const auto normalized = Normalize(item.toString());
		if (!normalized.isEmpty()
			&& indexOf(normalized) < 0
			&& _list.size() < _limit) {
			_list.push_back(normalized);
		}
	}
}

} // namespace Search
