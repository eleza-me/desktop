#include "groups/groups_info.h"

#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "groups/groups_boxes.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_groups.h"

#include <QtGui/QMouseEvent>

namespace Groups {
namespace {

namespace Rights = Data::GroupRights;

constexpr auto kLoadMoreThreshold = 400;

[[nodiscard]] QString NotifyLabel(const QString &level) {
	return (level == u"mentions")
		? tr::lng_groups_notify_mentions(tr::now)
		: (level == u"none")
		? tr::lng_groups_notify_none(tr::now)
		: tr::lng_groups_notify_all(tr::now);
}

// One centred column inside the scroll area, like the pages of the settings.
class ColumnWrap final : public Ui::RpWidget {
public:
	ColumnWrap(QWidget *parent, Ui::VerticalLayout *content)
	: RpWidget(parent)
	, _content(content) {
		_content->setParent(this);
		connect(_content, &Ui::RpWidget::heightChanged, this, [this] {
			if (!_inResize) {
				resizeToWidth(width());
			}
		});
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_inResize = true;
		const auto inner = std::min(newWidth, int(st::groupsColumnWidth));
		_content->resizeToWidth(inner);
		_content->move((newWidth - inner) / 2, 0);
		_inResize = false;
		return _content->height();
	}
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		_content->setVisibleTopBottom(
			visibleTop - _content->y(),
			visibleBottom - _content->y());
	}

private:
	Ui::VerticalLayout *_content = nullptr;
	bool _inResize = false;

};

} // namespace

Window::SectionFactory InfoFactory(const QString &groupId) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new InfoWidget(parent, controller, groupId));
	};
}

// ---- the picture, the name, how many members

class InfoWidget::Cover final : public Ui::RpWidget {
public:
	Cover(QWidget *parent, Main::Session *session)
	: RpWidget(parent)
	, _userpic(Ui::CreateChild<Support::UserpicButton>(
		this,
		session,
		QString(),
		st::groupsCoverUserpic)) {
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	void setGroup(const Data::Group &group) {
		const auto title = group.title.isEmpty()
			? tr::lng_groups_group(tr::now)
			: group.title;
		_userpic->setGroup(group.id, group.photo, title);
		_name.setText(st::groupsCoverNameStyle, title);
		_count = (group.memberCount > 0)
			? tr::lng_groups_members(tr::now, lt_count, group.memberCount)
			: QString();
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::groupsCoverPadding;
		const auto available = std::max(
			newWidth - padding.left() - padding.right(),
			1);
		_userpic->move(
			(newWidth - st::groupsCoverUserpic) / 2,
			padding.top());
		_nameTop = padding.top()
			+ st::groupsCoverUserpic
			+ st::groupsCoverNameSkip;
		_nameHeight = _name.countHeight(available, 2);
		_countTop = _nameTop + _nameHeight + st::groupsCoverCountSkip;
		return _countTop
			+ (_count.isEmpty() ? 0 : st::groupsCoverCountFont->height)
			+ padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::groupsCoverPadding;
		const auto available = std::max(
			width() - padding.left() - padding.right(),
			1);
		auto context = Ui::Text::PaintContext();
		context.position = QPoint(padding.left(), _nameTop);
		context.availableWidth = available;
		context.align = style::al_center;
		context.elisionLines = 2;
		p.setPen(st::textPrimary);
		_name.draw(p, context);
		if (!_count.isEmpty()) {
			p.setFont(st::groupsCoverCountFont);
			p.setPen(st::textSecondary);
			p.drawText(
				QRect(padding.left(), _countTop, available, st::groupsCoverCountFont->height),
				Qt::AlignHCenter | Qt::AlignTop,
				_count);
		}
	}

private:
	Support::UserpicButton *_userpic = nullptr;
	Ui::Text::String _name;
	QString _count;
	int _nameTop = 0;
	int _nameHeight = 0;
	int _countTop = 0;

};

// ---- the members, a page at a time

class InfoWidget::MembersList final : public Ui::RpWidget {
public:
	MembersList(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &groupId)
	: RpWidget(parent)
	, _controller(controller)
	, _groupId(groupId)
	, _userpics(&controller->session(), [this](const QString &) { update(); }) {
		setMouseTracking(true);
		auto &data = controller->session().data();
		connect(&data.users(), &Data::Users::changed, this, [this] {
			update();
		});
		connect(&data.groups(), &Data::Groups::membersChanged, this, [this](
				const QString &group) {
			if (group == _groupId) {
				reload();
			}
		});
		style::OnPaletteChanged(this, [this] {
			_userpics.clear();
			update();
		});
		reload();
	}

