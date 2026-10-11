#include "history/history_widget.h"
#include "data/data_groups.h"
#include "data/data_group_rules.h"

#include "api/api_client_config.h"
#include "core/core_application.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_graph.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "history/history_bars.h"
#include "history/history_boxes.h"
#include "history/history_compose_controls.h"
#include "history/history_list_widget.h"
#include "history/history_logic.h"
#include "history/history_tabbed_panel.h"
#include "history/history_top_bar.h"
#include "history/history_themes.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/shadow.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "window/window_embeds.h"
#include "window/window_session_controller.h"
#include "window/window_shortcuts.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QDateTime>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainterPath>

namespace History {
namespace {

[[nodiscard]] QString MuteLabel(const QString &duration) {
	if (duration == u"forever") {
		return tr::lng_history_mute_forever(tr::now);
	}
	const auto count = duration.left(duration.size() - 1).toInt();
	if (count <= 0) {
		return duration;
	} else if (duration.endsWith(u'h')) {
		return tr::lng_history_mute_hours(tr::now, lt_count, count);
	} else if (duration.endsWith(u'd')) {
		return tr::lng_history_mute_days(tr::now, lt_count, count);
	} else if (duration.endsWith(u'm')) {
		return tr::lng_minutes_short(tr::now, lt_count, count);
	}
	return duration;
}

} // namespace

Window::SectionFactory Factory(const QString &chatId, qint64 seq) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new Widget(parent, controller, chatId, seq));
	};
}

// ---- the floating «to the bottom» button

class Widget::DownButton final : public Ui::AbstractButton {
public:
	explicit DownButton(QWidget *parent) : AbstractButton(parent) {
		const auto extra = st::historyDownBadgeShift;
		resize(st::historyDownSize + 8, st::historyDownSize + 8 + extra);
		setPointerCursor(true);
	}

	void setUnread(int count) {
		if (_unread != count) {
			_unread = count;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto size = st::historyDownSize;
		const auto circle = QRect(
			(width() - size) / 2,
			height() - size - 4,
			size,
			size);
		Ui::Shadow::Paint(p, circle, size / 2, st::smallShadow);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyDownBg);
			p.drawEllipse(circle);
		}
		(isOver() ? st::historyDownIconOver : st::historyDownIcon).paintInCenter(
			p,
			circle);
		if (_unread > 0) {
			const auto text = Ui::BadgeText(_unread);
			const auto badge = Ui::BadgeWidth(text, st::defaultBadge);
			Ui::PaintBadge(
				p,
				text,
				(width() - badge) / 2,
				0,
				width(),
				st::defaultBadge);
		}
	}

	void onStateChanged(State was, StateChangeSource source) override {
		Q_UNUSED(was);
		Q_UNUSED(source);
		update();
	}

private:
	int _unread = 0;

};

// ---- «drop the files here»

class Widget::DropArea final : public Ui::RpWidget {
public:
	explicit DropArea(QWidget *parent) : RpWidget(parent) {
		Ui::MakeTransparentForMouse(this);
		hide();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto margin = st::historyDropMargin;
		const auto body = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
		p.fillRect(rect(), st::overlayScrim);
		auto pen = QPen(st::historyDropBorder->c);
		pen.setWidthF(2.);
		pen.setStyle(Qt::DashLine);
		p.setPen(pen);
		p.setBrush(st::historyDropBg);
		p.drawRoundedRect(body, st::historyDropRadius, st::historyDropRadius);

		const auto &icon = st::historyDropIcon;
		const auto titleHeight = st::historyDropFont->height;
		const auto subHeight = st::historyDropSubFont->height;
		const auto total = icon.height() + 12 + titleHeight + 6 + subHeight;
		auto top = int(body.center().y()) - total / 2;
		icon.paint(p, (width() - icon.width()) / 2, top, width());
		top += icon.height() + 12;
		p.setPen(st::historyDropFg);
		p.setFont(st::historyDropFont);
		p.drawText(
			QRect(0, top, width(), titleHeight),
			Qt::AlignHCenter | Qt::AlignTop,
			tr::lng_history_drop_title(tr::now));
		top += titleHeight + 6;
		p.setPen(st::textSecondary);
		p.setFont(st::historyDropSubFont);
		p.drawText(
			QRect(0, top, width(), subHeight),
			Qt::AlignHCenter | Qt::AlignTop,
			tr::lng_history_drop_text(tr::now));
	}

};

// ---- loading / empty / failed, in the middle of the list

class Widget::StateView final : public Ui::RpWidget {
public:
	enum class State {
		Hidden,
		Loading,
		Empty,
		Failed,
	};

	explicit StateView(QWidget *parent)
	: RpWidget(parent)
	, _retry(Ui::CreateChild<Ui::RoundButton>(
		this,
		tr::lng_try_again(tr::now),
		st::defaultRoundButton))
	, _loading([this] { update(); }) {
		_retry->hide();
		hide();
	}

