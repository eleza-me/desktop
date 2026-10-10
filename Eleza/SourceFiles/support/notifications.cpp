#include "support/notifications.h"

#include "core/core_application.h"
#include "data/data_chats.h"
#include "data/data_session.h"
#include "data/data_sync.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/media_image.h"
#include "ui/effects/shadow.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_support.h"

#include <QtCore/QDateTime>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtMultimedia/QSoundEffect>
#include <QtWidgets/QApplication>

namespace Support {
namespace {

constexpr auto kShowFor = 5000;
constexpr auto kSoundInterval = 1500;
constexpr auto kMaxQueued = 20;

[[nodiscard]] bool MutedNow(const Data::Chat &chat) {
	if (!chat.muted) {
		return false;
	}
	// mutedUntil is unix seconds; 0 = until switched off.
	return (chat.mutedUntil <= 0)
		|| (chat.mutedUntil > QDateTime::currentSecsSinceEpoch());
}

} // namespace

std::optional<NotificationData> NotificationFor(
		Main::Session *session,
		const Data::Message &message) {
	if (!session || message.mine || !message.visible()) {
		return std::nullopt;
	}
	auto &data = session->data();
	const auto chat = data.chats().chat(message.chat);
	if (chat && (chat->saved || chat->archived || MutedNow(*chat))) {
		return std::nullopt;
	}
	const auto &settings = Core::App().settings().notifications();
	auto result = NotificationData();
	result.key = message.chat;
	result.route = Window::Routes::Chat(message.chat);
	if (chat) {
		result.anonymous = chat->anonymous || chat->isConversation;
		result.userId = result.anonymous ? QString() : chat->peerUserId;
	}
	result.title = (settings.showSender && chat)
		? ChatTitle(session, *chat)
		: QString();
	if (result.title.isEmpty()) {
		result.title = tr::lng_app_name(tr::now);
		if (!settings.showSender) {
			result.userId = QString();
			result.anonymous = true;
		}
	}
	result.text = settings.showPreview
		? MessagePreview(message)
		: tr::lng_support_notify_new_message(tr::now);
	return result;
}

// One toast: a frameless always-on-top window that never takes the focus.
class NotificationWidget final : public Ui::RpWidget {
public:
	NotificationWidget(
		Main::Session *session,
		const NotificationData &data,
		std::function<void(const QString &route)> activated,
		std::function<void(NotificationWidget*)> removed);

	void setData(const NotificationData &data);
	[[nodiscard]] QString key() const {
		return _data.key;
	}
	void showAnimated();
	void hideAnimated();
	// Where the body (without the shadow) should be, in global coordinates.
	void moveBodyTo(QPoint topLeft);
	[[nodiscard]] QSize bodySize() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void enterEvent(QEnterEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] QRect bodyRect() const;
	[[nodiscard]] int textWidth() const;
	void relayout();
	void opacityCallback();

