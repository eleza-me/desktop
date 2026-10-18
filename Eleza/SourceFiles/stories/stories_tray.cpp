#include "stories/stories_tray.h"

#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "stories/stories_boxes.h"
#include "stories/stories_ring.h"
#include "stories/stories_viewer.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/rtl.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_stories.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

namespace Stories {
namespace {

constexpr auto kRefreshAfterMs = qint64(20) * 1000;
constexpr auto kLoadMoreDistance = 3; // items before the end

[[nodiscard]] QString ShortName(const Data::User &user) {
	if (user.deleted) {
		return tr::lng_deleted_account(tr::now);
	} else if (!user.displayName.isEmpty()) {
		return user.displayName;
	}
	return user.handle;
}

} // namespace

Tray::Tray(QWidget *parent, Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _userpics(std::make_unique<Support::UserpicPainter>(
	&controller->session(),
	[this](const QString &) { update(); },
	this)) {
	setMouseTracking(true);
	auto &stories = controller->session().data().stories();
	connect(&stories, &Data::Stories::trayChanged, this, [this] {
		updateVisibility();
		setScroll(_scroll);
		update();
	});
	connect(&stories, &Data::Stories::postingChanged, this, [this] {
		update();
	});
	connect(&stories, &Data::Stories::enabledChanged, this, [this] {
		updateVisibility();
	});
	connect(&controller->session().data().users(), &Data::Users::changed, this, [this] {
		update();
	});
	connect(&stories, &Data::Stories::posted, this, [=] {
		controller->showToast(tr::lng_stories_posted(tr::now));
	});
	updateVisibility();
	refresh();
}

Tray::~Tray() = default;

void Tray::updateVisibility() {
	const auto enabled = _controller->session().data().stories().enabled();
	if (enabled == isHidden()) {
		setVisible(enabled);
		resizeToWidth(width());
	}
}

void Tray::refresh() {
	auto &stories = _controller->session().data().stories();
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (stories.trayLoaded() && now - _lastRefresh < kRefreshAfterMs) {
		return;
	}
	_lastRefresh = now;
	stories.refreshTray();
}

int Tray::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	if (isHidden()) {
		return 0;
	}
	const auto &padding = st::storiesTrayPadding;
	return padding.top()
		+ st::storiesTrayUserpic
		+ 2 * RingOutset()
		+ st::storiesTrayNameTop
		+ st::storiesTrayNameFont->height
		+ padding.bottom();
}

int Tray::count() const {
	return 1 + int(_controller->session().data().stories().tray().size());
}

QString Tray::userIdAt(int index) const {
	const auto &stories = _controller->session().data().stories();
	if (index == 0) {
		return _controller->session().userId();
	}
	const auto &tray = stories.tray();
	return (index > 0 && index <= tray.size()) ? tray[index - 1].user.id : QString();
}

int Tray::contentWidth() const {
	const auto &padding = st::storiesTrayPadding;
	return padding.left() + count() * st::storiesTrayItemWidth + padding.right();
}

int Tray::maxScroll() const {
	return std::max(contentWidth() - width(), 0);
}

void Tray::setScroll(int value) {
	const auto clamped = std::clamp(value, 0, maxScroll());
	if (_scroll != clamped) {
		_scroll = clamped;
		update();
	}
	auto &stories = _controller->session().data().stories();
	const auto lastVisible = (_scroll + width()) / st::storiesTrayItemWidth;
	if (stories.trayHasMore() && lastVisible + kLoadMoreDistance >= count()) {
		stories.loadMoreTray();
	}
}

QRect Tray::itemRect(int index) const {
	const auto x = st::storiesTrayPadding.left()
		+ index * st::storiesTrayItemWidth
		- _scroll;
	return style::rtlrect(x, 0, st::storiesTrayItemWidth, height(), width());
}

QRect Tray::userpicRect(int index) const {
	const auto item = itemRect(index);
	const auto size = st::storiesTrayUserpic;
	return QRect(
		item.x() + (item.width() - size) / 2,
		st::storiesTrayPadding.top() + RingOutset(),
		size,
		size);
}

QRect Tray::plusRect() const {
	const auto userpic = userpicRect(0);
	const auto size = st::storiesTrayPlusSize;
	// At the bottom trailing corner of my face.
	const auto x = style::RightToLeft()
		? userpic.x() - 2
		: (userpic.x() + userpic.width() - size + 2);
	return QRect(x, userpic.y() + userpic.height() - size + 2, size, size);
}

QRect Tray::arrowRect(bool forward) const {
	const auto size = st::storiesTrayArrowSize;
	const auto top = st::storiesTrayPadding.top()
		+ RingOutset()
		+ (st::storiesTrayUserpic - size) / 2;
	const auto leading = !forward;
	const auto atLeft = (leading != style::RightToLeft());
	return QRect(atLeft ? 4 : (width() - 4 - size), top, size, size);
}

Tray::Hit Tray::hitAt(QPoint point) const {
	if (_scroll > 0 && arrowRect(false).contains(point)) {
		return { Part::ArrowBack, -1 };
	} else if (_scroll < maxScroll() && arrowRect(true).contains(point)) {
		return { Part::ArrowForward, -1 };
	}
	for (auto i = 0, n = count(); i != n; ++i) {
		const auto rect = itemRect(i);
		if (rect.right() < 0 || rect.left() > width()) {
			continue;
		}
		if (rect.contains(point)) {
			if (i == 0 && plusRect().adjusted(-4, -4, 4, 4).contains(point)) {
				return { Part::Plus, 0 };
			}
			return { Part::Item, i };
		}
	}
	return {};
}

void Tray::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto &stories = _controller->session().data().stories();
	const auto &posting = stories.posting();
	const auto myId = _controller->session().userId();
	for (auto i = 0, n = count(); i != n; ++i) {
		const auto item = itemRect(i);
		if (item.right() < 0 || item.left() > width()) {
			continue;
		}
		if (_over.index == i && _over.part != Part::None) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::storiesTrayBgOver);
			p.drawRoundedRect(item.adjusted(2, 2, -2, -2), 12, 12);
		}
		const auto userpic = userpicRect(i);
		const auto userId = userIdAt(i);
		auto name = QString();
		auto nameFg = st::storiesTrayNameFg->c;
		auto alpha = 1.;
		if (i == 0) {
			const auto &me = stories.me();
			if (posting.active) {
				PaintRing(p, userpic, Data::StoryRing::None, posting.progress);
				name = tr::lng_stories_posting(tr::now);
				nameFg = st::storiesTrayNameFgSeen->c;
			} else {
				PaintRing(p, userpic, Data::RingStateOf(me).ring());
				name = posting.failed
					? tr::lng_stories_post_failed(tr::now)
					: tr::lng_stories_yours(tr::now);
				nameFg = posting.failed
					? st::storiesTrayNameFgFailed->c
					: st::storiesTrayNameFgSeen->c;
			}
			_userpics->paint(p, userpic, myId);
			const auto plus = plusRect();
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::bgCanvas);
			const auto border = st::storiesTrayPlusBorder;
			p.drawEllipse(plus.adjusted(-border, -border, border, border));
			p.setBrush(posting.failed ? st::storiesTrayFailedBg : st::storiesTrayPlusBg);
			p.drawEllipse(plus);
			(posting.failed ? st::storiesTrayFailedIcon : st::storiesTrayPlusIcon)
				.paintInCenter(p, plus);
		} else {
			const auto &entry = stories.tray()[i - 1];
			const auto state = Data::RingStateOf(entry);
			alpha = entry.muted ? 0.6 : 1.;
			p.setOpacity(alpha);
			PaintRing(p, userpic, state.ring());
			_userpics->paint(p, userpic, entry.user);
			const auto card = _controller->session().data().users().peek(userId);
			name = ShortName(card.valid() ? card : entry.user);
			const auto unseen = (state.ring() == Data::StoryRing::Unseen)
				|| (state.ring() == Data::StoryRing::CloseFriends);
			nameFg = unseen ? st::storiesTrayNameFg->c : st::storiesTrayNameFgSeen->c;
		}
		p.setPen(nameFg);
		p.setFont(st::storiesTrayNameFont);
		const auto nameWidth = item.width() - 8;
		const auto elided = st::storiesTrayNameFont->elided(name, nameWidth);
		const auto textWidth = st::storiesTrayNameFont->width(elided);
		p.drawText(
			item.x() + (item.width() - textWidth) / 2,
			userpic.y() + userpic.height() + RingOutset() + st::storiesTrayNameTop
				+ st::storiesTrayNameFont->ascent,
			elided);
		p.setOpacity(1.);
	}
	// The arrows over the ends while there is more to the side.
	const auto paintArrow = [&](bool forward) {
		const auto rect = arrowRect(forward);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::storiesTrayLineFg->c, 1.));
		p.setBrush(st::storiesTrayArrowBg);
		p.drawEllipse(rect);
		const auto pointsLeft = (forward == style::RightToLeft());
		(pointsLeft ? st::storiesTrayArrowLeft : st::storiesTrayArrowRight)
			.paintInCenter(p, rect);
	};
	if (_scroll > 0) {
		paintArrow(false);
	}
	if (_scroll < maxScroll()) {
		paintArrow(true);
	}
	p.fillRect(QRect(0, height() - 1, width(), 1), st::storiesTrayLineFg);
}