	void setRetryCallback(Fn<void()> callback) {
		_retry->setClickedCallback(std::move(callback));
	}

	void setState(State state, const QString &text = QString()) {
		if (_state == state && _text == text) {
			return;
		}
		_state = state;
		_text = text;
		if (state == State::Loading) {
			_loading.start();
		} else {
			_loading.stop();
		}
		_retry->setVisible(state == State::Failed);
		setVisible(state != State::Hidden);
		// Only the retry button takes clicks; the rest lets the list work.
		setAttribute(Qt::WA_TransparentForMouseEvents, state != State::Failed);
		relayout();
		update();
	}

	void relayout() {
		const auto parent = parentWidget();
		if (!parent || _state == State::Hidden) {
			return;
		}
		const auto &padding = st::historyEmptyPadding;
		const auto &font = st::historyEmptyFont;
		const auto textWidth = std::min(font->width(_text), 360);
		auto w = 0;
		auto h = 0;
		if (_state == State::Loading) {
			w = h = st::historyLoadingSize + 16;
		} else {
			w = padding.left() + textWidth + padding.right();
			h = padding.top() + font->height + padding.bottom();
			if (_state == State::Failed) {
				_retry->resizeToNaturalWidth(360);
				w = std::max(w, _retry->width());
				h += 10 + _retry->height();
			}
		}
		_pill = QRect(0, 0, w, (_state == State::Failed)
			? (padding.top() + font->height + padding.bottom())
			: h);
		resize(w, h);
		if (_state == State::Failed) {
			_pill.moveLeft((w - _pill.width()) / 2);
			_retry->move((w - _retry->width()) / 2, _pill.height() + 10);
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(ServiceBg());
		if (_state == State::Loading) {
			p.drawEllipse(rect());
			const auto inner = rect().marginsRemoved(QMargins(10, 10, 10, 10));
			if (Ui::AnimationsDisabled()) {
				Ui::InfiniteRadialAnimation::DrawFrame(
					p,
					inner,
					2.5,
					st::historyServiceFg->c,
					400);
			} else {
				_loading.draw(p, inner, 2.5, st::historyServiceFg->c);
			}
			return;
		}
		const auto radius = _pill.height() / 2.;
		p.drawRoundedRect(_pill, radius, radius);
		const auto &padding = st::historyEmptyPadding;
		p.setPen(st::historyServiceFg);
		p.setFont(st::historyEmptyFont);
		p.drawText(
			_pill.marginsRemoved(padding),
			Qt::AlignCenter,
			st::historyEmptyFont->elided(
				_text,
				_pill.width() - padding.left() - padding.right()));
	}

private:
	State _state = State::Hidden;
	QString _text;
	QRect _pill;
	Ui::RoundButton *_retry = nullptr;
	Ui::InfiniteRadialAnimation _loading;

};

// ---- the section

Widget::Widget(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &chatId,
	qint64 aroundSeq)
: SectionWidget(parent, controller)
, _session(&controller->session())
, _chatId(chatId)
, _aroundSeq(aroundSeq)
, _group(Data::IsGroupId(chatId)) {
	setAcceptDrops(true);
	setFocusPolicy(Qt::StrongFocus);
	setupList();
	setupTopBar();
	setupBars();
	setupCompose();

	_down = Ui::CreateChild<DownButton>(this);
	_down->hide();
	_down->setClickedCallback([this] {
		_list->jumpToBottom();
		setInnerFocus();
	});
	Ui::InstallTooltip(_down, [] { return tr::lng_chat_jump_bottom(tr::now); });
	_state = Ui::CreateChild<StateView>(this);
	_state->setRetryCallback([this] { _list->retryLoad(); });
	_drop = Ui::CreateChild<DropArea>(this);

	auto &data = _session->data();
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](const QString &id) {
		if (id == _chatId) {
			refreshChat();
		}
	});
	connect(&data.chats(), &Data::Chats::chatRemoved, this, [this](const QString &id) {
		if (id != _chatId || _leaving) {
			return;
		}
		// Deleted here or on another device: nothing to show any more.
		_leaving = true;
		const auto weak = QPointer<Widget>(this);
		const auto window = this->controller();
		Ui::PostponeCall(window, [=] {
			if (weak && window->mainSection() == weak.data()) {
				if (!window->showBack()) {
					window->clearMainStack();
				}
			}
		});
	});
	connect(
		&data.conversations(),
		&Data::Conversations::stateChanged,
		this,
		[this](const QString &chat) {
			if (chat == _chatId) {
				refreshChat();
			}
		});
	connect(controller, &Window::SessionController::mainSectionChanged, this, [this] {
		refreshActive();
	});

	if (_group) {
		_groupTimer.setSingleShot(false);
		connect(&_groupTimer, &QTimer::timeout, this, [this] { refreshBottom(); });
		// Removed, banned, left on another device, or the group is gone: say
		// why before the screen closes (chatRemoved above closes it).
		connect(&data.groups(), &Data::Groups::groupRemoved, this, [this](
				const QString &group,
				const QString &status) {
			if (group != _chatId) {
				return;
			}
			this->controller()->showToast((status == u"banned")
				? tr::lng_groups_gone_banned(tr::now)
				: (status == u"deleted")
				? tr::lng_groups_gone_deleted(tr::now)
				: (status == u"kicked")
				? tr::lng_groups_gone_kicked(tr::now)
				: (status == u"left")
				? tr::lng_groups_left_toast(tr::now)
				: tr::lng_groups_gone_member(tr::now));
		});
	}
	if (!data.chats().chat(_chatId)) {
		_headerRequested = true;
		data.chats().loadHeader(_chatId, this, [this](const Data::ChatInfo &) {
			refreshChat();
		}, [this](const Api::Error &error) {
			Support::ShowError(this->controller(), error);
		});
	}
	refreshChat();
	_list->start(_aroundSeq);
	refreshState();
}

