// Core::Launcher — process start: command line, HiDPI setup, single instance,
// QApplication and Core::Application.
//
// Command line (see docs/ARCHITECTURE.md "Debug / test hooks"):
//   --api=<url>               API base (or env ELEZA_API), default https://api.eleza.me
//   --workdir=<dir>           data directory (default: platform app-data)
//   --lang=fa|en              interface language for this run
//   --theme=light|dark        palette for this run
//   --gallery[=<page>]        open the UI gallery instead of the app
//                             (pages: buttons, inputs, text, lists, layers, misc)
//   --debug-route=<route>     route to open after start ("chats", "eli", "settings",
//                             "chat/<id>"…); in the gallery: an overlay to show
//                             (box, confirm, sheet, toast, menu, dropdown)
//   --debug-size=<W>x<H>      window size
//   --debug-scale=<factor>    device pixel ratio (sets QT_SCALE_FACTOR)
//   --debug-grab=<png>        save a grab of the main window and exit
//   --debug-grab-delay=<ms>   delay before the grab (default 600)
//   --no-single-instance      allow a second process with the same workdir
#pragma once

#include "lang/lang_instance.h"
#include "ui/style/style_core_palette.h"

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace Core {

struct CommandLine {
	QString apiUrl;
	QString workDir;
	std::optional<Lang::Language> language;
	std::optional<style::Scheme> theme;
	bool gallery = false;
	QString galleryPage;
	QString debugRoute;
	QSize debugSize;
	double debugScale = 0.;
	QString debugGrab;
	int debugGrabDelay = 600;
	bool noSingleInstance = false;

	// True for screenshot runs: nothing is written to disk, no tray icon.
	[[nodiscard]] bool ephemeral() const {
		return !debugGrab.isEmpty();
	}
};

[[nodiscard]] CommandLine ParseCommandLine(const QStringList &arguments);

class Launcher final {
public:
	Launcher(int argc, char *argv[]);

	[[nodiscard]] int exec();

	[[nodiscard]] const CommandLine &commandLine() const {
		return _commandLine;
	}
	[[nodiscard]] QString workDir() const {
		return _workDir;
	}
	[[nodiscard]] QString apiUrl() const;

private:
	void prepareEnvironment();
	[[nodiscard]] QString computeWorkDir() const;

	int _argc = 0;
	char **_argv = nullptr;
	CommandLine _commandLine;
	QString _workDir;

};

} // namespace Core
