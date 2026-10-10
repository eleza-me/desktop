#include "history/history_bars.h"

#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "history/history_logic.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QTimerEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

namespace History {
namespace {

constexpr auto kBannerTick = 30 * 1000;
constexpr auto kSearchDelay = 180;
constexpr auto kPinsReloadDelay = 900;
constexpr auto kPinsRequestGap = 3000;

[[nodiscard]] QString ClockRemaining(qint64 expiresAt) {
	const auto left = std::max(
		expiresAt * 1000 - QDateTime::currentMSecsSinceEpoch(),
		qint64(0));
	const auto hours = left / 3600'000;
	const auto minutes = (left % 3600'000) / 60'000;
	return tr::lng_conv_time_left(
		tr::now,
		lt_time,
		Lang::Num(u"%1:%2"_s
			.arg(hours, 2, 10, QChar(u'0'))
			.arg(minutes, 2, 10, QChar(u'0'))));
}

[[nodiscard]] int DaysLeft(qint64 until) {
	const auto left = until * 1000 - QDateTime::currentMSecsSinceEpoch();
	return int(std::max((left + 86'400'000 - 1) / 86'400'000, qint64(1)));
}

} // namespace

// ---- PinnedBar

PinnedBar::PinnedBar(QWidget *parent, Main::Session *session, const QString &chatId)
: RpWidget(parent)
, _session(session)
, _chatId(chatId)
, _unpin(Ui::CreateChild<Ui::IconButton>(this, st::historyPinnedUnpin)) {
	setCursor(style::cur_pointer);
	_reloadTimer.setSingleShot(true);
	connect(&_reloadTimer, &QTimer::timeout, this, [this] {
		_requestedAt = Ui::Now();
		_session->data().messages().loadPins(_chatId, this);
	});
	_requestedAt = Ui::Now();
	session->data().messages().loadPins(_chatId, this);
	_unpin->setClickedCallback([this] {
		if (const auto pin = current()) {
			Q_EMIT unpinRequested(pin->message);
		}
	});
	Ui::InstallTooltip(_unpin, [] { return tr::lng_unpin(tr::now); });
	auto &messages = session->data().messages();
	connect(&messages, &Data::Messages::pinsChanged, this, [this](const QString &chat) {
		if (chat == _chatId) {
			refresh();
		}
	});
	connect(&messages, &Data::Messages::historyChanged, this, [this](const QString &chat) {
		// An edited or deleted pinned message.
		if (chat == _chatId && !_pins.isEmpty()) {
			refresh();
		}
	});
	refresh();
}

void PinnedBar::refresh() {
	const auto &history = _session->data().messages().history(_chatId);
	const auto was = !_pins.isEmpty();
	const auto shownSeq = current() ? current()->message.seq : qint64(0);
	auto pins = QVector<Data::PinnedMessage>();
	for (const auto &pin : history.pins) {
		if (pin.message.seq <= 0 || pin.message.deleted) {
			continue;
		}
		auto copy = pin;
		// The window may hold a newer version (an edit).
		if (const auto fresh = history.find(pin.message.seq)) {
			copy.message = *fresh;
		}
		pins.push_back(std::move(copy));
	}
	// A message pinned a moment ago is known by its seq only: show it from
	// the window and ask the server for the list again shortly.
	if (history.info) {
		for (const auto seq : history.info->pins) {
			const auto known = std::any_of(pins.begin(), pins.end(), [&](
					const Data::PinnedMessage &pin) {
				return pin.message.seq == seq;
			});
			if (known) {
				continue;
			} else if (const auto message = history.find(seq)) {
				if (!message->deleted) {
					auto pin = Data::PinnedMessage();
					pin.message = *message;
					pin.scope = u"both"_s;
					pins.push_back(std::move(pin));
				}
			}
		}
	}
	if (!history.pinsLoaded
		&& history.loaded
		&& !_reloadTimer.isActive()
		&& Ui::Now() - _requestedAt > kPinsRequestGap) {
		_reloadTimer.start(kPinsReloadDelay);
	}
	std::stable_sort(pins.begin(), pins.end(), [](
			const Data::PinnedMessage &a,
			const Data::PinnedMessage &b) {
		return a.message.seq > b.message.seq;
	});
	_pins = std::move(pins);
	_index = 0;
	for (auto i = 0; i != int(_pins.size()); ++i) {
		if (_pins[i].message.seq == shownSeq) {
			_index = i;
			break;
		}
	}
	refreshThumb();
	update();
	if (was != !_pins.isEmpty()) {
		Q_EMIT pinsChanged();
	}
}

const Data::PinnedMessage *PinnedBar::current() const {
	return (_index >= 0 && _index < _pins.size()) ? &_pins[_index] : nullptr;
}

void PinnedBar::refreshThumb() {
	const auto pin = current();
	const auto seq = pin ? pin->message.seq : qint64(0);
	if (_thumbSeq == seq) {
		return;
	}
	_thumbSeq = seq;
	QObject::disconnect(_thumbConnection);
	_thumb = nullptr;
	_thumbPrepared = QImage();
	if (!pin || pin->message.mediaSpoiler) {
		return;
	}
	_thumb = Support::MakeMessageImageSource(
		_session,
		pin->message,
		Data::MediaVariant::Thumb);
	if (_thumb) {
		_thumbConnection = connect(
			_thumb.get(),
			&Ui::ImageSource::updated,
			this,
			[this] { update(); });
	}
}

int PinnedBar::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::historyBarHeight;
}

void PinnedBar::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_unpin->moveToRight(
		st::historyBarPadding.right(),
		(height() - _unpin->height()) / 2,
		width());
}

void PinnedBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::historyBarBg);
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::historyBarLineFg);
	const auto pin = current();
	if (!pin) {
		return;
	}
	const auto &padding = st::historyBarPadding;
	// The line: one segment per pin, the shown one is bright.
	const auto count = int(_pins.size());
	const auto lineTop = (height() - st::historyPinnedLineHeight) / 2;
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		const auto segments = std::min(count, 4);
		const auto gap = (segments > 1) ? 2 : 0;
		const auto segment = (st::historyPinnedLineHeight - gap * (segments - 1))
			/ double(segments);
		// The newest pin is the lowest segment.
		const auto active = (count <= segments)
			? (segments - 1 - _index)
			: (_index == 0)
			? (segments - 1)
			: (_index == count - 1)
			? 0
			: (segments - 2);
		for (auto i = 0; i != segments; ++i) {
			p.setBrush((i == active)
				? st::historyPinnedLineFg
				: st::historyPinnedLineFgDim);
			p.drawRoundedRect(
				QRectF(myrtlrect(
					padding.left(),
					0,
					st::historyPinnedLine,
					0)).adjusted(
						0,
						lineTop + i * (segment + gap),
						0,
						lineTop + i * (segment + gap) + segment),
				st::historyPinnedLine / 2.,
				st::historyPinnedLine / 2.);
		}
	}
	auto left = padding.left() + st::historyPinnedLine + st::historyPinnedLineSkip;
	if (_thumb) {
		const auto size = st::historyPinnedThumb;
		const auto box = myrtlrect(left, (height() - size) / 2, size, size);
		const auto ratio = devicePixelRatioF();
		auto image = _thumb->request(box.size() * ratio);
		if (image.isNull()) {
			image = _thumb->preview();
		}
		if (!image.isNull()) {
			if (_thumbPrepared.isNull() || _thumbPreparedKey != image.cacheKey()) {
				_thumbPrepared = Ui::Images::Round(
					Ui::Images::PrepareCover(image, box.size(), ratio),
					st::historyPinnedThumbRadius);
				_thumbPreparedKey = image.cacheKey();
			}
			p.drawImage(box.topLeft(), _thumbPrepared);
			left += size + st::historyPinnedThumbSkip;
		}
	}
	const auto available = width()
		- left
		- padding.right()
		- _unpin->width()
		- 4;
	if (available <= 0) {
		return;
	}
	auto title = tr::lng_history_pinned_message(tr::now);
	if (count > 1) {
		title += u" #"_s + Lang::Num(count - _index);
	}
	p.setPen(st::historyPinnedTitleFg);
	p.setFont(st::historyPinnedTitleFont);
	p.drawTextLeft(
		left,
		st::historyPinnedTitleTop,
		width(),
		st::historyPinnedTitleFont->elided(title, available));
	const auto preview = Support::MessagePreview(pin->message);
	const auto &font = st::historyPinnedTextFont;
	p.setPen(st::historyPinnedTextFg);
	p.setFont(font);
	p.drawTextLeft(
		left,
		st::historyPinnedTextTop,
		width(),
		font->elided(preview, available));
}

