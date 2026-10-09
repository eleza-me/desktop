#include "profile/profile_people_list.h"

#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "profile/profile_follow.h"
#include "support/error_text.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>

namespace Profile {

PeopleList::PeopleList(
	QWidget *parent,
	Window::SessionController *controller,
	Mode mode)
: RpWidget(parent)
, _controller(controller)
, _mode(mode)
, _userpics(&controller->session(), [this](const QString &userId) {
	for (auto i = 0; i != int(_shown.size()); ++i) {
		if (shownRow(i).profile.id == userId) {
			updateRow(i);
		}
	}
}) {
	setMouseTracking(true);
	auto &data = controller->session().data();
	connect(
		&data.graph(),
		&Data::Graph::relationshipChanged,
		this,
		[this](const QString &userId, const Data::GraphState &state) {
			if (const auto row = rowById(userId)) {
				row->follow = FollowUiOf(state);
				row->profile.followState = state.follow;
				row->profile.followsYou = state.followsYou;
				row->subtitle = subtitleFor(*row);
				update();
			}
		});
	connect(
		&data.users(),
		&Data::Users::changed,
		this,
		[this](const QStringList &ids) {
			auto changed = false;
			auto &users = _controller->session().data().users();
			for (const auto &id : ids) {
				const auto row = rowById(id);
				if (!row) {
					continue;
				}
				const auto card = users.peek(id);
				if (!card.valid() || card.deleted) {
					continue;
				}
				if (row->profile.displayName != card.displayName) {
					row->profile.displayName = card.displayName;
					row->nameReady = false;
					changed = true;
				}
				if (row->profile.avatarId != card.avatarId
					|| row->profile.verified != card.verified) {
					row->profile.avatarId = card.avatarId;
					row->profile.verified = card.verified;
					changed = true;
				}
			}
			if (changed) {
				update();
			}
		});
	style::OnPaletteChanged(this, [this] {
		_userpics.clear();
		update();
	});
}

PeopleList::~PeopleList() = default;

void PeopleList::setRemovable(bool removable) {
	if (_removable != removable) {
		_removable = removable;
		update();
	}
}

QString PeopleList::subtitleFor(const Row &row) const {
	auto result = QString();
	if (!row.profile.handle.isEmpty()) {
		// The handle is always left-to-right, also inside a Persian line.
		result = LtrText(QLatin1Char('@') + row.profile.handle);
	}
	const auto followsYou = (_mode == Mode::Follow)
		&& !_removable
		&& !row.mine
		&& row.profile.followsYou;
	if (followsYou) {
		if (!result.isEmpty()) {
			result += u" · "_s;
		}
		result += tr::lng_profile_follows_you(tr::now);
	}
	return result;
}

void PeopleList::addRow(const Data::Profile &profile) {
	if (profile.deleted || profile.id.isEmpty() || rowById(profile.id)) {
		return;
	}
	auto row = std::make_unique<Row>();
	row->profile = profile;
	row->follow = FollowUiOf(profile);
	row->mine = profile.me
		|| (profile.id == _controller->session().data().users().myId());
	row->search = (profile.displayName + QLatin1Char('\n') + profile.handle)
		.toLower();
	row->subtitle = subtitleFor(*row);
	_rows.push_back(std::move(row));
}

void PeopleList::setPeople(const QVector<Data::Profile> &people) {
	_rows.clear();
	_over = _pressed = Hit();
	_selected = -1;
	for (const auto &profile : people) {
		addRow(profile);
	}
	refilter();
}

void PeopleList::appendPeople(const QVector<Data::Profile> &people) {
	for (const auto &profile : people) {
		addRow(profile);
	}
	refilter();
}

void PeopleList::removePerson(const QString &userId) {
	const auto i = std::find_if(_rows.begin(), _rows.end(), [&](const auto &row) {
		return row->profile.id == userId;
	});
	if (i == _rows.end()) {
		return;
	}
	_rows.erase(i);
	_over = _pressed = Hit();
	_selected = -1;
	refilter();
}

void PeopleList::setFilter(const QString &query) {
	const auto filter = query.trimmed().toLower();
	if (_filter == filter) {
		return;
	}
	_filter = filter;
	_selected = -1;
	refilter();
}

void PeopleList::setLoadingMore(bool loading) {
	if (_loadingMore == loading) {
		return;
	}
	_loadingMore = loading;
	if (loading) {
		if (!_spinner) {
			_spinner = std::make_unique<Ui::InfiniteRadialAnimation>([this] {
				update(
					0,
					int(_shown.size()) * st::profilePersonHeight,
					width(),
					st::profilePersonHeight);
			});
		}
		_spinner->start();
	} else if (_spinner) {
		_spinner->stop();
	}
	refreshHeight();
}

void PeopleList::refilter() {
	_shown.clear();
	_shown.reserve(_rows.size());
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_filter.isEmpty() || _rows[i]->search.contains(_filter)) {
			_shown.push_back(i);
		}
	}
	_over = _pressed = Hit();
	if (_selected >= int(_shown.size())) {
		_selected = -1;
	}
	refreshHeight();
	update();
}