	void reload() {
		++_generation;
		_loading = false;
		load(true);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		const auto extra = (_failed || (_loading && _rows.isEmpty()) || !_cursor.isEmpty())
			? 1
			: 0;
		return (int(_rows.size()) + extra) * st::groupsMemberHeight;
	}

	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		Q_UNUSED(visibleTop);
		if (!_cursor.isEmpty()
			&& !_loading
			&& !_failed
			&& visibleBottom + kLoadMoreThreshold >= height()) {
			load(false);
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto clip = e->rect();
		const auto outer = width();
		const auto rowHeight = int(st::groupsMemberHeight);
		const auto &padding = st::groupsMemberPadding;
		const auto size = int(st::groupsMemberUserpic);
		const auto session = &_controller->session();
		const auto me = session->userId();
		const auto from = std::max(clip.top() / rowHeight, 0);
		const auto till = std::min(
			clip.bottom() / rowHeight + 1,
			int(_rows.size()));
		for (auto i = from; i < till; ++i) {
			const auto &member = _rows[i];
			const auto top = i * rowHeight;
			if (i == _over) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::stateHover);
				p.drawRoundedRect(
					QRect(
						st::groupsMemberMargin,
						top + 1,
						outer - 2 * st::groupsMemberMargin,
						rowHeight - 2),
					st::groupsMemberRadius,
					st::groupsMemberRadius);
			}
			_userpics.paint(
				p,
				style::rtlrect(padding.left(), top + (rowHeight - size) / 2, size, size, outer),
				member.user);
			const auto left = padding.left() + size + st::groupsMemberUserpicSkip;
			auto available = outer - left - padding.right();
			// The role on the trailing side: «مالک», «مدیر», a title, «تو».
			auto tag = MemberTag(member);
			if (tag.isEmpty() && member.user == me) {
				tag = tr::lng_groups_you_tag(tr::now);
			}
			if (!tag.isEmpty()) {
				const auto &font = st::groupsMemberTagFont;
				tag = font->elided(tag, std::max(available / 3, 1));
				const auto tagWidth = font->width(tag);
				p.setFont(font);
				p.setPen((member.role == u"owner" || member.role == u"admin")
					? st::brandText
					: st::textTertiary);
				p.drawTextRight(
					padding.right(),
					top + (rowHeight - font->height) / 2,
					outer,
					tag,
					tagWidth);
				available -= tagWidth + st::groupsMemberTagSkip;
			}
			const auto user = session->data().users().peek(member.user);
			const auto name = st::groupsMemberNameFont->elided(
				Support::MemberName(session, member.user),
				std::max(available, 1));
			const auto status = user.handle.isEmpty()
				? QString()
				: st::groupsMemberStatusFont->elided(
					Lang::Handle(user.handle),
					std::max(available, 1));
			p.setFont(st::groupsMemberNameFont);
			p.setPen(st::textPrimary);
			p.drawTextLeft(
				left,
				status.isEmpty()
					? (top + (rowHeight - st::groupsMemberNameFont->height) / 2)
					: (top + st::groupsMemberNameTop),
				outer,
				name);
			if (!status.isEmpty()) {
				p.setFont(st::groupsMemberStatusFont);
				p.setPen(st::textTertiary);
				p.drawTextLeft(left, top + st::groupsMemberStatusTop, outer, status);
			}
		}
		// The last row: «loading…», «show more», or what went wrong.
		const auto extraTop = int(_rows.size()) * rowHeight;
		if (height() > extraTop && clip.bottom() >= extraTop) {
			const auto text = _failed
				? tr::lng_groups_members_failed(tr::now)
				: _loading
				? tr::lng_loading(tr::now)
				: tr::lng_groups_show_more(tr::now);
			p.setFont(st::groupsMemberStatusFont);
			p.setPen((_failed || _loading) ? st::textTertiary : st::brandText);
			p.drawText(
				QRect(0, extraTop, outer, rowHeight),
				Qt::AlignCenter,
				text);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		setOver(rowAt(e->pos()));
	}
	void leaveEvent(QEvent *e) override {
		Q_UNUSED(e);
		setOver(-1);
	}
	void mousePressEvent(QMouseEvent *e) override {
		_pressed = rowAt(e->pos());
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto pressed = std::exchange(_pressed, -1);
		const auto index = rowAt(e->pos());
		if (index != pressed
			|| (e->button() != Qt::LeftButton && e->button() != Qt::RightButton)) {
			return;
		} else if (index >= 0 && index < _rows.size()) {
			const auto weak = QPointer<MembersList>(this);
			ShowMemberMenu(
				_controller,
				_groupId,
				_rows[index],
				e->globalPosition().toPoint(),
				[=] {
					if (weak) {
						weak->reload();
					}
				});
		} else if (index == _rows.size() && e->button() == Qt::LeftButton) {
			if (_failed) {
				reload();
			} else if (!_cursor.isEmpty()) {
				load(false);
			}
		}
	}

private:
	[[nodiscard]] int rowAt(QPoint position) const {
		if (position.y() < 0 || position.x() < 0 || position.x() >= width()) {
			return -1;
		}
		const auto index = position.y() / st::groupsMemberHeight;
		return (index * st::groupsMemberHeight < height()) ? index : -1;
	}
	void setOver(int index) {
		const auto over = (index >= 0 && index < _rows.size()) ? index : -1;
		setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
		if (_over != over) {
			_over = over;
			update();
		}
	}
	void changed() {
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}
	void load(bool first) {
		if (_loading) {
			return;
		}
		_loading = true;
		_failed = false;
		const auto generation = _generation;
		const auto session = &_controller->session();
		changed();
		session->data().groups().loadMembers(
			_groupId,
			u"all"_s,
			first ? QString() : _cursor,
			this,
			[=, this](const Data::GroupMembersPage &page) {
				if (generation != _generation) {
					return;
				}
				_loading = false;
				_cursor = page.nextCursor;
				if (first) {
					_rows.clear();
					_known.clear();
				}
				auto ids = QStringList();
				for (const auto &member : page.members) {
					if (!_known.contains(member.user)) {
						_known.insert(member.user);
						_rows.push_back(member);
						ids.push_back(member.user);
					}
				}
				// Their cards (names, pictures): Users::changed repaints.
				session->data().users().request(ids);
				changed();
			},
			[=, this](const Api::Error &) {
				if (generation != _generation) {
					return;
				}
				_loading = false;
				_failed = true;
				changed();
			});
	}