void PinnedBar::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton);
}

void PinnedBar::mouseReleaseEvent(QMouseEvent *e) {
	if (!std::exchange(_pressed, false)
		|| e->button() != Qt::LeftButton
		|| !rect().contains(e->pos())) {
		return;
	}
	const auto pin = current();
	if (!pin) {
		return;
	}
	const auto seq = pin->message.seq;
	// Each click goes one pin further back.
	if (_pins.size() > 1) {
		_index = (_index + 1) % int(_pins.size());
		refreshThumb();
		update();
	}
	Q_EMIT jumpRequested(seq);
}

// ---- ConversationBanner

ConversationBanner::ConversationBanner(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &chatId)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _chatId(chatId)
, _continue(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_conv_continue_messages(tr::now),
	st::smallLightButton)) {
	_continue->hide();
	_continue->setClickedCallback([this] { requestContinue(); });
	auto &data = _session->data();
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](const QString &id) {
		if (id == _chatId) {
			refresh();
		}
	});
	connect(
		&data.conversations(),
		&Data::Conversations::stateChanged,
		this,
		[this](const QString &chat, const QString &, const QString &) {
			if (chat == _chatId) {
				refresh();
			}
		});
	refresh();
}

void ConversationBanner::refresh() {
	const auto chat = _session->data().chats().chat(_chatId);
	const auto was = _state;
	auto state = State::None;
	auto text = QString();
	auto extra = QString();
	auto canContinue = false;
	if (chat && chat->conversation) {
		const auto &conversation = *chat->conversation;
		_conversationId = conversation.id;
		const auto &name = conversation.state;
		if (name == u"open") {
			state = State::Waiting;
			text = tr::lng_chat_waiting_listener(tr::now);
			if (conversation.expiresAt > 0) {
				extra = ClockRemaining(conversation.expiresAt);
			}
		} else if (name == u"active") {
			state = State::Active;
			text = (conversation.expiresAt > 0)
				? ClockRemaining(conversation.expiresAt)
				: tr::lng_history_conversation_open(tr::now);
		} else if (name == u"readonly") {
			state = State::Readonly;
			text = (conversation.readonlyUntil > 0)
				? tr::lng_conv_readable_days(
					tr::now,
					lt_count,
					DaysLeft(conversation.readonlyUntil))
				: tr::lng_chat_conversation_has_ended(tr::now);
			if (conversation.canContinue) {
				if (conversation.continueRequested || _requested) {
					extra = tr::lng_conv_continue_request_sent(tr::now);
				} else {
					canContinue = true;
				}
			}
		} else if (name == u"continued") {
			state = State::Continued;
			text = tr::lng_chat_conversation_continued(tr::now);
		} else if (ConversationEnded(name)) {
			state = State::Ended;
			text = tr::lng_chat_conversation_has_ended(tr::now);
		}
	}
	_state = state;
	_text = text;
	_extra = extra;
	_canContinue = canContinue;
	_continue->setVisible(canContinue);
	if (state == State::Waiting || state == State::Active || state == State::Readonly) {
		if (!_timer.isActive()) {
			_timer.start(kBannerTick, this);
		}
	} else {
		_timer.stop();
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
	if (was != _state) {
		Q_EMIT stateChanged();
	}
}

void ConversationBanner::requestContinue() {
	if (_conversationId.isEmpty() || _continue->loading()) {
		return;
	}
	_continue->setLoading(true);
	const auto controller = _controller;
	_session->data().conversations().requestContinue(
		_conversationId,
		this,
		[=, this](const Data::Conversation &) {
			_continue->setLoading(false);
			_requested = true;
			controller->showToast(tr::lng_conv_continue_request_sent(tr::now));
			refresh();
		},
		[=, this](const Api::Error &error) {
			_continue->setLoading(false);
			Support::ShowError(controller, error);
		});
}

int ConversationBanner::resizeGetHeight(int newWidth) {
	if (_canContinue) {
		_continue->resizeToNaturalWidth(newWidth / 2);
		_continue->moveToRight(
			st::historyBannerPadding.right(),
			(st::historyBannerHeight - _continue->height()) / 2,
			newWidth);
	}
	return st::historyBannerHeight;
}

void ConversationBanner::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (_canContinue) {
		_continue->moveToRight(
			st::historyBannerPadding.right(),
			(height() - _continue->height()) / 2,
			width());
	}
}