void PeopleList::refreshHeight() {
	const auto wanted = resizeGetHeight(width());
	if (height() != wanted) {
		resize(width(), wanted);
	}
}

int PeopleList::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	const auto rows = int(_shown.size())
		+ ((_loadingMore && !_shown.empty()) ? 1 : 0);
	return rows * st::profilePersonHeight;
}

PeopleList::Row *PeopleList::rowById(const QString &userId) {
	for (const auto &row : _rows) {
		if (row->profile.id == userId) {
			return row.get();
		}
	}
	return nullptr;
}

PeopleList::Layout PeopleList::layoutFor(const Row &row) const {
	auto result = Layout();
	const auto outer = width();
	const auto height = int(st::profilePersonHeight);
	const auto &padding = st::profilePersonPadding;
	const auto size = int(st::profilePersonUserpic);
	result.userpic = style::rtlrect(
		padding.left(),
		(height - size) / 2,
		size,
		size,
		outer);
	auto trailing = padding.right();
	const auto place = [&](int buttonWidth, int buttonHeight) {
		const auto rect = style::rtlrect(
			outer - trailing - buttonWidth,
			(height - buttonHeight) / 2,
			buttonWidth,
			buttonHeight,
			outer);
		trailing += buttonWidth + st::profilePersonButtonSkip;
		return rect;
	};
	if (_mode == Mode::Requests) {
		const auto &decline = st::profileRowFollowing;
		const auto &accept = st::profileRowFollow;
		result.secondary = place(
			decline.font->width(tr::lng_decline(tr::now)) + 2 * decline.padding,
			decline.height);
		result.primary = place(
			accept.font->width(tr::lng_accept(tr::now)) + 2 * accept.padding,
			accept.height);
	} else {
		if (_removable) {
			result.more = place(st::profilePersonMore, st::profilePersonMore);
		}
		if (!row.mine) {
			result.primary = place(
				FollowPillWidth(row.follow, true),
				st::profileRowFollow.height);
		}
	}
	result.textLeft = padding.left() + size + st::profilePersonUserpicSkip;
	result.textWidth = std::max(
		outer - result.textLeft - trailing - st::profilePersonTextSkip,
		0);
	return result;
}

PeopleList::Hit PeopleList::hitTest(QPoint position) const {
	const auto height = int(st::profilePersonHeight);
	if (position.y() < 0 || position.x() < 0 || position.x() >= width()) {
		return Hit();
	}
	const auto index = position.y() / height;
	if (index >= int(_shown.size())) {
		return Hit();
	}
	const auto &row = *_rows[_shown[index]];
	const auto layout = layoutFor(row);
	const auto local = position - QPoint(0, index * height);
	if (!layout.primary.isEmpty() && layout.primary.contains(local)) {
		return { index, row.busy ? Part::Row : Part::Primary };
	} else if (!layout.secondary.isEmpty()
		&& layout.secondary.contains(local)) {
		return { index, row.busy ? Part::Row : Part::Secondary };
	} else if (!layout.more.isEmpty() && layout.more.contains(local)) {
		return { index, Part::More };
	}
	return { index, Part::Row };
}

void PeopleList::updateRow(int index) {
	if (index >= 0) {
		update(
			0,
			index * st::profilePersonHeight,
			width(),
			st::profilePersonHeight);
	}
}

void PeopleList::setOver(Hit hit) {
	if (_over == hit) {
		return;
	}
	const auto was = _over.index;
	_over = hit;
	updateRow(was);
	updateRow(hit.index);
	setCursor((hit.part == Part::None)
		? style::cur_default
		: style::cur_pointer);
}

void PeopleList::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto height = int(st::profilePersonHeight);
	const auto count = int(_shown.size());
	const auto from = std::clamp(clip.top() / height, 0, count);
	const auto till = std::clamp((clip.bottom() / height) + 1, 0, count);
	for (auto i = from; i < till; ++i) {
		paintRow(p, i, i * height);
	}
	if (_loadingMore && _spinner && count > 0) {
		const auto size = int(st::profileTailSpinner);
		const auto rect = QRect(
			(width() - size) / 2,
			count * height + (height - size) / 2,
			size,
			size);
		if (rect.intersects(clip)) {
			_spinner->draw(p, rect, 2.5, st::brandPrimary->c);
		}
	}
}

