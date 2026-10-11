#include "history/history_top_bar.h"

#include "data/data_calls.h"
#include "data/data_chats.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/connection_label.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/tooltip.h"
#include "window/window_calls.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtGui/QMouseEvent>

#include <cmath>

namespace History {
namespace {

[[nodiscard]] QString ActivityText(const QString &kind) {
	if (kind == u"recording_voice") {
		return tr::lng_action_recording_voice(tr::now);
	} else if (kind == u"uploading_photo") {
		return tr::lng_action_sending_photo(tr::now);
	} else if (kind == u"uploading_video") {
		return tr::lng_action_sending_video(tr::now);
	} else if (kind == u"uploading_file") {
		return tr::lng_action_sending_file(tr::now);
	}
	return tr::lng_action_typing(tr::now);
}

// The dots are animated: the text goes without its ellipsis.
[[nodiscard]] QString WithoutEllipsis(QString text) {
	while (text.endsWith(QChar(0x2026)) || text.endsWith(u'.')) {
		text.chop(1);
	}
	return text.trimmed();
}

} // namespace

TopBar::TopBar(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &chatId)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _chatId(chatId)
, _connection(Support::ConnectionState::For(_session))
, _back(Ui::CreateChild<Ui::IconButton>(this, st::topBarBackButton))
, _userpic(Ui::CreateChild<Support::UserpicButton>(
	this,
	_session,
	QString(),
	st::historyTopBarUserpic))
, _call(Ui::CreateChild<Ui::IconButton>(this, st::topBarCallButton))
, _search(Ui::CreateChild<Ui::IconButton>(this, st::topBarSearchButton))
, _mute(Ui::CreateChild<Ui::IconButton>(this, st::historyTopBarMuteButton))
, _more(Ui::CreateChild<Ui::IconButton>(this, st::topBarMoreButton))
, _selectionCancel(Ui::CreateChild<Ui::IconButton>(this, st::historySelectionCancel))
, _selectionCopy(Ui::CreateChild<Ui::IconButton>(this, st::historySelectionCopy))
, _selectionForward(Ui::CreateChild<Ui::IconButton>(this, st::historySelectionForward))
, _selectionDelete(Ui::CreateChild<Ui::IconButton>(this, st::historySelectionDelete))
, _dots([this](qint64) {
	rtlupdate(titleRect());
	return _typing;
}) {
	setMouseTracking(true);
	_back->setClickedCallback([this] { Q_EMIT backRequested(); });
	_search->setClickedCallback([this] { Q_EMIT searchRequested(); });
	_call->setClickedCallback([this] {
		// It just calls: the person can still hang up.
		const auto chat = _session->data().chats().chat(_chatId);
		if (chat && _callEnabled) {
			Window::StartCall(_controller, chat->peerUserId);
		}
	});
	_more->setClickedCallback([this] { Q_EMIT moreRequested(); });
	_userpic->setClickedCallback([this] {
		if (titleClickable()) {
			Q_EMIT profileRequested();
		}
	});
	_mute->setClickedCallback([this] {
		_session->data().chats().setMuted(_chatId, !_muted);
	});
	_selectionCancel->setClickedCallback([this] { Q_EMIT selectionCancel(); });
	_selectionCopy->setClickedCallback([this] { Q_EMIT selectionCopy(); });
	_selectionForward->setClickedCallback([this] { Q_EMIT selectionForward(); });
	_selectionDelete->setClickedCallback([this] { Q_EMIT selectionDelete(); });

	Ui::InstallTooltip(_back, [] { return tr::lng_back(tr::now); });
	Ui::InstallTooltip(_search, [] { return tr::lng_history_search(tr::now); });
	Ui::InstallTooltip(_call, [this] {
		return _callEnabled
			? tr::lng_calls_voice_call(tr::now)
			: tr::lng_calls_not_possible_here(tr::now);
	});
	Ui::InstallTooltip(_more, [] { return tr::lng_more(tr::now); });
	Ui::InstallTooltip(_mute, [this] {
		return _muted ? tr::lng_chat_unmute(tr::now) : tr::lng_chat_mute(tr::now);
	});
	Ui::InstallTooltip(_selectionCancel, [] { return tr::lng_history_clear_selection(tr::now); });
	Ui::InstallTooltip(_selectionCopy, [] { return tr::lng_copy(tr::now); });
	Ui::InstallTooltip(_selectionForward, [] { return tr::lng_forward(tr::now); });
	Ui::InstallTooltip(_selectionDelete, [] { return tr::lng_delete(tr::now); });

	auto &data = _session->data();
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](const QString &id) {
		if (id == _chatId) {
			refresh();
		}
	});
	connect(&data.chats(), &Data::Chats::activityChanged, this, [this](const QString &id) {
		if (id == _chatId) {
			refreshStatus();
		}
	});
	connect(&data.chats(), &Data::Chats::presenceChanged, this, [this](const QString &user) {
		if (user == _watched) {
			refreshStatus();
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this](const QStringList &) {
		refresh();
	});
	if (_connection) {
		connect(_connection, &Support::ConnectionState::changed, this, [this] {
			refreshStatus();
		});
	}
	refresh();
	updateControls();
}

