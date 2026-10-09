// Texts the widget library shows by itself (context menus of inputs and labels).
// ElezaUi does not depend on the lang module: the application fills these from
// tr::lng_* at startup and again whenever the language changes.
#pragma once

#include <QtCore/QString>

namespace Ui {

struct Phrases {
	QString undo = QStringLiteral("Undo");
	QString redo = QStringLiteral("Redo");
	QString cut = QStringLiteral("Cut");
	QString copy = QStringLiteral("Copy");
	QString paste = QStringLiteral("Paste");
	QString remove = QStringLiteral("Delete");
	QString selectAll = QStringLiteral("Select all");
	QString copyLink = QStringLiteral("Copy link");
	QString openLink = QStringLiteral("Open link");
	QString copied = QStringLiteral("Copied");
	QString ok = QStringLiteral("OK");
	QString cancel = QStringLiteral("Cancel");
	QString close = QStringLiteral("Close");
	QString back = QStringLiteral("Back");
	QString show = QStringLiteral("Show");
	QString hide = QStringLiteral("Hide");
	// Digits are converted with this for counters painted by the library
	// (badges): identity for Latin digits, Persian digits in the fa interface.
	QString (*formatNumber)(const QString &latin) = nullptr;
};

void SetPhrases(Phrases phrases);
[[nodiscard]] const Phrases &CurrentPhrases();

// CurrentPhrases().formatNumber applied to a number.
[[nodiscard]] QString FormatNumber(qint64 value);

} // namespace Ui