Widget::~Widget() {
	auto &chats = _session->data().chats();
	if (chats.visibleChat() == _chatId) {
		chats.setVisibleChat(QString());
	}
	if (const auto top = window()) {
		top->removeEventFilter(this);
	}
}

QString Widget::sectionKey() const {
	return u"chat/"_s + _chatId;
}

void Widget::setupList() {
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this);
	_list = _scroll->setOwnedWidget(Ui::object_ptr<ListWidget>(
		_scroll,
		controller(),
		_chatId,
		_scroll));
	connect(_list, &ListWidget::replyRequested, this, [this](
			const Data::Message &message,
			const std::optional<Data::Quote> &quote) {
		if (_bottom == Bottom::Compose) {
			_compose->replyTo(message, quote);
		}
	});
	connect(_list, &ListWidget::editRequested, this, [this](const Data::Message &message) {
		if (_bottom == Bottom::Compose) {
			_compose->editMessage(message);
		}
	});
	connect(_list, &ListWidget::forwardRequested, this, [this](
			const QVector<Data::Message> &messages) {
		const auto weak = QPointer<Widget>(this);
		ShowForwardBox(controller(), messages, [=] {
			if (weak) {
				weak->_list->clearSelection();
			}
		});
	});
	connect(_list, &ListWidget::deleteRequested, this, [this](
			const QVector<Data::Message> &messages) {
		const auto weak = QPointer<Widget>(this);
		ShowDeleteMessagesBox(controller(), _chatId, messages, [=] {
			if (weak) {
				weak->_list->clearSelection();
			}
		});
	});
	connect(_list, &ListWidget::selectionChanged, this, [this] { selectionChanged(); });
	connect(_list, &ListWidget::stateChanged, this, [this] {
		refreshDownButton();
		refreshState();
	});
	connect(_list, &ListWidget::focusRequested, this, [this] { setInnerFocus(); });
	connect(_scroll, &Ui::ScrollArea::scrolled, this, [this] { refreshDownButton(); });
}

void Widget::setupTopBar() {
	_topBar = Ui::CreateChild<TopBar>(this, controller(), _chatId);
	connect(_topBar, &TopBar::backRequested, this, [this] {
		controller()->showBack();
	});
	connect(_topBar, &TopBar::profileRequested, this, [this] {
		const auto userId = _topBar->profileUserId();
		if (_group) {
			controller()->openRoute(Window::Routes::Group(_chatId));
		} else if (!userId.isEmpty()) {
			controller()->openRoute(Window::Routes::User(userId));
		}
	});
	connect(_topBar, &TopBar::searchRequested, this, [this] { showSearch(); });
	connect(_topBar, &TopBar::moreRequested, this, [this] { showMoreMenu(); });
	connect(_topBar, &TopBar::selectionCancel, this, [this] { _list->clearSelection(); });
	connect(_topBar, &TopBar::selectionCopy, this, [this] { copySelection(); });
	connect(_topBar, &TopBar::selectionForward, this, [this] { forwardSelection(); });
	connect(_topBar, &TopBar::selectionDelete, this, [this] { deleteSelection(); });
}

void Widget::setupBars() {
	_pinned = Ui::CreateChild<PinnedBar>(this, _session, _chatId);
	_pinned->setVisible(_pinned->hasPins());
	connect(_pinned, &PinnedBar::jumpRequested, this, [this](qint64 seq) {
		_list->jumpTo(seq);
	});
	connect(_pinned, &PinnedBar::unpinRequested, this, [this](const Data::Message &message) {
		unpin(message.seq);
	});
	connect(_pinned, &PinnedBar::pinsChanged, this, [this] {
		_pinned->setVisible(_pinned->hasPins());
		updateLayout();
	});
	_banner = Ui::CreateChild<ConversationBanner>(this, controller(), _chatId);
	_banner->setVisible(!_banner->empty());
	connect(_banner, &ConversationBanner::stateChanged, this, [this] {
		_banner->setVisible(!_banner->empty());
		updateLayout();
	});
}