TopBar::~TopBar() {
	watchPeer(QString());
}

void TopBar::watchPeer(const QString &userId) {
	if (_watched == userId) {
		return;
	}
	auto &chats = _session->data().chats();
	if (!_watched.isEmpty()) {
		chats.unwatchPresence({ _watched });
	}
	_watched = userId;
	if (!_watched.isEmpty()) {
		chats.watchPresence({ _watched });
	}
}

QString TopBar::profileUserId() const {
	const auto chat = _session->data().chats().chat(_chatId);
	return (chat && !chat->anonymous && !chat->saved && !chat->peerDeleted)
		? chat->peerUserId
		: QString();
}

bool TopBar::titleClickable() const {
	return !_selected && !profileUserId().isEmpty();
}

void TopBar::refresh() {
	const auto chat = _session->data().chats().chat(_chatId);
	if (!chat) {
		if (_nameText.isEmpty()) {
			_name.setText(st::historyTopBarNameStyle, QString());
		}
		refreshStatus();
		return;
	}
	const auto name = Support::ChatTitle(_session, *chat);
	const auto topic = chat->isConversation;
	if (_nameText != name || _topic != topic) {
		_nameText = name;
		_topic = topic;
		_name.setText(
			topic ? st::historyTopBarTopicStyle : st::historyTopBarNameStyle,
			name);
	}
	_verified = chat->verified;
	if (_muted != chat->muted) {
		_muted = chat->muted;
	}
	_mute->setIconOverride(
		_muted ? nullptr : &st::historyTopBarUnmuteIcon,
		_muted ? nullptr : &st::historyTopBarUnmuteIconOver);
	// The picture: the peer's, or a special one.
	const auto pictureKey = chat->saved
		? u"saved"_s
		: chat->anonymous
		? (u"anon:"_s + chat->peerRef + name)
		: chat->peerDeleted
		? u"deleted"_s
		: (u"user:"_s + chat->peerUserId);
	if (_userpicFor != pictureKey) {
		_userpicFor = pictureKey;
		if (chat->saved) {
			_userpic->setSpecial(Ui::UserpicKind::Saved);
		} else if (chat->anonymous) {
			_userpic->setSpecial(Ui::UserpicKind::Anonymous, name, chat->peerRef);
		} else if (chat->peerDeleted || chat->peerUserId.isEmpty()) {
			_userpic->setSpecial(
				chat->peerDeleted ? Ui::UserpicKind::Deleted : Ui::UserpicKind::User,
				name,
				chat->id);
		} else {
			_userpic->setUserId(chat->peerUserId);
		}
	}
	const auto availability = Window::CallsSupported()
		? Data::CallAvailabilityFor(*chat)
		: Data::CallAvailability::Hidden;
	_callShown = (availability != Data::CallAvailability::Hidden);
	_callEnabled = (availability == Data::CallAvailability::Available);
	_call->setIconColorOverride(_callEnabled
		? std::nullopt
		: std::make_optional(st::historyTopBarCallDisabledFg->c));
	_call->setPointerCursor(_callEnabled);
	const auto watch = (!chat->anonymous
		&& !chat->saved
		&& !chat->isBot
		&& !chat->peerDeleted)
		? chat->peerUserId
		: QString();
	watchPeer(watch);
	refreshStatus();
	updateControls();
}

