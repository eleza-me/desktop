#include "dialogs/dialogs_notes.h"

#include "data/data_chats.h"
#include "data/data_notes.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "dialogs/dialogs_notes_box.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/rtl.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

namespace Dialogs {
namespace {

[[nodiscard]] QString ShortName(const Data::User &user) {
	if (user.deleted) {
		return tr::lng_deleted_account(tr::now);
	} else if (!user.displayName.isEmpty()) {
		return user.displayName;
	}
	return user.handle.isEmpty() ? QString() : Lang::Handle(user.handle);
}

// The card as the users' store knows it now (renames since the list was read).
[[nodiscard]] Data::User CardOf(
		Window::SessionController *controller,
		const Data::User &fallback) {
	const auto card = controller->session().data().users().peek(fallback.id);
	return card.valid() ? card : fallback;
}

} // namespace

void PaintNoteBubble(
		QPainter &p,
		const QRect &rect,
		int radius,
		bool closeFriends) {
	auto hq = PainterHighQualityEnabler(p);
	const auto rim = closeFriends
		? QPen(st::dialogsNotesCloseFriendsFg->c, 1.5)
		: QPen(st::dialogsNotesBubbleBorder->c, 1.);
	// Two dots under the bubble, a little to the leading side of its middle.
	const auto big = std::max(rect.height() / 4, 6);
	const auto small = std::max(big / 2, 3);
	const auto lean = style::RightToLeft() ? 1 : -1;
	const auto middle = rect.x() + rect.width() / 2;
	const auto bigRect = QRectF(
		middle + lean * big - big / 2.,
		rect.y() + rect.height() - big / 2.,
		big,
		big);
	const auto smallRect = QRectF(
		bigRect.center().x() + lean * big * 0.9 - small / 2.,
		bigRect.bottom() + 1.,
		small,
		small);
	p.setBrush(st::dialogsNotesBubbleBg);
	p.setPen(rim);
	p.drawEllipse(smallRect);
	p.drawEllipse(bigRect);
	p.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
	// The big dot joins the box: no rim between them.
	p.setPen(Qt::NoPen);
	p.drawEllipse(bigRect.adjusted(1.5, 1.5, -1.5, -1.5));
	p.drawRect(QRectF(
		bigRect.x() + 1.5,
		rect.y() + rect.height() - big,
		big - 3.,
		big / 2. + 0.5));
}

// ---- the row

NotesRow::NotesRow(QWidget *parent, Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _userpics(std::make_unique<Support::UserpicPainter>(
	&controller->session(),
	[this](const QString &) { update(); },
	this)) {
	setMouseTracking(true);
	const auto notes = &controller->session().data().notes();
	connect(notes, &Data::Notes::changed, this, [this] {
		setScroll(_scroll);
		update();
	});
	connect(
		&controller->session().data().users(),
		&Data::Users::changed,
		this,
		[this] { update(); });
	connect(
		&controller->session().data().users(),
		&Data::Users::meChanged,
		this,
		[this] { update(); });
	resize(width(), resizeGetHeight(width()));
}

NotesRow::~NotesRow() = default;

void NotesRow::setHideCallback(Fn<void()> callback) {
	_hide = std::move(callback);
}

int NotesRow::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	const auto &padding = st::dialogsNotesPadding;
	return padding.top()
		+ st::dialogsNotesBubbleHeight
		+ st::dialogsNotesBubbleSkip
		+ st::dialogsNotesUserpic
		+ st::dialogsNotesNameTop
		+ st::dialogsNotesNameFont->height
		+ padding.bottom();
}

int NotesRow::count() const {
	return 1 + int(_controller->session().data().notes().feed().items.size());
}

int NotesRow::maxScroll() const {
	const auto &padding = st::dialogsNotesPadding;
	const auto content = padding.left()
		+ count() * st::dialogsNotesItemWidth
		+ padding.right();
	return std::max(content - width(), 0);
}

void NotesRow::setScroll(int value) {
	const auto clamped = std::clamp(value, 0, maxScroll());
	if (_scroll != clamped) {
		_scroll = clamped;
		update();
	}
}

QRect NotesRow::itemRect(int index) const {
	const auto x = st::dialogsNotesPadding.left()
		+ index * st::dialogsNotesItemWidth
		- _scroll;
	return style::rtlrect(x, 0, st::dialogsNotesItemWidth, height(), width());
}

QRect NotesRow::userpicRect(int index) const {
	const auto item = itemRect(index);
	const auto size = int(st::dialogsNotesUserpic);
	return QRect(
		item.x() + (item.width() - size) / 2,
		st::dialogsNotesPadding.top()
			+ st::dialogsNotesBubbleHeight
			+ st::dialogsNotesBubbleSkip,
		size,
		size);
}

QRect NotesRow::arrowRect(bool forward) const {
	const auto size = int(st::dialogsNotesArrowSize);
	const auto top = userpicRect(0).y() + (st::dialogsNotesUserpic - size) / 2;
	const auto atLeft = (!forward != style::RightToLeft());
	return QRect(atLeft ? 4 : (width() - 4 - size), top, size, size);
}

NotesRow::Hit NotesRow::hitAt(QPoint point) const {
	if (_scroll > 0 && arrowRect(false).contains(point)) {
		return { Part::ArrowBack, -1 };
	} else if (_scroll < maxScroll() && arrowRect(true).contains(point)) {
		return { Part::ArrowForward, -1 };
	}
	for (auto i = 0, n = count(); i != n; ++i) {
		const auto rect = itemRect(i);
		if (rect.right() >= 0 && rect.left() <= width() && rect.contains(point)) {
			return { Part::Item, i };
		}
	}
	return {};
}

void NotesRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	p.setClipRect(e->rect());
	const auto &notes = _controller->session().data().notes();
	const auto &feed = notes.feed();
	const auto &font = st::dialogsNotesBubbleFont;
	const auto &nameFont = st::dialogsNotesNameFont;
	const auto bubblePadding = int(st::dialogsNotesBubblePadding);
	const auto bubbleMax = int(st::dialogsNotesItemWidth) - 4;
	for (auto i = 0, n = count(); i != n; ++i) {
		const auto item = itemRect(i);
		if (item.right() < 0 || item.left() > width()) {
			continue;
		}
		if (_over.part == Part::Item && _over.index == i) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::dialogsNotesBgOver);
			p.drawRoundedRect(item.adjusted(2, 2, -2, -2), 12, 12);
		}
		const auto userpic = userpicRect(i);
		auto text = QString();
		auto name = QString();
		auto empty = false;
		auto closeFriends = false;
		if (i == 0) {
			_userpics->paint(p, userpic, _controller->session().userId());
			if (const auto mine = notes.mine()) {
				text = mine->text;
				closeFriends = (mine->audience == Data::NoteAudience::CloseFriends);
			} else {
				text = tr::lng_notes_leave(tr::now);
				empty = true;
				// «+» at the bottom trailing corner of my face.
				const auto size = int(st::dialogsNotesAddSize);
				const auto plus = QRect(
					style::RightToLeft()
						? userpic.x() - 2
						: (userpic.x() + userpic.width() - size + 2),
					userpic.y() + userpic.height() - size + 2,
					size,
					size);
				auto hq = PainterHighQualityEnabler(p);
				const auto border = int(st::dialogsNotesAddBorder);
				p.setPen(Qt::NoPen);
				p.setBrush(st::bgCanvas);
				p.drawEllipse(plus.adjusted(-border, -border, border, border));
				p.setBrush(st::dialogsNotesAddBg);
				p.drawEllipse(plus);
				st::dialogsNotesAddIcon.paintInCenter(p, plus);
			}
			name = tr::lng_notes_yours(tr::now);
		} else {
			const auto &entry = feed.items[i - 1];
			const auto card = CardOf(_controller, entry.user);
			_userpics->paint(p, userpic, card);
			text = entry.note.text;
			closeFriends = (entry.note.audience == Data::NoteAudience::CloseFriends);
			name = ShortName(card);
		}

		// The bubble over the face: one line, as wide as its text.
		const auto elided = font->elided(text, bubbleMax - 2 * bubblePadding);
		const auto textWidth = font->width(elided);
		const auto bubbleWidth = std::clamp(
			textWidth + 2 * bubblePadding,
			int(st::dialogsNotesBubbleHeight) + 8,
			bubbleMax);
		const auto bubble = QRect(
			item.x() + (item.width() - bubbleWidth) / 2,
			st::dialogsNotesPadding.top(),
			bubbleWidth,
			st::dialogsNotesBubbleHeight);
		PaintNoteBubble(p, bubble, st::dialogsNotesBubbleRadius, closeFriends);
		p.setFont(font);
		p.setPen(empty ? st::dialogsNotesBubbleFgEmpty : st::dialogsNotesBubbleFg);
		p.drawText(bubble, Qt::AlignCenter, elided);

		p.setFont(nameFont);
		p.setPen(st::dialogsNotesNameFg);
		const auto nameElided = nameFont->elided(name, item.width() - 8);
		const auto nameWidth = nameFont->width(nameElided);
		p.drawText(
			item.x() + (item.width() - nameWidth) / 2,
			userpic.y() + userpic.height() + st::dialogsNotesNameTop + nameFont->ascent,
			nameElided);
	}
	// The arrows over the ends while there is more to the side.
	const auto paintArrow = [&](bool forward) {
		const auto rect = arrowRect(forward);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::dialogsNotesLineFg->c, 1.));
		p.setBrush(st::dialogsNotesArrowBg);
		p.drawEllipse(rect);
		const auto pointsLeft = (forward == style::RightToLeft());
		(pointsLeft ? st::dialogsNotesArrowLeft : st::dialogsNotesArrowRight)
			.paintInCenter(p, rect);
	};
	if (_scroll > 0) {
		paintArrow(false);
	}
	if (_scroll < maxScroll()) {
		paintArrow(true);
	}
	p.fillRect(QRect(0, height() - 1, width(), 1), st::dialogsNotesLineFg);
}