void ConversationBanner::timerEvent(QTimerEvent *e) {
	if (e->timerId() == _timer.timerId()) {
		refresh();
	} else {
		RpWidget::timerEvent(e);
	}
}

void ConversationBanner::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto bg = st::historyBannerEndedBg;
	auto fg = st::historyBannerEndedFg;
	const style::icon *icon = nullptr;
	switch (_state) {
	case State::None:
		return;
	case State::Waiting:
		bg = st::historyBannerWaitingBg;
		fg = st::historyBannerWaitingFg;
		break;
	case State::Active:
		bg = st::historyBannerActiveBg;
		fg = st::historyBannerActiveFg;
		icon = &st::historyBannerTimerIcon;
		break;
	case State::Readonly:
		bg = st::historyBannerReadonlyBg;
		fg = st::historyBannerReadonlyFg;
		icon = &st::historyBannerLockIcon;
		break;
	case State::Continued:
		bg = st::historyBannerReadonlyBg;
		fg = st::historyBannerReadonlyFg;
		icon = &st::historyBannerMessageIcon;
		break;
	case State::Ended:
		icon = &st::historyBannerEndedIcon;
		break;
	}
	p.fillRect(rect(), st::historyBarBg);
	p.fillRect(rect(), bg);
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::historyBarLineFg);
	const auto &padding = st::historyBannerPadding;
	auto left = padding.left();
	if (_state == State::Waiting) {
		auto hq = PainterHighQualityEnabler(p);
		const auto dot = st::historyBannerDot;
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyBannerDotFg);
		p.drawEllipse(myrtlrect(left, (height() - dot) / 2, dot, dot));
		left += dot + st::historyBannerSkip;
	} else if (icon) {
		icon->paint(p, left, (height() - icon->height()) / 2, width());
		left += icon->width() + st::historyBannerSkip;
	}
	const auto right = padding.right()
		+ (_canContinue ? (_continue->width() + st::historyBannerSkip) : 0);
	const auto extraWidth = _extra.isEmpty()
		? 0
		: (st::historyBannerSmallFont->width(_extra)
			+ ((_state == State::Waiting)
				? (st::historyBannerTimerSmallIcon.width() + 4)
				: 0));
	const auto available = width() - left - right - extraWidth - st::historyBannerSkip;
	p.setPen(fg);
	p.setFont(st::historyBannerFont);
	p.drawTextLeft(
		left,
		(height() - st::historyBannerFont->height) / 2,
		width(),
		st::historyBannerFont->elided(_text, std::max(available, 0)));
	if (!_extra.isEmpty()) {
		// On the trailing side: the countdown / «request sent».
		const auto textWidth = st::historyBannerSmallFont->width(_extra);
		p.setPen(st::textSecondary);
		p.setFont(st::historyBannerSmallFont);
		p.drawTextRight(
			right,
			(height() - st::historyBannerSmallFont->height) / 2,
			width(),
			_extra,
			textWidth);
		if (_state == State::Waiting) {
			const auto &timer = st::historyBannerTimerSmallIcon;
			timer.paint(
				p,
				width() - right - textWidth - 4 - timer.width(),
				(height() - timer.height()) / 2,
				width());
		}
	}
}

// ---- SearchBar

SearchBar::SearchBar(QWidget *parent, Main::Session *session, const QString &chatId)
: RpWidget(parent)
, _session(session)
, _chatId(chatId)
, _field(Ui::CreateChild<Ui::SearchField>(
	this,
	tr::lng_history_search_placeholder(tr::now)))
