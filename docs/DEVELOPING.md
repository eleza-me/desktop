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
signed in (`--debug-login` is skipped then). Delete the directory to start clean.

## 2. Debug hooks

| Flag | Meaning |
| --- | --- |
| `--api=<url>` / env `ELEZA_API` | API base (default `https://api.eleza.me`) |
| `--workdir=<dir>` | data directory; screenshot runs without it use a temporary one |
| `--lang=fa\|en`, `--theme=light\|dark` | for this run only |
| `--debug-size=<W>x<H>`, `--debug-scale=<f>` | window size, device pixel ratio |
| `--debug-login=<phone>:<code>[:<password>]` | signs in through the API at startup when nobody is signed in (no intro UI); exits with code 2 if it fails in a grab run |
| `--debug-route=<route>` | opened once the session layout is shown (section 5; `debug/…` and `intro/…` below) |
| `--debug-actions=<script>` | scripted input, played after the route was opened |
| `--debug-grab=<png>` | save a grab and exit |
| `--debug-grab-delay=<ms>` | wait this long after the route was opened and the actions finished (default 600) |
| `--gallery[=<page>]` | the UI gallery instead of the app |
| `--no-single-instance` | allow a second process on the same workdir |

A run with `--debug-grab` or `--debug-actions` is *ephemeral*: preferences are not saved, no
tray icon, no sounds, animations jump to their end, the process exits when done. Hover
tooltips stay hidden until the script hovers on purpose (`move:x,y`), so the resting pointer
of a headless platform never puts one into a grab; the window counts as active (an open chat
marks its messages read as it would on a desktop).

### `--debug-actions`

Steps separated by `;` (write `\;` for a literal semicolon). Coordinates are logical pixels
of the main window.

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
| `debug/notification`, `debug/notifications` | one / three sample notification toasts |
| `debug/viewer[?file=<path>][&video=<path>]` | the image viewer with generated pictures (and your file / video) |
| `debug/crop`, `debug/about`, `debug/peers`, `debug/peers-multi` | the shared boxes |
| `debug/logout`, `debug/link`, `debug/error`, `debug/upgrade` | sign-out confirmation, external link confirmation, an error toast, the "update required" screen |
| `debug/session-expired` | ends the session as the server would |

Add your own in your module's route handler (`route.startsWith(u"debug/…")`).

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
* **Reports** — one form for posts, replies, accounts and conversation topics:
  `Boxes::ShowReportBox(controller, args)` (`boxes/report_box.h`).

### Routes

One string per place (`window/window_routes.h`). Never build them by hand: use
`Window::Routes::…`, and parse with `Window::ParseRoute()` (handlers get the parsed form).

| Route | Builder | Tab | Owner |
| --- | --- | --- | --- |
| `chats` | `Routes::Chats()` | Chats | dialogs |
| `chat/<chatId>[?seq=N]` | `Routes::Chat(id, seq)` | Chats | history |
| `user/<userId>` | `Routes::User(id)` | current | profile |
| `u/<handle>` | `Routes::Handle(handle)` | current | profile |
| `eli` | `Routes::Eli()` | Eli | eli |
| `eli/post/<postId>` | `Routes::Post(id)` | Eli | eli |
| `eli/share/<slug>` | `Routes::SharedPost(slug)` | Eli | eli (resolves the share link, then opens the post) |
| `eli/topic/<slug>` | `Routes::Topic(slug)` | Eli | eli |
| `eli/notifications` | `Routes::EliNotifications()` | Eli | eli |
| `search[?q=text]` | `Routes::Search(query)` | Search | search |
| `me` | `Routes::Me()` | Me | profile |
| `settings[/<section>]` | `Routes::Settings(section)` | Settings | settings |
| `conversation/topics` | `Routes::ConversationTopics()` | Chats | conversation |
| `conversation/<id>` | `Routes::Conversation(id)` | Chats | conversation |

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
`https://eleza.me/s/<share slug>`, `eleza://chat/c1`, `@sara`, `#tag` into routes, asks before
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
  controls, see `SectionWidget::toastBottomSkip()`).
* **Top bar**: `Ui::TopBar` (`setTitle`, `setSubtitle(text, accent)`, `addAction(st)`,
  `setLeadingWidget`).

### Shared helpers (`support/`, `boxes/`, `media/`)