void NotesRow::mouseMoveEvent(QMouseEvent *e) {
	const auto hit = hitAt(e->pos());
	if (_over != hit) {
		_over = hit;
		setCursor((hit.part == Part::None) ? style::cur_default : style::cur_pointer);
		update();
	}
}

void NotesRow::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over.part != Part::None) {
		_over = Hit();
		update();
	}
}

void NotesRow::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = hitAt(e->pos());
	}
}

void NotesRow::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, Hit());
	const auto hit = hitAt(e->pos());
	if (pressed == hit && hit.part != Part::None) {
		activate(hit);
	}
}

void NotesRow::wheelEvent(QWheelEvent *e) {
	// Only a sideways wheel (or Shift + wheel) moves the row: the ordinary
	// one scrolls the chat list under it.
	const auto delta = e->angleDelta();
	const auto sideways = (std::abs(delta.x()) > std::abs(delta.y()))
		? delta.x()
		: (e->modifiers() & Qt::ShiftModifier)
		? delta.y()
		: 0;
	if (!sideways || maxScroll() <= 0) {
		e->ignore();
		return;
	}
	setScroll(_scroll - sideways);
	e->accept();
}

void NotesRow::contextMenuEvent(QContextMenuEvent *e) {
	const auto hit = hitAt(e->pos());
	showMenu((hit.part == Part::Item) ? hit.index : -1, e->globalPos());
}

