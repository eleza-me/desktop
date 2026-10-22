# Developing Eleza Desktop

The working guide for feature modules (dialogs, history, conversation / search, eli, profile /
settings). Read `docs/ARCHITECTURE.md` first for the layout and the conventions; this file is
the "how do I…" part. Everything below was checked against the headers — when in doubt the
header wins, and its top comment usually has a usage example.

## 1. Build, run, grab

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build -j2
ctest --test-dir build --output-on-failure

# the test backend (HTTP only, no websocket: the app runs on the HTTP fallback)
node <pwa>/scripts/mock-api.mjs &                 # http://127.0.0.1:8000
#   any 6-digit code signs in; code 222222 -> two-step password "secret"
node tests/tools/mock-auth-proxy.mjs &            # http://127.0.0.1:8001 -> :8000, adds:
#   code 000000 -> wrong code; code 333333 -> a new account (profile step);
#   two-step recovery (any code confirms, 111111 is wrong); UPGRADE=1 -> "update required"

./build/Eleza/Eleza --api=http://127.0.0.1:8000 --workdir=/tmp/wd1 --no-single-instance
```

A headless screenshot of a signed-in screen:

```sh
QT_QPA_PLATFORM=offscreen ./build/Eleza/Eleza --api=http://127.0.0.1:8000 \
  --workdir=/tmp/wd1 --no-single-instance --lang=fa --theme=light --debug-size=1200x760 \
  --debug-login=+989121234567:123456 --debug-route=chat/c1 \
  --debug-grab=/tmp/x.png --debug-grab-delay=1500
```

Then look at the PNG. Check both languages (`--lang=fa|en`: RTL and LTR) and both themes
(`--theme=light|dark`), and a narrow window (`--debug-size=520x700`: single column).
Under a real X server use `xvfb-run -a ./build/Eleza/Eleza …` with the same flags.

The session is stored in `--workdir`: the second run with the same directory is already
signed in (`--debug-login` is skipped then). Delete the directory to start clean. The stored
session belongs to the API origin that issued it: a run with another `--api` starts signed
out and sends it nothing, and signing in there replaces it (one workdir per server keeps both).

## 2. Debug hooks

Everything in this section except the first table exists only in a build with the CMake
option `ELEZA_DEBUG_TOOLS`: ON by default for a Debug build (and for a configure without
a build type), OFF for Release / RelWithDebInfo / MinSizeRel on every platform — pass
`-DELEZA_DEBUG_TOOLS=ON` to get the hooks in an optimized build of your own. Nothing that
is shipped has them (`docs/BUILDING.md`, "Debug tools"). With the option off the code is
compiled out; a `--debug-*` / `--gallery` argument prints `… not available in this build
(ignored).` and the app starts normally.

When you add a hook, put it behind the same switch:

```cpp
#ifdef ELEZA_DEBUG_TOOLS
	Window::RegisterRouteHandler(HandleDebugRoute); // and the handler itself
#endif // ELEZA_DEBUG_TOOLS
```

The definition reaches every target (the libraries and the tests too). Routes that start
with `debug/` are refused by `Window::HandleRoute` in a build without the option, whoever
registered them.

In every build:

| Flag | Meaning |
| --- | --- |
| `--api=<url>` / env `ELEZA_API` | API base (default `https://api.eleza.me`): `https://`, or `http://` on the loopback interface; `http://` to another machine only with the debug tools |
| `--workdir=<dir>` | data directory; screenshot runs without it use a temporary one |
| `--lang=fa\|en`, `--theme=light\|dark` | for this run only |
| `--no-single-instance` | allow a second process on the same workdir |
| `--system-title-bar` / env `ELEZA_SYSTEM_TITLE_BAR=1` | the window keeps the title bar of the system for this run, whatever the preference says ("The window frame" in section 3) |
| `--version`, `--help` | print and exit (no display needed) |

Only with `ELEZA_DEBUG_TOOLS`:

| Flag | Meaning |
| --- | --- |
| `--debug-size=<W>x<H>`, `--debug-scale=<f>` | size of the window **body**, device pixel ratio. With the custom title bar the window (and the grab) is taller by the bar (30 px), so the content of a grab is the same with and without it |
| `--debug-title-bar[=<states>]` | the custom title bar also where this run would not have it (Linux with the default preference, the offscreen platform). States for a grab, separated by commas: `inactive`, `maximized`, `dark`, `hover-min\|max\|close`, `press-min\|max\|close` |
| `--debug-login=<phone>:<code>[:<password>]` | signs in through the API at startup when nobody is signed in (no intro UI); exits with code 2 if it fails in a grab run |
| `--debug-route=<route>` | opened once the session layout is shown (section 5; `debug/…` and `intro/…` below) |
| `--debug-actions=<script>` | scripted input, played after the route was opened |
| `--debug-grab=<png>` | save a grab and exit |
| `--debug-grab-delay=<ms>` | wait this long after the route was opened and the actions finished (default 600) |
| `--gallery[=<page>]` | the UI gallery instead of the app |
| `--update-url=<url>`, `--update-key=<base64>` | a test update manifest (http on the loopback interface is accepted) and the public key it is signed with, instead of the built-in ones (`docs/RELEASING.md`, "Testing an update locally") |
| `--update-now` | check at once and install as soon as the update is ready |
| `--update-state=<state>` | the update UI in a state, for screenshots: `checking`, `uptodate`, `available`, `announced`, `downloading`, `preparing`, `ready`, `failed`, `failed-install`, `required`, `required-ready` |
| env `ELEZA_DEBUG_FONTS` | logs the font families that were picked |

A run with `--debug-grab` or `--debug-actions` is *ephemeral*: preferences are not saved, no
tray icon, no sounds, animations jump to their end, the process exits when done. Hover
tooltips stay hidden until the script hovers on purpose (`move:x,y`), so the resting pointer
of a headless platform never puts one into a grab; the window counts as active (an open chat
marks its messages read as it would on a desktop).

### `--debug-actions`

Steps separated by `;` (write `\;` for a literal semicolon). Coordinates are logical pixels
of the window body: a script means the same places with and without the custom title bar,
which lies above `y = 0` (`move:1257,-15` hovers the close button of a 1280 px wide window).

```
click:x,y    rclick:x,y    dclick:x,y    move:x,y
press:x,y    release:x,y   drag:x1,y1,x2,y2
scroll:x,y,dy          wheel at a point; dy > 0 scrolls the content down by dy pixels
type:متن               types into the focused widget
key:Return             Return, Esc, Tab, Backspace, Up, Down, Ctrl+A, Ctrl+V, Ctrl+1 …
paste:text             puts the text on the clipboard and presses Ctrl+V
route:chat/c1          opens a route
resize:520x800         resizes the window (wide <-> single column)
wait:300               milliseconds
grab:/tmp/a.png        saves a grab now and continues
```

Input is injected the way a platform plugin delivers it, so hover, popups, shortcuts and
double clicks behave as with a real mouse and keyboard. Grabs (also the final `--debug-grab`)
include popup menus and other top-level windows of the app at their place over the window.

```sh
# the whole sign-in through the UI, with a grab on the way
… --debug-actions="key:Return;wait:300;type:09121234567;key:Return;wait:700;type:123456;wait:1200;grab:/tmp/in.png"
# a context menu
… --debug-login=+989121234567:123456 --debug-route=chats --debug-actions="wait:800;rclick:900,120;wait:200" --debug-grab=/tmp/menu.png
```

File dialogs cannot be driven by the script: set `ELEZA_DEBUG_PICK="/path/a.jpg;/path/b.png"`
to answer every "open" dialog at once and `ELEZA_DEBUG_SAVE=/path/out.bin` for "save as".

### Debug routes

