#include "ui/style/style_core_palette.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace style {
namespace {

Scheme SchemeValue = Scheme::Light;
ThemeMode ModeValue = ThemeMode::Light;
int VersionValue = 0;
bool SystemDarkAtStart = false;

[[nodiscard]] PaletteNotifier *Notifier() {
	static const auto result = new PaletteNotifier(qApp);
	return result;
}

[[nodiscard]] QColor FromRgba(quint32 value) {
	return QColor(
		int((value >> 24) & 0xFF),
		int((value >> 16) & 0xFF),
		int((value >> 8) & 0xFF),
		int(value & 0xFF));
}

void Fill(Scheme scheme) {
	const auto palette = internal::GeneratedPalette();
	for (auto i = 0; i != palette.count; ++i) {
		const auto &row = palette.rows[i];
		palette.data[i].set(FromRgba(
			(scheme == Scheme::Dark) ? row.dark : row.light));
	}
}

[[nodiscard]] bool DetectSystemDark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	const auto scheme = QGuiApplication::styleHints()->colorScheme();
	if (scheme != Qt::ColorScheme::Unknown) {
		return (scheme == Qt::ColorScheme::Dark);
	}
#endif // Qt >= 6.5
#ifdef Q_OS_WIN
	const auto settings = QSettings(
		QStringLiteral(
			"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows"
			"\\CurrentVersion\\Themes\\Personalize"),
		QSettings::NativeFormat);
	const auto value = settings.value(QStringLiteral("AppsUseLightTheme"));
	if (value.isValid()) {
		return (value.toInt() == 0);
	}
#endif // Q_OS_WIN
	const auto gtkTheme = qEnvironmentVariable("GTK_THEME").toLower();
	if (gtkTheme.endsWith(QStringLiteral(":dark"))
		|| gtkTheme.endsWith(QStringLiteral("-dark"))) {
		return true;
	}
	return SystemDarkAtStart;
}

// Watches the OS for light / dark switches while the mode is System.
class SystemWatcher final : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (object == qApp
			&& (event->type() == QEvent::ApplicationPaletteChange
				|| event->type() == QEvent::ThemeChange)) {
			QTimer::singleShot(0, this, [] {
				if (ModeValue == ThemeMode::System) {
					SetScheme(DetectSystemDark() ? Scheme::Dark : Scheme::Light);
				}
			});
		}
		return QObject::eventFilter(object, event);
	}

};

void EnsureWatcher() {
	static auto installed = false;
	if (installed || !qApp) {
		return;
	}
	installed = true;
	const auto watcher = new SystemWatcher(qApp);
	qApp->installEventFilter(watcher);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	QObject::connect(
		QGuiApplication::styleHints(),
		&QStyleHints::colorSchemeChanged,
		watcher,
		[] {
			if (ModeValue == ThemeMode::System) {
				SetScheme(DetectSystemDark() ? Scheme::Dark : Scheme::Light);
			}
		});
#endif // Qt >= 6.5
}

// Native child controls (text cursors, selections of the inner editors) read
// the application palette, keep it in sync with ours.
void ApplyApplicationPalette() {
	if (!qobject_cast<QApplication*>(qApp)) {
		return;
	}
	const auto find = [](const char *name, const QColor &fallback) {
		const auto found = FindPaletteColor(QString::fromLatin1(name));
		return found ? (*found)->c : fallback;
	};
	const auto window = find("bgCanvas", Qt::white);
	const auto base = find("bgSurface", Qt::white);
	const auto text = find("textPrimary", Qt::black);
	const auto faded = find("textTertiary", Qt::gray);
	const auto brand = find("brandPrimary", Qt::darkGreen);
	const auto selection = find("selectionBg", Qt::green);
	auto palette = QPalette();
	palette.setColor(QPalette::Window, window);
	palette.setColor(QPalette::WindowText, text);
	palette.setColor(QPalette::Base, base);
	palette.setColor(QPalette::AlternateBase, window);
	palette.setColor(QPalette::Text, text);
	palette.setColor(QPalette::Button, base);
	palette.setColor(QPalette::ButtonText, text);
	palette.setColor(QPalette::PlaceholderText, faded);
	palette.setColor(QPalette::Highlight, selection);
	palette.setColor(QPalette::HighlightedText, text);
	palette.setColor(QPalette::Link, brand);
	palette.setColor(QPalette::ToolTipBase, find("tooltipBg", Qt::black));
	palette.setColor(QPalette::ToolTipText, find("tooltipFg", Qt::white));
	QApplication::setPalette(palette);
}

} // namespace

Scheme CurrentScheme() {
	return SchemeValue;
}

void SetScheme(Scheme scheme) {
	if (SchemeValue == scheme && VersionValue > 0) {
		return;
	}
	SchemeValue = scheme;
	Fill(scheme);
	++VersionValue;
	ApplyApplicationPalette();
	emit Notifier()->changed();
	if (qobject_cast<QApplication*>(qApp)) {
		const auto widgets = QApplication::allWidgets();
		for (const auto widget : widgets) {
			widget->update();
		}
	}
}

void SetThemeMode(ThemeMode mode) {
	ModeValue = mode;
	if (mode == ThemeMode::System) {
		EnsureWatcher();
		SetScheme(DetectSystemDark() ? Scheme::Dark : Scheme::Light);
	} else {
		SetScheme((mode == ThemeMode::Dark) ? Scheme::Dark : Scheme::Light);
	}
}

ThemeMode CurrentThemeMode() {
	return ModeValue;
}

bool SystemPrefersDark() {
	return DetectSystemDark();
}

PaletteNotifier *PaletteChanged() {
	return Notifier();
}

void OnPaletteChanged(QObject *context, std::function<void()> callback) {
	QObject::connect(
		Notifier(),
		&PaletteNotifier::changed,
		context,
		std::move(callback));
}

int PaletteVersion() {
	return VersionValue;
}

std::optional<color> FindPaletteColor(const QString &name) {
	const auto palette = internal::GeneratedPalette();
	const auto latin = name.toLatin1();
	for (auto i = 0; i != palette.count; ++i) {
		if (latin == palette.rows[i].name) {
			return color(&palette.data[i]);
		}
	}
	return std::nullopt;
}

namespace internal {

void StartPalette(Scheme initial) {
	// The platform theme's own palette, read before we replace it.
	SystemDarkAtStart = qGuiApp
		&& (QGuiApplication::palette().color(QPalette::Window).lightness()
			< 128);
	SchemeValue = initial;
	ModeValue = (initial == Scheme::Dark) ? ThemeMode::Dark : ThemeMode::Light;
	Fill(initial);
	++VersionValue;
	ApplyApplicationPalette();
}

} // namespace internal
} // namespace style