void NotesRow::activate(const Hit &hit) {
	const auto page = std::max(
		width() - 2 * st::dialogsNotesItemWidth,
		int(st::dialogsNotesItemWidth));
	switch (hit.part) {
	case Part::ArrowBack: setScroll(_scroll - page); break;
	case Part::ArrowForward: setScroll(_scroll + page); break;
	case Part::Item: {
		const auto &feed = _controller->session().data().notes().feed();
		if (hit.index == 0) {
			ShowNoteComposer(_controller);
		} else if (hit.index - 1 < feed.items.size()) {
			ShowFriendNote(_controller, feed.items[hit.index - 1].user.id);
		}
	} break;
	case Part::None: break;
	}
}

void NotesRow::showMenu(int index, QPoint global) {
	const auto controller = _controller;
	const auto &notes = controller->session().data().notes();
	const auto menu = new Ui::PopupMenu(this);
	if (index == 0) {
		menu->addAction(tr::lng_notes_new(tr::now), [=] {
			ShowNoteComposer(controller);
		}, &st::dialogsNotesMenuEdit);
		if (notes.mine()) {
			auto remove = Ui::MenuItem();
			remove.text = tr::lng_notes_delete(tr::now);
			remove.icon = &st::menuIconDelete;
			remove.destructive = true;
			remove.callback = [=] {
				DeleteMyNote(controller);
			};
			menu->addItem(std::move(remove));
		}
	} else if (index > 0 && index - 1 < notes.feed().items.size()) {
		const auto card = CardOf(controller, notes.feed().items[index - 1].user);
		const auto userId = card.id;
		menu->addAction(
			tr::lng_notes_menu_message(tr::now, lt_name, Lang::Isolated(ShortName(card))),
			[=] { OpenNoteChat(controller, userId); },
			&st::dialogsNotesMenuMessage);
		menu->addAction(tr::lng_profile_view_profile(tr::now), [=] {
			controller->openRoute(Window::Routes::User(userId));
		}, &st::dialogsNotesMenuProfile);
	}
	if (_hide) {
		const auto hide = _hide;
		menu->addAction(tr::lng_notes_hide(tr::now), [=] {
			hide();
		}, &st::dialogsNotesMenuHide);
	}
	if (menu->empty()) {
		delete menu;
		return;
	}
	menu->popup(global);
}