	Window::SessionController *_controller = nullptr;
	QString _groupId;
	Support::UserpicPainter _userpics;
	QVector<Data::GroupMember> _rows;
	QSet<QString> _known;
	QString _cursor;
	int _generation = 0;
	int _over = -1;
	int _pressed = -1;
	bool _loading = false;
	bool _failed = false;

};

// ---- the screen

InfoWidget::InfoWidget(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &groupId)
: SectionWidget(parent, controller)
, _groupId(groupId)
, _bar(new Ui::TopBar(this))
, _scroll(new Ui::ScrollArea(this)) {
	setFocusPolicy(Qt::StrongFocus);
	_bar->setTitle(tr::lng_groups_info(tr::now));
	setTopBar(_bar);
	_content = new Ui::VerticalLayout();
	_scroll->setOwnedWidget(Ui::object_ptr<ColumnWrap>(_scroll, _content));

	auto &data = controller->session().data();
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](const QString &id) {
		if (id == _groupId) {
			refresh();
		}
	});
	connect(&data.chats(), &Data::Chats::chatRemoved, this, [this](const QString &id) {
		if (id == _groupId) {
			close();
		}
	});
	connect(&data.groups(), &Data::Groups::enabledChanged, this, [this] {
		refresh();
	});
	if (!data.groups().group(_groupId)) {
		// Opened by a link before the list had the group.
		data.chats().loadHeader(_groupId, this, [this](const Data::ChatInfo &) {
			refresh();
		}, [this](const Api::Error &) {
			close();
		});
	} else {
		// My rights may have changed since the row was read.
		data.groups().refreshHeader(_groupId);
	}
	refresh();
}

