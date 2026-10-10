// Recent searches: the last queries the user ran, newest first, kept per
// account on this device. No widgets here (the unit tests compile this file
// alone).
//
//   Search::RecentList recent;
//   recent.fromJson(stored);
//   if (recent.add(query)) { save(recent.toJson()); }
#pragma once

#include <QtCore/QJsonValue>
#include <QtCore/QStringList>

namespace Search {

class RecentList final {
public:
	static constexpr auto kLimit = 20;
	static constexpr auto kMaxLength = 64;

	explicit RecentList(int limit = kLimit);

	// Whitespace collapsed and trimmed, cut to kMaxLength; empty for a text
	// that is not worth remembering (nothing, or a lone "@" / "#").
	[[nodiscard]] static QString Normalize(const QString &query);

	// Puts the query first (moving an equal one up; "Sara" equals "sara").
	// Returns true if the list changed.
	bool add(const QString &query);
	bool remove(const QString &query);
	bool clear();

	[[nodiscard]] const QStringList &list() const {
		return _list;
	}
	[[nodiscard]] bool empty() const {
		return _list.isEmpty();
	}

	[[nodiscard]] QJsonValue toJson() const;
	// Replaces the list with a stored one (garbage and duplicates are dropped).
	void fromJson(const QJsonValue &value);

private:
	[[nodiscard]] int indexOf(const QString &normalized) const;

	QStringList _list;
	int _limit = kLimit;

};

} // namespace Search