| Route | Shows |
| --- | --- |
| `intro/welcome\|phone\|country\|code\|password\|recovery\|profile` | an intro step with sample data (signed out) |
| `debug/main[/<n>]` | n placeholder sections pushed in the main column |
| `debug/title[/<states>]` | what the custom title bar shows, the states of `--debug-title-bar` (`debug/title/inactive,hover-close`); without states: back to normal |
| `debug/notification`, `debug/notifications` | one / three sample notification toasts |
| `debug/viewer[?file=<path>][&video=<path>]` | the image viewer with generated pictures (and your file / video) |
| `debug/player[/voice\|/box]` | the «now playing» bar with made-up songs (or a voice message), paused, nothing opened (no audio device needed); `box` opens the music player too |
| `debug/crop`, `debug/about`, `debug/peers`, `debug/peers-multi` | the shared boxes |
| `debug/logout`, `debug/link`, `debug/error`, `debug/upgrade` | sign-out confirmation, external link confirmation, an error toast, the "update required" screen |
| `debug/session-expired` | ends the session as the server would |
| `debug/stories-sample`, `debug/stories/<user>`, `debug/story/<owner>/<story>`, `debug/story-compose?file=<path>`, `debug/story-share/<post>`, `debug/story-send/<story>`, `debug/story-viewers/<story>`, `debug/story-settings` | stories: a made-up tray and stories (no server), the viewer, the boxes («ارسال به…» for one of the sample's stories: `stu10`, `stme0`…), the settings page |
| `debug/calls/<state>` | a voice call in a given state: the bar, the call view, the incoming window (section 12) |
| `debug/dialogs/note-new`, `debug/dialogs/note/<user>` | «نوت جدید», a friend's note (from the list the server sent; the mock API has notes for `u_sara`, `u_nika`, `u_reza`) |

Add your own in your module's route handler (`route.startsWith(u"debug/…")`), inside
`#ifdef ELEZA_DEBUG_TOOLS`.

### Logs

`qInfo` / `qWarning` lines never hold tokens, phone numbers, codes, passwords, message
text, file names or people's names — in any build. Ids, handles and routes are for a build
with the debug tools only:

* an `Api::Error` goes to the log as `error.forLog()` (status, code, request id; with the
  debug tools also the server's message), never `describe()`;
* a request path as `Api::RouteForLog(path)`: the path with the debug tools, otherwise its
  template (`/v1/chats/:id/messages/:id/attachment`, see `Api::RouteTemplate` — a new fixed
  word of the API has to be added to its list or it is logged as `:id`);
* anything else that names a chat, a user, a post or a file (a subject, a local path, the
  text of a system error) goes inside `#ifdef ELEZA_DEBUG_TOOLS`.

## 3. How the app is wired

```
Core::Launcher -> Core::Application                     core/
    owns Core::Settings, Main::Account, Window::MainWindow
    signed out / profile step pending -> the intro   (Intro::Widget, intro/)
    signed in -> Window::MainLayout: nav rail | side column | main column
                 Window::SessionController: navigation, boxes, toasts, session()
                 Support::SessionServices: error toasts, desktop notifications
```

* `Core::App().account()` — `Main::Account` (device, tokens, `api()`, `auth()`,
  `clientConfig()`); `Core::App().session()` — `Main::Session*` or null.
* Inside a section always use `controller()->session()` (a reference, valid for the whole
  life of the section).
* The session ends -> the application destroys the session layout *synchronously* and shows
  the intro (with a toast when the session expired or was revoked). Nothing you own may
  outlive that: parent your objects to your widgets and guard callbacks (section 9).
* `Core::App().setLanguage()` rebuilds the whole window content (the direction changes);
  `setThemeMode()` only repaints — never cache colors, read `st::…` when painting, and use
  `style::OnPaletteChanged(this, …)` to drop prepared pixmaps.
* Window title, taskbar / tray badge and the rail badges (chats unread, Eli notifications)
  are kept by `SessionController` from `Data::Chats::unreadChanged` and
  `Data::Notifications::unreadChanged`.
* `controller->requestLogout()` is the sign-out flow (confirmation box, then
  `account.logout()`).
* `Core::App().updater()` — `Updater::Checker` (state, `info()`, `checkNow()`,
  `download()`; signals `stateChanged`, `progressChanged`); `Core::App().restartToUpdate()`
  is «به‌روزرسانی الزا». It exists signed out as well and never uses the account.

### The window frame

The app draws its own title bar (`Window::TitleBar`, `window/window_title_bar.h`): a 30 px
strip of the nav rail's surface with the title and the minimize / maximize / close buttons.
The buttons are at the physical top right in both interface directions; the title follows
the direction.

* **When.** On Windows by default; on Linux the system frame is the default and the custom
  one is a choice. The preference is `Core::Settings::systemTitleBar()` (تنظیمات → ظاهر و
  زبان → «استفاده از نوار عنوان سیستم»), read once when the window is created — a change
  applies with the next start. `--system-title-bar` and `ELEZA_SYSTEM_TITLE_BAR=1` force
  the system frame for a run (the way back if the custom one misbehaves on some machine).
  `Window::UseCustomTitleBar()` is the one place that decides.
* **The body.** `MainWindow::bodyWidget()` is the area below the bar. Everything that
  covers "the window" — the layer stack and its dimming, toasts, the intro, the image
  viewer, the video pager, the "update required" screen — is a child of the body and never
  covers the bar: the window can always be moved, minimized and closed. Position things
  against `bodyWidget()`, never against the `MainWindow` itself.
* **Dark overlays.** A dark overlay over the whole body calls
  `window->registerDarkOverlay(widget)`: the bar is dark while that widget is shown.
* **Platform halves** (`window/window_title_frame.h`). Windows
  (`window_title_win.cpp`): the window keeps its native styles — shadow, snapping, the
  animations and the taskbar behave as for any window — the non-client area is removed in
  `WM_NCCALCSIZE` and `WM_NCHITTEST` is answered from the bar's geometry. Linux
  (`window_title_frame.cpp`): a frameless window with an invisible resize border; the bar
  starts a system move.
* **The arithmetic** (hit-test zones, the client rectangle of a maximized window, logical
  and physical pixels) is in `window/window_title_hit_test.h`, free of Qt and of the
  platform, and unit-tested: `tests/app/test_title_hit_test.cpp`. Change the layout of the
  bar there, not in the widget.
* Sizes and colors: the `title…` values at the end of `window/window.style`.

## 4. Adding a module

1. Files under `Eleza/SourceFiles/<module>/`, named `<module>_<thing>.h/.cpp`, namespace
   `<Module>::`. CMake picks them up by itself (`Eleza/CMakeLists.txt` globs the module
   folders listed in `eleza_gui_modules`; run `cmake -S . -B build` once after adding files
   if ninja does not notice).
2. One `<module>.style` in the folder (see below) -> `#include "styles/style_<module>.h"`.
3. A `void <Module>::Register();` in `<module>_module.h` that registers tabs and route
   handlers, and one line in `core/core_modules.cpp`.
4. Strings in **both** `Eleza/Resources/langs/lang.strings` (Persian, the source) and
   `lang_en.strings`. Append your keys in one block at the **end** of each file under a
   `// ---- <module> ----` header, with the prefix `lng_<module>_`. (A union merge driver is
   configured for `*.strings`; blocks at the end merge without conflicts.) The lang codegen
   fails the build when a key or a `{tag}` is missing in one of the files.

```cpp
// dialogs/dialogs_module.cpp
void Dialogs::Register() {
	Window::RegisterTab(Window::Tab::Chats, [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(new Dialogs::Widget(parent, controller));
	});
}
```

### Styles

```
// history/history.style
using "ui/basic.style";
using "ui/widgets/widgets.style";

historyBubbleRadius: 16px;
historyTextStyle: TextStyle(defaultTextStyle) { font: font(15px); lineHeight: 22px; }
historySendButton: IconButton(defaultIconButton) {
	icon: icon {{ "send:mirror-ltr", brandPrimary }};
	iconOver: icon {{ "send:mirror-ltr", brandPressed }};
}
```

* Value names are global (`st::historyBubbleRadius`): prefix them with the module name.
* Colors come from `Eleza/Resources/colors.palette` (light | dark); do not add literals for
  things that must follow the theme.
* Icons are `Eleza/Resources/icons/<name>.svg`; `:mirror-ltr` / `:mirror-rtl` flip
  directional glyphs. A nested struct value must be a named value (define the
  `RippleAnimation` separately and refer to it).

### Strings

```cpp
#include "lang_auto.h"
tr::lng_settings(tr::now)
tr::lng_intro_code_sent_to(tr::now, lt_phone, text)       // {phone}
tr::lng_eli_new_posts(tr::now, lt_count, 5)               // "#one" / "#other" + localized digits
Lang::Num(42), Lang::Clock(time), Lang::ListTime(dateTime), Lang::Duration(seconds), Lang::Bytes(n)
Lang::Handle(handle), Lang::Phone(e164), Lang::Ltr(text), Lang::Isolated(name)   // section 10
```

No user-visible literal in code, ever — including "…" suffixes and separators that differ
between the languages. In English nothing Persian may appear (and the reverse where it
matters): take names and wording from `tr::`.

## 5. Sections, tabs, routes

`Window::SectionWidget` is the base of everything in the columns (see its header for the
skeleton). The controller API (`window/window_session_controller.h`):

```cpp
controller->showTab(Window::Tab::Chats);
controller->showMainSection(factory);                               // push on the tab's stack
controller->showMainSection(factory, { .stack = false });           // replace the stack
controller->showMainSection(factory, { .tab = Window::Tab::Eli });   // in another tab
controller->showBack();   controller->clearMainStack();
controller->mainSection();  controller->isSingleColumn();
controller->openRoute(Window::Routes::Chat(chatId));
```

A section whose `sectionKey()` equals the current one's is not pushed twice. Call
`setTopBar(bar)` and the back button / Esc work by themselves.

`showTab(tab)` behaves like a click on the rail: on the current tab it returns to the tab's
list. Use `activateTab(tab)` when the tab only has to be current and what it shows must stay
(a shortcut that focuses the chat search while a chat is open).

What a section can tell the window (all optional, `window/window_section_widget.h`):

| Override | For |
| --- | --- |
| `onBackRequest()` | Esc / back closes something inside the section first |
| `handleCommand(Window::Command)` | the window shortcuts `Find`, `Refresh`… (section 5a) |
| `toastBottomSkip()` | the height of a composer / bottom bar: toasts are lifted above it |
| `restoreRoute()` | the route that reopens the section after the language was switched (the window content is rebuilt, then every tab's stack is reopened from these; the default is `sectionKey()`, return an empty string to opt out) |

### 5a. Keyboard shortcuts

One table for the whole window: `window/window_shortcuts.h`. A module **never** creates a
window-wide `QShortcut` (two shortcuts on one sequence are ambiguous in Qt and neither
fires); keys that only mean something inside one widget stay in its `keyPressEvent`.

| Keys | Command | Who handles it |
| --- | --- | --- |
| `Ctrl+1` … `Ctrl+5` | the tabs: الی, گفتگو, جستجو, من, تنظیمات | window |
| `Ctrl+,` | settings | window |
| `Ctrl+N` | new message | dialogs (window-wide handler) |
| `Ctrl+K` | search the chats (from any tab; an open chat stays) | dialogs |
| `Ctrl+F` | search in what is on screen | the focused section, then the other visible one: the chat, the chat list, the search tab |
| `F5`, `Ctrl+R` | refresh the visible list | the feed, the chat list |
| `Ctrl+Tab` / `Ctrl+Shift+Tab`, `Ctrl+PgDown` / `Ctrl+PgUp`, `Alt+Down` / `Alt+Up` | next / previous chat | dialogs |
| `Esc`, `Alt+Left`, mouse "back" | back: closes a box, then asks the section (`onBackRequest`), then pops it | window |
| `Ctrl+W` | closes the top box, else the current section, else the window | window |
| `Ctrl+Shift+D` | light / dark | window |
| `Ctrl+Q` | quit | window |

A command the window does not do itself goes to `handleCommand()` of the section that has
the keyboard focus, then of the other visible one, then to the handlers registered with
`Window::RegisterCommandHandler(command, [](Window::SessionController *controller) { …; return true; })`.
While a box is shown only back / close / theme / quit work.

**Esc is handled once.** A field (`InputField`, `MaskedInputField`, `CodeInput`) gives the
key to whoever listens to its `cancelled()` signal and then the key stops there; with no
listener it travels on to the parents and ends in the window's "back". `SearchField` clears
its text on the first Esc and only then reports `cancelled()` (or passes the key on). So:
either connect `cancelled()` and do everything Esc means there, or do not connect it and
handle the key in `keyPressEvent` / `onBackRequest()` — never both.

### 5b. Hooks between modules

`window/window_embeds.h` holds the small registries modules use to plug into each other
without including each other's headers:

* **Embedded lists** — `Window::RegisterEmbed("eli.posts", factory)` /
  `Window::CreateEmbed(…)`: the feed's post list inside a profile or the search results.
* **Side sub-tabs** — `Window::RegisterSideSubTab(Window::Tab::Chats, { .id = "topics", … })`;
  `controller->requestSideSubTab(tab, id)` selects one (the route `conversation/topics` does
  it; `conversation/topics?new=<text>` also opens «گفتگوی تازه» prefilled).
* **Chat menu fillers** — `Window::RegisterChatMenuFiller(fn)`; the chat list's row menu and
  the chat's «…» menu call `Window::FillChatMenu(menu, controller, chatId)` so the owner of a
  kind of chat adds its actions in both («پایان گفتگو», «مسدود کردن» of a conversation).
* **Calls** — `window/window_calls.h`: `Window::CallsSupported()` and
  `Window::StartCall(controller, userId)`; the calls module registers behind them (section 12).
* **Reports** — one form for posts, replies, accounts and conversation topics:
  `Boxes::ShowReportBox(controller, args)` (`boxes/report_box.h`).

### Routes

One string per place (`window/window_routes.h`). Never build them by hand: use
`Window::Routes::…`, and parse with `Window::ParseRoute()` (handlers get the parsed form).

| Route | Builder | Tab | Owner |
| --- | --- | --- | --- |
| `chats` | `Routes::Chats()` | Chats | dialogs |
| `chat/<chatId>[?seq=N]` | `Routes::Chat(id, seq)` | Chats | history |
| `chat/<chatId>?start=<payload>` | `Routes::BotStart(id, payload)` | Chats | history (a bot's chat with «شروع» waiting) |
| `chat/<chatId>?show=media` | `Routes::ChatMedia(id)` | current | media (the shared media of a direct chat; a group's is `group/<id>?show=media`, the same page) |
| `user/<userId>` | `Routes::User(id)` | current | profile |
| `u/<handle>` | `Routes::Handle(handle)` | current | profile |
| `u/<handle>?start=<payload>` | `Routes::HandleStart(handle, payload)` | Chats | profile (a bot's deep link `eleza.me/<bot>?start=…`: resolves the handle, opens the chat with «شروع»; a person's profile otherwise) |
| `eli` | `Routes::Eli()` | Eli | eli |
| `eli/post/<postId>[?root=<id>]` | `Routes::Post(id, rootHint)` | Eli | eli (a comment opens under its post, its thread first and lit up; `root`: the post at the top of its conversation as a notification named it, read at the same time) |
| `eli/share/<slug>` | `Routes::SharedPost(slug)` | Eli | eli (resolves the share link, then opens the post) |
| `eli/topic/<slug>` | `Routes::Topic(slug)` | Eli | eli |
| `eli/notifications` | `Routes::EliNotifications()` | Eli | eli |
| `search[?q=text]` | `Routes::Search(query)` | Search | search |
| `search/people` | — | Search | search («دیدن همه» of the people you may know) |
| `me` | `Routes::Me()` | Me | profile |
| `settings[/<section>]` | `Routes::Settings(section)` | Settings | settings |
| `conversation/topics` | `Routes::ConversationTopics()` | Chats | conversation |
| `conversation/<id>` | `Routes::Conversation(id)` | Chats | conversation |
| `group/new` | `Routes::NewGroup()` | Chats | groups («گروه تازه») |
| `group/<groupId>` | `Routes::Group(id)` | Chats | groups (the info screen; the chat of a group is `chat/<groupId>`) |
| `g/<token>` | `Routes::GroupInvite(token)` | Chats | groups (the box of an invite link) |

```cpp
Window::RegisterRouteHandler(Window::RouteType::Chat, [](
		Window::SessionController *controller,
		const Window::Route &route) {
	controller->showMainSection(
		History::Factory(route.id, route.seq()),
		{ .tab = Window::Tab::Chats });
	return true;
});
```

Until a handler is registered the route opens a placeholder section in the right tab, so
you can call `openRoute()` for another module's screen today. Raw handlers
(`RegisterRouteHandler([](controller, const QString &route) { … })`) see every string
first and are for `debug/…` routes.

Links: `Support::OpenLink(controller, url)` (`support/links.h`) turns
`https://eleza.me/u/sara`, `https://eleza.me/@sara`, `https://eleza.me/p/<post>`,
`https://eleza.me/s/<share slug>`, `https://eleza.me/g/<invite token>`, `eleza://chat/c1`,
`@sara`, `#tag` into routes, asks before
opening a foreign site in the browser, and refuses anything that is not http(s) / mailto /
tel. For rich text: `label->setLinkHandler(Support::LinkHandler(controller))`, or
`Support::ActivateTextLink(controller, text.link(index))` in custom painted text.

## 6. Widgets cheat-sheet

All in `lib/ui/ui/**`; every header starts with an example. The gallery (`--gallery`,
`gallery/gallery_pages.cpp`) shows each widget in use.

* **Layout**: no Qt layouts. Place children in `resizeEvent` with `moveToLeft` /
  `moveToRight` (leading / trailing side, RTL-aware) or stack rows in `Ui::VerticalLayout`.
  `resizeToWidth(w)` asks a widget for its height.
* **Buttons** (`ui/widgets/buttons.h`): `RoundButton` (`st::defaultRoundButton`,
  `lightRoundButton`, `dangerRoundButton`, `outlineRoundButton`, `setLoading(true)`),
  `IconButton`, `LinkButton`, `SettingsButton` (rows with value / chevron / toggle),
  `PillButton`.
* **Inputs** (`ui/widgets/input_fields.h`): `InputField` (single / multi-line, `changed`,
  `submitted`, `cancelled`), `MaskedInputField`, `PasswordInput`, `PhoneInput`, `CodeInput`,
  `SearchField`.
* **Text**: keep one `Ui::Text::String` per painted item (`setText` / `setMarkedText` with
  entities in UTF-16 units), `countHeight(width)`, `draw` / `drawLeftElided`, `getState` for
  link hit-testing. `Ui::FlatLabel` wraps it as a widget (selectable, links).
* **Emoji** (`ui/emoji/*.h`): `Ui::Text::String` and `Ui::InputField` draw Telegram's emoji
  (the pack's pictures) by themselves; text painted with `QPainter::drawText` keeps the
  fonts' emoji. One emoji in a cell / chip: `Ui::Emoji::PaintOne(p, emoji, rect, size,
  fallbackFont)` (and `OneWidth` to measure it). Pictures are decoded on a pool thread
  when first drawn; a widget that drew a missing one is repainted, a picture painted into
  an image needs `Ui::Emoji::OnUpdated(guard, callback)`.
* **Lists**: `Ui::ScrollArea` + `Ui::ListWidget` with your `Ui::ListDelegate`
  (`rowsCount`, `rowHeight`, `paintRow`); the list paints hover / active / ripple and gives
  `rowClicked`, `rowContextMenu`, keyboard selection. `scroll->nearBottom` for pagination.
  `Ui::SkeletonList` while loading, `Ui::EmptyPlaceholder` for empty / error states.
* **Menus**: `new Ui::PopupMenu(this)` + `addAction(text, callback, &st::menuIcon…)` +
  `popup(globalPos)`; `Ui::DropdownMenu` for "more" buttons inside the window.
* **Boxes**: `controller->show(Ui::Box<MyBox>(args…))` for a `Ui::BoxContent` subclass,
  `Ui::Box([=](Ui::GenericBox *box) { … })` built in place,
  `Ui::MakeConfirmBox({ .text = …, .confirmText = …, .confirmed = [=](auto close) { … close(); }, .destructive = true })`,
  `controller->showSheet(…)` for the side sheet.
* **Toasts**: `controller->showToast(text)` (lifted above the visible section's bottom
  controls, see `SectionWidget::toastBottomSkip()`); an undo toast is a `Ui::Toast::Config`
  with `.action = tr::lng_toast_undo(tr::now)` and `.actionClicked`.
* **Top bar**: `Ui::TopBar` (`setTitle`, `setSubtitle(text, accent)`, `addAction(st)`,
  `setLeadingWidget`).

### Shared helpers (`support/`, `boxes/`, `media/`)

| Need | Use |
| --- | --- |
| An image from the media cache in a widget | `Support::MakeImageSource(&session, ref, variant, { .blurhash = …, .aspect = … })` -> `Ui::ImageView::setSource` |
| A photo / video poster of a message | `Support::MakeMessageImageSource(&session, message)` |
| A userpic widget bound to a user | `Support::UserpicButton(parent, &session, userId, size)`, `setShowOnline(true)`, `setSpecial(kind, name, id)`, `setGroup(groupId, photoId, title)` |
| Userpics in a custom painted list | `Support::UserpicPainter _userpics{ &session, [=](const QString &userId) { … update rows … } };` then `_userpics.paint(p, rect, userId)` / `paintGroup(p, rect, groupId, photoId, title)` / `paintSpecial(…)` |
| Chat title / one-line message preview | `Support::ChatTitle(&session, chat)`, `Support::MessagePreview(message)`; with names in the notes of a group: `Support::MessagePreview(&session, message)`, `Support::MemberName(&session, userId)` (`support/chat_text.h`) |
| An error to the user | `Support::ErrorText(error, fallback)` for inline text, `Support::ShowError(controller, error[, fallback])` for a toast |
| «در حال اتصال…» in a top bar | `new Support::ConnectionTitle(bar, &session, title)` (or `Mode::Subtitle` for a chat header); `Support::ConnectionState::For(&session)` for the raw state |
| Open a link / mention / hashtag | `Support::OpenLink`, `Support::LinkHandler` |
| Pick / save files, paste, drop | `Support::PickFiles`, `PickImage`, `SaveFileAs`, `SaveImageAs`, `ReadMimeData`, `CanAcceptDrop`, `SaveImageToTemp` (`support/file_dialogs.h`) |
| The brand mark | `Support::PaintBrandMark(p, rect)` |
| A searchable list of people / chats | `Boxes::PeerListBox` with your `Boxes::PeerListController` (a button per row: `PeerListRow::action` + `rowActionClicked`; a line of text: `note`); ready-made `Boxes::ChooseChatsBox(&session, args, done)` |
| A round photo crop | `Boxes::PickAndCropPhoto(parent, show, done)`, `Boxes::SaveCroppedPhoto(image, guard, done)` |
| Country list, about | `Boxes::CountrySelectBox`, `Boxes::AboutBox` |
| «گزارش» (post / reply / account / conversation / group / a message of a group) | `Boxes::ShowReportBox(controller, args)` |
| Shared media of a chat or a group (tabs with counts, server pages) | `controller->openRoute(Window::Routes::ChatMedia(chatId))` / `GroupMedia(groupId)` (`media/media_shared.h`); the data side: `data/data_shared_media.h` |
| Full-window photo / video viewer | `Media::ShowImageViewer(controller->window(), items, index, options)`; `options.paging` pages past the items handed (by `ViewerItem::key`), `options.origin` gives the item's place on screen (it grows out of it and closes into it) |
| Voice / music playback | `Media::Player().toggle(Media::AudioTrack::ForMessage(message), &session[, queue])`; signals `stateChanged`, `positionChanged`, `queueChanged`, `failed(id, reason)`; `next()` / `previous()`, `cycleSpeed()`, repeat / shuffle / reverse. The «now playing» bar and the music player are `media/media_player_widgets.h`; `Media::OpenTrackPlace(controller, track)` goes to a track's message (a chat open further back comes to the front: `controller->showExistingSection(key)`) |
| Inline video | `Media::VideoPlayerWidget` (`setSource(localPath)`, `setPoster`, `play`) |

Failed fire-and-forget actions of the stores (`Data::Session::actionFailed`) already show a
toast (`Support::ErrorReporter`); do not subscribe to it again. Desktop notifications for
incoming messages are shown by `Support::NotificationsManager`; the history screen only has
to keep `Data::Chats::setVisibleChat(id)` correct (set it while a chat is on screen and the
window shows it, clear it when leaving).

Preferences that exist already in `Core::Settings` (`Core::App().settings()`):
`notifications()` (`desktopEnabled`, `soundEnabled`, `showPreview` = «نمایش متن پیام»,
`showSender`, `onlyWhenInactive`, `flashTaskbar`, `countUnreadMuted`), `closeToTray`,
`themeMode`, `language`, `sideColumnWidth`, `lastTab`, `playbackVolume`,
`voicePlaybackSpeed`, `autoUpdate`, `systemTitleBar` (applies with the next start). Change them
through the setters (they save themselves); language and theme through
`Core::App().setLanguage / setThemeMode`.

## 7. The core API

`auto &data = controller->session().data();` — `Data::Session` owns the stores. Models are
plain structs in `data/data_types.h` (`User`, `Profile`, `Chat`, `ChatInfo`, `Message`,
`Draft`, `Presence`…) and `data/data_social_types.h` (`Post`, `FeedPage`, `GraphState`,
`NotificationEntry`…). `FailCallback` is `Fn<void(const Api::Error &)>`; `base::Guard` is
constructed from a `QObject*` (pass `this`).

**Rule: no polling.** Read the store, fetch at most once what it does not hold, then live
on signals.

```cpp
// ---- session (main/main_session.h)
session.userId();  session.isReady();  session.connectionState();   // Rt::State
session.api().request(Api::Get, u"/v1/…"_s).query(u"q"_s, text).guard(this)
	.done([=](const QJsonObject &result) { … })
	.fail([=](const Api::Error &error) { … }).send();
session.rpc(u"chat.pins"_s).body({ { u"chat"_s, id } }).guard(this).done(…).fail(…).send();
session.account().clientConfig().maxMessageLength();     // limits and feature flags

// ---- users (data/data_users.h)
User Users::get(const QString &id);            // the card, or queues a batched fetch -> changed(ids)
User Users::peek(const QString &id) const;     // never fetches
void Users::ensure(const QStringList &ids, base::Guard guard, Fn<void()> done);
void Users::seed(const QVector<User> &users);
const Profile &Users::me() const;   QString Users::myId() const;
void Users::refreshMe(base::Guard guard = {}, Fn<void(const Profile &)> done = nullptr, Fn<void(const Api::Error &)> fail = nullptr);
signals: changed(const QStringList &ids), meChanged()

// ---- chats (data/data_chats.h)
QVector<Chat> Chats::list(Section section = Section::Messages) const;   // Messages | Conversations | Requests | Archived | All
const Chat *Chats::chat(const QString &id) const;        // valid until the list changes
int Chats::unreadChats() const;   int Chats::unreadRequests() const;
bool Chats::inboxLoading() const;  bool Chats::inboxLoadedOnce() const;  const std::optional<Api::Error> &Chats::inboxError() const;
void Chats::loadInbox(base::Guard guard = {}, Fn<void()> done = nullptr, Fn<void(const Api::Error &)> fail = nullptr);
void Chats::openDirect(const QString &userId, base::Guard guard, Fn<void(const Chat &)> done, Fn<void(const Api::Error &)> fail = nullptr);
void Chats::openSaved(base::Guard guard, Fn<void(const Chat &)> done, Fn<void(const Api::Error &)> fail = nullptr);
void Chats::setVisibleChat(const QString &id);
void Chats::setMuted(chat, bool);  muteFor(chat, "1h"|"8h"|"2d"|"forever"|"off");  setArchived(chat, bool);  setPinned(chat, bool);
void Chats::deleteChat(const QString &chat, bool forEveryone);  acceptRequest(chat);  declineRequest(chat);
void Chats::saveDraft(const QString &chat, const Draft &draft);  clearDraft(chat);
void Chats::sendTyping(const QString &chat, const QString &kind = u"typing"_s);
std::optional<Activity> Chats::activity(const QString &chat) const;
void Chats::watchPresence(const QStringList &users);  unwatchPresence(users);  std::optional<Presence> presence(user) const;
signals: listChanged(), chatChanged(id), chatRemoved(id), unreadChanged(), inboxLoadingChanged(), activityChanged(chat), presenceChanged(user)

// ---- messages (data/data_messages.h)
const History &Messages::history(const QString &chat);   // items, loaded, hasMoreBefore, hasMoreAfter, info, pins
void Messages::loadCached(const QString &chat, base::Guard guard, Fn<void(int)> done);
void Messages::loadLatest(const QString &chat, base::Guard guard = {}, Fn<void()> done = nullptr, Fn<void(const Api::Error &)> fail = nullptr);
void Messages::loadOlder(const QString &chat, base::Guard guard, Fn<void(int)> done, Fn<void(const Api::Error &)> fail = nullptr);
void Messages::loadNewer(…same…);
void Messages::loadAround(const QString &chat, qint64 seq, base::Guard guard, Fn<void()> done, Fn<void(const Api::Error &)> fail = nullptr);
void Messages::markRead(const QString &chat, qint64 seq);
void Messages::react(const QString &chat, qint64 seq, const QString &emoji);
void Messages::edit(const QString &chat, qint64 seq, const QString &text, const QVector<Entity> &entities = {}, base::Guard guard = {}, Fn<void()> done = nullptr, Fn<void(const Api::Error &)> fail = nullptr);
void Messages::deleteMessage(const QString &chat, qint64 seq, bool forEveryone);
void Messages::pin(chat, seq, scope = "both");  unpin(…);  loadPins(chat, guard = {}, done = nullptr);
void Messages::markPlayed(const QString &chat, qint64 seq);
void Messages::pressButton(chat, seq, row, column, guard, Fn<void(const CallbackAnswer &)> done, fail = nullptr);
signals: historyChanged(chat), pinsChanged(chat)

// ---- outbox (data/data_outbox.h): every send returns the client message id
QString Outbox::sendText(const QString &chat, const QString &text, const TextOptions &options = {});   // replyToSeq, entities, quote, linkPreview
QString Outbox::sendFile(const QString &chat, const QString &filePath, const FileOptions &options = {}); // caption, asVoice, kind, durationMs, spoiler…
QString Outbox::sendSticker(const QString &chat, const QString &stickerId, qint64 replyToSeq = 0);
QString Outbox::sendLocation(chat, const Location &location, qint64 replyToSeq = 0);
QString Outbox::sendPost(const QString &chat, const QString &postId);
QString Outbox::forward(const QString &toChat, const Message &source);
void Outbox::retry(const QString &clientMessageId);  discard(clientMessageId);
signals: sent(chat, clientMessageId, seq), failed(chat, clientMessageId, error)
// A queued message appears in Messages::history at once (Message::local(), status, uploadProgress).

// ---- media (data/data_media.h) — prefer the Support:: adapters
MediaHandlePtr Media::request(const MediaRef &ref, MediaVariant variant = MediaVariant::Thumb);
MediaHandlePtr Media::request(const MediaRef &ref, MediaVariant variant, const Options &options);  // decode, fit, force
QImage Media::cached(const MediaRef &ref, MediaVariant variant, QSize fit = QSize()) const;
QImage Media::picture(const MediaRef &ref, MediaVariant variant = MediaVariant::Thumb);
// ^ a person's or a group's picture ready to paint NOW: from the pictures' own store (nothing
//   else evicts it) or decoded on the spot from its small cached file. Null only when it was
//   never fetched. Support::UserpicPainter / UserpicButton use it, so a face that was seen
//   once never flashes its initial again; paint userpics through them.
MediaRef::Avatar(userId, avatarId) | Attachment(chat, seq) | PostMedia(postId, mediaId) | Url(url)
        | GroupPhoto(groupId, photoId) | InvitePhoto(inviteToken, photoId)
MediaVariant::Thumb | Feed | Full | Poster | Mp4 | Audio | File | Cover
MediaHandle: isReady(), isFailed(), image(), filePath(), progress(); signals ready(), failed(), progressChanged()
// Keep the handle while the media is on screen.
// Options::expectedBytes: the size the message declares for the bytes (the File variant). The
// download stops with the error code "too_large" beyond Media::DownloadLimit(expectedBytes)
// = max(declared × 1.05, declared + 64 KiB); nothing declared: the global caps.

// ---- pictures from files (data/data_image_decode.h): the only decoder for anything the user
// attached, pasted, dropped or received. JPEG / PNG / GIF / WebP by their first bytes, size caps.
// Never QImage(path) / QImageReader(path) / QPixmap(path) on such a file (":/…" resources are ours).
QImage Data::DecodeImageFile(const QString &path, int maxSide = 0);   // any thread; null: not a picture
QSize Data::ImageFileDimensions(const QString &path);                 // header only, orientation applied
bool Data::IsSupportedImageFile(const QString &path);                 // false: attach it as a file
// Support::IsImageFile(path) (the name and the content) and Upload::Probe(path).kind already use it.

// ---- upload (data/data_upload.h) — chat attachments go through Outbox::sendFile
Upload::Id Upload::start(const UploadRequest &request, base::Guard guard, UploadCallbacks callbacks);  // filePath, kind, purpose
void Upload::cancel(Id id);   static FileProbe Upload::Probe(const QString &path, bool asVoice = false);

// ---- feed (data/data_feed.h)
void Feed::loadFeed(Tab tab, const QString &cursor, base::Guard guard, Fn<void(const FeedPage &)> done, FailCallback fail = nullptr);  // ForYou | Following
const Post *Feed::post(const QString &id) const;
void Feed::fetchPost(const QString &id, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail = nullptr);
void Feed::createPost(const CreatePost &body, const QString &key, base::Guard guard, Fn<void(const Post &)> done, FailCallback fail = nullptr);
void Feed::react(const QString &id, Reaction reaction);  repost(id, bool);  bookmark(id, bool);  vote(id, option, …);
void Feed::replies(const QString &root, const QString &cursor, base::Guard guard, Fn<void(const RepliesPage &)> done, FailCallback fail = nullptr);
void Feed::topic(slug, guard, done, fail);  topicPosts(slug, tab, before, guard, done, fail);  trending(guard, done, fail);
void Feed::bookmarks(cursor, guard, done, fail);  userPosts(userId, tab, before, kind, guard, done, fail);
void Feed::userTimeline(userId, cursor, cursorIsBefore, guard, done, fail);   // posts tab with reposts (PostsPage::repostedBy)
void Feed::reportViews(const QStringList &ids);  sendReport(targetKind, target, reason, note, …);
signals: postChanged(id), postPublished(id), postRemoved(id), authorBlocked(userId), newPosts(count, since)

// ---- graph (data/data_graph.h): profiles, follow, block, search, settings, devices
void Graph::profileByHandle(const QString &handle, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
void Graph::profile(const QString &userId, …same…);
void Graph::follow / unfollow / block / unblock(const QString &userId, base::Guard guard = {}, Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
void Graph::followers / following(const QString &userId, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail = nullptr);
void Graph::search(const QString &query, const QString &type, int offset, base::Guard guard, Fn<void(const SearchResults &)> done, FailCallback fail = nullptr);
void Graph::searchPeople(const QString &query, base::Guard guard, Fn<void(const QVector<Profile> &)> done);
void Graph::suggestions(cursor, limit, guard, done, fail);  dismissSuggestion(userId);   // signal suggestionDismissed(userId)
void Graph::updateProfile(const ProfilePatch &patch, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
void Graph::loadSettings(bool force, base::Guard guard, Fn<void(const QJsonObject &)> done, FailCallback fail = nullptr);
QJsonValue Graph::settingValue(const QString &dotted) const;  void Graph::setSetting(const QString &dotted, const QJsonValue &value);
void Graph::sessions(guard, done, fail);  revokeSession(id, …);  revokeOtherSessions(…);
void Graph::latestExport(guard, done, fail);  requestExportCode(deviceId, guard, done, fail);  requestExport(challengeId, code, deviceId, guard, done, fail);
signals: settingsChanged(), relationshipChanged(userId, state)

// ---- conversations (data/data_conversations.h)
ConversationConfig Conversations::config() const;   QString categoryTitle(const QString &slug) const;
void Conversations::loadOpen(const QString &category, const QString &cursor, base::Guard guard, Fn<void(const OpenConversationsPage &)> done, FailCallback fail = nullptr);
void Conversations::watchOpen(const QString &category);  unwatchOpen();
void Conversations::create(const NewConversation &request, base::Guard guard, Fn<void(const Conversation &)> done, FailCallback fail = nullptr);
void Conversations::join(const QString &id, bool masked, base::Guard guard, Fn<void(const Conversation &)> done, FailCallback fail = nullptr);
void Conversations::end(id, …);  requestContinue(id, guard, done, fail);  block(id, …);   // end / block mark the chat row ended at once
void Conversations::findChat(const QString &conversationId, base::Guard guard, Fn<void(const QString &)> done, int timeoutMs = 6000);
signals: stateChanged(chat, state, event), openListChanged(category)

// ---- stories (data/data_stories.h): rt `stories.*`, media through MediaRef::Story
bool Stories::enabled() const;   // client-config `feature.stories`
const StoryTrayMe &me() const;  const QVector<StoryTrayItem> &tray() const;  refreshTray();  loadMoreTray();
StoryRing ring(userId) const;   // tray > loaded stories > feed author hints; ringsChanged(ids)
void loadUser(userId, guard, done(const UserStories &), fail);  markSeen(storyId);  flushSeen();
react / unreact / reply / viewers / deleteStory / create / post(PostStoryRequest) / setMuted / hidden / setHidden
closeFriends / setCloseFriend / updateCloseFriends / settings / updateSettings / setAllowSharing
void share(storyId, recipients, text, guard, done(const StoryShareResult &), fail);   // «ارسال به…»; shareCap()
void get(storyId, guard, done(const User &owner, const Story &), fail);   // one story by id, decided for me
markGone(storyId);  isGone(storyId);   // a 404 of the story or of its thumbnail, for this session
signals: trayChanged(), ringsChanged(ids), userStoriesChanged(userId), postingChanged(), posted(story), storyGone(id)
// UI: Stories::OpenUserStories(controller, userId, queue), Stories::OpenStory, Stories::OpenStoryFromChat
// (a chat message's story: stories.get first), Stories::ShowComposeBox, Stories::ShareToStory,
// Stories::ShowShareBox («ارسال به…»), Stories::PaintRing (stories/).
// Chat messages: story_reply / story_reaction / story_share carry Message::story (StoryRef); a
// story_share's Message::text is the sender's comment (never the fallback line for older apps);
// StoryRef::gone(now) is the rule «استوری دیگر در دسترس نیست» starts from.

// ---- notes (data/data_notes.h; HTTP only: GET /v1/notes, PUT|DELETE /v1/me/note)
bool Notes::enabled() const;   // client-config `feature.notes` (absent: on), no 404 for the list
int maxChars() const;   qint64 expirySeconds() const;   // notes.max_chars, notes.expiry
const NotesFeed &feed() const;  const Note *mine() const;  bool loaded() const;
void refresh();   // at most once a minute (the chat list shows, the tab comes back); reload(): F5
void set(text, audience, guard, done(const Note &), fail);  remove(guard, done, fail);
void reply(userId, noteText, answer, guard, done(const QString &chat), fail);  // the 1:1 chat, NoteReply::Build
signals: changed(), enabledChanged()
// No realtime: the list lives on what this app changes; a note leaves the feed at its expiry.
// The rules (length in code points, one line, the reply message «↩︎ «note»\nanswer» and the old
// «↩︎ نوتت: «…»» first line) are plain functions: data_note_types.h (tests/core/test_notes.cpp).
// UI: Dialogs::NotesRow / NotesFaces (dialogs/dialogs_notes.h), the boxes in dialogs_notes_box.h,
// History::View::NoteReplyView (a note reply drawn above its bubble), Support::MessagePreview.

// ---- notifications (data/data_notifications.h)
int Notifications::unread() const;   int kind(const QString &name) const;
void Notifications::load(const QString &tab, qint64 before, base::Guard guard, Fn<void(const QVector<NotificationEntry> &)> done, FailCallback fail = nullptr);  // "" | "mentions" | "requests"
void Notifications::markAllRead();        signal: unreadChanged()

// ---- stickers (data/data_stickers.h)
void Stickers::load(base::Guard guard = {}, Fn<void()> done = nullptr);
const QVector<StickerPack> &Stickers::packs() const;  QVector<StickerPack> installedPacks() const;  const QVector<Sticker> &recent() const;
void Stickers::setInstalled(const QString &packId, bool installed);  noteUsed(const Sticker &sticker);     signal: changed()

// ---- bots (data/data_bots.h; HTTP only)
const BotInfo *Bots::info(const QString &botId) const;     // GET /v1/bots/{id}, cached (refreshed after 10 min)
void Bots::load(botId, guard, Fn<void(const BotInfo &)> done, FailCallback fail = nullptr);
HandleState Bots::handleState(handle) const;  const BotInfo *byHandle(handle) const;  void resolve(handle, guard, done);
void Bots::inlineQuery(botId, query, offset, guard, Fn<void(const InlineResults &)> done, FailCallback fail = nullptr);
QString Outbox::sendInlineResult(chat, bot, queryId, resultId, previewText = {}, replyToSeq = 0);   // inline_result
signal: changed(botId)
// The chat screen's rules for bots (inline queries, commands, «شروع», buttons) are plain
// functions in history/history_bot_logic.h (tests/app/test_history_bots.cpp).

// ---- sync (data/data_sync.h)
bool Sync::updating() const;     signals: updatingChanged(bool), messageReceived(const Data::Message &)

// ---- groups (data/data_groups.h; section 13)
bool Groups::enabled() const;          // client-config `feature.groups`
bool Groups::administration() const;   // the second release: admins, links, pins, mentions
const Group *Groups::group(const QString &id) const;   // the header of a group in the list
void Groups::create(request, guard, done, fail);   leave / deleteGroup / clearHistory / setNotify / edit / transfer
void Groups::loadMembers(group, filter, cursor, guard, done, fail);   addMembers / removeMember / searchMembers
void Groups::setAdmin / unsetAdmin / restrict / ban / unban / setPermissions / deleteMessages
void Groups::loadInvites / createInvite / revokeInvite / invitePreview / joinByInvite / loadJoinRequests / decideJoinRequest
signals: groupRemoved(group, status), membersChanged(group), messageReceived(message, addressesMe), enabledChanged()
// A group is a row of Data::Chats (Chat::isGroup, Chat::group = the header) and its messages
// are in Data::Messages under the group's id: Data::IsGroupId(id) routes every call.
```

Errors: `Api::Error { status, code, message, retryable, retryAfter, requestId }` with
`offline()`, `notFound()`, `rateLimited()`, `serverSide()`. Show them through
`Support::ErrorText` / `Support::ShowError`; add a new well-known code to
`Support::ErrorPhrase` (`support/error_text.cpp`) instead of comparing codes in screens.

## 8. Threading

Everything runs on the GUI thread (the audio of a voice call is the one long-lived exception:
section 12). Never block it: no `waitFor…`, no nested event loops
(`exec()` of a dialog included — use `Support::PickFiles` and boxes), no synchronous file
reads of user files, no image decoding of anything larger than an icon. For heavy work use
`QThreadPool::globalInstance()->start(…)` and come back with
`QMetaObject::invokeMethod(qApp, …, Qt::QueuedConnection)` guarded by a `QPointer`. The
stores already decode images, hash uploads and write the database off the GUI thread.

## 9. Lifetime of callbacks

* Every request takes a guard: `.guard(this)` / a `base::Guard(this)` argument. The
  callback is dropped when the guard object dies. Use the widget that reacts as the guard.
* Timers: `Ui::CallDelayed(ms, this, fn)`, `Ui::PostponeCall(this, fn)`, `base::Timer`.
* A callback that may destroy its own widget (closing a box, switching sections, signing
  out) must do that last, or defer it with `Ui::PostponeCall`.
* `const Data::Chat*` / `const Data::Post*` from a store are valid only until the store
  changes: copy what you keep.

## 10. RTL rules

* Persian is the primary language: design for RTL first, then check LTR.
* "Left" in our APIs means the *leading* side: `moveToLeft`, `Painter::drawTextLeft`,
  `icon.paint(p, x, y, outerWidth)`, `style::rtlrect(...)`, margins in `.style`. Use them and
  never branch on `style::RightToLeft()` for ordinary layout.
* What is always left-to-right: phone numbers, codes, handles, URLs, timelines of media
  players, zoom sliders. Force it (`setForcedDirection(Qt::LeftToRight)`) and isolate such
  text with the helpers of `lang/lang_format.h` — do not concatenate `'@'` or a `+…` number
  into a label by hand:
  `Lang::Handle(handle)` -> `@sara`, `Lang::Phone(e164)` -> `+98 912 123 4567` (grouped,
  digits of the interface language), `Lang::Ltr(text)` for addresses / e-mails / codes
  (U+2066 … U+2069), `Lang::Isolated(name)` for a name inside a sentence (U+2068 … U+2069:
  it keeps its own direction and does not reorder its neighbours — isolate the *name*, not
  the phrase around it). Never a raw control character in the source.
* Message / post text aligns by its own direction (`PaintContext::autoDirection = true`);
  labels follow the interface.
* Digits follow the interface language: `Lang::Num()`. Inputs store Latin digits and accept
  Persian / Arabic ones.

## 11. Do / don't

Do

* take every size, color, font and icon from `st::`;
* check fa + en, light + dark, wide + narrow in screenshots before calling a screen done;
* reuse the helpers in section 6 instead of re-implementing userpics, error texts, links,
  pickers, viewers;
* keep the keyboard working: Enter submits, Esc goes back, focus lands where typing goes
  (`setInnerFocus`).

Don't

* no QSS, QML, `.ui` files, Qt layouts or native dialogs other than the file dialog;
* no user-visible literals, no hard-coded left / right;
* no polling timers for server data, no blocking calls;
* no `QDesktopServices::openUrl` on addresses from the network — `Support::OpenLink`;
* no stubs / TODOs left behind; no third-party client code; authorship is `eleza`.

## 12. Voice and video calls (`calls/`)

One-to-one voice and video calls with the mobile apps. The server decides everything about a call
(`/v1/calls…`, the `call.*` signals of the realtime connection, `privacy.calls`); the audio
goes through an SFU room of two that the server names in the join payload. The module is
compiled with `ELEZA_ENABLE_CALLS` (ON by default; `docs/BUILDING.md`, "Voice calls: the
media library") and nothing outside it includes its headers.

| File | What |
| --- | --- |
| `data/data_calls.*` (core) | the wire: `Data::CallsApi` (start, accept, reject, cancel, hangup, ringing, connected, rejoin, qos, get, incoming, active), `Data::CallSignal`, call records (`ClassifyCallRecord`), `CallAvailabilityFor(chat)` — when the top bar shows, disables or hides the call button |
| `calls_call.*` | `Calls::Call`: one call's state machine (Calling → Ringing / Incoming → Connecting → InCall ↔ Reconnecting → Ended) between the API, the signals and the media. Its header lists the rules it shares with the mobile apps (who reports what, polling, the reconnect budget). No widgets |
| `calls_instance.*` | `Calls::Instance`: which call there is — one at a time, glare (both call each other), a call that rings while the start is on its way, calls found after a restart (`calls/active`) or without the realtime connection (`calls/incoming`), "no microphone" |
| `calls_media.h` | `Calls::Media`, the interface the call talks to: join / leave / mute / devices / stats, and the cameras (`setCameraEnabled`, `localVideoFrame`, `remoteVideoFrame`, `remoteVideoChanged`). Everything behind it can be replaced without touching the rest |
| `calls_video.*` | `Calls::Video::Camera`: the frames of a camera (Qt Multimedia) as RGBA pictures of at most 640×480 at 15 per second; the list of cameras |
| `calls_media_livekit.cpp`, `calls_ffi.*`, `calls_ffi_proto.h`, `calls_audio_io.*` | the implementation: the LiveKit library loaded at run time (four C functions exchanging protobuf messages), the microphone and the loudspeaker through Qt Multimedia, the library's audio processing (echo, noise, gain). All of it lives on one thread ("Calls media"); the GUI thread only posts to it |
| `calls_service.*` | `Calls::Service`, one per signed-in session: owns the instance, the sounds, the incoming-call window, the settings on their way to a running call |
| `calls_panel.*`, `calls_view.*`, `calls_incoming_window.*`, `calls_settings.*`, `calls_widgets.*`, `calls_text.*`, `calls_sounds.*` | what the user sees and hears: the bar on top of the window (`MainWindow::setTopStrip`), the expanded view, the always-on-top incoming window, Settings → «تماس‌ها», the phrases of every state |
| `calls_module.cpp` | `Calls::Register()`: the hooks below |

How it is plugged in (all of these are general hooks, calls are their first user):

* `window/window_calls.h` — `Window::CallsSupported()` / `Window::StartCall(controller, userId[, video])`
  / `Window::VideoCallsOffered(controller)`: what the chat top bar, the profile and a call
  record in the history use.
* `Settings::RegisterCallsPage(factory)` — the «تماس‌ها» section exists only when somebody
  registered it.
* `Core::Application::registerExitTask` (hang up before the process ends or the user signs
  out; bounded to 1.5 s), `registerRestartBlocker` (no restart into an update during a
  call), `registerCloseGuard` (closing the window with no tray to stay in asks first).

The sounds (`Resources/sounds/call_*.wav`) are generated: `python3
codegen/scripts/gen_call_sounds.py` writes the same bytes every time.

### Video

A call has a `video` *intent* (`POST /v1/calls {"video": true}`), which the server repeats on
the join, the call views, every signal and the call record; it words the call («تماس
تصویری») and decides whether the caller's camera starts with it. The camera itself is a track
of the SFU room that either side publishes or stops at any time in a call of either kind —
the backend is never told — and the layout of the call view follows the tracks, not the flag
(the table is in `calls_view.h`).

* **Gate.** `Api::ClientConfig::videoCallsEnabled()` (`feature.video_calls`; absent = off):
  without it there is no video call button, no camera toggle, no camera row in the settings
  and no `video` in a request. `call.video_disabled` on a start is answered by starting the
  same call as a voice call.
* **Sending.** `Calls::Video::Camera` (GUI thread) → `Media::setCameraEnabled` → the engine
  creates a video source of the picture's size, a track with the source `camera`, publishes
  it and feeds it RGBA frames (`CaptureVideoFrame`); turning the camera off unpublishes it.
  After a rejoin the camera is published again by its next frame.
* **Receiving.** A subscribed video track gets a video stream that delivers RGBA frames;
  each is copied into a `QImage` and handed to the view at up to 25 per second. A muted or
  unsubscribed track ends it (`remoteVideoChanged(false)`).
* **Falling back to audio.** No camera, a camera that cannot be opened or that is unplugged,
  a publish the SFU refuses: a notice, and the call goes on as it was.
* **The incoming window** of a video call has «فقط صدا» and «تصویری»; Enter answers without
  the camera.

`calls_media_probe --camera=pattern` publishes generated frames and prints what arrives from
the other side (`video local=… remote=… size=…`): two of them in one room are the end-to-end
check of video through a real SFU without a camera.

### Testing calls

* **Unit tests** — `test_calls_call` drives `Call` and `Instance` with a fake server and a
  fake media (accept against cancel against timeout, glare, signals that arrive before the
  HTTP answer, duplicates, restart recovery, the reconnect budget); `test_calls_audio` and
  `test_calls_ffi_proto` cover the audio helpers and the wire codec;
  `test_calls_media_library` loads the real library and asks it for a processing module
  (skipped where there is no library for the platform).
* **Two apps calling each other** — `tests/tools/calls-e2e.sh <build dir>` (Linux, a Debug
  build): a real `livekit-server` (the pinned release, downloaded and verified, or
  `LIVEKIT_SERVER=<binary>`), `tests/tools/mock-calls.mjs` playing the backend's call API
  and signals for two users, two copies of Eleza that send a test tone instead of the
  microphone and record what they would play. It checks answered / declined / cancelled /
  busy / not allowed / no answer, audio in both directions with its delay, mute, a media
  drop with rejoin, a killed and restarted app, a call without the realtime connection and
  quitting during a call; it skips itself when the server binary cannot be had. The header
  of the script lists everything.
* **By hand, two windows on one machine** —

  ```sh
  livekit-server --dev --bind 127.0.0.1 &
  node tests/tools/mock-calls.mjs &        # A on :8600, B on :8601; its header has the controls
  Eleza --api=http://127.0.0.1:8600 --workdir=/tmp/wd-calls-a --no-single-instance --debug-login=+989121234567:123456
  Eleza --api=http://127.0.0.1:8601 --workdir=/tmp/wd-calls-b --no-single-instance --debug-login=+989121234568:123456
  ```

  Each of the two sees the other as سارا in the first chat. `curl -X POST
  :8600/control/kick -d '{"user":"a"}'` cuts A's audio connection, `/control/privacy`,
  `/control/busy` and `/control/signals` play the other refusals and a dead realtime
  connection.
* **The media layer alone** — `build/tests/calls_media_probe --url=ws://… --token=$(python3
  tests/tools/livekit-token.py --room r --identity a) --input=tone:440 --output=a.wav`
  joins a room without the app; `tests/tools/calls-audio-check.py a.wav --expect 880` says
  what was heard and how late.

Debug hooks of the module (`ELEZA_DEBUG_TOOLS` only):

| Hook | Does |
| --- | --- |
| route `debug/calls/<state>[?user=<id>]` | a call in that state without a server, for grabs: `calling`, `ringing`, `connecting`, `connected`, `muted`, `reconnecting`, `ended`, `declined`, `no-answer`, `busy`, `failed`, `lost`, `not-allowed` (the bar; add `-expanded` for the call view), `incoming` (the window), `missed` (the notification); video calls: `video-incoming` (the window with «فقط صدا» / «تصویری»), `video-ringing` (my camera while it rings), `video` (both cameras), `video-remote`, `video-local` (generated pictures; they open the call view). Run it after the chats are loaded (`wait:700;route:debug/calls/connected`) |
| `ELEZA_DEBUG_CALL_CAMERA=pattern` | generated video frames (colour bars, a moving bar, a frame counter) instead of a camera |
| `ELEZA_DEBUG_CALL_INPUT=tone:<Hz>` | a tone instead of the microphone (on during the first half of every second of the clock, so a recording shows the delay); also makes "no microphone" pass |
| `ELEZA_DEBUG_CALL_OUTPUT=<file.wav>` | records what would be played (48 kHz mono) and writes `<file.wav>.json` with the time of its first sample |
| `ELEZA_DEBUG_CALL_CONTROL=<folder>` | drives the calls from a script: a line in `<folder>/command` (`start <user id>`, `start-video <user id>`, `answer`, `answer-video`, `camera`, `hangup`, `mute`, `dismiss`, `input <device id>`, `quit`), the call's state in `<folder>/state` (also `video`, `camera`, `remote_video`, `local_frame`, `remote_frame`), every change in `<folder>/events` |
| `ELEZA_DEBUG_CALL_INCOMING_INTERVAL_MS=<n>` | how often a copy without the realtime connection asks whether a call rings (a minute otherwise) |
| `ELEZA_LIVEKIT_FFI=<file>` | another build of the media library |

## 13. Groups (`data/data_group*`, `groups/`, and the chat list / chat screen)

A group is not a chat on the server — its own ids (`j…`), routes (`/v1/groups…`,
`groups.*`), counters and sync — but the user meets it in the same places: a row of the chat
list and the ordinary chat screen. So there is no second list and no second chat screen:

| Where | What |
| --- | --- |
| `data/data_group_rules.*` | every decision as a plain function, tested one by one (`tests/core/test_group_rules.cpp`): the pts rule of a live frame, what the answer of a send means, catch-up entries, unread from a difference, service notes, composer states, the rights table the server evaluates (mirrored from its tests), invite links, mentions (where one starts, UTF-16 offsets, "does it address me") |
| `data/data_group_types.*` | `Group` (the header as I see it), `GroupMember`, `GroupInvite`, `GroupInvitePreview`, `GroupJoinRequest` |
| `data/data_groups.*` | `Data::Groups`: the sync track (catch-up after every Hello, live frames, per-group difference, my own place from the user-level log) and every action. Its header explains the two counters kept per row. `tests/core/test_groups.cpp` drives it on the fake transport |
| `data/data_chats.*`, `data_messages.*`, `data_outbox.*` | route by `Data::IsGroupId(id)`: a group's row is a `Data::Chat` with `isGroup` and `group`, its history lives in `Data::Messages`, its sends go through the outbox like any other (idempotent by client message id) |
| `dialogs/` | group rows: the group's photo, «سارا: …» previews, notes with names, the «@» badge of an unread mention, «گروه تازه» in «پیام جدید» |
| `history/` | the chat screen in a group: the sender's name (one of eight colours by user id) and role tag above the first bubble of a run and the picture next to the last one (`View::Delegate::elementGroup / elementSender`), notes with names (each name opens that profile: `Support::ServiceNoteLine`), two ticks on my messages up to `outbox_read_seq` (the furthest another member has read; `Data::Groups::applyReads`), the composer's states (restricted with the time left, slow mode with a countdown), «@» suggestions (`history_mentions.*`), the «@» button of unread mentions stacked above «to the end», delete for everyone by rights, pins by permission |
| `groups/` | everything else: `groups_info.*` (the info screen, route `group/<id>`), `groups_boxes.*` (creating a group, adding members, the box of an invite link, notification level, edit, permissions, admin rights, restrictions, bans, invite links, join requests, delete, report, the menu of a member), `groups_module.cpp` (routes, the chat menu filler, debug routes) |

**Gates.** `Groups::enabled()` (`feature.groups`): off means no group row, no request and no
entry point anywhere. `Groups::administration()` (the server lists `groups.max_admins`): the
second release — admins, restrictions, bans, permissions, invite links, join requests, pins,
mentions, reports, server drafts. What a screen offers beyond that comes from the header:
`group.can("add_members")` for permissions, `group.hasRight("ban_users")` for admin rights,
`Data::GroupRights::CanPromote / CanRestrict / CanBan(me, target)` for a member row. The
server has the last word; `group.right_required` re-reads the header.

**Testing by hand.** `tests/tools/mock-groups.mjs` is a proxy in front of the usual mock
stack that owns everything of groups (and gives people pictures):

```sh
node <pwa>/scripts/mock-api.mjs &                       # :8000
PORT=8102 TARGET=http://127.0.0.1:8000 node tests/tools/mock-history.mjs &
PORT=8103 TARGET=http://127.0.0.1:8102 node tests/tools/mock-groups.mjs &
Eleza --api=http://127.0.0.1:8103 --workdir=/tmp/wd-groups --no-single-instance --debug-login=+989121234567:123456
```

Its header lists the groups it starts with (one I own, one where I am restricted, one in
slow mode, one of 240 members where I am an admin), the invite tokens for every answer of a
preview, and the switches: `GROUPS=off` (the feature flag absent), `PHASE=1` (a server of the
first release), `VIDEO=off`, `AVATAR_DELAY=<ms>`, `POST /control/frame` (a live frame),
`GET /control/stats` (how often each picture was fetched).

Debug routes (`ELEZA_DEBUG_TOOLS`): `debug/group-new`, `debug/group-add/<g>`,
`debug/group-edit/<g>`, `debug/group-notify/<g>`, `debug/group-perms/<g>`,
`debug/group-links/<g>`, `debug/group-requests/<g>`, `debug/group-members/<g>/<admins|
restricted|banned>`, `debug/group-admin/<g>/<u>`, `debug/group-restrict/<g>/<u>`,
`debug/group-member-menu/<g>/<u>[/<role>]`, `debug/group-leave/<g>`, `debug/group-clear/<g>`,
`debug/group-delete/<g>`, `debug/group-report/<g>`; the screens themselves are routes
(`group/<g>`, `g/<token>`, `chat/<g>`).

## 14. The report / block bar and self-destructing media

**The report / block bar** (Telegram's action bar; the backend's `docs/report-block-bar.md` and
`docs/groups.md` §9.1): `ChatInfo::actionBar` ("stranger") and `Group::actionBar` /
`Group::addedBy` ("added_by_stranger"). Which bar shows, the plans of «مسدود کردن» / «گزارش
هرزنامه» / «ترک گروه» (a report always first, a leave last) and how a failure is read are plain
functions in `data/data_action_bar.h` (`tests/core/test_action_bar.cpp`); the bar itself is
`history/history_action_bar.*`. The × is `Data::Chats::dismissActionBar`: the bar goes at once, the
dismissal is kept in the account's database until the server has it and sent again with every
connection, and meanwhile headers that still carry the bar lose it here. When nothing of a choice
went through, the bar comes back.

**Self-destructing photos and videos** (`docs/self-destruct-media.md` of the backend). Only the phone
apps open them: a desktop session is sent a text («only on the phone») with `self_destruct` — this
app never gets the picture, the caption or a way to open it. `Data::Message::selfDestruct` holds what
it is and how far it got. As on Telegram Desktop the chat shows it as a centred line
(`History::IsService`, worded by `Support::SelfDestructLine`): «‹sender› sent you a self-destructing
message, view it on your phone» (the name opens the profile), «you sent one», «expired»; it has a
menu (reply, pin, delete) but no reactions. `Support::SelfDestructLabel` is the one-line label of the
chat list, notifications and reply headers. It is never copied, forwarded, edited, saved, searched
or shown in the viewer. Sending (`SendFileOptions::selfDestruct`, the timer button of the
send-files box) needs `chat.self_destruct.enabled` (absent: off) and a direct chat with a person; it
is uploaded on its own (no dedup) and its local line never shows the file. The screenshot notice
(`self_destruct_screenshot:<taker>:<seq>`) is worded here, the taker's name opening their profile.

Mock data: `chat/spam` (a stranger wrote first) and `chat/sd` in `tests/tools/mock-history.mjs`,
`chat/j1promo` (a stranger added me) in `tests/tools/mock-groups.mjs`.
