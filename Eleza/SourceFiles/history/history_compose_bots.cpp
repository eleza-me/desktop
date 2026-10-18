// The bot parts of the composer: the command menu, inline mode, prefilled
// texts (history_compose_controls.h has the overview, history_bot_logic.h the
// rules).
#include "history/history_compose_controls.h"

#include "api/api_client_config.h"
#include "data/data_bots.h"
#include "data/data_chats.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "history/history_bot_widgets.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_history.h"

namespace History {
namespace {

// A pause in typing before the bot is asked.
constexpr auto kInlineQueryDelay = 400;

[[nodiscard]] bool BotGone(const Api::Error &error) {
	return (error.code == u"bot.inline_disabled")
		|| (error.code == u"bot.disabled")
		|| (error.code == u"bot.switched_off")
		|| (error.code == u"bot.suspended");
}

} // namespace

void ComposeControls::setupBots() {
	_keyboardToggle->hide();
	_botCommands->hide();
	_keyboardToggle->setClickedCallback([this] { toggleBotKeyboard(); });
	_botCommands->setClickedCallback([this] { toggleCommands(); });
	Ui::InstallTooltip(_keyboardToggle, [this] {
		return keyboardShown()
			? tr::lng_bot_keyboard_hide(tr::now)
			: tr::lng_bot_keyboard_show(tr::now);
	});
	Ui::InstallTooltip(_botCommands, [] { return tr::lng_bot_commands(tr::now); });

	_botPanel->setCommandChosen([this](const Data::BotCommand &command, bool insert) {
		pickCommand(command, insert);
	});
	_botPanel->setResultChosen([this](const Data::InlineResult &result) {
		pickInlineResult(result);
	});
	_botPanel->setNeedMore([this] { inlineNeedMore(); });

	auto &bots = _session->data().bots();
	connect(&bots, &Data::Bots::changed, this, [this](const QString &botId) {
		if (botId != _botId) {
			return;
		} else if (const auto info = _session->data().bots().info(botId)) {
			applyBotInfo(*info);
		}
	});
	connect(
		&_session->data().chats(),
		&Data::Chats::chatChanged,
		this,
		[this](const QString &id) {
			if (id == _chatId) {
				refreshBotChat();
			}
		});
	refreshBotChat();
}

void ComposeControls::refreshBotChat() {
	const auto chat = _session->data().chats().chat(_chatId);
	const auto bot = chat
		&& chat->isBot
		&& !_conversation
		&& !_group
		&& !chat->peerUserId.isEmpty();
	const auto id = bot ? chat->peerUserId : QString();
	if (bot == _botChat && id == _botId) {
		return;
	}
	_botChat = bot;
	_botId = id;
	_commands.clear();
	if (bot) {
		auto &bots = _session->data().bots();
		if (const auto info = bots.info(id)) {
			applyBotInfo(*info);
		}
		// A failure is asked again the next time the chat is opened.
		bots.load(id, this, [this](const Data::BotInfo &info) {
			if (info.id == _botId) {
				applyBotInfo(info);
			}
		});
	}
	updateControls();
	updateBotSuggestions();
}

void ComposeControls::applyBotInfo(const Data::BotInfo &info) {
	if (!_botChat || info.id != _botId || _commands == info.commands) {
		return;
	}
	_commands = info.commands;
	updateControls();
	updateBotSuggestions();
}

bool ComposeControls::inlineModeAllowed() const {
	const auto config = _session->data().clientConfig();
	if (_conversation || _group || (config && !config->botsInlineEnabled())) {
		// Never in an anonymous conversation; a group's send has no
		// inline_result.
		return false;
	}
	const auto chat = _session->data().chats().chat(_chatId);
	return chat && !chat->anonymous && !chat->isConversation;
}

void ComposeControls::updateBotSuggestions() {
	const auto free = (_mode != Mode::Edit) && !recording();
	if (free && inlineModeAllowed()) {
		if (const auto query = ParseInlineQuery(_text)) {
			inlineText(query->handle, query->query);
			return;
		}
	}
	leaveInline();
	if (free && _botChat && !_commands.isEmpty()) {
		if (const auto prefix = CommandPrefix(_text)) {
			const auto list = FilterCommands(_commands, *prefix);
			if (!list.isEmpty()) {
				placeBotPanel();
				_botPanel->showCommands(list);
				return;
			}
		}
	}
	if (_botPanel->mode() == BotSuggestPanel::Mode::Commands) {
		hideBotPanel();
	}
}

void ComposeControls::hideBotPanel() {
	if (_botPanel->mode() == BotSuggestPanel::Mode::None) {
		return;
	}
	_botPanel->hideFast();
	updateControls();
}

QRect ComposeControls::suggestionsAnchor() const {
	const auto content = contentRect();
	const auto &padding = st::historyComposePadding;
	return QRect(
		mapTo(_panelParent, QPoint(content.x() + padding.left(), 0)),
		QSize(content.width() - padding.left() - padding.right(), height()));
}

void ComposeControls::placeBotPanel() {
	if (_panelParent) {
		_botPanel->placeAbove(suggestionsAnchor());
	}
}

void ComposeControls::toggleCommands() {
	if (_commands.isEmpty() || _mode == Mode::Edit || recording()) {
		return;
	}
	if (_botPanel->mode() == BotSuggestPanel::Mode::Commands) {
		hideBotPanel();
	} else {
		leaveInline();
		placeBotPanel();
		_botPanel->showCommands(_commands);
		updateControls();
	}
	_field->setFocusFast();
}

void ComposeControls::pickCommand(const Data::BotCommand &command, bool insert) {
	if (_mode == Mode::Edit || recording() || command.command.isEmpty()) {
		return;
	}
	hideBotPanel();
	const auto text = u'/' + command.command;
	if (insert) {
		// Tab: into the field, arguments may follow.
		setFieldText(text + u' ', {});
		_field->setFocusFast();
		_field->setCursorToEnd();
		_draft->changed(currentDraft(), Ui::Now());
		return;
	}
	// What was typed of it goes; the command is sent at once.
	if (_text.startsWith(u'/')) {
		_draft->sent();
		setFieldText(QString(), {});
	}
	sendCommand(text);
	_field->setFocusFast();
}

void ComposeControls::sendCommand(const QString &command) {
	const auto text = command.trimmed();
	if (text.isEmpty()) {
		return;
	}
	_session->data().outbox().sendText(_chatId, text);
	Q_EMIT sent();
}

void ComposeControls::inlineText(const QString &handle, const QString &query) {
	auto &bots = _session->data().bots();
	switch (bots.handleState(handle)) {
	case Data::Bots::HandleState::Unknown:
		if (_inline.active && _inline.handle != handle) {
			leaveInline();
		}
		// Looked up once; the text is read again when the answer is here
		// (a failed lookup is asked again by the next keystroke).
		bots.resolve(handle, this, [=, this] {
			if (_session->data().bots().handleState(handle)
				!= Data::Bots::HandleState::Unknown) {
				updateBotSuggestions();
			}
		});
		return;
	case Data::Bots::HandleState::NotBot:
		leaveInline();
		if (_botPanel->mode() == BotSuggestPanel::Mode::Commands) {
			hideBotPanel();
		}
		return;
	case Data::Bots::HandleState::Bot:
		break;
	}
	const auto info = bots.byHandle(handle);
	if (!info || !info->inlineEnabled || info->id.isEmpty()) {
		leaveInline();
		if (_botPanel->mode() == BotSuggestPanel::Mode::Commands) {
			hideBotPanel();
		}
		return;
	}
	const auto config = _session->data().clientConfig();
	const auto limited = LimitInlineQuery(
		query,
		config ? config->botsInlineQueryMaxChars() : 0);
	if (_inline.active
		&& _inline.handle == handle
		&& _inline.query == limited) {
		return; // nothing new
	}
	const auto sameBot = _inline.active && (_inline.botId == info->id);
	_inline.active = true;
	_inline.handle = handle;
	_inline.botId = info->id;
	_inline.placeholder = info->inlinePlaceholder;
	_inline.query = limited;
	_inline.next = QString();
	_inline.loadingMore = false;
	++_inline.generation;
	if (!sameBot) {
		_inline.results.clear();
		_inline.queryIds.clear();
	}
	if (_inline.results.isEmpty()) {
		// The bot's hint (or «searching») until its answer is here.
		placeBotPanel();
		_botPanel->showInline(
			(limited.isEmpty() && !_inline.placeholder.isEmpty())
				? _inline.placeholder
				: tr::lng_bot_inline_loading(tr::now),
			{});
	}
	updateControls();
	_inlineTimer.callOnce(kInlineQueryDelay);
}

void ComposeControls::leaveInline() {
	if (!_inline.active) {
		if (_botPanel->mode() == BotSuggestPanel::Mode::Inline) {
			hideBotPanel();
		}
		return;
	}
	const auto generation = _inline.generation + 1;
	_inline = InlineState();
	_inline.generation = generation;
	_inlineTimer.cancel();
	if (_botPanel->mode() == BotSuggestPanel::Mode::Inline) {
		hideBotPanel();
	}
}

void ComposeControls::runInlineQuery(const QString &offset) {
	if (!_inline.active || _inline.botId.isEmpty()) {
		return;
	}
	const auto generation = _inline.generation;
	const auto more = !offset.isEmpty();
	const auto query = _inline.query;
	if (more) {
		_inline.loadingMore = true;
	}
	_session->data().bots().inlineQuery(
		_inline.botId,
		query,
		offset,
		this,
		[=, this](const Data::InlineResults &page) {
			if (!_inline.active || generation != _inline.generation) {
				return;
			}
			_inline.loadingMore = false;
			if (!more) {
				_inline.results.clear();
				_inline.queryIds.clear();
			}
			for (const auto &result : page.results) {
				// A result is sent with the query it came from.
				if (!_inline.queryIds.contains(result.id)) {
					_inline.results.push_back(result);
					_inline.queryIds.insert(result.id, page.queryId);
				}
			}
			_inline.next = page.nextOffset;
			auto header = QString();
			if (_inline.results.isEmpty()) {
				header = !page.answered
					? tr::lng_bot_inline_no_answer(tr::now)
					: (query.isEmpty() && !_inline.placeholder.isEmpty())
					? _inline.placeholder
					: tr::lng_bot_inline_empty(tr::now);
			}
			if (_mode == Mode::Edit || recording()) {
				return;
			}
			placeBotPanel();
			_botPanel->showInline(header, _inline.results, more);
			updateControls();
		},
		[=, this](const Api::Error &error) {
			if (!_inline.active || generation != _inline.generation) {
				return;
			}
			_inline.loadingMore = false;
			if (BotGone(error)) {
				leaveInline();
				showToast(Support::ErrorText(error));
			} else if (error.rateLimited()) {
				showToast(Support::ErrorText(error));
			} else if (!more && _inline.results.isEmpty()) {
				hideBotPanel();
			}
		});
}

void ComposeControls::inlineNeedMore() {
	if (_inline.active && !_inline.loadingMore && !_inline.next.isEmpty()) {
		runInlineQuery(_inline.next);
	}
}

void ComposeControls::pickInlineResult(const Data::InlineResult &result) {
	if (!_inline.active
		|| _inline.botId.isEmpty()
		|| _mode == Mode::Edit
		|| recording()) {
		return;
	}
	const auto queryId = _inline.queryIds.value(result.id);
	if (queryId.isEmpty()) {
		return;
	}
	const auto replyTo = (_mode == Mode::Reply) ? _target.seq : qint64(0);
	// The message comes back with its `via_bot`; a result that expired
	// meanwhile fails on its bubble (bot.inline_result_expired).
	_session->data().outbox().sendInlineResult(
		_chatId,
		_inline.botId,
		queryId,
		result.id,
		result.title,
		replyTo);
	const auto prefix = u'@' + _inline.handle;
	leaveInline();
	// That bot's query is still in the field: it was used up.
	if (_text.startsWith(prefix, Qt::CaseInsensitive)) {
		_draft->sent();
		setFieldText(QString(), {});
	}
	if (_mode == Mode::Reply) {
		_mode = Mode::None;
		_quote = std::nullopt;
		setMode(Mode::None);
	}
	Q_EMIT sent();
	_field->setFocusFast();
}

void ComposeControls::prefill(const QString &text) {
	if (recording()) {
		return;
	}
	if (_mode == Mode::Edit) {
		cancelMode();
	}
	setFieldText(text, {});
	_field->setFocusFast();
	_field->setCursorToEnd();
	_draft->changed(currentDraft(), Ui::Now());
}

} // namespace History
