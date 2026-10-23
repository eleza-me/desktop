// The widget-free rules of the media screens: month headings and the grid of
// the shared media page (media/media_shared_logic.h).
#include "media/media_shared_logic.h"

#include <QtTest/QtTest>

using namespace Media;

class TestMediaLogic final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void monthKeys() {
		QCOMPARE(SharedMonthKey(QDate(2026, 10, 22), false), 202610);
		// 30 Mehr 1405.
		QCOMPARE(SharedMonthKey(QDate(2026, 10, 22), true), 140507);
		// 1 Aban 1405: another month in the Jalali calendar, the same
		// Gregorian one.
		QCOMPARE(SharedMonthKey(QDate(2026, 10, 23), true), 140508);
		QCOMPARE(SharedMonthKey(QDate(2026, 10, 23), false), 202610);
		QCOMPARE(SharedMonthKey(QDate(), true), 0);
	}

	void monthStarts() {
		QCOMPARE(SharedMonthStarts({}), QVector<int>());
		QCOMPARE(SharedMonthStarts({ 5 }), QVector<int>({ 0 }));
		QCOMPARE(
			SharedMonthStarts({ 202610, 202610, 202609, 202607, 202607 }),
			QVector<int>({ 0, 2, 3 }));
		// A month met again out of order starts again.
		QCOMPARE(
			SharedMonthStarts({ 202610, 202609, 202610 }),
			QVector<int>({ 0, 1, 2 }));
	}

	void gridColumns() {
		// As many squares of at least 110 as fit, between 2 and 6.
		QCOMPARE(SharedGridColumns(696, 110, 3, 2, 6, 0), 6);
		QCOMPARE(SharedGridColumns(400, 110, 3, 2, 6, 0), 3);
		QCOMPARE(SharedGridColumns(100, 110, 3, 2, 6, 0), 2);
		QCOMPARE(SharedGridColumns(5000, 110, 3, 2, 6, 0), 6);
		// The zoom: fewer, bigger squares, never outside the bounds.
		QCOMPARE(SharedGridColumns(400, 110, 3, 2, 6, 1), 2);
		QCOMPARE(SharedGridColumns(400, 110, 3, 2, 6, 5), 2);
		QCOMPARE(SharedGridColumns(400, 110, 3, 2, 6, -2), 5);
		QCOMPARE(SharedGridColumns(400, 110, 3, 2, 6, -9), 6);
		QCOMPARE(SharedTileSize(450, 3, 3), 148);
		QCOMPARE(SharedTileSize(10, 6, 3), 1);
	}
};

QTEST_GUILESS_MAIN(TestMediaLogic)
#include "test_media_logic.moc"