// ---- the faces next to the title

NotesFaces::NotesFaces(QWidget *parent, Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _userpics(std::make_unique<Support::UserpicPainter>(
	&controller->session(),
	[this](const QString &) { update(); },
	this)) {
	setMouseTracking(true);
	setCursor(style::cur_pointer);
	connect(
		&controller->session().data().users(),
		&Data::Users::changed,
		this,
		[this] { update(); });
	resize(0, st::dialogsNotesFacesHeight);
}

NotesFaces::~NotesFaces() = default;

void NotesFaces::setUsers(const QStringList &userIds) {
	const auto users = userIds.mid(0, kNotesFacesMax);
	if (_users == users) {
		return;
	}
	_users = users;
	_over = _pressed = -1;
	const auto count = int(_users.size());
	const auto face = int(st::dialogsNotesFace) + 2 * st::dialogsNotesFaceBorder;
	resize(
		count ? (face + (count - 1) * (face - st::dialogsNotesFaceOverlap)) : 0,
		st::dialogsNotesFacesHeight);
	update();
}

void NotesFaces::setClickedCallback(Fn<void(const QString &)> callback) {
	_clicked = std::move(callback);
}

QRect NotesFaces::faceRect(int index) const {
	const auto border = int(st::dialogsNotesFaceBorder);
	const auto face = int(st::dialogsNotesFace) + 2 * border;
	const auto step = face - st::dialogsNotesFaceOverlap;
	return style::rtlrect(
		index * step,
		(height() - face) / 2,
		face,
		face,
		width());
}

int NotesFaces::faceAt(QPoint point) const {
	// The one on top where two overlap: the first.
	for (auto i = 0, count = int(_users.size()); i != count; ++i) {
		if (faceRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void NotesFaces::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto border = int(st::dialogsNotesFaceBorder);
	// The last one first: the first lies on top.
	for (auto i = int(_users.size()); i != 0;) {
		--i;
		const auto rect = faceRect(i);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush((_over == i) ? st::dialogsNotesBgOver : st::bgCanvas);
			p.drawEllipse(rect);
		}
		_userpics->paint(
			p,
			rect.marginsRemoved({ border, border, border, border }),
			_users[i]);
	}
}

void NotesFaces::mouseMoveEvent(QMouseEvent *e) {
	const auto over = faceAt(e->pos());
	if (_over != over) {
		_over = over;
		update();
	}
}

void NotesFaces::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

void NotesFaces::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = faceAt(e->pos());
	}
}

void NotesFaces::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	const auto face = faceAt(e->pos());
	if (_clicked && rect().contains(e->pos()) && pressed == face) {
		_clicked((face >= 0) ? _users[face] : QString());
	}
}

} // namespace Dialogs
