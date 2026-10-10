#include "intro/intro_module.h"

#include "intro/intro_widget.h"
#include "window/window_registry.h"

namespace Intro {

void Register() {
	Window::RegisterIntro([](QWidget *parent, Window::MainWindow *window) {
		return Ui::Owned<Ui::RpWidget>(new Widget(parent, window));
	});
}

} // namespace Intro
