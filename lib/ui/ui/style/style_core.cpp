#include "ui/style/style_core.h"

#include <QtGui/QGuiApplication>

#include <vector>

namespace style {
namespace {

bool RightToLeftValue = false;
bool StartedValue = false;

[[nodiscard]] std::vector<void(*)()> &Modules() {
	static auto result = std::vector<void(*)()>();
	return result;
}

} // namespace

namespace internal {

ModuleRegistrar::ModuleRegistrar(void (*init)()) {
	Modules().push_back(init);
	if (StartedValue) {
		init();
	}
}

} // namespace internal

bool RightToLeft() {
	return RightToLeftValue;
}

void SetRightToLeft(bool rtl) {
	RightToLeftValue = rtl;
}

void Start(Scheme initial) {
	if (StartedValue) {
		return;
	}
	Q_ASSERT(qGuiApp != nullptr);
	internal::StartFonts();
	internal::StartPalette(initial);
	internal::StartIcons();
	StartedValue = true;
	for (const auto init : Modules()) {
		init();
	}
}

bool Started() {
	return StartedValue;
}

} // namespace style