void Tray::mouseMoveEvent(QMouseEvent *e) {
	const auto hit = hitAt(e->pos());
	if (_over != hit) {
		_over = hit;
		setCursor((hit.part == Part::None) ? style::cur_default : style::cur_pointer);
		update();
	}
}

void Tray::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over.part != Part::None) {
		_over = Hit();
		update();
	}
}

void Tray::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = hitAt(e->pos());
	}
}

void Tray::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, Hit());
	const auto hit = hitAt(e->pos());
	if (pressed == hit && hit.part != Part::None) {
		activate(hit);
	}
}

void Tray::wheelEvent(QWheelEvent *e) {
	const auto delta = e->angleDelta();
	const auto value = (std::abs(delta.x()) > std::abs(delta.y()))
		? delta.x()
		: delta.y();
	if (!value || maxScroll() <= 0) {
		e->ignore();
		return;
	}
	setScroll(_scroll - value);
	e->accept();
}

void Tray::contextMenuEvent(QContextMenuEvent *e) {
	const auto hit = hitAt(e->pos());
	if (hit.index >= 0) {
		showMenu(hit.index, e->globalPos());
	}
}

void Tray::activate(const Hit &hit) {
	const auto page = std::max(width() - 2 * st::storiesTrayItemWidth, st::storiesTrayItemWidth);
	switch (hit.part) {
	case Part::ArrowBack:
		setScroll(_scroll - page);
		break;
	case Part::ArrowForward:
		setScroll(_scroll + page);
		break;
	case Part::Plus:
		if (_controller->session().data().stories().posting().failed) {
			showMenu(0, QCursor::pos());
		} else {
			ShowComposeBox(_controller);
		}
		break;
	case Part::Item:
		if (hit.index == 0) {
			openMine();
		} else {
			const auto &stories = _controller->session().data().stories();
			OpenUserStories(_controller, userIdAt(hit.index), stories.trayUserIds());
		}
		break;
	case Part::None:
		break;
	}
}

