// The widget-free arithmetic of the shared media page (media_shared.h):
// month headings and the columns of the grid. Unit-tested in
// tests/app/test_media_logic.cpp.
#pragma once

#include <QtCore/QDate>
#include <QtCore/QVector>

namespace Media {

// year × 100 + month in the calendar dates are shown in: the Jalali one
// for Persian, the Gregorian one otherwise. 0 for an invalid date.
[[nodiscard]] int SharedMonthKey(const QDate &date, bool jalali);

// The items (newest first) that start a month: the first one, and every one
// whose month differs from the item before it (a month met again out of
// order — a message dated oddly — starts again rather than merging).
[[nodiscard]] QVector<int> SharedMonthStarts(const QVector<int> &monthKeys);

// The columns of the grid in `innerWidth`: as many squares of at least
// `minTile` as fit (with `skip` between them), moved by the user's zoom
// (`zoom` > 0: fewer, bigger squares), within [minColumns, maxColumns].
[[nodiscard]] int SharedGridColumns(
	int innerWidth,
	int minTile,
	int skip,
	int minColumns,
	int maxColumns,
	int zoom);

// The side of a square for `columns` in `innerWidth`.
[[nodiscard]] int SharedTileSize(int innerWidth, int columns, int skip);

} // namespace Media