void Widget::setupCompose() {
	_compose = Ui::CreateChild<ComposeControls>(this, controller(), _chatId, this);
	_compose->hide();
	_compose->setNameResolver([this](const Data::Message &message) {
		return _list->senderName(message);
	});
	_compose->setLastEditable([this] { return _list->lastEditable(); });
	connect(_compose, &Ui::RpWidget::heightChanged, this, [this] { updateLayout(); });
	connect(_compose, &ComposeControls::sent, this, [this] {
		_list->clearSelection();
		_list->jumpToBottom();
	});
	connect(_compose, &ComposeControls::jumpRequested, this, [this](qint64 seq) {
		_list->jumpTo(seq);
	});
	connect(_compose, &ComposeControls::scrollRequested, this, [this](bool up) {
		_list->scrollByPage(up);
	});
	connect(_compose, &ComposeControls::copyRequested, this, [this] { copySelection(); });
	_notice = Ui::CreateChild<BottomNotice>(this);
	_notice->hide();
}

// ---- the chat's state

void Widget::refreshChat() {
	const auto chat = _session->data().chats().chat(_chatId);
	if (chat) {
		_list->setTheme((chat->isConversation || chat->saved) ? QString() : chat->theme);
		_list->setAnonymousPeer(chat->anonymous);
		_down->setUnread(chat->unread);
	}
	refreshRelationship();
	refreshBottom();
	refreshState();
}

void Widget::refreshRelationship() {
	const auto chat = _session->data().chats().chat(_chatId);
	const auto userId = (chat && !chat->isConversation && !chat->saved && !chat->peerDeleted)
		? chat->peerUserId
		: QString();
	if (userId.isEmpty() || _relationshipFor == userId) {
		return;
	}
	_relationshipFor = userId;
	_session->data().graph().relationship(userId, this, [this](
			const Data::GraphState &state) {
		_relationshipKnown = true;
		if (_blockedByYou != state.blockedByYou) {
			_blockedByYou = state.blockedByYou;
			refreshBottom();
		}
	});
}