| Need | Use |
| --- | --- |
| An image from the media cache in a widget | `Support::MakeImageSource(&session, ref, variant, { .blurhash = …, .aspect = … })` -> `Ui::ImageView::setSource` |
| A photo / video poster of a message | `Support::MakeMessageImageSource(&session, message)` |
| A userpic widget bound to a user | `Support::UserpicButton(parent, &session, userId, size)`, `setShowOnline(true)`, `setSpecial(kind, name, id)` |
| Userpics in a custom painted list | `Support::UserpicPainter _userpics{ &session, [=](const QString &userId) { … update rows … } };` then `_userpics.paint(p, rect, userId)` / `paintSpecial(…)` |
| Chat title / one-line message preview | `Support::ChatTitle(&session, chat)`, `Support::MessagePreview(message)` (`support/chat_text.h`) |
| An error to the user | `Support::ErrorText(error, fallback)` for inline text, `Support::ShowError(controller, error[, fallback])` for a toast |
| «در حال اتصال…» in a top bar | `new Support::ConnectionTitle(bar, &session, title)` (or `Mode::Subtitle` for a chat header); `Support::ConnectionState::For(&session)` for the raw state |
| Open a link / mention / hashtag | `Support::OpenLink`, `Support::LinkHandler` |
| Pick / save files, paste, drop | `Support::PickFiles`, `PickImage`, `SaveFileAs`, `SaveImageAs`, `ReadMimeData`, `CanAcceptDrop`, `SaveImageToTemp` (`support/file_dialogs.h`) |
| The brand mark | `Support::PaintBrandMark(p, rect)` |
| A searchable list of people / chats | `Boxes::PeerListBox` with your `Boxes::PeerListController`; ready-made `Boxes::ChooseChatsBox(&session, args, done)` |
| A round photo crop | `Boxes::PickAndCropPhoto(parent, show, done)`, `Boxes::SaveCroppedPhoto(image, guard, done)` |
| Country list, about | `Boxes::CountrySelectBox`, `Boxes::AboutBox` |
| «گزارش» (post / reply / account / conversation) | `Boxes::ShowReportBox(controller, args)` |
| Full-window photo / video viewer | `Media::ShowImageViewer(controller->window(), items, index)` |
| Voice / music playback | `Media::Player().toggle(track, &session)`; signals `stateChanged`, `positionChanged`; `cycleSpeed()` |
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
`voicePlaybackSpeed`. Change them through the setters (they save themselves); language and
theme through `Core::App().setLanguage / setThemeMode`.

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
MediaRef::Avatar(userId, avatarId) | Attachment(chat, seq) | PostMedia(postId, mediaId) | Url(url)
MediaVariant::Thumb | Feed | Full | Poster | Mp4 | Audio | File | Cover
MediaHandle: isReady(), isFailed(), image(), filePath(), progress(); signals ready(), failed(), progressChanged()
// Keep the handle while the media is on screen.

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
void Feed::reportViews(const QStringList &ids);  sendReport(targetKind, target, reason, note, …);
signals: postChanged(id), postPublished(id), postRemoved(id), authorBlocked(userId), newPosts(count, since)

// ---- graph (data/data_graph.h): profiles, follow, block, search, settings, devices
void Graph::profileByHandle(const QString &handle, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
void Graph::profile(const QString &userId, …same…);
void Graph::follow / unfollow / block / unblock(const QString &userId, base::Guard guard = {}, Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
void Graph::followers / following(const QString &userId, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail = nullptr);
void Graph::search(const QString &query, const QString &type, int offset, base::Guard guard, Fn<void(const SearchResults &)> done, FailCallback fail = nullptr);
void Graph::searchPeople(const QString &query, base::Guard guard, Fn<void(const QVector<Profile> &)> done);
void Graph::suggestions(cursor, limit, guard, done, fail);
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

// ---- notifications (data/data_notifications.h)
int Notifications::unread() const;   int kind(const QString &name) const;
void Notifications::load(const QString &tab, qint64 before, base::Guard guard, Fn<void(const QVector<NotificationEntry> &)> done, FailCallback fail = nullptr);  // "" | "mentions" | "requests"
void Notifications::markAllRead();        signal: unreadChanged()

// ---- stickers (data/data_stickers.h)
void Stickers::load(base::Guard guard = {}, Fn<void()> done = nullptr);
const QVector<StickerPack> &Stickers::packs() const;  QVector<StickerPack> installedPacks() const;  const QVector<Sticker> &recent() const;
void Stickers::setInstalled(const QString &packId, bool installed);  noteUsed(const Sticker &sticker);     signal: changed()

// ---- sync (data/data_sync.h)
bool Sync::updating() const;     signals: updatingChanged(bool), messageReceived(const Data::Message &)
```

Errors: `Api::Error { status, code, message, retryable, retryAfter, requestId }` with
`offline()`, `notFound()`, `rateLimited()`, `serverSide()`. Show them through
`Support::ErrorText` / `Support::ShowError`; add a new well-known code to
`Support::ErrorPhrase` (`support/error_text.cpp`) instead of comparing codes in screens.

## 8. Threading

Everything runs on the GUI thread. Never block it: no `waitFor…`, no nested event loops
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
