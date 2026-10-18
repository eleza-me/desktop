// The rules of the bot platform in a chat, without widgets (unit-tested in
// tests/app/test_history_bots.cpp):
//
//   * inline mode — «@<handle ending in bot> <query>» in a composer;
//   * the command menu — «/» and the start of a command name;
//   * the «شروع» bar of an empty bot chat and the text it sends;
//   * what a button of an inline keyboard does (its `kind`).
//
// The contract is the backend's docs/bot-api.md: a bot's handle ends in
// "bot", a command is [a-z0-9_]{1,32}, a deep-link payload is at most 64
// characters of [A-Za-z0-9_-].
#pragma once

#include "data/data_bots.h"
#include "data/data_types.h"

#include <optional>

namespace History {

// ---- inline mode

struct InlineQueryText {
	QString handle; // lower-case, without «@»
	QString query;  // everything after the first whitespace (may be empty)
};

// The text of a composer as an inline query: «@weather_bot tehran» ->
// { "weather_bot", "tehran" }. One whitespace after the handle is required
// (the handle alone is still being typed); nullopt for anything else.
[[nodiscard]] std::optional<InlineQueryText> ParseInlineQuery(const QString &text);

// The text a «switch inline» button / a «via @bot» label puts into a composer.
[[nodiscard]] QString InlineQueryPrefill(const QString &handle, const QString &query);

// The query as sent: cut to the server's limit (in characters).
[[nodiscard]] QString LimitInlineQuery(const QString &query, int maxChars);

// ---- commands

// What is typed of a command: «/he» -> "he", «/» -> "". Nullopt once the text
// is not a command name being typed (a space, a new line, another character).
[[nodiscard]] std::optional<QString> CommandPrefix(const QString &text);

// The commands whose name starts with `prefix` (case-insensitive), in order.
[[nodiscard]] QVector<Data::BotCommand> FilterCommands(
	const QVector<Data::BotCommand> &commands,
	const QString &prefix);

// ---- start

// A deep-link payload the server accepts (1..64 of [A-Za-z0-9_-]).
[[nodiscard]] bool ValidStartPayload(const QString &payload);

// The text of «شروع»: "/start", or "/start <payload>" with a valid payload.
[[nodiscard]] QString StartCommandText(const QString &payload);

struct BotStartState {
	bool isBot = false;        // the peer of the chat is a bot
	bool writable = false;     // the composer would be shown
	bool sent = false;         // «شروع» was pressed here already
	bool pending = false;      // opened from a ?start= link
	bool listLoaded = false;   // the history was read at least once
	bool listEmpty = false;    // nothing in it (no local message either)
	qint64 lastSeq = 0;        // of the chat row
	int outboxPending = 0;     // queued messages of this chat
};

// The «شروع» bar replaces the composer: a bot chat that has no message yet
// (or one opened from a deep link) until the first message goes out.
[[nodiscard]] bool ShowBotStart(const BotStartState &state);

// ---- inline keyboard buttons

enum class ButtonAction {
	None,               // a kind this build does not know: nothing happens
	Callback,           // ask the bot (chat.callback)
	Url,                // open the address
	SwitchInlineCurrent, // «@bot query» into this chat's composer
	SwitchInlinePick,    // pick a chat, then «@bot query» into its composer
};

[[nodiscard]] ButtonAction ButtonActionFor(const Data::MarkupButton &button);

// The bot a message's buttons belong to: the one it came via, else the
// chat's bot. Empty when neither handle is known.
[[nodiscard]] QString ButtonBotHandle(
	const Data::Message &message,
	const QString &chatBotHandle);

// ---- reply keyboard

// The keyboard is shown or hidden by the user; a new keyboard (another seq)
// shows itself, a persistent one comes back after every message I send.
struct ReplyKeyboardView {
	qint64 seq = 0;      // the message that carries the keyboard
	bool hidden = false; // the user put it away
};
[[nodiscard]] ReplyKeyboardView ReplyKeyboardAfterChange(
	ReplyKeyboardView was,
	qint64 newSeq);
[[nodiscard]] ReplyKeyboardView ReplyKeyboardAfterSend(
	ReplyKeyboardView was,
	bool persistent);

} // namespace History
