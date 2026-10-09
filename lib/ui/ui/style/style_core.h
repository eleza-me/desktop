// The style runtime: everything generated `styles/style_*.h` files need.
//
//   style::Start(style::Scheme::Light);   // once, after QApplication is created
//   #include "styles/style_widgets.h"
//   p.setFont(st::normalFont); p.setPen(st::textPrimary);
#pragma once

#include "ui/style/style_core_types.h"
#include "ui/style/style_core_color.h"
#include "ui/style/style_core_palette.h"
#include "ui/style/style_core_font.h"
#include "ui/style/style_core_icon.h"

namespace style {
namespace internal {

// Every generated style_*.cpp registers its init function.
class ModuleRegistrar {
public:
	explicit ModuleRegistrar(void (*init)());

};

} // namespace internal

// Registers the bundled fonts, applies the palette and fills every st:: value.
// Must be called after the QGuiApplication is created and before any widget.
void Start(Scheme initial = Scheme::Light);
[[nodiscard]] bool Started();

} // namespace style
