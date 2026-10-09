// Conversation time math: the expiry ring, the «N ساعت مانده» captions, the
// radar clock, the duration choices and the topic check.
#include "conversation/conversation_time.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"

#include <QtTest/QtTest>

using namespace Conversation;

class TestConversationTime : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}
	// A fixed "now": 1 000 000 s after the epoch.
	static constexpr auto kNowSec = qint64(1'000'000);
	static constexpr auto kNowMs = kNowSec * 1000;

private Q_SLOTS:
	void init() {
		Lang::SetCurrent(Lang::Language::Persian);
	}

	void remainingParts() {
		using Unit = Remaining::Unit;
		QCOMPARE(RemainingParts(kNowSec, kNowMs), (Remaining{ Unit::Ended, 0 }));
		QCOMPARE(RemainingParts(kNowSec - 5, kNowMs), (Remaining{ Unit::Ended, 0 }));
		// Minutes round up: one second left is still «۱ دقیقه».
		QCOMPARE(RemainingParts(kNowSec + 1, kNowMs), (Remaining{ Unit::Minutes, 1 }));
		QCOMPARE(RemainingParts(kNowSec + 60, kNowMs), (Remaining{ Unit::Minutes, 1 }));
		QCOMPARE(RemainingParts(kNowSec + 61, kNowMs), (Remaining{ Unit::Minutes, 2 }));
		QCOMPARE(RemainingParts(kNowSec + 59 * 60, kNowMs), (Remaining{ Unit::Minutes, 59 }));
		// Hours round down from 60 minutes on.
		QCOMPARE(RemainingParts(kNowSec + 59 * 60 + 1, kNowMs), (Remaining{ Unit::Hours, 1 }));
		QCOMPARE(RemainingParts(kNowSec + 3600, kNowMs), (Remaining{ Unit::Hours, 1 }));
		QCOMPARE(RemainingParts(kNowSec + 2 * 3600 - 1, kNowMs), (Remaining{ Unit::Hours, 2 }));
		QCOMPARE(RemainingParts(kNowSec + 23 * 3600, kNowMs), (Remaining{ Unit::Hours, 23 }));
		QCOMPARE(RemainingParts(kNowSec + 47 * 3600, kNowMs), (Remaining{ Unit::Hours, 47 }));
		// Days only from two whole days.
		QCOMPARE(RemainingParts(kNowSec + 48 * 3600, kNowMs), (Remaining{ Unit::Days, 2 }));
		QCOMPARE(RemainingParts(kNowSec + 100 * 3600, kNowMs), (Remaining{ Unit::Days, 4 }));
	}

	void remainingText() {
		QCOMPARE(RemainingText(kNowSec - 1, kNowMs), tr::lng_ended(tr::now));
		QCOMPARE(RemainingText(kNowSec + 45 * 60, kNowMs), U("۴۵ دقیقه مانده"));
		QCOMPARE(RemainingText(kNowSec + 23 * 3600, kNowMs), U("۲۳ ساعت مانده"));
		QCOMPARE(RemainingText(kNowSec + 72 * 3600, kNowMs), U("۳ روز مانده"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(RemainingText(kNowSec + 45 * 60, kNowMs), QStringLiteral("45m left"));
		QCOMPARE(RemainingText(kNowSec + 23 * 3600, kNowMs), QStringLiteral("23h left"));
	}

	void clock() {
		QCOMPARE(ClockRemaining(kNowSec + 4 * 3600 + 59 * 60 + 30, kNowMs), U("۰۴:۵۹ مانده"));
		QCOMPARE(ClockRemaining(kNowSec + 59, kNowMs), U("۰۰:۰۰ مانده"));
		QCOMPARE(ClockRemaining(kNowSec - 100, kNowMs), U("۰۰:۰۰ مانده"));
		QCOMPARE(ClockRemaining(kNowSec + 30 * 3600, kNowMs), U("۳۰:۰۰ مانده"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(ClockRemaining(kNowSec + 3600 + 5 * 60, kNowMs), QStringLiteral("01:05 left"));
	}

	void longRemaining() {
		QCOMPARE(LongRemaining(kNowSec + 4 * 3600 + 59 * 60, kNowMs), U("۴ ساعت و ۵۹ دقیقه"));
		QCOMPARE(LongRemaining(kNowSec + 59 * 60, kNowMs), U("۵۹ دقیقه"));
		QCOMPARE(LongRemaining(kNowSec - 1, kNowMs), U("۰ دقیقه"));
	}

	void ring() {
		const auto created = kNowSec - 6 * 3600;
		const auto expires = created + 24 * 3600;
		QCOMPARE(ExpiryFraction(expires, created, kNowMs), 0.75);
		QCOMPARE(ExpiryFraction(expires, created, created * 1000), 1.);
		QCOMPARE(ExpiryFraction(expires, created, expires * 1000), 0.);
		// Clamped on both sides.
		QCOMPARE(ExpiryFraction(expires, created, (created - 500) * 1000), 1.);
		QCOMPARE(ExpiryFraction(expires, created, (expires + 500) * 1000), 0.);
		// A broken window does not divide by zero.
		QCOMPARE(ExpiryFraction(kNowSec + 10, kNowSec + 10, kNowMs), 1.);

		// The open list has no creation time: the ring is drawn against a day.
		const auto fiveHours = kNowSec + 5 * 3600;
		const auto fraction = ExpiryFraction(
			fiveHours,
			fiveHours - kDefaultWindowSeconds,
			kNowMs);
		QVERIFY(qAbs(fraction - 5. / 24.) < 1e-9);
	}

	void finalHour() {
		QVERIFY(!ExpiryFinal(kNowSec + 3601, kNowMs));
		QVERIFY(ExpiryFinal(kNowSec + 3600, kNowMs));
		QVERIFY(ExpiryFinal(kNowSec + 1, kNowMs));
		QVERIFY(!ExpiryFinal(kNowSec, kNowMs));
		QVERIFY(!ExpiryFinal(kNowSec - 1, kNowMs));
		QVERIFY(Expired(kNowSec, kNowMs));
		QVERIFY(!Expired(kNowSec + 1, kNowMs));
	}

	void nextTick() {
		// Nothing on screen (or everything expired): the slowest tick.
		QCOMPARE(NextTickDelay({}, kNowMs), 30'000);
		QCOMPARE(NextTickDelay({ kNowSec - 10 }, kNowMs), 30'000);
		// 10 min 20 s left -> the caption changes in 20 s.
		QCOMPARE(NextTickDelay({ kNowSec + 620 }, kNowMs), 20'000);
		// The soonest card decides.
		QCOMPARE(NextTickDelay({ kNowSec + 620, kNowSec + 3605 }, kNowMs), 5'000);
		// Never faster than a second, never slower than half a minute.
		QCOMPARE(NextTickDelay({ kNowSec + 60 }, kNowMs + 59'800), 1'000);
		QCOMPARE(NextTickDelay({ kNowSec + 600 }, kNowMs), 30'000);
		// A whole number of minutes left: the caption holds for a minute.
		QCOMPARE(NextTickDelay({ kNowSec + 600 }, kNowMs + 1), 30'000);
		QCOMPARE(NextTickDelay({ kNowSec + 45 }, kNowMs), 30'000);
		QCOMPARE(NextTickDelay({ kNowSec + 7 }, kNowMs), 7'000);
	}

	void durations() {
		QCOMPARE(DurationTitle(QStringLiteral("24h")), U("۱ روز"));
		QCOMPARE(DurationTitle(QStringLiteral("48h")), U("۲ روز"));
		QCOMPARE(DurationTitle(QStringLiteral("6h")), U("۶ ساعت"));
		QCOMPARE(DurationTitle(QStringLiteral("36h")), U("۳۶ ساعت"));
		QCOMPARE(DurationTitle(QStringLiteral("30m")), U("۳۰ دقیقه"));
		QCOMPARE(DurationTitle(QStringLiteral("3d")), U("۳ روز"));
		QCOMPARE(DurationTitle(QStringLiteral("soon")), QStringLiteral("soon"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(DurationTitle(QStringLiteral("24h")), QStringLiteral("1 day"));
		QCOMPARE(DurationTitle(QStringLiteral("3d")), QStringLiteral("3 days"));

		const auto sorted = SortedDurations({
			QStringLiteral("1h"),
			QStringLiteral("6h"),
			QStringLiteral("24h"),
			QStringLiteral("30m"),
			QStringLiteral("6h"),
		});
		QCOMPARE(sorted, (QStringList{
			QStringLiteral("24h"),
			QStringLiteral("6h"),
			QStringLiteral("1h"),
			QStringLiteral("30m"),
		}));
		QCOMPARE(DefaultDuration(sorted, QStringLiteral("6h"), QStringLiteral("24h")), QStringLiteral("6h"));
		// A preference that is not offered falls back to the longest choice.
		QCOMPARE(DefaultDuration(sorted, QStringLiteral("12h"), QStringLiteral("1h")), QStringLiteral("24h"));
		QCOMPARE(DefaultDuration(sorted, QString(), QStringLiteral("1h")), QStringLiteral("24h"));
		QCOMPARE(DefaultDuration({}, QString(), QStringLiteral("1h")), QStringLiteral("1h"));
	}

	void topic() {
		QCOMPARE(NormalizedTopic(U("  سلام دنیا \n")), U("سلام دنیا"));
		QCOMPARE(CheckTopic(QString(), 120), TopicProblem::Empty);
		QCOMPARE(CheckTopic(QStringLiteral("   \n "), 120), TopicProblem::Empty);
		QCOMPARE(CheckTopic(QStringLiteral("ok"), 120), TopicProblem::None);
		QCOMPARE(CheckTopic(QString(120, QLatin1Char('a')), 120), TopicProblem::None);
		QCOMPARE(CheckTopic(QString(121, QLatin1Char('a')), 120), TopicProblem::TooLong);
		// Surrounding spaces do not count.
		QCOMPARE(CheckTopic(QStringLiteral("  ") + QString(120, QLatin1Char('a')) + QStringLiteral("  "), 120), TopicProblem::None);
		// No limit configured.
		QCOMPARE(CheckTopic(QString(500, QLatin1Char('a')), 0), TopicProblem::None);
	}

};

QTEST_GUILESS_MAIN(TestConversationTime)
#include "test_conversation_time.moc"