void TopBar::refreshStatus() {
	auto &chats = _session->data().chats();
	const auto chat = chats.chat(_chatId);
	auto status = QString();
	auto active = false;
	auto typing = false;
	if (_connection && !_connection->online()) {
		status = _connection->text();
	} else if (!chat) {
		status = QString();
	} else if (const auto activity = chats.activity(_chatId)) {
		status = WithoutEllipsis(ActivityText(activity->kind));
		active = true;
		typing = true;
	} else if (chat->saved || chat->peerDeleted) {
		status = QString();
	} else if (chat->isBot) {
		status = tr::lng_status_bot(tr::now);
	} else if (chat->isConversation) {
		status = chat->anonymous ? tr::lng_anonymous(tr::now) : QString();
	} else if (!chat->peerUserId.isEmpty() && !chat->anonymous) {
		if (const auto presence = chats.presence(chat->peerUserId)) {
			status = Lang::LastSeen(presence->status, presence->lastSeenAt);
			active = presence->online();
		}
	}
	if (_status == status && _statusActive == active && _typing == typing) {
		return;
	}
	_status = status;
	_statusActive = active;
	_typing = typing;
	if (_typing && !Ui::AnimationsDisabled()) {
		_dots.start();
	} else {
		_dots.stop();
	}
	update();
}

void TopBar::setBackShown(bool shown) {
	if (_backShown != shown) {
		_backShown = shown;
		updateControls();
		update();
	}
}

void TopBar::setSelection(int count, bool canForward) {
	if (_selected == count && _selectedForward == canForward) {
		return;
	}
	_selected = count;
	_selectedForward = canForward;
	updateControls();
	update();
}

int TopBar::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::historyTopBarHeight;
}

void TopBar::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateControls();
}

void TopBar::updateControls() {
	const auto &padding = st::historyTopBarPadding;
	const auto selecting = (_selected > 0);
	const auto top = (height() - _back->height()) / 2;
	_back->setVisible(_backShown && !selecting);
	_userpic->setVisible(!selecting);
	_call->setVisible(!selecting && _callShown);
	_search->setVisible(!selecting);
	_mute->setVisible(!selecting);
	_more->setVisible(!selecting);
	_selectionCancel->setVisible(selecting);
	_selectionCopy->setVisible(selecting);
	_selectionForward->setVisible(selecting && _selectedForward);
	_selectionDelete->setVisible(selecting);

	auto left = padding.left();
	if (selecting) {
		_selectionCancel->moveToLeft(left, top, width());
	} else {
		if (_backShown) {
			_back->moveToLeft(left, top, width());
			left += _back->width() + 4;
		} else {
			left += 8;
		}
		_userpic->moveToLeft(
			left,
			(height() - st::historyTopBarUserpic) / 2,
			width());
	}
	auto right = padding.right();
	if (selecting) {
		for (const auto button : { _selectionDelete, _selectionForward, _selectionCopy }) {
			if (!button->isHidden()) {
				button->moveToRight(right, top, width());
				right += button->width();
			}
		}
	} else {
		for (const auto button : { _more, _mute, _search, _call }) {
			if (button->isHidden()) {
				continue;
			}
			button->moveToRight(right, top, width());
			right += button->width();
		}
	}
}

QRect TopBar::titleRect() const {
	const auto &padding = st::historyTopBarPadding;
	auto left = padding.left() + (_backShown ? (_back->width() + 4) : 8);
	left += st::historyTopBarUserpic + st::historyTopBarUserpicSkip;
	const auto right = padding.right()
		+ _more->width()
		+ _mute->width()
		+ _search->width()
		+ (_callShown ? _call->width() : 0)
		+ 8;
	return QRect(left, 0, std::max(width() - left - right, 0), height());
}

void TopBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::historyBarBg);
	p.fillRect(
		0,
		height() - st::lineWidth,
		width(),
		st::lineWidth,
		st::historyBarLineFg);
	if (_selected > 0) {
		const auto left = st::historyTopBarPadding.left()
			+ _selectionCancel->width()
			+ st::historyTopBarSelectionSkip;
		p.setPen(st::textPrimary);
		p.setFont(st::historyTopBarSelectionFont);
		p.drawTextLeft(
			left,
			(height() - st::historyTopBarSelectionFont->height) / 2,
			width(),
			tr::lng_history_selected_count(tr::now, lt_count, _selected));
		return;
	}
	const auto title = titleRect();
	if (title.width() <= 0) {
		return;
	}
	const auto &icon = st::historyTopBarVerified;
	const auto verified = _verified
		? (icon.width() + st::historyTopBarVerifiedSkip)
		: 0;
	const auto nameWidth = std::min(
		_name.maxWidth(),
		std::max(title.width() - verified, 0));
	const auto hasStatus = !_status.isEmpty();
	const auto nameTop = hasStatus
		? int(st::historyTopBarNameTop)
		: (height() - _name.minHeight()) / 2;
	p.setPen(st::textPrimary);
	_name.drawLeftElided(p, title.x(), nameTop, nameWidth, width());
	if (_verified) {
		icon.paint(
			p,
			title.x() + nameWidth + st::historyTopBarVerifiedSkip,
			nameTop + (_name.minHeight() - icon.height()) / 2 + 1,
			width());
	}
	if (!hasStatus) {
		return;
	}
	const auto &font = st::historyTopBarStatusFont;
	const auto dots = _typing
		? (3 * st::historyTopBarDotSize + 3 * st::historyTopBarDotSkip)
		: 0;
	const auto available = title.width() - dots;
	const auto text = (font->width(_status) > available)
		? font->elided(_status, available)
		: _status;
	const auto textWidth = font->width(text);
	p.setFont(font);
	p.setPen(_statusActive
		? st::historyTopBarStatusFgActive
		: st::historyTopBarStatusFg);
	p.drawTextLeft(title.x(), st::historyTopBarStatusTop, width(), text, textWidth);
	if (_typing) {
		// Three dots pulsing one after another.
		auto hq = PainterHighQualityEnabler(p);
		const auto size = st::historyTopBarDotSize;
		const auto skip = st::historyTopBarDotSkip;
		const auto phase = (Ui::Now() % 1200) / 1200.;
		p.setPen(Qt::NoPen);
		for (auto i = 0; i != 3; ++i) {
			const auto shifted = std::fmod(phase - i * (0.2 / 1.2) + 1., 1.);
			const auto opacity = (shifted < 0.3)
				? (0.3 + 0.7 * (shifted / 0.3))
				: (shifted < 0.6)
				? (1. - 0.7 * ((shifted - 0.3) / 0.3))
				: 0.3;
			auto color = st::historyTopBarStatusFgActive->c;
			color.setAlphaF(float(color.alphaF() * opacity));
			p.setBrush(color);
			p.drawEllipse(myrtlrect(
				title.x() + textWidth + skip + 1 + i * (size + skip),
				st::historyTopBarStatusTop + font->ascent - size,
				size,
				size));
		}
	}
}

void TopBar::mouseMoveEvent(QMouseEvent *e) {
	const auto area = myrtlrect(titleRect());
	const auto over = titleClickable() && area.contains(e->pos());
	if (_titleOver != over) {
		_titleOver = over;
		setCursor(over ? style::cur_pointer : style::cur_default);
	}
}

void TopBar::mousePressEvent(QMouseEvent *e) {
	_titlePressed = (e->button() == Qt::LeftButton) && _titleOver;
}

void TopBar::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_titlePressed, false);
	if (pressed
		&& e->button() == Qt::LeftButton
		&& titleClickable()
		&& myrtlrect(titleRect()).contains(e->pos())) {
		Q_EMIT profileRequested();
	}
}

void TopBar::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_titleOver) {
		_titleOver = false;
		setCursor(style::cur_default);
	}
}

} // namespace History
