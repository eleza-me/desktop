#include "media/media_shared_logic.h"

#include "lang/lang_format.h"

#include <algorithm>

namespace Media {

int SharedMonthKey(const QDate &date, bool jalali) {
	if (!date.isValid()) {
		return 0;
	} else if (jalali) {
		const auto converted = Lang::ToJalali(date);
		return converted.year * 100 + converted.month;
	}
	return date.year() * 100 + date.month();
}

QVector<int> SharedMonthStarts(const QVector<int> &monthKeys) {
	auto result = QVector<int>();
	for (auto i = 0; i != int(monthKeys.size()); ++i) {
		if (i == 0 || monthKeys[i] != monthKeys[i - 1]) {
			result.push_back(i);
		}
	}
	return result;
}

int SharedGridColumns(
		int innerWidth,
		int minTile,
		int skip,
		int minColumns,
		int maxColumns,
		int zoom) {
	minColumns = std::max(minColumns, 1);
	maxColumns = std::max(maxColumns, minColumns);
	const auto fit = std::max(
		(std::max(innerWidth, 1) + skip) / std::max(minTile + skip, 1),
		1);
	const auto base = std::clamp(fit, minColumns, maxColumns);
	return std::clamp(base - zoom, minColumns, maxColumns);
}

int SharedTileSize(int innerWidth, int columns, int skip) {
	columns = std::max(columns, 1);
	return std::max((innerWidth - (columns - 1) * skip) / columns, 1);
}

} // namespace Media