void Widget::refreshBottom() {
	const auto chat = _session->data().chats().chat(_chatId);
	auto bottom = Bottom::None;
	if (!chat) {
		bottom = Bottom::None;
	} else if (chat->isGroup) {
		// What the header says I may do; slow mode makes me wait between
		// two messages of mine.
		const auto now = QDateTime::currentMSecsSinceEpoch();
		const auto group = chat->group ? *chat->group : Data::Group();
		const auto until = _session->data().groups().slowModeUntil(_chatId);
		switch (Data::GroupRules::ComposerFor(
			group.canSend,
			group.sendBlocked,
			until,
			now)) {
		case Data::GroupRules::Composer::Compose:
			bottom = Bottom::Compose;
			break;
		case Data::GroupRules::Composer::Restricted:
			bottom = Bottom::GroupRestricted;
			break;
		case Data::GroupRules::Composer::Wait:
			bottom = Bottom::GroupWait;
			break;
		}
	} else if (chat->peerDeleted) {
		bottom = Bottom::Deleted;
	} else if (chat->request == u"incoming") {
		bottom = Bottom::Request;
	} else if (_blockedByYou) {
		bottom = Bottom::Blocked;
	} else if (chat->isConversation
		&& ConversationEnded(chat->conversation ? chat->conversation->state : chat->state)) {
		bottom = Bottom::Ended;
	} else if (!chat->canSend) {
		// A conversation that can only be read is over for writing.
		bottom = chat->isConversation ? Bottom::Ended : Bottom::Readonly;
	} else {
		bottom = Bottom::Compose;
	}
	const auto changed = (_bottom != bottom);
	_bottom = bottom;
	const auto writable = (bottom == Bottom::Compose);
	const auto groupNotice = (bottom == Bottom::GroupRestricted)
		|| (bottom == Bottom::GroupWait);
	_list->setCanWrite(writable);
	// Someone who waits or is restricted in a group still reacts.
	_list->setCanReact(writable || groupNotice);
	if (groupNotice) {
		// The words count down: a restriction by the minute, slow mode by
		// the second.
		const auto now = QDateTime::currentMSecsSinceEpoch();
		auto text = QString();
		if (bottom == Bottom::GroupWait) {
			const auto left = Data::GroupRules::WaitSeconds(
				_session->data().groups().slowModeUntil(_chatId),
				now);
			text = tr::lng_groups_slow_wait(
				tr::now,
				lt_time,
				tr::lng_groups_time_seconds(tr::now, lt_count, int(left)));
			_groupTimer.start(1000);
		} else {
			const auto left = (chat->group && chat->group->restrictedUntil > 0)
				? Support::GroupTimeLeft(chat->group->restrictedUntil, now)
				: QString();
			text = left.isEmpty()
				? tr::lng_groups_cannot_send(tr::now)
				: tr::lng_groups_restricted_for(tr::now, lt_time, left);
			if (left.isEmpty()) {
				_groupTimer.stop();
			} else {
				_groupTimer.start(20 * 1000);
			}
		}
		if (!changed && _groupNotice == text) {
			return;
		}
		_groupNotice = text;
	} else {
		_groupTimer.stop();
		_groupNotice = QString();
		if (!changed && bottom != Bottom::Request) {
			return;
		}
	}
	const auto session = _session;
	const auto chatId = _chatId;
	_compose->setVisible(writable);
	if (!writable) {
		_compose->panel()->hideFast();
	}
	_notice->setVisible(bottom != Bottom::Compose && bottom != Bottom::None);
	const auto name = chat ? Support::ChatTitle(_session, *chat) : QString();
	switch (bottom) {
	case Bottom::Request:
		_notice->setContent(
			name + u' ' + tr::lng_chat_wants_to_message(tr::now),
			tr::lng_history_request_about(tr::now),
			std::vector<NoticeButton>{
				NoticeButton{
					tr::lng_chat_block(tr::now),
					&st::historyDangerFlatButton,
					[this] { confirmBlock(); },
				},
				NoticeButton{
					tr::lng_decline(tr::now),
					&st::defaultFlatButton,
					[=] { session->data().chats().declineRequest(chatId); },
				},
				NoticeButton{
					tr::lng_accept(tr::now),
					&st::defaultRoundButton,
					[=] { session->data().chats().acceptRequest(chatId); },
				},
			});
		break;
	case Bottom::Blocked:
		_notice->setContent(QString(), tr::lng_history_blocked_notice(tr::now), std::vector<NoticeButton>{
			NoticeButton{
				tr::lng_unblock(tr::now),
				&st::defaultFlatButton,
				[this] {
					const auto chat = _session->data().chats().chat(_chatId);
					if (!chat || chat->peerUserId.isEmpty()) {
						return;
					}
					_notice->setLoading(true);
					_session->data().graph().unblock(chat->peerUserId, this, [this](
							const Data::GraphState &state) {
						_blockedByYou = state.blockedByYou;
						refreshBottom();
					}, [this](const Api::Error &error) {
						_notice->setLoading(false);
						Support::ShowError(controller(), error);
					});
				},
			},
		});
		break;
	case Bottom::Deleted:
		_notice->setContent(QString(), tr::lng_history_deleted_notice(tr::now), {});
		break;
	case Bottom::Ended:
		_notice->setContent(QString(), tr::lng_chat_conversation_has_ended(tr::now), {});
		break;
	case Bottom::Readonly:
		_notice->setContent(QString(), tr::lng_chat_cant_message_chat(tr::now), {});
		break;
	case Bottom::GroupRestricted:
	case Bottom::GroupWait:
		_notice->setContent(QString(), _groupNotice, {});
		break;
	case Bottom::Compose:
	case Bottom::None:
		break;
	}
	updateLayout();
	if (writable && _shown && !_list->selectedCount()) {
		_compose->setInnerFocus();
	}
}

void Widget::refreshState() {
	using State = StateView::State;
	if (!_list->empty()) {
		_state->setState(State::Hidden);
	} else if (_list->loadFailed()) {
		_state->setState(State::Failed, tr::lng_history_load_failed(tr::now));
	} else if (_list->loading()) {
		_state->setState(State::Loading);
	} else {
		const auto chat = _session->data().chats().chat(_chatId);
		_state->setState(State::Empty, (chat && chat->waitingForListener())
			? tr::lng_chat_waiting_listener(tr::now)
			: tr::lng_history_empty(tr::now));
	}
	if (!_state->isHidden()) {
		_state->relayout();
		_state->move(
			_scroll->x() + (_scroll->width() - _state->width()) / 2,
			_scroll->y() + (_scroll->height() - _state->height()) / 2);
		_state->raise();
	}
}

void Widget::refreshDownButton() {
	const auto shown = _list->awayFromBottom() && !_list->empty();
	if (_down->isHidden() == shown) {
		_down->setVisible(shown);
		if (shown) {
			_down->raise();
		}
	}
}

void Widget::refreshActive() {
	const auto top = window();
	// Scripted runs have no window manager to activate the window.
	const auto active = _shown
		&& isVisible()
		&& top
		&& (top->isActiveWindow() || Core::App().commandLine().ephemeral())
		&& (controller()->mainSection() == this);
	_list->setActive(active);
	auto &chats = _session->data().chats();
	if (active) {
		chats.setVisibleChat(_chatId);
	} else if (chats.visibleChat() == _chatId) {
		chats.setVisibleChat(QString());
	}
}

void Widget::selectionChanged() {
	const auto count = _list->selectedCount();
	const auto selected = _list->selectedMessages();
	const auto canForward = std::any_of(selected.begin(), selected.end(), CanForward);
	_topBar->setSelection(count, canForward);
	if (count > 0 && _search) {
		hideSearch();
	}
}

