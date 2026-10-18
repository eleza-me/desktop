#include "history/history_bot_logic.h"

#include <QtCore/QRegularExpression>

namespace History {
namespace {

constexpr auto kMaxCommandLength = 32;
constexpr auto kMaxStartPayload = 64;

[[nodiscard]] bool IsCommandChar(QChar ch) {
	const auto code = ch.unicode();
	return (code >= 'a' && code <= 'z')
		|| (code >= 'A' && code <= 'Z')
		|| (code >= '0' && code <= '9')
		|| (code == '_');
}

} // namespace

std::optional<InlineQueryText> ParseInlineQuery(const QString &text) {
	// The contract: the handle ends with «bot»; one whitespace, then the query.
	static const auto kInline = QRegularExpression(
		QStringLiteral("^@([A-Za-z0-9_]*bot)\\s(.*)$"),
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::DotMatchesEverythingOption);
	const auto match = kInline.match(text);
	if (!match.hasMatch()) {
		return std::nullopt;
	}
	auto result = InlineQueryText();
	result.handle = match.captured(1).toLower();
	result.query = match.captured(2);
	// "@bot" alone is no handle: at least one character before the «bot».
	if (result.handle.size() < 4) {
		return std::nullopt;
	}
	return result;
}

QString InlineQueryPrefill(const QString &handle, const QString &query) {
	auto clean = handle.trimmed();
	if (clean.startsWith(u'@')) {
		clean.remove(0, 1);
	}
	return u'@' + clean + u' ' + query;
}

QString LimitInlineQuery(const QString &query, int maxChars) {
	if (maxChars <= 0 || query.size() <= maxChars) {
		return query;
	}
	auto length = maxChars;
	// Never half of a surrogate pair.
	if (length > 0 && query[length - 1].isHighSurrogate()) {
		--length;
	}
	return query.left(length);
}

std::optional<QString> CommandPrefix(const QString &text) {
	if (!text.startsWith(u'/') || text.size() > kMaxCommandLength + 1) {
		return std::nullopt;
	}
	for (auto i = 1; i < text.size(); ++i) {
		if (!IsCommandChar(text[i])) {
			return std::nullopt;
		}
	}
	return text.mid(1).toLower();
}

QVector<Data::BotCommand> FilterCommands(
		const QVector<Data::BotCommand> &commands,
		const QString &prefix) {
	auto result = QVector<Data::BotCommand>();
	const auto lower = prefix.toLower();
	for (const auto &command : commands) {
		if (!command.command.isEmpty()
			&& command.command.toLower().startsWith(lower)) {
			result.push_back(command);
		}
	}
	return result;
}

bool ValidStartPayload(const QString &payload) {
	if (payload.isEmpty() || payload.size() > kMaxStartPayload) {
		return false;
	}
	for (const auto ch : payload) {
		if (!IsCommandChar(ch) && ch != u'-') {
			return false;
		}
	}
	return true;
}

QString StartCommandText(const QString &payload) {
	const auto trimmed = payload.trimmed();
	return ValidStartPayload(trimmed)
		? (u"/start "_s + trimmed)
		: u"/start"_s;
}

bool ShowBotStart(const BotStartState &state) {
	if (!state.isBot || !state.writable || state.sent) {
		return false;
	} else if (state.pending) {
		return true;
	} else if (state.outboxPending > 0) {
		return false;
	} else if (state.listLoaded) {
		return state.listEmpty;
	}
	// Still reading the history: the row says whether anything was ever sent.
	return (state.lastSeq <= 0);
}

ButtonAction ButtonActionFor(const Data::MarkupButton &button) {
	const auto &kind = button.kind;
	if (kind == u"callback") {
		return ButtonAction::Callback;
	} else if (kind == u"url") {
		return button.url.isEmpty() ? ButtonAction::None : ButtonAction::Url;
	} else if (kind == u"switch_inline_current") {
		return ButtonAction::SwitchInlineCurrent;
	} else if (kind == u"switch_inline") {
		return ButtonAction::SwitchInlinePick;
	} else if (kind.isEmpty()) {
		// A message cached by an older build: what it carries says it.
		return !button.url.isEmpty()
			? ButtonAction::Url
			: button.query
			? ButtonAction::SwitchInlinePick
			: ButtonAction::Callback;
	}
	return ButtonAction::None;
}

QString ButtonBotHandle(
		const Data::Message &message,
		const QString &chatBotHandle) {
	if (message.viaBot && !message.viaBot->handle.isEmpty()) {
		return message.viaBot->handle;
	}
	return chatBotHandle;
}

ReplyKeyboardView ReplyKeyboardAfterChange(
		ReplyKeyboardView was,
		qint64 newSeq) {
	if (newSeq == was.seq) {
		return was;
	}
	// Another keyboard (or none): it takes the keyboard's place, shown.
	return ReplyKeyboardView{ newSeq, false };
}

ReplyKeyboardView ReplyKeyboardAfterSend(
		ReplyKeyboardView was,
		bool persistent) {
	if (persistent) {
		was.hidden = false;
	}
	return was;
}

} // namespace History