	QPointer<Main::Session> _session;
	NotificationData _data;
	std::function<void(const QString &route)> _activated;
	std::function<void(NotificationWidget*)> _removed;
	UserpicButton *_userpic = nullptr;
	Ui::IconButton *_close = nullptr;
	Ui::Text::String _text;
	QTimer _hideTimer;
	Ui::Animations::Simple _opacity;
	bool _hiding = false;
	bool _pressed = false;

};

NotificationWidget::NotificationWidget(
	Main::Session *session,
	const NotificationData &data,
	std::function<void(const QString &route)> activated,
	std::function<void(NotificationWidget*)> removed)
: RpWidget(nullptr)
, _session(session)
, _activated(std::move(activated))
, _removed(std::move(removed))
, _userpic(new UserpicButton(this, session, QString(), st::notifyUserpicSize))
, _close(new Ui::IconButton(this, st::notifyClose)) {
	setWindowFlags(Qt::Tool
		| Qt::FramelessWindowHint
		| Qt::WindowStaysOnTopHint
		| Qt::NoDropShadowWindowHint
		| Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_MacAlwaysShowToolWindow);
	setFocusPolicy(Qt::NoFocus);
	setCursor(style::cur_pointer);
	setLayoutDirection(Ui::LayoutDirection());

	Ui::MakeTransparentForMouse(_userpic);
	_userpic->setBorderColor(st::notifyBg);
	_close->setClickedCallback([this] { hideAnimated(); });
	_close->hide();

	_hideTimer.setSingleShot(true);
	QObject::connect(&_hideTimer, &QTimer::timeout, this, [this] {
		hideAnimated();
	});
	setData(data);
}

void NotificationWidget::setData(const NotificationData &data) {
	_data = data;
	if (data.anonymous || data.userId.isEmpty()) {
		_userpic->setSpecial(
			data.anonymous
				? Ui::UserpicKind::Anonymous
				: Ui::UserpicKind::User,
			data.title,
			data.key);
	} else {
		_userpic->setUserId(data.userId);
	}
	_text.setText(st::notifyTextStyle, data.text);
	relayout();
	update();
	if (!_hiding && isVisible() && !underMouse()) {
		_hideTimer.start(kShowFor);
	}
}

int NotificationWidget::textWidth() const {
	const auto &padding = st::notifyPadding;
	return st::notifyWidth
		- padding.left()
		- padding.right()
		- st::notifyUserpicSize
		- st::notifyUserpicSkip;
}

QSize NotificationWidget::bodySize() const {
	const auto &padding = st::notifyPadding;
	const auto textHeight = _text.countHeight(textWidth(), st::notifyTextLines);
	const auto content = st::notifyTitleFont->height
		+ st::notifyTextTop
		+ textHeight;
	const auto inner = std::max(content, int(st::notifyUserpicSize));
	return QSize(
		st::notifyWidth,
		std::max(
			inner + padding.top() + padding.bottom(),
			int(st::notifyMinHeight)));
}

QRect NotificationWidget::bodyRect() const {
	const auto &extend = st::notifyShadow.extend;
	return QRect(QPoint(extend.left(), extend.top()), bodySize());
}

void NotificationWidget::relayout() {
	const auto &extend = st::notifyShadow.extend;
	const auto body = bodySize();
	const auto was = isVisible() ? (geometry().topLeft() + bodyRect().topLeft()) : QPoint();
	resize(
		body.width() + extend.left() + extend.right(),
		body.height() + extend.top() + extend.bottom());
	if (isVisible()) {
		moveBodyTo(was);
	}
	const auto &padding = st::notifyPadding;
	const auto rect = bodyRect();
	// Children are placed physically: the window has no RTL-aware parent.
	const auto rtl = style::RightToLeft();
	_userpic->move(
		rtl
			? (rect.x() + rect.width() - padding.left() - st::notifyUserpicSize)
			: (rect.x() + padding.left()),
		rect.y() + (rect.height() - st::notifyUserpicSize) / 2);
	_close->move(
		rtl
			? (rect.x() + st::notifyCloseSkip)
			: (rect.x() + rect.width() - st::notifyCloseSkip - _close->width()),
		rect.y() + st::notifyCloseSkip);
}

void NotificationWidget::moveBodyTo(QPoint topLeft) {
	const auto &extend = st::notifyShadow.extend;
	move(topLeft - QPoint(extend.left(), extend.top()));
}

void NotificationWidget::showAnimated() {
	_hiding = false;
	setWindowOpacity(0.);
	show();
	_opacity.start(
		[this] { opacityCallback(); },
		0.,
		1.,
		st::notifyShowDuration);
	opacityCallback();
	_hideTimer.start(kShowFor);
}

void NotificationWidget::hideAnimated() {
	if (_hiding) {
		return;
	}
	_hiding = true;
	_hideTimer.stop();
	_opacity.start(
		[this] { opacityCallback(); },
		1.,
		0.,
		st::notifyHideDuration);
	opacityCallback();
}

void NotificationWidget::opacityCallback() {
	const auto value = _opacity.value(_hiding ? 0. : 1.);
	setWindowOpacity(value);
	if (_hiding && !_opacity.animating()) {
		hide();
		if (const auto removed = _removed) {
			removed(this);
		}
	}
}

void NotificationWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto rect = bodyRect();
	Ui::Shadow::Paint(p, rect, st::notifyRadius, st::notifyShadow);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::notifyBorder->c, 1.));
		p.setBrush(st::notifyBg);
		p.drawRoundedRect(
			QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
			st::notifyRadius,
			st::notifyRadius);
	}
	const auto &padding = st::notifyPadding;
	const auto available = textWidth();
	const auto rtl = style::RightToLeft();
	const auto left = rtl
		? (rect.x() + padding.right())
		: (rect.x() + padding.left() + st::notifyUserpicSize + st::notifyUserpicSkip);
	const auto textHeight = _text.countHeight(available, st::notifyTextLines);
	const auto content = st::notifyTitleFont->height
		+ st::notifyTextTop
		+ textHeight;
	auto top = rect.y() + (rect.height() - content) / 2;

	// The close button covers the end of the title while hovered.
	const auto titleWidth = available
		- (_close->isVisible() ? (_close->width() - st::notifyCloseSkip) : 0);
	p.setFont(st::notifyTitleFont);
	p.setPen(st::notifyTitleFg);
	const auto title = st::notifyTitleFont->elided(_data.title, titleWidth);
	p.drawText(
		QRect(
			(rtl && _close->isVisible()) ? (left + available - titleWidth) : left,
			top,
			titleWidth,
			st::notifyTitleFont->height),
		(rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
		title);
	top += st::notifyTitleFont->height + st::notifyTextTop;
	p.setPen(st::notifyTextFg);
	_text.drawElided(p, left, top, available, st::notifyTextLines);
}