void Widget::copySelection() {
	if (_list->copySelection()) {
		controller()->showToast(tr::lng_copied(tr::now));
		_list->clearSelection();
	}
}

void Widget::forwardSelection() {
	const auto weak = QPointer<Widget>(this);
	ShowForwardBox(controller(), _list->selectedMessages(), [=] {
		if (weak) {
			weak->_list->clearSelection();
		}
	});
}

void Widget::deleteSelection() {
	const auto weak = QPointer<Widget>(this);
	ShowDeleteMessagesBox(controller(), _chatId, _list->selectedMessages(), [=] {
		if (weak) {
			weak->_list->clearSelection();
		}
	});
}

void Widget::unpin(qint64 seq) {
	if (seq <= 0) {
		return;
	}
	auto &messages = _session->data().messages();
	auto scope = u"both"_s;
	for (const auto &known : messages.history(_chatId).pins) {
		if (known.message.seq == seq && !known.scope.isEmpty()) {
			scope = known.scope;
		}
	}
	messages.unpin(_chatId, seq, scope);
	controller()->showToast(tr::lng_history_unpinned(tr::now));
}

// ---- search

void Widget::showSearch() {
	if (_search) {
		_search->activate();
		return;
	}
	_list->clearSelection();
	_search = Ui::CreateChild<SearchBar>(this, _session, _chatId);
	connect(_search, &SearchBar::jumpRequested, this, [this](qint64 seq) {
		_list->jumpTo(seq);
	});
	connect(_search, &SearchBar::closeRequested, this, [this] { hideSearch(); });
	_topBar->hide();
	_search->show();
	updateLayout();
	_search->activate();
}

void Widget::hideSearch() {
	if (!_search) {
		return;
	}
	const auto search = std::exchange(_search, nullptr);
	search->hide();
	search->deleteLater();
	_topBar->show();
	updateLayout();
	setInnerFocus();
}

// ---- the «…» menu

void Widget::fillMuteMenu(Ui::PopupMenu *menu) {
	const auto config = _session->data().clientConfig();
	auto durations = config ? config->muteDurations() : QStringList();
	if (durations.isEmpty()) {
		durations = QStringList{ u"1h"_s, u"8h"_s, u"2d"_s, u"forever"_s };
	}
	const auto session = _session;
	const auto chatId = _chatId;
	for (const auto &duration : std::as_const(durations)) {
		if (duration == u"off") {
			continue;
		}
		menu->addAction(MuteLabel(duration), [=] {
			session->data().chats().muteFor(chatId, duration);
		});
	}
}

void Widget::showMoreMenu() {
	if (_menu) {
		_menu->hideMenu();
		return;
	}
	const auto chat = _session->data().chats().chat(_chatId);
	if (!chat) {
		return;
	}
	const auto session = _session;
	const auto chatId = _chatId;
	const auto window = controller();
	_menu = new Ui::PopupMenu(this);
	const auto menu = _menu.data();
	const auto userId = _topBar->profileUserId();
	if (!userId.isEmpty()) {
		menu->addAction(tr::lng_history_view_profile(tr::now), [=] {
			window->openRoute(Window::Routes::User(userId));
		}, &st::menuIconUser);
	}
	menu->addAction(tr::lng_history_search(tr::now), [this] {
		showSearch();
	}, &st::menuIconSearch);
	if (!chat->saved) {
		if (chat->muted) {
			menu->addAction(tr::lng_chat_unmute(tr::now), [=] {
				session->data().chats().setMuted(chatId, false);
			}, &st::menuIconUnmute);
		} else {
			auto item = Ui::MenuItem();
			item.text = tr::lng_chat_mute(tr::now);
			item.icon = &st::menuIconMute;
			item.fillSubmenu = [this](Ui::PopupMenu *submenu) {
				fillMuteMenu(submenu);
			};
			menu->addItem(std::move(item));
		}
	}
	if (!chat->isRequest()) {
		const auto pinned = chat->pinned;
		menu->addAction(
			pinned ? tr::lng_history_unpin_chat(tr::now) : tr::lng_history_pin_chat(tr::now),
			[=] { session->data().chats().setPinned(chatId, !pinned); },
			pinned ? &st::menuIconUnpin : &st::menuIconPin);
	}
	if (!chat->isConversation
		&& !chat->saved
		&& !chat->peerDeleted
		&& !chat->isRequest()
		&& !chat->isGroup) {
		menu->addAction(tr::lng_history_change_theme(tr::now), [this] {
			showThemeBox();
		}, &st::menuIconTheme);
	}
	menu->addSeparator();
	if (chat->isConversation || chat->isGroup) {
		// The module of this kind of chat owns these («پایان گفتگو»,
		// «مسدود کردن»; «اطلاعات گروه», «ترک گروه»); the chat list's row
		// menu gets the same ones.
		Window::FillChatMenu(menu, window, chatId);
	} else if (!chat->saved && !chat->peerDeleted && !chat->peerUserId.isEmpty()) {
		if (_blockedByYou) {
			const auto peer = chat->peerUserId;
			menu->addAction(tr::lng_unblock(tr::now), [=, this] {
				session->data().graph().unblock(peer, this, [this](
						const Data::GraphState &state) {
					_blockedByYou = state.blockedByYou;
					refreshBottom();
				}, [=](const Api::Error &error) {
					Support::ShowError(window, error);
				});
			}, &st::menuIconBlock);
		} else {
			auto item = Ui::MenuItem();
			item.text = tr::lng_block_user(tr::now);
			item.icon = &st::menuIconBlock;
			item.destructive = true;
			item.callback = [this] { confirmBlock(); };
			menu->addItem(std::move(item));
		}
	}
	if (!chat->isConversation) {
		menu->addAction(tr::lng_history_clear_history(tr::now), [=] {
			ShowClearHistoryBox(window, chatId);
		}, &st::menuIconClear);
	}
	{
		auto item = Ui::MenuItem();
		item.text = tr::lng_chat_delete_chat(tr::now);
		item.icon = &st::menuIconDelete;
		item.destructive = true;
		item.callback = [=] { ShowDeleteChatBox(window, chatId); };
		menu->addItem(std::move(item));
	}
	menu->popupAnchored(_topBar->moreButton());
}

