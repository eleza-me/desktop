// Core::Settings — application preferences stored as JSON in <workdir>/settings.json:
// language, theme, window geometry, layout and notification preferences.
//
//   Core::App().settings().setThemeMode(style::ThemeMode::Dark);   // saved shortly after
//   connect(&Core::App().settings(), &Core::Settings::changed, ...);
//
// Setters only store the value and schedule a save; applying a setting (switching the
// language, the palette) is Core::Application's job.
#pragma once

#include "lang/lang_instance.h"
#include "ui/style/style_core_palette.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QRect>
#include <QtCore/QTimer>

namespace Core {

struct NotificationSettings {
	bool desktopEnabled = true; // native notifications
	bool soundEnabled = true;
	bool showPreview = true; // message text in the notification
	bool showSender = true;
	bool countUnreadMuted = false; // muted chats in the taskbar / tray badge
	bool flashTaskbar = true;

	friend bool operator==(
			const NotificationSettings &a,
			const NotificationSettings &b) {
		return (a.desktopEnabled == b.desktopEnabled)
			&& (a.soundEnabled == b.soundEnabled)
			&& (a.showPreview == b.showPreview)
			&& (a.showSender == b.showSender)
			&& (a.countUnreadMuted == b.countUnreadMuted)
			&& (a.flashTaskbar == b.flashTaskbar);
	}
};

struct WindowGeometry {
	QRect rect; // null = never saved
	bool maximized = false;
};

class Settings final : public QObject {
	Q_OBJECT

public:
	// `path` empty = keep everything in memory (screenshot runs, tests).
	explicit Settings(const QString &path, QObject *parent = nullptr);
	~Settings();

	[[nodiscard]] Lang::Language language() const {
		return _language;
	}
	void setLanguage(Lang::Language language);

	[[nodiscard]] style::ThemeMode themeMode() const {
		return _themeMode;
	}
	void setThemeMode(style::ThemeMode mode);

	[[nodiscard]] WindowGeometry windowGeometry() const {
		return _windowGeometry;
	}
	void setWindowGeometry(const WindowGeometry &geometry);

	// Width of the side column (chat list) in the two-column layout.
	[[nodiscard]] int sideColumnWidth() const {
		return _sideColumnWidth;
	}
	void setSideColumnWidth(int width);

	[[nodiscard]] bool closeToTray() const {
		return _closeToTray;
	}
	void setCloseToTray(bool enabled);

	[[nodiscard]] const NotificationSettings &notifications() const {
		return _notifications;
	}
	void setNotifications(const NotificationSettings &settings);

	// The tab that was open last ("eli", "chats", "search", "me", "settings").
	[[nodiscard]] QString lastTab() const {
		return _lastTab;
	}
	void setLastTab(const QString &tab);

	// Writes pending changes right now (on quit).
	void flush();

Q_SIGNALS:
	void changed();

private:
	void read();
	void scheduleWrite();
	[[nodiscard]] QJsonObject serialize() const;

	QString _path;
	QTimer _saveTimer;
	bool _dirty = false;

	Lang::Language _language = Lang::Language::Persian;
	style::ThemeMode _themeMode = style::ThemeMode::System;
	WindowGeometry _windowGeometry;
	int _sideColumnWidth = 0;
	bool _closeToTray = true;
	NotificationSettings _notifications;
	QString _lastTab;

};

} // namespace Core