void NotificationWidget::enterEvent(QEnterEvent *e) {
	Q_UNUSED(e);
	_hideTimer.stop();
	_close->show();
	update();
}

void NotificationWidget::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	_close->hide();
	update();
	if (!_hiding) {
		_hideTimer.start(kShowFor);
	}
}

void NotificationWidget::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton);
}

void NotificationWidget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::RightButton) {
		hideAnimated();
		return;
	}
	if (!std::exchange(_pressed, false)
		|| e->button() != Qt::LeftButton
		|| !rect().contains(e->position().toPoint())) {
		return;
	}
	const auto route = _data.route;
	const auto activated = _activated;
	hideAnimated();
	if (activated) {
		activated(route);
	}
}

NotificationsManager::NotificationsManager(
	Window::SessionController *controller,
	QObject *parent)
: QObject(parent)
, _controller(controller) {
	auto &data = controller->session().data();
	QObject::connect(
		&data.sync(),
		&Data::Sync::messageReceived,
		this,
		[this](const Data::Message &message) { messageReceived(message); });
	// A chat that was read (here or on another device) needs no toast.
	QObject::connect(
		&data.chats(),
		&Data::Chats::chatChanged,
		this,
		[this](const QString &id) {
			const auto chat = _controller->session().data().chats().chat(id);
			if (!chat || chat->unread <= 0) {
				clearFor(id);
			}
		});
	QObject::connect(
		&data.chats(),
		&Data::Chats::chatRemoved,
		this,
		[this](const QString &id) { clearFor(id); });
	style::OnPaletteChanged(this, [this] {
		for (const auto widget : _visible) {
			widget->update();
		}
	});
}

NotificationsManager::~NotificationsManager() {
	_queue.clear();
	for (const auto widget : std::exchange(_visible, {})) {
		delete widget;
	}
}

bool NotificationsManager::shouldShow(const Data::Message &message) const {
	const auto &settings = Core::App().settings().notifications();
	if (!settings.desktopEnabled) {
		return false;
	}
	const auto active = Core::App().windowActive();
	if (!active) {
		return true;
	} else if (settings.onlyWhenInactive) {
		return false;
	}
	// The window is active: only a chat that is not on screen notifies.
	const auto visible = _controller->session().data().chats().visibleChat();
	return (visible != message.chat);
}