void PeopleList::paintRow(Painter &p, int index, int top) {
	auto &row = shownRow(index);
	const auto outer = width();
	const auto height = int(st::profilePersonHeight);
	const auto layout = layoutFor(row);
	const auto over = (_over.index == index) || (_selected == index);
	const auto pressed = (_pressed.index == index)
		&& (_pressed.part == Part::Row)
		&& (_over == _pressed);
	if (pressed) {
		p.fillRect(0, top, outer, height, st::statePressed);
	} else if (over) {
		p.fillRect(0, top, outer, height, st::stateHover);
	}
	p.translate(0, top);

	_userpics.paint(p, layout.userpic, row.profile);

	if (!row.nameReady) {
		row.name.setText(st::profilePersonNameStyle, row.profile.displayName);
		row.nameReady = true;
	}
	const auto single = row.subtitle.isEmpty();
	const auto nameTop = single
		? ((height - st::profilePersonNameStyle.font->height) / 2)
		: int(st::profilePersonNameTop);
	auto badges = 0;
	if (row.profile.verified) {
		badges += st::profileRowVerifiedIcon.width() + st::profileBadgeSkip;
	}
	if (row.profile.isPrivate) {
		badges += st::profileRowLockIcon.width() + st::profileBadgeSkip;
	}
	const auto nameAvailable = std::max(layout.textWidth - badges, 0);
	const auto nameWidth = std::min(row.name.maxWidth(), nameAvailable);
	p.setPen(st::textPrimary);
	row.name.drawLeftElided(p, layout.textLeft, nameTop, nameWidth, outer);
	auto badgeLeft = layout.textLeft + nameWidth + st::profileBadgeSkip;
	const auto nameHeight = st::profilePersonNameStyle.font->height;
	if (row.profile.verified) {
		const auto &icon = st::profileRowVerifiedIcon;
		icon.paint(
			p,
			badgeLeft,
			nameTop + (nameHeight - icon.height()) / 2,
			outer);
		badgeLeft += icon.width() + st::profileBadgeSkip;
	}
	if (row.profile.isPrivate) {
		const auto &icon = st::profileRowLockIcon;
		icon.paint(
			p,
			badgeLeft,
			nameTop + (nameHeight - icon.height()) / 2,
			outer);
	}
	if (!single) {
		p.setPen(st::textTertiary);
		p.setFont(st::profilePersonStatusFont->f);
		const auto text = st::profilePersonStatusFont->elided(
			row.subtitle,
			layout.textWidth);
		p.drawTextLeft(
			layout.textLeft,
			st::profilePersonStatusTop,
			outer,
			text);
	}

	const auto partOver = [&](Part part) {
		return (_over.index == index) && (_over.part == part);
	};
	const auto partDown = [&](Part part) {
		return partOver(part)
			&& (_pressed.index == index)
			&& (_pressed.part == part);
	};
	if (_mode == Mode::Requests) {
		PaintPill(
			p,
			layout.primary,
			st::profileRowFollow,
			tr::lng_accept(tr::now),
			partOver(Part::Primary),
			partDown(Part::Primary),
			nullptr,
			row.busy ? 0.6 : 1.);
		PaintPill(
			p,
			layout.secondary,
			st::profileRowFollowing,
			tr::lng_decline(tr::now),
			partOver(Part::Secondary),
			partDown(Part::Secondary),
			nullptr,
			row.busy ? 0.6 : 1.);
	} else {
		if (!layout.primary.isEmpty()) {
			PaintPill(
				p,
				layout.primary,
				FollowStyle(row.follow, true),
				FollowText(row.follow),
				partOver(Part::Primary),
				partDown(Part::Primary),
				nullptr,
				row.busy ? 0.6 : 1.);
		}
		if (!layout.more.isEmpty() && (over || partOver(Part::More))) {
			if (partOver(Part::More)) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::stateHover);
				p.drawEllipse(layout.more);
			}
			st::profilePersonMoreIcon.paintInCenter(p, layout.more);
		}
	}
	p.translate(0, -top);
}

void PeopleList::mouseMoveEvent(QMouseEvent *e) {
	if (_selected >= 0) {
		const auto was = _selected;
		_selected = -1;
		updateRow(was);
	}
	setOver(hitTest(e->pos()));
}

void PeopleList::mousePressEvent(QMouseEvent *e) {
	setOver(hitTest(e->pos()));
	if (e->button() == Qt::LeftButton) {
		_pressed = _over;
		updateRow(_pressed.index);
	}
}

void PeopleList::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = _pressed;
	_pressed = Hit();
	updateRow(pressed.index);
	setOver(hitTest(e->pos()));
	if (e->button() == Qt::LeftButton
		&& pressed.index >= 0
		&& pressed == _over) {
		activate(pressed, e->globalPosition().toPoint());
	}
}

void PeopleList::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	setOver(Hit());
}

void PeopleList::contextMenuEvent(QContextMenuEvent *e) {
	const auto hit = hitTest(e->pos());
	if (hit.index >= 0) {
		showMenu(hit.index, e->globalPos());
	}
}

