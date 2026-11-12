// The preferences file of this computer (core/core_settings.h): the interface
// scale and «کاهش حرکت», and that the launcher can read the scale alone.
#include "core/core_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

class TestCoreSettings : public QObject {
	Q_OBJECT

private:
	[[nodiscard]] static QJsonObject ReadJson(const QString &path) {
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			return {};
		}
		return QJsonDocument::fromJson(file.readAll()).object();
	}
	static void WriteJson(const QString &path, const QJsonObject &object) {
		auto file = QFile(path);
		QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		file.write(QJsonDocument(object).toJson());
	}

private Q_SLOTS:
	void uiScale() {
		QCOMPARE(Core::Settings::NormalizeUiScale(90), 100);
		QCOMPARE(Core::Settings::NormalizeUiScale(125), 125);
		QCOMPARE(Core::Settings::NormalizeUiScale(400), 150);

		auto dir = QTemporaryDir();
		QVERIFY(dir.isValid());
		const auto path = QDir(dir.path()).filePath(u"settings.json"_qs);
		// No file, no key: the system's own scale.
		QCOMPARE(Core::Settings::ReadUiScale(path), 100);
		QCOMPARE(Core::Settings::ReadUiScale(QString()), 100);
		{
			auto settings = Core::Settings(path);
			QCOMPARE(settings.uiScale(), 100);
			settings.setUiScale(125);
			QCOMPARE(settings.uiScale(), 125);
			settings.flush();
		}
		QCOMPARE(ReadJson(path).value(u"ui_scale"_qs).toInt(), 125);
		QCOMPARE(Core::Settings::ReadUiScale(path), 125);
		{
			auto settings = Core::Settings(path);
			QCOMPARE(settings.uiScale(), 125);
			settings.setUiScale(1000);
			QCOMPARE(settings.uiScale(), 150);
		}
		// A value written by hand is kept inside the range.
		auto object = ReadJson(path);
		object.insert(u"ui_scale"_qs, 20);
		WriteJson(path, object);
		QCOMPARE(Core::Settings::ReadUiScale(path), 100);
		WriteJson(path, QJsonObject{ { u"ui_scale"_qs, u"big"_qs } });
		QCOMPARE(Core::Settings::ReadUiScale(path), 100);
		auto broken = QFile(path);
		QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
		broken.write("{ not json");
		broken.close();
		QCOMPARE(Core::Settings::ReadUiScale(path), 100);
	}

	void reduceMotion() {
		auto dir = QTemporaryDir();
		QVERIFY(dir.isValid());
		const auto path = QDir(dir.path()).filePath(u"settings.json"_qs);
		{
			auto settings = Core::Settings(path);
			// Until the user chooses the system decides: nothing is written.
			QVERIFY(!settings.reduceMotion().has_value());
			settings.setCloseToTray(false);
			settings.flush();
		}
		QVERIFY(!ReadJson(path).contains(u"reduce_motion"_qs));
		QVERIFY(!ReadJson(path).contains(u"ui_scale"_qs));
		{
			auto settings = Core::Settings(path);
			QVERIFY(!settings.reduceMotion().has_value());
			settings.setReduceMotion(false);
			QCOMPARE(settings.reduceMotion(), std::make_optional(false));
		}
		QCOMPARE(ReadJson(path).value(u"reduce_motion"_qs), QJsonValue(false));
		{
			auto settings = Core::Settings(path);
			QCOMPARE(settings.reduceMotion(), std::make_optional(false));
			settings.setReduceMotion(true);
		}
		auto settings = Core::Settings(path);
		QCOMPARE(settings.reduceMotion(), std::make_optional(true));
		// Off the launcher's way: the scale stays the default.
		QCOMPARE(Core::Settings::ReadUiScale(path), 100);
	}

};

QTEST_GUILESS_MAIN(TestCoreSettings)
#include "test_core_settings.moc"