, _up(Ui::CreateChild<Ui::IconButton>(this, st::historySearchUp))
, _down(Ui::CreateChild<Ui::IconButton>(this, st::historySearchDown))
, _close(Ui::CreateChild<Ui::IconButton>(this, st::historySearchClose)) {
	_timer.setSingleShot(true);
	_timer.setInterval(kSearchDelay);
	connect(&_timer, &QTimer::timeout, this, [this] { search(); });
	connect(_field, &Ui::SearchField::changed, this, [this](const QString &) {
		_timer.start();
	});
	connect(_field, &Ui::SearchField::submitted, this, [this] {
		if (_timer.isActive()) {
			_timer.stop();
			search();
		} else {
			step(1);
		}
	});
	connect(_field, &Ui::SearchField::cancelled, this, [this] {
		Q_EMIT closeRequested();
	});
	_up->setClickedCallback([this] { step(1); });
	_down->setClickedCallback([this] { step(-1); });
	_close->setClickedCallback([this] { Q_EMIT closeRequested(); });
	Ui::InstallTooltip(_up, [] { return tr::lng_history_search_older(tr::now); });
	Ui::InstallTooltip(_down, [] { return tr::lng_history_search_newer(tr::now); });
	Ui::InstallTooltip(_close, [] { return tr::lng_close(tr::now); });
	_up->setDisabled(true);
	_down->setDisabled(true);
}

void SearchBar::activate() {
	_field->setFocusFast();
}

QString SearchBar::query() const {
	return _field->text().trimmed();
}

void SearchBar::search() {
	const auto &history = _session->data().messages().history(_chatId);
	const auto text = query();
	_results.clear();
	_current = -1;
	_searched = !text.isEmpty();
	if (_searched) {
		for (const auto index : SearchMessages(history.items, text)) {
			_results.push_back(history.items[index].seq);
		}
		if (!_results.isEmpty()) {
			_current = 0;
		}
	}
	showCurrent();
}

void SearchBar::step(int delta) {
	if (_results.isEmpty()) {
		return;
	}
	const auto next = std::clamp(_current + delta, 0, int(_results.size()) - 1);
	if (next == _current) {
		return;
	}
	_current = next;
	showCurrent();
}

void SearchBar::showCurrent() {
	_up->setDisabled(_current < 0 || _current >= _results.size() - 1);
	_down->setDisabled(_current <= 0);
	update();
	if (_current >= 0) {
		Q_EMIT jumpRequested(_results[_current]);
	}
}

QString SearchBar::countText() const {
	if (!_searched) {
		return QString();
	} else if (_results.isEmpty()) {
		return tr::lng_nothing_found(tr::now);
	}
	return tr::lng_viewer_counter(
		tr::now,
		lt_index,
		Lang::Num(_current + 1),
		lt_total,
		Lang::Num(_results.size()));
}

int SearchBar::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::historyTopBarHeight;
}

void SearchBar::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto &padding = st::historySearchPadding;
	auto right = padding.right();
	for (const auto button : { _close, _down, _up }) {
		button->moveToRight(right, (height() - button->height()) / 2, width());
		right += button->width();
	}
	const auto count = st::historySearchCountFont->width(countText());
	const auto countSpace = std::max(count, 64) + 16;
	const auto fieldWidth = std::max(
		width() - padding.left() - right - countSpace,
		80);
	_field->resizeToWidth(fieldWidth);
	_field->moveToLeft(
		padding.left(),
		(height() - _field->height()) / 2,
		width());
}

void SearchBar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::historyBarBg);
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::historyBarLineFg);
	const auto text = countText();
	if (text.isEmpty()) {
		return;
	}
	const auto right = st::historySearchPadding.right()
		+ _close->width()
		+ _down->width()
		+ _up->width()
		+ 8;
	p.setPen(st::historySearchCountFg);
	p.setFont(st::historySearchCountFont);
	p.drawTextRight(
		right,
		(height() - st::historySearchCountFont->height) / 2,
		width(),
		text);
}

void SearchBar::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		Q_EMIT closeRequested();
	} else if (e->key() == Qt::Key_Up) {
		step(1);
	} else if (e->key() == Qt::Key_Down) {
		step(-1);
	} else {
		RpWidget::keyPressEvent(e);
	}
}

// ---- BottomNotice

BottomNotice::BottomNotice(QWidget *parent)
: RpWidget(parent) {
}

void BottomNotice::setContent(
		const QString &title,
		const QString &text,
		std::vector<NoticeButton> buttons) {
	_title = title;
	_text = text;
	for (const auto button : _buttons) {
		delete button;
	}
	_buttons.clear();
	for (auto &descriptor : buttons) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			this,
			descriptor.text,
			descriptor.st ? *descriptor.st : st::defaultRoundButton);
		button->setClickedCallback(std::move(descriptor.callback));
		button->show();
		_buttons.push_back(button);
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void BottomNotice::setLoading(bool loading) {
	for (const auto button : _buttons) {
		button->setDisabled(loading);
	}
}

