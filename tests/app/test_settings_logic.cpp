// The widget-free parts of the settings (settings/settings_logic.h): section
// ids, provinces, the two-step verification form checks, wording.
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "settings/settings_logic.h"

#include <QtTest/QtTest>

class TestSettingsLogic : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}

private Q_SLOTS:
	void init() {
		Lang::SetCurrent(Lang::Language::Persian);
	}

	void sections() {
		const auto all = {
			Settings::Section::Account,
			Settings::Section::Privacy,
			Settings::Section::Notifications,
			Settings::Section::Messages,
			Settings::Section::Filters,
			Settings::Section::Region,
			Settings::Section::Appearance,
			Settings::Section::Devices,
			Settings::Section::Storage,
			Settings::Section::Update,
			Settings::Section::About,
		};
		auto ids = QSet<QString>();
		for (const auto section : all) {
			const auto id = Settings::SectionId(section);
			QVERIFY(!id.isEmpty());
			QVERIFY(!ids.contains(id));
			ids.insert(id);
			QCOMPARE(Settings::SectionFromId(id), std::optional(section));
			QVERIFY(!Settings::SectionTitle(section).isEmpty());
		}
		// The ids the web client uses.
		QCOMPARE(
			Settings::SectionId(Settings::Section::Filters),
			QStringLiteral("filters"));
		QVERIFY(!Settings::SectionFromId(QStringLiteral("nope")).has_value());
		QVERIFY(!Settings::SectionFromId(QString()).has_value());
	}

	void provinces() {
		const auto list = Settings::Provinces();
		QCOMPARE(list.size(), 31);
		QCOMPARE(list.front().code, QStringLiteral("IR-01"));
		QCOMPARE(list.back().code, QStringLiteral("IR-31"));
		QCOMPARE(Settings::ProvinceTitle(QStringLiteral("IR-07")), U("تهران"));
		QCOMPARE(Settings::ProvinceTitle(QStringLiteral("IR-31")), U("البرز"));
		QCOMPARE(
			Settings::ProvinceTitle(QStringLiteral("IR-99")),
			QStringLiteral("IR-99"));
		QVERIFY(Settings::ProvinceTitle(QString()).isEmpty());
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(
			Settings::ProvinceTitle(QStringLiteral("IR-07")),
			QStringLiteral("Tehran"));
		for (const auto &province : Settings::Provinces()) {
			// Nothing Persian in the English list.
			for (const auto ch : province.title) {
				QVERIFY(ch.unicode() < 0x0600);
			}
		}
	}

	void passwords() {
		using Settings::PasswordProblem;
		QCOMPARE(
			Settings::CheckNewPassword(QString(), QString(), 8),
			PasswordProblem::Empty);
		QCOMPARE(
			Settings::CheckNewPassword(
				QStringLiteral("short"),
				QStringLiteral("short"),
				8),
			PasswordProblem::TooShort);
		QCOMPARE(
			Settings::CheckNewPassword(
				QStringLiteral("long enough"),
				QStringLiteral("long enougH"),
				8),
			PasswordProblem::Mismatch);
		QCOMPARE(
			Settings::CheckNewPassword(
				QStringLiteral("long enough"),
				QStringLiteral("long enough"),
				8),
			PasswordProblem::None);
		QCOMPARE(
			Settings::CheckNewPassword(
				QString(257, QLatin1Char('a')),
				QString(257, QLatin1Char('a')),
				8),
			PasswordProblem::TooLong);
		// Characters, not UTF-16 units: eight emoji are eight characters.
		const auto emoji = U("😀😀😀😀😀😀😀😀");
		QCOMPARE(emoji.size(), 16);
		QCOMPARE(
			Settings::CheckNewPassword(emoji, emoji, 8),
			PasswordProblem::None);
		QCOMPARE(
			Settings::CheckNewPassword(emoji.left(14), emoji.left(14), 8),
			PasswordProblem::TooShort);
	}

	void normalizedPassword() {
		// "é" typed as e + a combining accent is sent precomposed.
		const auto decomposed = U("caf\x65\xCC\x81");
		const auto composed = U("caf\xC3\xA9");
		QVERIFY(decomposed != composed);
		QCOMPARE(Settings::NormalizePassword(decomposed), composed);
		QCOMPARE(Settings::NormalizePassword(composed), composed);
	}

	void hints() {
		const auto password = QStringLiteral("Secret-12");
		QVERIFY(Settings::HintAllowed(QString(), password));
		QVERIFY(Settings::HintAllowed(U("اسم اولین معلم"), password));
		QVERIFY(!Settings::HintAllowed(U("رمز: secret-12"), password));
		QVERIFY(!Settings::HintAllowed(QStringLiteral("two\nlines"), password));
		QVERIFY(Settings::HintAllowed(QString(64, QLatin1Char('x')), password));
		QVERIFY(!Settings::HintAllowed(QString(65, QLatin1Char('x')), password));
	}

	void emails() {
		QVERIFY(Settings::EmailLooksValid(QStringLiteral("hadi@example.com")));
		QVERIFY(Settings::EmailLooksValid(
			QStringLiteral("first.last+tag@mail.example.co")));
		QVERIFY(!Settings::EmailLooksValid(QString()));
		QVERIFY(!Settings::EmailLooksValid(QStringLiteral("hadi")));
		QVERIFY(!Settings::EmailLooksValid(QStringLiteral("hadi@example")));
		QVERIFY(!Settings::EmailLooksValid(QStringLiteral("hadi@@example.com")));
		QVERIFY(!Settings::EmailLooksValid(QStringLiteral("ha di@example.com")));
		QVERIFY(!Settings::EmailLooksValid(
			QStringLiteral("Hadi <hadi@example.com>")));
		QVERIFY(!Settings::EmailLooksValid(U("هادی@example.com")));
		QVERIFY(!Settings::EmailLooksValid(
			QString(250, QLatin1Char('a')) + QStringLiteral("@example.com")));
	}

	void errorFields() {
		using Settings::TwoStepField;
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("two_step.password_invalid")),
			TwoStepField::Current);
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("two_step.password_weak")),
			TwoStepField::Password);
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("two_step.hint_invalid")),
			TwoStepField::Hint);
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("two_step.email_invalid")),
			TwoStepField::Email);
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("two_step.code_expired")),
			TwoStepField::Code);
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("account.sensitive_cooldown")),
			TwoStepField::None);
		QCOMPARE(
			Settings::FieldForError(QStringLiteral("rate_limited")),
			TwoStepField::None);
	}

	void durations() {
		QCOMPARE(Settings::DurationTitle(QStringLiteral("1h")), U("۱ ساعت"));
		QCOMPARE(Settings::DurationTitle(QStringLiteral("6h")), U("۶ ساعت"));
		QCOMPARE(Settings::DurationTitle(QStringLiteral("24h")), U("۱ روز"));
		QCOMPARE(Settings::DurationTitle(QStringLiteral("30m")), U("۳۰ دقیقه"));
		QCOMPARE(
			Settings::DurationTitle(QStringLiteral("soon")),
			QStringLiteral("soon"));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(
			Settings::DurationTitle(QStringLiteral("1h")),
			QStringLiteral("1 hour"));
		QCOMPARE(
			Settings::DurationTitle(QStringLiteral("48h")),
			QStringLiteral("2 days"));
	}

	void exportStates() {
		QCOMPARE(
			Settings::ExportStateText(QStringLiteral("ready")),
			tr::lng_settings_export_ready(tr::now));
		QCOMPARE(
			Settings::ExportStateText(QStringLiteral("pending")),
			tr::lng_settings_export_preparing(tr::now));
		QCOMPARE(
			Settings::ExportStateText(QStringLiteral("running")),
			tr::lng_settings_export_preparing(tr::now));
		QCOMPARE(
			Settings::ExportStateText(QStringLiteral("failed")),
			tr::lng_settings_export_failed(tr::now));
	}

	void sessionSubtitle() {
		Lang::SetCurrent(Lang::Language::English);
		const auto now = QDateTime(
			QDate(2026, 10, 10),
			QTime(12, 0)).toMSecsSinceEpoch();
		auto session = Data::DeviceSession();
		session.platform = QStringLiteral("ios");
		session.appVersion = QStringLiteral("1.4.0");
		session.city = QStringLiteral("Tehran");
		session.lastActiveAt = now - 60 * 1000;
		// Every part is isolated, so compare without the bidi marks.
		const auto plain = [](QString text) {
			return text.remove(QChar(0x2068)).remove(QChar(0x2069));
		};
		QCOMPARE(
			plain(Settings::SessionSubtitle(session, now)),
			U("iOS 1.4.0 · Tehran · online"));
		session.lastActiveAt = now - 3 * 3600 * 1000;
		QCOMPARE(
			plain(Settings::SessionSubtitle(session, now)),
			U("iOS 1.4.0 · Tehran · 09:00"));
		session.current = true;
		QVERIFY(plain(Settings::SessionSubtitle(session, now)).endsWith(
			QStringLiteral("online")));
		auto bare = Data::DeviceSession();
		bare.platform = QStringLiteral("web");
		QCOMPARE(plain(Settings::SessionSubtitle(bare, now)), QStringLiteral("Web"));
	}

	void cacheLimits() {
		const auto steps = Settings::CacheLimitSteps();
		QVERIFY(steps.size() >= 4);
		QVERIFY(std::is_sorted(steps.begin(), steps.end()));
		const auto mb = qint64(1) << 20;
		QCOMPARE(steps[Settings::CacheLimitIndex(512 * mb)], 512 * mb);
		QCOMPARE(Settings::CacheLimitIndex(0), 0);
		QCOMPARE(
			Settings::CacheLimitIndex(qint64(1) << 40),
			int(steps.size()) - 1);
		// The nearest step.
		QCOMPARE(steps[Settings::CacheLimitIndex(300 * mb)], 256 * mb);
	}

};

QTEST_GUILESS_MAIN(TestSettingsLogic)
#include "test_settings_logic.moc"
