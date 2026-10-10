// Core::Launcher — process start: command line, HiDPI setup, single instance,
// QApplication and Core::Application.
//
// Command line:
//   --api=<url>               API base (or env ELEZA_API), default https://api.eleza.me
//   --workdir=<dir>           data directory (default: platform app-data)
//   --lang=fa|en              interface language for this run
//   --theme=light|dark        palette for this run
//   --no-single-instance      allow a second process with the same workdir
//   --version, --help         print and exit
//
// Only in a build with ELEZA_DEBUG_TOOLS (docs/DEVELOPING.md "Debug hooks"; a
// build without it has none of this code, says once that the flag is not
// available and goes on):
//   --gallery[=<page>]        open the UI gallery instead of the app
//                             (pages: buttons, inputs, text, lists, layers, misc)
//   --debug-route=<route>     route to open after start ("chats", "eli", "settings",
//                             "chat/<id>"…); in the gallery: an overlay to show
//                             (box, confirm, sheet, toast, menu, dropdown)
//   --debug-size=<W>x<H>      window size
//   --debug-scale=<factor>    device pixel ratio (sets QT_SCALE_FACTOR)
//   --debug-grab=<png>        save a grab of the main window and exit
//   --debug-grab-delay=<ms>   delay before the grab, counted from the moment the
//                             route was opened and the scripted actions finished
//                             (default 600)
//   --debug-login=<phone>:<code>[:<password>]
//                             sign in through the API at startup when nobody is
//                             signed in (skips the intro screens)
//   --debug-actions=<script>  scripted input played on the main window, e.g.
//                             "click:600,400;type:متن;key:Return;wait:300;grab:/tmp/a.png"
//                             (core_debug.h has the whole list)
//   --update-url=<url>        an update manifest to use instead of the built-in
//                             sources (may be repeated; http:// on the loopback
//                             interface is accepted)
//   --update-key=<base64>     the public key the test manifest is signed with
//                             (may be repeated; replaces the built-in keys)
//   --update-now              check at once and install as soon as the update
//                             is ready (docs/RELEASING.md, "Testing an update")
//   --update-state=<state>    screenshots: shows the update UI in a state
//                             without any network (checking, uptodate,
//                             available, announced, downloading, preparing,
//                             ready, failed, failed-install, required,
//                             required-ready)
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
	bool noSingleInstance = false;
	bool showVersion = false;
	bool showHelp = false;
	// Debug flags given to a build that has none.
	QStringList unavailable;

#ifdef ELEZA_DEBUG_TOOLS
	bool gallery = false;
	QString galleryPage;
	QString debugRoute;
	QSize debugSize;
	double debugScale = 0.;
	QString debugGrab;
	int debugGrabDelay = 600;
	QString debugLoginPhone;
	QString debugLoginCode;
	QString debugLoginPassword;
	QString debugActions;
	QStringList updateUrls;
	QStringList updateKeys;
	bool updateNow = false;
	QString updateState;

	// True for screenshot / scripted runs: preferences are not written to disk,
	// no tray icon, animations jump to the end, the process exits when done.
	[[nodiscard]] bool ephemeral() const {
		return !debugGrab.isEmpty() || !debugActions.isEmpty();
	}
	[[nodiscard]] bool debugLogin() const {
		return !debugLoginPhone.isEmpty() && !debugLoginCode.isEmpty();
	}
#else // ELEZA_DEBUG_TOOLS
	// No screenshot / scripted runs in this build.
	[[nodiscard]] constexpr bool ephemeral() const {
		return false;
	}
#endif // ELEZA_DEBUG_TOOLS
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