void PeopleList::activate(const Hit &hit, QPoint globalPosition) {
	if (hit.index < 0 || hit.index >= int(_shown.size())) {
		return;
	}
	const auto &row = shownRow(hit.index);
	const auto id = row.profile.id;
	switch (hit.part) {
	case Part::Primary:
		if (_mode == Mode::Requests) {
			answerRequest(id, true);
		} else {
			toggleFollow(id);
		}
		break;
	case Part::Secondary: answerRequest(id, false); break;
	case Part::More: showMenu(hit.index, globalPosition); break;
	case Part::Row: openProfile(row); break;
	case Part::None: break;
	}
}

void PeopleList::openProfile(const Row &row) {
	// May replace this section: nothing after it touches the list.
	_controller->openRoute(Window::Routes::User(row.profile.id));
}

void PeopleList::toggleFollow(const QString &userId) {
	const auto row = rowById(userId);
	if (!row || row->busy || row->mine) {
		return;
	}
	auto target = FollowTarget();
	target.id = userId;
	target.name = row->profile.displayName;
	target.isPrivate = row->profile.isPrivate;
	target.followsYou = row->profile.followsYou;
	target.state = row->follow;
	PerformFollowToggle(_controller, this, target, [=, this](
			FollowUi state,
			bool busy) {
		if (const auto row = rowById(userId)) {
			row->follow = state;
			row->busy = busy;
			update();
		}
	});
}

void PeopleList::answerRequest(const QString &userId, bool accept) {
	const auto row = rowById(userId);
	if (!row || row->busy) {
		return;
	}
	row->busy = true;
	update();
	const auto done = [=, this](const Data::GraphState &) {
		Q_EMIT requestAnswered(userId, accept);
	};
	const auto fail = [=, this](const Api::Error &error) {
		if (const auto row = rowById(userId)) {
			row->busy = false;
			update();
		}
		Support::ShowError(
			_controller,
			error,
			tr::lng_profile_action_failed(tr::now));
	};
	auto &graph = _controller->session().data().graph();
	if (accept) {
		graph.acceptFollowRequest(userId, this, done, fail);
	} else {
		graph.declineFollowRequest(userId, this, done, fail);
	}
}

void PeopleList::showMenu(int index, QPoint globalPosition) {
	if (index < 0 || index >= int(_shown.size())) {
		return;
	}
	if (_menu) {
		_menu->hideMenu(true);
	}
	const auto &row = shownRow(index);
	const auto id = row.profile.id;
	const auto name = row.profile.displayName;
	const auto handle = row.profile.handle;
	const auto controller = _controller;
	_menu = new Ui::PopupMenu(this);
	_menu->addAction(tr::lng_profile_view_profile(tr::now), [=] {
		controller->openRoute(Window::Routes::User(id));
	}, &st::profileMenuProfileIcon);
	if (_mode == Mode::Follow && !row.mine && !row.busy) {
		const auto unfollow = (row.follow == FollowUi::Following)
			|| (row.follow == FollowUi::Requested);
		_menu->addAction(
			unfollow ? tr::lng_unfollow(tr::now) : FollowText(row.follow),
			[=, this] { toggleFollow(id); },
			unfollow ? &st::profileMenuUnfollowIcon : &st::profileMenuFollowIcon);
	}
	if (!handle.isEmpty()) {
		_menu->addAction(tr::lng_copy_link(tr::now), [=] {
			QGuiApplication::clipboard()->setText(ProfileLink(handle));
			controller->showToast(tr::lng_link_copied(tr::now));
		}, &st::menuIconLink);
	}
	if (_removable && !row.mine) {
		_menu->addSeparator();
		auto item = Ui::MenuItem();
		item.text = tr::lng_profile_remove_follower(tr::now);
		item.icon = &st::profileMenuRemoveIcon;
		item.destructive = true;
		item.callback = [=, this] {
			Q_EMIT removeFollowerRequested(id, name);
		};
		_menu->addItem(std::move(item));
	}
	_menu->popup(globalPosition);
}

void PeopleList::selectSkip(int direction) {
	const auto count = int(_shown.size());
	if (!count) {
		return;
	}
	const auto was = _selected;
	_selected = (_selected < 0)
		? ((direction > 0) ? 0 : (count - 1))
		: std::clamp(_selected + direction, 0, count - 1);
	updateRow(was);
	updateRow(_selected);
	const auto height = int(st::profilePersonHeight);
	Q_EMIT scrollToRequested(_selected * height, (_selected + 1) * height);
}

bool PeopleList::chooseSelected() {
	if (_selected < 0 || _selected >= int(_shown.size())) {
		return false;
	}
	openProfile(shownRow(_selected));
	return true;
}

} // namespace Profile