void Widget::showThemeBox() {
	controller()->show(ChatThemeBox(_session, _chatId));
}

void Widget::confirmBlock() {
	const auto chat = _session->data().chats().chat(_chatId);
	if (!chat) {
		return;
	}
	const auto session = _session;
	const auto chatId = _chatId;
	const auto window = controller();
	const auto weak = QPointer<Widget>(this);
	const auto name = Support::ChatTitle(_session, *chat);
	const auto conversationId = (chat->isConversation && chat->conversation)
		? chat->conversation->id
		: QString();
	const auto userId = chat->peerUserId;
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_block_user(tr::now);
	args.text = tr::lng_history_block_sure(tr::now, lt_name, name);
	args.confirmText = tr::lng_chat_block(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		const auto fail = [=](const Api::Error &error) {
			Support::ShowError(window, error);
		};
		if (!conversationId.isEmpty()) {
			session->data().conversations().block(conversationId, window, [=] {
				window->showToast(tr::lng_blocked_toast(tr::now));
			}, fail);
		} else if (!userId.isEmpty()) {
			session->data().graph().block(userId, window, [=](const Data::GraphState &state) {
				window->showToast(tr::lng_blocked_toast(tr::now));
				if (weak) {
					weak->_blockedByYou = state.blockedByYou;
					weak->refreshBottom();
				}
				// A blocked stranger's request goes away with them.
				const auto row = session->data().chats().chat(chatId);
				if (row && row->request == u"incoming") {
					session->data().chats().declineRequest(chatId);
				}
			}, fail);
		}
		close();
	};
	controller()->show(Ui::MakeConfirmBox(std::move(args)));
}

// ---- section API

void Widget::showFinished() {
	_shown = true;
	if (const auto top = window()) {
		top->installEventFilter(this);
	}
	refreshActive();
	setInnerFocus();
}

void Widget::showMessage(qint64 seq) {
	if (seq > 0) {
		_list->jumpTo(seq);
	}
}

void Widget::setInnerFocus() {
	if (_search) {
		_search->activate();
	} else if (_bottom == Bottom::Compose && !_list->selectedCount()) {
		_compose->setInnerFocus();
	} else {
		setFocus();
	}
}

bool Widget::onBackRequest() {
	if (_drop && !_drop->isHidden()) {
		_drop->hide();
		return true;
	} else if (_list->selectedCount() > 0 || _list->hasTextSelection()) {
		_list->clearSelection();
		return true;
	} else if (_search) {
		hideSearch();
		return true;
	} else if (_bottom == Bottom::Compose && _compose->cancelCurrent()) {
		return true;
	}
	return false;
}

void Widget::setBackButtonShown(bool shown) {
	SectionWidget::setBackButtonShown(shown);
	_topBar->setBackShown(shown);
}

bool Widget::eventFilter(QObject *object, QEvent *event) {
	if (object == window()
		&& (event->type() == QEvent::WindowActivate
			|| event->type() == QEvent::WindowDeactivate)) {
		Ui::PostponeCall(this, [this] { refreshActive(); });
	}
	return SectionWidget::eventFilter(object, event);
}

void Widget::showEvent(QShowEvent *e) {
	SectionWidget::showEvent(e);
	Ui::PostponeCall(this, [this] { refreshActive(); });
}

void Widget::hideEvent(QHideEvent *e) {
	SectionWidget::hideEvent(e);
	if (_bottom == Bottom::Compose) {
		_compose->flushDraft();
	}
	refreshActive();
}