int BottomNotice::resizeGetHeight(int newWidth) {
	const auto &padding = st::historyRequestPadding;
	auto buttonsWidth = 0;
	for (const auto button : _buttons) {
		button->resizeToNaturalWidth(newWidth);
		buttonsWidth += button->width() + st::historyRequestSkip;
	}
	const auto textWidth = std::max(
		st::historyRequestTitleFont->width(_title),
		st::historyRequestTextFont->width(_text));
	const auto hasText = !_title.isEmpty() || !_text.isEmpty();
	_stacked = hasText
		&& !_buttons.empty()
		&& (padding.left() + textWidth + 16 + buttonsWidth + padding.right() > newWidth);
	if (_buttons.empty()) {
		return st::historyComposeNoticeHeight;
	}
	const auto result = _stacked
		? (st::historyRequestHeight + 40)
		: (hasText ? int(st::historyRequestHeight) : int(st::historyComposeNoticeHeight));
	const auto buttonHeight = _buttons.front()->height();
	if (!hasText || _stacked) {
		// The buttons alone: centred in a row.
		auto left = (newWidth - (buttonsWidth - st::historyRequestSkip)) / 2;
		const auto top = _stacked
			? (result - buttonHeight - 10)
			: (result - buttonHeight) / 2;
		for (const auto button : _buttons) {
			button->moveToLeft(left, top, newWidth);
			left += button->width() + st::historyRequestSkip;
		}
	} else {
		auto right = padding.right();
		for (auto i = _buttons.rbegin(); i != _buttons.rend(); ++i) {
			(*i)->moveToRight(right, (result - buttonHeight) / 2, newWidth);
			right += (*i)->width() + st::historyRequestSkip;
		}
	}
	return result;
}

void BottomNotice::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
}

void BottomNotice::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::historyComposeBg);
	p.fillRect(0, 0, width(), st::lineWidth, st::historyComposeLineFg);
	const auto &padding = st::historyRequestPadding;
	if (_buttons.empty()) {
		p.setPen(st::historyComposeNoticeFg);
		p.setFont(st::historyComposeNoticeFont);
		const auto text = st::historyComposeNoticeFont->elided(
			_text.isEmpty() ? _title : _text,
			width() - padding.left() - padding.right());
		p.drawText(rect(), Qt::AlignCenter, text);
		return;
	} else if (_title.isEmpty() && _text.isEmpty()) {
		return;
	}
	auto buttonsWidth = 0;
	for (const auto button : _buttons) {
		buttonsWidth += button->width() + st::historyRequestSkip;
	}
	const auto available = _stacked
		? (width() - padding.left() - padding.right())
		: (width() - padding.left() - padding.right() - buttonsWidth - 8);
	const auto blockHeight = st::historyRequestTitleFont->height
		+ (_text.isEmpty() ? 0 : (st::historyRequestTextFont->height + 2));
	const auto areaHeight = _stacked ? (height() - 50) : height();
	auto top = (areaHeight - blockHeight) / 2 + (_stacked ? 4 : 0);
	p.setPen(st::textPrimary);
	p.setFont(st::historyRequestTitleFont);
	const auto title = st::historyRequestTitleFont->elided(_title, available);
	if (_stacked) {
		p.drawText(
			QRect(padding.left(), top, available, st::historyRequestTitleFont->height),
			Qt::AlignCenter,
			title);
	} else {
		p.drawTextLeft(padding.left(), top, width(), title);
	}
	if (!_text.isEmpty()) {
		top += st::historyRequestTitleFont->height + 2;
		p.setPen(st::historyRequestTextFg);
		p.setFont(st::historyRequestTextFont);
		const auto text = st::historyRequestTextFont->elided(_text, available);
		if (_stacked) {
			p.drawText(
				QRect(padding.left(), top, available, st::historyRequestTextFont->height),
				Qt::AlignCenter,
				text);
		} else {
			p.drawTextLeft(padding.left(), top, width(), text);
		}
	}
}

} // namespace History
