// Eleza Desktop entry point.
#include "core/core_launcher.h"
#include "updater/updater_install.h"

int main(int argc, char *argv[]) {
	// An update that was cut off in the middle is undone before anything
	// of the app is used (and the program starts again).
	Updater::RecoverInterruptedInstall(argc, argv);

	auto code = 0;
	{
		auto launcher = Core::Launcher(argc, argv);
		code = launcher.exec();
	}
	// «به‌روزرسانی الزا» was chosen: the files are swapped and the new
	// version starts now that everything of this one is closed.
	return Updater::FinishPendingInstall(code);
}