void Widget::keyPressEvent(QKeyEvent *e) {
	const auto ctrl = (e->modifiers() & Qt::ControlModifier) != 0;
	if (ctrl && e->key() == Qt::Key_F) {
		showSearch();
	} else if (e->matches(QKeySequence::Copy)) {
		copySelection();
	} else if (e->key() == Qt::Key_Delete && _list->selectedCount() > 0) {
		deleteSelection();
	} else if (e->key() == Qt::Key_PageUp || e->key() == Qt::Key_PageDown) {
		_list->scrollByPage(e->key() == Qt::Key_PageUp);
	} else if (!e->text().isEmpty()
		&& e->text().front().isPrint()
		&& !ctrl
		&& _bottom == Bottom::Compose
		&& !_list->selectedCount()) {
		// Typing anywhere in the chat goes to the field.
		_compose->typeFromOutside(e);
	} else {
		SectionWidget::keyPressEvent(e);
	}
}

// ---- drag and drop

bool Widget::canAttach() const {
	if (_bottom != Bottom::Compose) {
		return false;
	} else if (_group) {
		const auto chat = _session->data().chats().chat(_chatId);
		return chat && chat->group && chat->group->can(u"send_media");
	}
	return true;
}

void Widget::dragEnterEvent(QDragEnterEvent *e) {
	if (canAttach() && Support::CanAcceptDrop(e->mimeData())) {
		e->acceptProposedAction();
		_drop->setGeometry(_scroll->geometry());
		_drop->show();
		_drop->raise();
	} else {
		e->ignore();
	}
}

void Widget::dragMoveEvent(QDragMoveEvent *e) {
	if (canAttach() && Support::CanAcceptDrop(e->mimeData())) {
		e->acceptProposedAction();
	} else {
		e->ignore();
	}
}

void Widget::dragLeaveEvent(QDragLeaveEvent *e) {
	Q_UNUSED(e);
	_drop->hide();
}

void Widget::dropEvent(QDropEvent *e) {
	_drop->hide();
	if (!canAttach()) {
		return;
	}
	const auto data = Support::ReadMimeData(e->mimeData());
	if (!data.files.isEmpty()) {
		e->acceptProposedAction();
		_compose->attachFiles(data.files);
	} else if (!data.image.isNull()) {
		e->acceptProposedAction();
		_compose->attachImage(data.image);
	}
}

#ifdef ELEZA_DEBUG_TOOLS
void Widget::debugShowDropArea() {
	_drop->setGeometry(_scroll->geometry());
	_drop->show();
	_drop->raise();
}
#endif // ELEZA_DEBUG_TOOLS

// ---- layout

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateLayout();
}

bool Widget::handleCommand(Window::Command command) {
	if (command == Window::Command::Find) {
		showSearch();
		return true;
	}
	return false;
}

int Widget::toastBottomSkip() const {
	// The toast is centred in the window, the bottom controls span this
	// column only; lifting it is right for both the wide and the narrow layout.
	if (isHidden()) {
		return 0;
	} else if (!_compose->isHidden()) {
		return _compose->height();
	} else if (!_notice->isHidden()) {
		return _notice->height();
	}
	return 0;
}

void Widget::updateLayout() {
	if (width() <= 0 || height() <= 0) {
		return;
	}
	auto top = 0;
	if (_search) {
		_search->resizeToWidth(width());
		_search->move(0, top);
		top += _search->height();
	} else {
		_topBar->resizeToWidth(width());
		_topBar->move(0, top);
		top += _topBar->height();
	}
	if (!_pinned->isHidden()) {
		_pinned->resizeToWidth(width());
		_pinned->move(0, top);
		top += _pinned->height();
	}
	if (!_banner->isHidden()) {
		_banner->resizeToWidth(width());
		_banner->move(0, top);
		top += _banner->height();
	}
	auto bottom = height();
	if (!_compose->isHidden()) {
		_compose->resizeToWidth(width());
		bottom -= _compose->height();
		_compose->move(0, bottom);
	} else if (!_notice->isHidden()) {
		_notice->resizeToWidth(width());
		bottom -= _notice->height();
		_notice->move(0, bottom);
	}
	const auto listGeometry = QRect(0, top, width(), std::max(bottom - top, 0));
	if (_scroll->geometry() != listGeometry) {
		_scroll->setGeometry(listGeometry);
		_list->resizeToWidth(listGeometry.width());
	}
	const auto &margin = st::historyDownMargin;
	_down->moveToRight(
		margin.right() - 4,
		bottom - margin.bottom() - _down->height() + 4,
		width());
	if (!_drop->isHidden()) {
		_drop->setGeometry(listGeometry);
	}
	_compose->updatePanelPosition();
	refreshState();
	refreshDownButton();
}

void Widget::paintEvent(QPaintEvent *e) {
	SectionWidget::paintEvent(e);
}

} // namespace History
