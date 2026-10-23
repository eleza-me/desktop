#include "core/core_settings.h"

#include "base/platform_file.h"

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
	_musicRepeat = std::clamp(root.value(QStringLiteral("music_repeat")).toInt(0), 0, 2);
	_musicShuffle = root.value(QStringLiteral("music_shuffle")).toBool(false);
	_musicReversed = root.value(QStringLiteral("music_reversed")).toBool(false);
	_sendByCtrlEnter = root.value(
		QStringLiteral("send_by_ctrl_enter")).toBool(false);
	const auto systemTitleBar = root.value(QStringLiteral("system_title_bar"));
	_systemTitleBar = systemTitleBar.isBool()
		? std::make_optional(systemTitleBar.toBool())
		: std::nullopt;
	_mediaCacheLimit = std::max(
		qint64(root.value(QStringLiteral("media_cache_limit")).toDouble(0.)),
		qint64(0));
	_autoUpdate = root.value(QStringLiteral("auto_update")).toBool(true);
	const auto calls = root.value(QStringLiteral("calls")).toObject();
	_calls = CallSettings();
	_calls.inputDevice = QByteArray::fromBase64(
		calls.value(QStringLiteral("input_device")).toString().toLatin1());
	_calls.outputDevice = QByteArray::fromBase64(
		calls.value(QStringLiteral("output_device")).toString().toLatin1());
	_calls.cameraDevice = QByteArray::fromBase64(
		calls.value(QStringLiteral("camera_device")).toString().toLatin1());
	_calls.echoCancellation = calls.value(
		QStringLiteral("echo_cancellation")).toBool(true);
	_calls.noiseSuppression = calls.value(
		QStringLiteral("noise_suppression")).toBool(true);
	_calls.ringtoneVolume = std::clamp(
		calls.value(QStringLiteral("ringtone_volume")).toDouble(0.7),
		0.,
		1.);
	_emojiUses = std::nullopt;
	const auto uses = root.value(QStringLiteral("emoji_uses"));
	if (uses.isObject()) {
		auto map = QMap<QString, int>();
		const auto object = uses.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			const auto count = i.value().toInt(0);
			if (!i.key().isEmpty() && count > 0) {
				map.insert(i.key(), count);
			}
		}
		_emojiUses = std::move(map);
	} else if (const auto recent = root.value(QStringLiteral("recent_emoji"));
			recent.isArray()) {
		// The list of an older version, newest first: the newest counts most.
		auto map = QMap<QString, int>();
		const auto list = recent.toArray();
		auto count = int(list.size());
		for (const auto &item : list) {
			if (item.isString() && !item.toString().isEmpty()) {
				map.insert(item.toString(), count);
			}
			--count;
		}
		_emojiUses = std::move(map);
	}
	_emojiTones.clear();
	const auto tones = root.value(QStringLiteral("emoji_tones")).toObject();
	for (auto i = tones.begin(); i != tones.end(); ++i) {
		const auto tone = i.value().toInt(-1);
		if (!i.key().isEmpty() && tone >= 0 && tone < 5) {
			_emojiTones.insert(i.key(), tone);
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
	root.insert(QStringLiteral("music_repeat"), _musicRepeat);
	root.insert(QStringLiteral("music_shuffle"), _musicShuffle);
	root.insert(QStringLiteral("music_reversed"), _musicReversed);
	root.insert(QStringLiteral("send_by_ctrl_enter"), _sendByCtrlEnter);
	if (_systemTitleBar) {
		root.insert(QStringLiteral("system_title_bar"), *_systemTitleBar);
	}
	if (_mediaCacheLimit > 0) {
		root.insert(
			QStringLiteral("media_cache_limit"),
			double(_mediaCacheLimit));
	}
	root.insert(QStringLiteral("auto_update"), _autoUpdate);
	if (!(_calls == CallSettings())) {
		auto calls = QJsonObject();
		if (!_calls.inputDevice.isEmpty()) {
			calls.insert(
				QStringLiteral("input_device"),
				QString::fromLatin1(_calls.inputDevice.toBase64()));
		}
		if (!_calls.outputDevice.isEmpty()) {
			calls.insert(
				QStringLiteral("output_device"),
				QString::fromLatin1(_calls.outputDevice.toBase64()));
		}
		if (!_calls.cameraDevice.isEmpty()) {
			calls.insert(
				QStringLiteral("camera_device"),
				QString::fromLatin1(_calls.cameraDevice.toBase64()));
		}
		calls.insert(
			QStringLiteral("echo_cancellation"),
			_calls.echoCancellation);
		calls.insert(
			QStringLiteral("noise_suppression"),
			_calls.noiseSuppression);
		calls.insert(QStringLiteral("ringtone_volume"), _calls.ringtoneVolume);
		root.insert(QStringLiteral("calls"), calls);
	}
	if (_emojiUses) {
		auto uses = QJsonObject();
		for (auto i = _emojiUses->begin(); i != _emojiUses->end(); ++i) {
			uses.insert(i.key(), i.value());
		}
		root.insert(QStringLiteral("emoji_uses"), uses);
	}
	if (!_emojiTones.isEmpty()) {
		auto tones = QJsonObject();
		for (auto i = _emojiTones.begin(); i != _emojiTones.end(); ++i) {
			tones.insert(i.key(), i.value());
		}
		root.insert(QStringLiteral("emoji_tones"), tones);
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
	// For the user only: the last used folder and the recent emoji are
	// nobody else's business on a shared machine.
	if (!base::platform::WriteFileAtomic(
			_path,
			QJsonDocument(serialize()).toJson(QJsonDocument::Indented),
			true)) {
		qWarning("Settings: could not save the settings file.");
	}
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

void Settings::setMusicRepeat(int mode) {
	mode = std::clamp(mode, 0, 2);
	if (_musicRepeat != mode) {
		_musicRepeat = mode;
		scheduleWrite();
	}
}

void Settings::setMusicShuffle(bool shuffle) {
	if (_musicShuffle != shuffle) {
		_musicShuffle = shuffle;
		scheduleWrite();
	}
}

void Settings::setMusicReversed(bool reversed) {
	if (_musicReversed != reversed) {
		_musicReversed = reversed;
		scheduleWrite();
	}
}

void Settings::setSendByCtrlEnter(bool enabled) {
	if (_sendByCtrlEnter != enabled) {
		_sendByCtrlEnter = enabled;
		scheduleWrite();
	}
}

bool Settings::systemTitleBar() const {
#ifdef Q_OS_WIN
	constexpr auto byDefault = false;
#else // Q_OS_WIN
	constexpr auto byDefault = true;
#endif // Q_OS_WIN
	return _systemTitleBar.value_or(byDefault);
}

void Settings::setSystemTitleBar(bool enabled) {
	if (systemTitleBar() != enabled) {
		_systemTitleBar = enabled;
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

void Settings::setCalls(const CallSettings &settings) {
	if (!(_calls == settings)) {
		_calls = settings;
		_calls.ringtoneVolume = std::clamp(_calls.ringtoneVolume, 0., 1.);
		scheduleWrite();
	}
}

void Settings::setAutoUpdate(bool enabled) {
	if (_autoUpdate != enabled) {
		_autoUpdate = enabled;
		scheduleWrite();
	}
}

void Settings::setEmojiUses(const QMap<QString, int> &uses) {
	if (_emojiUses != uses) {
		_emojiUses = uses;
		scheduleWrite();
	}
}

void Settings::setEmojiTones(const QMap<QString, int> &tones) {
	if (_emojiTones != tones) {
		_emojiTones = tones;
		scheduleWrite();
	}
}

} // namespace Core