QString InfoWidget::sectionKey() const {
	return Window::Routes::Group(_groupId);
}

void InfoWidget::setInnerFocus() {
	setFocus();
}

void InfoWidget::close() {
	if (_closing) {
		return;
	}
	_closing = true;
	const auto weak = QPointer<InfoWidget>(this);
	const auto window = controller();
	Ui::PostponeCall(window, [=] {
		if (weak && window->mainSection() == weak.data()) {
			if (!window->showBack()) {
				window->clearMainStack();
			}
		}
	});
}

QString InfoWidget::shape(const Data::Group &group) const {
	auto &groups = controller()->session().data().groups();
	return QStringList{
		group.role,
		group.rights.join(u','),
		group.permissions.join(u','),
		groups.administration() ? u"2"_s : u"1"_s,
		group.about.trimmed().isEmpty() ? u"-"_s : u"a"_s,
	}.join(u'|');
}

Ui::SettingsButton *InfoWidget::addRow(
		const QString &text,
		const style::icon *icon,
		Fn<void()> clicked,
		bool danger) {
	const auto row = _content->add(Ui::object_ptr<Ui::SettingsButton>(
		_content,
		text,
		danger ? st::groupsDangerButton : st::groupsButton));
	row->setIcon(icon);
	if (clicked) {
		row->setClickedCallback(std::move(clicked));
	}
	return row;
}

void InfoWidget::addTitle(const QString &text) {
	_content->add(
		Ui::object_ptr<Ui::FlatLabel>(_content, text, st::groupsSectionTitle),
		st::groupsSectionTitlePadding);
}

void InfoWidget::addDivider() {
	_content->add(
		Ui::object_ptr<Ui::FixedHeightWidget>(_content, st::groupsDividerHeight));
}

void InfoWidget::refresh() {
	auto &groups = controller()->session().data().groups();
	const auto group = groups.group(_groupId);
	if (!group) {
		return;
	}
	const auto now = shape(*group);
	if (_shape != now || !_cover) {
		_shape = now;
		rebuild();
		return;
	}
	_cover->setGroup(*group);
	if (_about) {
		_about->setText(group->about.trimmed());
	}
	if (_notify) {
		_notify->setValueText(NotifyLabel(group->notify));
	}
	if (_requests) {
		_requests->setValueText(group->joinRequests > 0
			? Lang::Num(group->joinRequests)
			: QString());
	}
	updateGeometries();
}

