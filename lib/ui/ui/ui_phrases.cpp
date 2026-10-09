#include "ui/ui_phrases.h"

namespace Ui {
namespace {

[[nodiscard]] Phrases &Storage() {
	static auto result = Phrases();
	return result;
}

} // namespace

void SetPhrases(Phrases phrases) {
	Storage() = std::move(phrases);
}

const Phrases &CurrentPhrases() {
	return Storage();
}

QString FormatNumber(qint64 value) {
	const auto latin = QString::number(value);
	const auto format = Storage().formatNumber;
	return format ? format(latin) : latin;
}

} // namespace Ui
