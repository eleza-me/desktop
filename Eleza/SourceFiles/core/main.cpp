// Eleza Desktop entry point.
#include "core/core_launcher.h"

int main(int argc, char *argv[]) {
	auto launcher = Core::Launcher(argc, argv);
	return launcher.exec();
}