void InfoWidget::rebuild() {
	auto &groups = controller()->session().data().groups();
	const auto found = groups.group(_groupId);
	if (!found) {
		return;
	}
	const auto group = *found;
	const auto window = controller();
	const auto id = _groupId;
	const auto administration = groups.administration();
	const auto me = Rights::Member::Of(
		window->session().userId(),
		group.role,
		Rights::RightsMask(group.rights));
	const auto defaults = Rights::PermissionsMask(group.defaultPermissions);
	const auto may = [&](const Rights::Action &action) {
		return Rights::Can(defaults, me, action);
	};

	_content->clear();
	_about = nullptr;
	_notify = nullptr;
	_requests = nullptr;
	_members = nullptr;

	_cover = _content->add(
		Ui::object_ptr<Cover>(_content, &window->session()));
	_cover->setGroup(group);
	if (!group.about.trimmed().isEmpty()) {
		_about = _content->add(
			Ui::object_ptr<Ui::FlatLabel>(
				_content,
				group.about.trimmed(),
				st::groupsAboutLabel),
			st::groupsAboutPadding);
		_about->setAutoDirection(true);
		_about->setSelectable(true);
		_about->setLinkHandler(Support::LinkHandler(window));
	}

	addRow(tr::lng_groups_open_chat(tr::now), &st::groupsIconChat, [=] {
		window->openRoute(Window::Routes::Chat(id));
	});
	_notify = addRow(tr::lng_groups_notifications(tr::now), &st::groupsIconNotify, [=] {
		ShowNotifyBox(window, id);
	});
	_notify->setValueText(NotifyLabel(group.notify));
	if (group.can(u"change_info")) {
		addRow(tr::lng_groups_edit(tr::now), &st::groupsIconEdit, [=] {
			ShowEditBox(window, id);
		});
	}

	if (administration) {
		const auto links = may(Rights::kActInvites);
		const auto moderation = may(Rights::kActBan);
		addDivider();
		addTitle(tr::lng_groups_administration(tr::now));
		if (links) {
			addRow(tr::lng_groups_links_title(tr::now), &st::groupsIconLink, [=] {
				ShowInviteLinksBox(window, id);
			});
		}
		if (moderation) {
			_requests = addRow(
				tr::lng_groups_requests_title(tr::now),
				&st::groupsIconRequests,
				[=] { ShowJoinRequestsBox(window, id); });
			_requests->setValueText(group.joinRequests > 0
				? Lang::Num(group.joinRequests)
				: QString());
			addRow(tr::lng_groups_perms_title(tr::now), &st::groupsIconPermissions, [=] {
				ShowPermissionsBox(window, id);
			});
		}
		addRow(tr::lng_groups_admins(tr::now), &st::groupsIconAdmins, [=] {
			ShowMembersBox(window, id, u"admins"_s);
		});
		if (moderation) {
			addRow(
				tr::lng_groups_restricted_title(tr::now),
				&st::groupsIconRestricted,
				[=] { ShowMembersBox(window, id, u"restricted"_s); });
			addRow(tr::lng_groups_banned_title(tr::now), &st::groupsIconBanned, [=] {
				ShowMembersBox(window, id, u"banned"_s);
			});
		}
	}

	addDivider();
	addTitle(tr::lng_groups_members_title(tr::now));
	if (group.can(u"add_members")) {
		addRow(tr::lng_groups_add_members(tr::now), &st::groupsIconAdd, [=] {
			ShowAddMembers(window, id);
		});
	}
	_members = _content->add(Ui::object_ptr<MembersList>(_content, window, id));

	addDivider();
	addRow(tr::lng_groups_clear_history(tr::now), &st::groupsIconClear, [=] {
		ShowClearHistoryBox(window, id);
	});
	if (administration && !group.isOwner()) {
		addRow(tr::lng_groups_report_group(tr::now), &st::groupsIconReport, [=] {
			ShowReportGroup(window, id);
		});
	}
	addRow(tr::lng_groups_leave(tr::now), &st::groupsIconLeaveDanger, [=] {
		ShowLeaveBox(window, id);
	}, true);
	if (administration && group.isOwner()) {
		addRow(tr::lng_groups_delete_group(tr::now), &st::groupsIconDeleteDanger, [=] {
			ShowDeleteGroupBox(window, id);
		}, true);
	}
	_content->add(
		Ui::object_ptr<Ui::FixedHeightWidget>(_content, st::groupsBottomSkip));
	updateGeometries();
}

void InfoWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void InfoWidget::updateGeometries() {
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto top = _bar->height();
	_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
}

} // namespace Groups