void Tray::openMine() {
	auto &stories = _controller->session().data().stories();
	if (stories.posting().failed) {
		showMenu(0, QCursor::pos());
	} else if (stories.me().count > 0) {
		OpenUserStories(_controller, _controller->session().userId());
	} else if (!stories.posting().active) {
		ShowComposeBox(_controller);
	}
}

void Tray::showMenu(int index, QPoint global) {
	const auto controller = _controller;
	auto &stories = controller->session().data().stories();
	const auto menu = new Ui::PopupMenu(this);
	if (index == 0) {
		const auto &posting = stories.posting();
		if (posting.failed) {
			menu->addAction(tr::lng_stories_post_retry(tr::now), [=] {
				controller->session().data().stories().retryPosting();
			}, &st::storiesMenuIconRetry);
		}
		if (posting.active || posting.failed) {
			auto cancel = Ui::MenuItem();
			cancel.text = tr::lng_stories_post_cancel(tr::now);
			cancel.icon = &st::menuIconDelete;
			cancel.destructive = true;
			cancel.callback = [=] {
				controller->session().data().stories().cancelPosting();
				controller->showToast(tr::lng_stories_post_cancelled(tr::now));
			};
			menu->addItem(std::move(cancel));
		} else {
			menu->addAction(tr::lng_stories_add(tr::now), [=] {
				ShowComposeBox(controller);
			}, &st::storiesMenuIconAdd);
		}
		if (stories.me().count > 0) {
			menu->addAction(tr::lng_stories_open_of(
				tr::now,
				lt_name,
				tr::lng_stories_yours(tr::now)), [=] {
				OpenUserStories(controller, controller->session().userId());
			}, &st::storiesMenuIconEye);
		}
	} else {
		const auto &tray = stories.tray();
		if (index - 1 >= tray.size()) {
			delete menu;
			return;
		}
		const auto entry = tray[index - 1];
		const auto userId = entry.user.id;
		const auto name = ShortName(entry.user);
		menu->addAction(
			tr::lng_stories_open_of(tr::now, lt_name, name),
			[=] {
				OpenUserStories(
					controller,
					userId,
					controller->session().data().stories().trayUserIds());
			},
			&st::storiesMenuIconEye);
		const auto muted = entry.muted;
		menu->addAction(
			muted
				? tr::lng_stories_unmute(tr::now, lt_name, name)
				: tr::lng_stories_mute(tr::now, lt_name, name),
			[=] {
				controller->session().data().stories().setMuted(userId, !muted);
				controller->showToast(muted
					? tr::lng_stories_unmuted(tr::now, lt_name, name)
					: tr::lng_stories_muted(tr::now, lt_name, name));
			},
			muted ? &st::storiesMenuIconEye : &st::storiesMenuIconHide);
	}
	menu->popup(global);
}

} // namespace Stories
