// The current palette: light / dark scheme, following the system or a manual override.
#pragma once

#include "ui/style/style_core_color.h"

#include <QtCore/QObject>

#include <functional>
#include <optional>

namespace style {

enum class Scheme {
	Light,
	Dark,
};

// What the user chose in settings.
enum class ThemeMode {
	System,
	Light,
	Dark,
};

class PaletteNotifier final : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;

Q_SIGNALS:
	// Emitted after the colors behind every style::color were replaced.
	// All widgets are repainted automatically; connect only to drop caches.
	void changed();

};

[[nodiscard]] Scheme CurrentScheme();
[[nodiscard]] inline bool IsDark() {
	return CurrentScheme() == Scheme::Dark;
}

// Applies a scheme right now (does not change the theme mode's notion of "system").
void SetScheme(Scheme scheme);

// System = follow the OS setting (and track its changes), Light / Dark = override.
void SetThemeMode(ThemeMode mode);
[[nodiscard]] ThemeMode CurrentThemeMode();
[[nodiscard]] bool SystemPrefersDark();

// The notifier object, for QObject::connect.
[[nodiscard]] PaletteNotifier *PaletteChanged();

// Shortcut: calls `callback` on every palette change while `context` is alive.
void OnPaletteChanged(QObject *context, std::function<void()> callback);

// Incremented on every change, handy as a cache key.
[[nodiscard]] int PaletteVersion();

// Looks a palette color up by its name ("brandPrimary").
[[nodiscard]] std::optional<color> FindPaletteColor(const QString &name);

namespace internal {

void StartPalette(Scheme initial);

} // namespace internal
} // namespace style
