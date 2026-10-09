#include "core/core_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

#include <algorithm>

namespace Core {
namespace {

constexpr auto kSaveDelay = 600;

[[nodiscard]] QString ThemeToString(style::ThemeMode mode) {
	switch (mode) {
	case style::ThemeMode::Light: return QStringLiteral("light");
	case style::ThemeMode::Dark: return QStringLiteral("dark");
	case style::ThemeMode::System: break;
	}
	return QStringLiteral("system");
}

[[nodiscard]] style::ThemeMode ThemeFromString(const QString &value) {
	if (value == QStringLiteral("light")) {
		return style::ThemeMode::Light;
	} else if (value == QStringLiteral("dark")) {
		return style::ThemeMode::Dark;
	}
	return style::ThemeMode::System;
}

} // namespace

Settings::Settings(const QString &path, QObject *parent)
: QObject(parent)
, _path(path) {
	_saveTimer.setSingleShot(true);
	_saveTimer.setInterval(kSaveDelay);
	QObject::connect(&_saveTimer, &QTimer::timeout, this, [this] { flush(); });
	read();
}

Settings::~Settings() {
	flush();
}

void Settings::read() {
	if (_path.isEmpty()) {
		return;
	}
	auto file = QFile(_path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return;
	}
	const auto root = document.object();
	_language = Lang::FromId(
		root.value(QStringLiteral("language")).toString(),
		Lang::Language::Persian);
	_themeMode = ThemeFromString(
		root.value(QStringLiteral("theme")).toString());
	const auto window = root.value(QStringLiteral("window")).toObject();
	if (!window.isEmpty()) {
		_windowGeometry.rect = QRect(
			window.value(QStringLiteral("x")).toInt(),
			window.value(QStringLiteral("y")).toInt(),
			window.value(QStringLiteral("w")).toInt(),
			window.value(QStringLiteral("h")).toInt());
		_windowGeometry.maximized = window.value(
			QStringLiteral("maximized")).toBool();
	}
	_sideColumnWidth = root.value(QStringLiteral("side_column_width")).toInt();
	_closeToTray = root.value(QStringLiteral("close_to_tray")).toBool(true);
	_lastTab = root.value(QStringLiteral("last_tab")).toString();
	const auto notify = root.value(QStringLiteral("notifications")).toObject();
	_notifications.desktopEnabled = notify.value(
		QStringLiteral("desktop")).toBool(true);
	_notifications.soundEnabled = notify.value(
		QStringLiteral("sound")).toBool(true);
	_notifications.showPreview = notify.value(
		QStringLiteral("preview")).toBool(true);
	_notifications.showSender = notify.value(
		QStringLiteral("sender")).toBool(true);
	_notifications.countUnreadMuted = notify.value(
		QStringLiteral("count_muted")).toBool(false);
	_notifications.flashTaskbar = notify.value(
		QStringLiteral("flash")).toBool(true);
	_notifications.onlyWhenInactive = notify.value(
		QStringLiteral("only_inactive")).toBool(false);
	_lastDialogDirectory = root.value(
		QStringLiteral("last_dialog_directory")).toString();
	_playbackVolume = std::clamp(
		root.value(QStringLiteral("playback_volume")).toDouble(1.),
		0.,
		1.);
	const auto speed = root.value(QStringLiteral("voice_speed")).toDouble(1.);
	_voicePlaybackSpeed = (speed == 1.5 || speed == 2.) ? speed : 1.;
	_sendByCtrlEnter = root.value(
		QStringLiteral("send_by_ctrl_enter")).toBool(false);
	_mediaCacheLimit = std::max(
		qint64(root.value(QStringLiteral("media_cache_limit")).toDouble(0.)),
		qint64(0));
	_recentEmoji.clear();
	for (const auto &item : root.value(QStringLiteral("recent_emoji")).toArray()) {
		if (item.isString() && !item.toString().isEmpty()) {
			_recentEmoji.push_back(item.toString());
		}
	}
}

QJsonObject Settings::serialize() const {
	auto window = QJsonObject();
	if (!_windowGeometry.rect.isNull()) {
		window.insert(QStringLiteral("x"), _windowGeometry.rect.x());
		window.insert(QStringLiteral("y"), _windowGeometry.rect.y());
		window.insert(QStringLiteral("w"), _windowGeometry.rect.width());
		window.insert(QStringLiteral("h"), _windowGeometry.rect.height());
		window.insert(QStringLiteral("maximized"), _windowGeometry.maximized);
	}
	auto notify = QJsonObject();
	notify.insert(QStringLiteral("desktop"), _notifications.desktopEnabled);
	notify.insert(QStringLiteral("sound"), _notifications.soundEnabled);
	notify.insert(QStringLiteral("preview"), _notifications.showPreview);
	notify.insert(QStringLiteral("sender"), _notifications.showSender);
	notify.insert(
		QStringLiteral("count_muted"),
		_notifications.countUnreadMuted);
	notify.insert(QStringLiteral("flash"), _notifications.flashTaskbar);
	notify.insert(
		QStringLiteral("only_inactive"),
		_notifications.onlyWhenInactive);

	auto root = QJsonObject();
	root.insert(QStringLiteral("language"), Lang::Id(_language));
	root.insert(QStringLiteral("theme"), ThemeToString(_themeMode));
	if (!window.isEmpty()) {
		root.insert(QStringLiteral("window"), window);
	}
	if (_sideColumnWidth > 0) {
		root.insert(QStringLiteral("side_column_width"), _sideColumnWidth);
	}
	root.insert(QStringLiteral("close_to_tray"), _closeToTray);
	if (!_lastTab.isEmpty()) {
		root.insert(QStringLiteral("last_tab"), _lastTab);
	}
	root.insert(QStringLiteral("notifications"), notify);
	if (!_lastDialogDirectory.isEmpty()) {
		root.insert(
			QStringLiteral("last_dialog_directory"),
			_lastDialogDirectory);
	}
	root.insert(QStringLiteral("playback_volume"), _playbackVolume);
	root.insert(QStringLiteral("voice_speed"), _voicePlaybackSpeed);
	root.insert(QStringLiteral("send_by_ctrl_enter"), _sendByCtrlEnter);
	if (_mediaCacheLimit > 0) {
		root.insert(
			QStringLiteral("media_cache_limit"),
			double(_mediaCacheLimit));
	}
	if (!_recentEmoji.isEmpty()) {
		root.insert(
			QStringLiteral("recent_emoji"),
			QJsonArray::fromStringList(_recentEmoji));
	}
	return root;
}

void Settings::scheduleWrite() {
	_dirty = true;
	if (!_path.isEmpty()) {
		_saveTimer.start();
	}
	emit changed();
}

void Settings::flush() {
	_saveTimer.stop();
	if (!_dirty || _path.isEmpty()) {
		return;
	}
	_dirty = false;
	QDir().mkpath(QFileInfo(_path).absolutePath());
	auto file = QSaveFile(_path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(serialize()).toJson(QJsonDocument::Indented));
	file.commit();
}

void Settings::setLanguage(Lang::Language language) {
	if (_language != language) {
		_language = language;
		scheduleWrite();
	}
}

void Settings::setThemeMode(style::ThemeMode mode) {
	if (_themeMode != mode) {
		_themeMode = mode;
		scheduleWrite();
	}
}

void Settings::setWindowGeometry(const WindowGeometry &geometry) {
	if (_windowGeometry.rect != geometry.rect
		|| _windowGeometry.maximized != geometry.maximized) {
		_windowGeometry = geometry;
		scheduleWrite();
	}
}

void Settings::setSideColumnWidth(int width) {
	if (_sideColumnWidth != width) {
		_sideColumnWidth = width;
		scheduleWrite();
	}
}

void Settings::setCloseToTray(bool enabled) {
	if (_closeToTray != enabled) {
		_closeToTray = enabled;
		scheduleWrite();
	}
}

void Settings::setNotifications(const NotificationSettings &settings) {
	if (!(_notifications == settings)) {
		_notifications = settings;
		scheduleWrite();
	}
}

void Settings::setLastTab(const QString &tab) {
	if (_lastTab != tab) {
		_lastTab = tab;
		scheduleWrite();
	}
}

void Settings::setLastDialogDirectory(const QString &path) {
	if (_lastDialogDirectory != path) {
		_lastDialogDirectory = path;
		scheduleWrite();
	}
}

void Settings::setPlaybackVolume(double volume) {
	volume = std::clamp(volume, 0., 1.);
	if (_playbackVolume != volume) {
		_playbackVolume = volume;
		scheduleWrite();
	}
}

void Settings::setVoicePlaybackSpeed(double speed) {
	if (speed != 1.5 && speed != 2.) {
		speed = 1.;
	}
	if (_voicePlaybackSpeed != speed) {
		_voicePlaybackSpeed = speed;
		scheduleWrite();
	}
}

void Settings::setSendByCtrlEnter(bool enabled) {
	if (_sendByCtrlEnter != enabled) {
		_sendByCtrlEnter = enabled;
		scheduleWrite();
	}
}

void Settings::setMediaCacheLimit(qint64 bytes) {
	bytes = std::max(bytes, qint64(0));
	if (_mediaCacheLimit != bytes) {
		_mediaCacheLimit = bytes;
		scheduleWrite();
	}
}

void Settings::setRecentEmoji(const QStringList &emoji) {
	if (_recentEmoji != emoji) {
		_recentEmoji = emoji;
		scheduleWrite();
	}
}

} // namespace Core