void NotificationsManager::messageReceived(const Data::Message &message) {
	if (!shouldShow(message)) {
		return;
	}
	const auto data = NotificationFor(&_controller->session(), message);
	if (!data) {
		return;
	}
	show(*data);
	const auto &settings = Core::App().settings().notifications();
	if (settings.soundEnabled) {
		playSound();
	}
	if (settings.flashTaskbar && !Core::App().windowActive()) {
		QApplication::alert(_controller->window());
	}
}

void NotificationsManager::showCustom(const NotificationData &data) {
	show(data);
}

void NotificationsManager::playSound() {
	if (Core::App().commandLine().ephemeral()) {
		return; // screenshot runs have no audio device to open
	}
	const auto now = Ui::Now();
	if (_lastSoundAt && (now - _lastSoundAt) < kSoundInterval) {
		return;
	}
	_lastSoundAt = now;
	if (!_sound) {
		_sound = std::make_unique<QSoundEffect>();
		_sound->setSource(QUrl(QStringLiteral("qrc:/gui/sounds/notify.wav")));
		_sound->setVolume(0.6f);
	}
	_sound->play();
}

void NotificationsManager::show(const NotificationData &data) {
	// One toast per chat: the newest message replaces what it says.
	for (const auto widget : _visible) {
		if (widget->key() == data.key && !data.key.isEmpty()) {
			widget->setData(data);
			relayout();
			return;
		}
	}
	for (auto &queued : _queue) {
		if (queued.key == data.key && !data.key.isEmpty()) {
			queued = data;
			return;
		}
	}
	if (int(_queue.size()) >= kMaxQueued) {
		_queue.pop_front();
	}
	_queue.push_back(data);
	showNext();
}

void NotificationsManager::showNext() {
	while (int(_visible.size()) < kMaxVisible && !_queue.empty()) {
		const auto data = _queue.front();
		_queue.pop_front();
		const auto widget = new NotificationWidget(
			&_controller->session(),
			data,
			[this](const QString &route) { activated(route); },
			[this](NotificationWidget *which) { removed(which); });
		_visible.push_back(widget);
		relayout();
		widget->showAnimated();
	}
}

void NotificationsManager::removed(NotificationWidget *widget) {
	const auto i = std::find(_visible.begin(), _visible.end(), widget);
	if (i == _visible.end()) {
		return;
	}
	_visible.erase(i);
	widget->deleteLater();
	relayout();
	showNext();
}

void NotificationsManager::clearFor(const QString &key) {
	for (auto i = _queue.begin(); i != _queue.end();) {
		i = (i->key == key) ? _queue.erase(i) : (i + 1);
	}
	for (const auto widget : _visible) {
		if (widget->key() == key) {
			widget->hideAnimated();
		}
	}
}

void NotificationsManager::clearAll() {
	_queue.clear();
	// hideAnimated() may remove the widget from the list right away.
	const auto copy = _visible;
	for (const auto widget : copy) {
		widget->hideAnimated();
	}
}

QRect NotificationsManager::anchorRect() const {
	const auto window = _controller->window();
	if (Core::App().commandLine().ephemeral()) {
		// Screenshot runs grab the main window: keep the toasts inside it.
		return QRect(window->mapToGlobal(QPoint()), window->size());
	}
	auto screen = window->screen();
	if (!screen) {
		screen = QGuiApplication::primaryScreen();
	}
	return screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
}

void NotificationsManager::relayout() {
	const auto anchor = anchorRect();
	const auto margin = int(st::notifyScreenMargin);
	auto bottom = anchor.y() + anchor.height() - margin;
	for (const auto widget : _visible) {
		const auto size = widget->bodySize();
		// The corner where the system shows its own: bottom right.
		widget->moveBodyTo(QPoint(
			anchor.x() + anchor.width() - margin - size.width(),
			bottom - size.height()));
		bottom -= size.height() + st::notifySkip;
	}
}

void NotificationsManager::activated(const QString &route) {
	const auto window = _controller->window();
	window->activateAndShow();
	if (!route.isEmpty()) {
		_controller->openRoute(route);
	}
}

} // namespace Support
