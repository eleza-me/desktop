#include "ui/style/style_core_font.h"

#include <QtCore/QHash>
#include <QtCore/QStringList>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontInfo>
#include <QtGui/QGuiApplication>

static void InitFontsResource() {
	Q_INIT_RESOURCE(fonts);
}

namespace style {
namespace {

QString TextFamily;
QString DisplayFamily;
QString MonospaceFamily;
QString EmojiFamily;
bool FontsStarted = false;

QString LoadFont(const QString &path, const QString &fallback) {
	const auto id = QFontDatabase::addApplicationFont(path);
	if (id < 0) {
		qWarning("Fonts: could not load '%s'.", qPrintable(path));
		return fallback;
	}
	const auto families = QFontDatabase::applicationFontFamilies(id);
	return families.isEmpty() ? fallback : families.front();
}

[[nodiscard]] QString FirstExisting(const QStringList &candidates) {
	const auto all = QFontDatabase::families();
	for (const auto &candidate : candidates) {
		if (all.contains(candidate, Qt::CaseInsensitive)) {
			return candidate;
		}
	}
	return QString();
}

[[nodiscard]] QHash<QString, internal::FontData*> &Registry() {
	static auto result = QHash<QString, internal::FontData*>();
	return result;
}

[[nodiscard]] QString ResolveFamily(const QString &family) {
	if (family.isEmpty() || family == QStringLiteral("text")) {
		return TextFamily;
	} else if (family == QStringLiteral("display")) {
		return DisplayFamily;
	}
	return family;
}

} // namespace

namespace internal {

void StartFonts() {
	if (FontsStarted) {
		return;
	}
	FontsStarted = true;
	InitFontsResource();

	const auto vazir = QStringLiteral("Vazirmatn");
	const auto markazi = QStringLiteral("Markazi Text");
	TextFamily = LoadFont(QStringLiteral(":/fonts/Vazirmatn-Regular.ttf"), vazir);
	LoadFont(QStringLiteral(":/fonts/Vazirmatn-Medium.ttf"), vazir);
	LoadFont(QStringLiteral(":/fonts/Vazirmatn-SemiBold.ttf"), vazir);
	LoadFont(QStringLiteral(":/fonts/Vazirmatn-Bold.ttf"), vazir);
	DisplayFamily = LoadFont(
		QStringLiteral(":/fonts/MarkaziText-Medium.ttf"),
		markazi);
	LoadFont(QStringLiteral(":/fonts/MarkaziText-Bold.ttf"), markazi);

	EmojiFamily = FirstExisting({
		QStringLiteral("Segoe UI Emoji"),
		QStringLiteral("Noto Color Emoji"),
		QStringLiteral("Apple Color Emoji"),
		QStringLiteral("Twemoji Mozilla"),
		QStringLiteral("Noto Emoji"),
		QStringLiteral("Segoe UI Symbol"),
	});
	MonospaceFamily = FirstExisting({
		QStringLiteral("Cascadia Mono"),
		QStringLiteral("Consolas"),
		QStringLiteral("JetBrains Mono"),
		QStringLiteral("DejaVu Sans Mono"),
		QStringLiteral("Liberation Mono"),
		QStringLiteral("Courier New"),
	});
	if (MonospaceFamily.isEmpty()) {
		MonospaceFamily = QFontDatabase::systemFont(
			QFontDatabase::FixedFont).family();
	}

#ifdef ELEZA_DEBUG_TOOLS
	if (qEnvironmentVariableIsSet("ELEZA_DEBUG_FONTS")) {
		qInfo(
			"Fonts: text '%s', display '%s', monospace '%s', emoji '%s'.",
			qPrintable(TextFamily),
			qPrintable(DisplayFamily),
			qPrintable(MonospaceFamily),
			qPrintable(EmojiFamily));
	}
#endif // ELEZA_DEBUG_TOOLS

	// The default font for anything that is not painted through style::font
	// (native inner editors, tray menu).
	auto appFont = QFont();
	appFont.setFamilies(EmojiFamily.isEmpty()
		? QStringList{ TextFamily }
		: QStringList{ TextFamily, EmojiFamily });
	appFont.setPixelSize(15);
	QGuiApplication::setFont(appFont);
}

FontData::FontData(int size, uint flags, const QString &family)
: m(f)
, _size(size)
, _flags(flags)
, _family(family) {
	auto families = QStringList();
	if (flags & FontMonospace) {
		families.push_back(MonospaceFamily);
	}
	families.push_back(ResolveFamily(family));
	if (families.back() != TextFamily) {
		families.push_back(TextFamily);
	}
	if (!EmojiFamily.isEmpty()) {
		families.push_back(EmojiFamily);
	}
	f.setFamilies(families);
	f.setPixelSize(size);
	f.setWeight((flags & FontBold)
		? QFont::Bold
		: (flags & FontSemibold)
		? QFont::DemiBold
		: (flags & FontMedium)
		? QFont::Medium
		: QFont::Normal);
	f.setItalic((flags & FontItalic) != 0);
	f.setUnderline((flags & FontUnderline) != 0);
	f.setStrikeOut((flags & FontStrikeOut) != 0);
	f.setStyleStrategy(QFont::PreferAntialias);
	f.setKerning(true);

	m = QFontMetrics(f);
	ascent = m.ascent();
	descent = m.descent();
	height = ascent + descent;
	spacew = m.horizontalAdvance(QLatin1Char(' '));
	elidew = m.horizontalAdvance(QChar(0x2026));
}

int FontData::width(const QString &text) const {
	return m.horizontalAdvance(text);
}

int FontData::width(QChar ch) const {
	return m.horizontalAdvance(ch);
}

QString FontData::elided(
		const QString &text,
		int width,
		Qt::TextElideMode mode) const {
	return m.elidedText(text, mode, width);
}

font FontData::withFlags(uint flags) const {
	return font(_size, flags, _family);
}

font FontData::regular() const {
	return withFlags(_flags & ~(FontBold | FontSemibold | FontMedium));
}

font FontData::medium() const {
	return withFlags((_flags & ~(FontBold | FontSemibold)) | FontMedium);
}

font FontData::semibold() const {
	return withFlags((_flags & ~(FontBold | FontMedium)) | FontSemibold);
}

font FontData::bold() const {
	return withFlags((_flags & ~(FontSemibold | FontMedium)) | FontBold);
}

font FontData::italic(bool set) const {
	return withFlags(set ? (_flags | FontItalic) : (_flags & ~FontItalic));
}

font FontData::underline(bool set) const {
	return withFlags(set ? (_flags | FontUnderline) : (_flags & ~FontUnderline));
}

font FontData::strikeout(bool set) const {
	return withFlags(set ? (_flags | FontStrikeOut) : (_flags & ~FontStrikeOut));
}

font FontData::monospace() const {
	return withFlags(_flags | FontMonospace);
}

font FontData::withSize(int size) const {
	return font(size, _flags, _family);
}

} // namespace internal

font::font(int size, uint flags, const QString &family) {
	internal::StartFonts();
	const auto key = QString::number(size)
		+ QLatin1Char('/')
		+ QString::number(flags)
		+ QLatin1Char('/')
		+ family;
	auto &registry = Registry();
	const auto i = registry.constFind(key);
	if (i != registry.cend()) {
		_data = i.value();
		return;
	}
	const auto created = new internal::FontData(size, flags, family);
	registry.insert(key, created);
	_data = created;
}

QString TextFontFamily() {
	return TextFamily;
}

QString DisplayFontFamily() {
	return DisplayFamily;
}

QString MonospaceFontFamily() {
	return MonospaceFamily;
}

QString EmojiFontFamily() {
	return EmojiFamily;
}

} // namespace style
