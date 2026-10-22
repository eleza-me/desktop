#include "groups/groups_admin.h"

#include "boxes/peer_list_box.h"
#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "groups/groups_common.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_image_viewer.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_groups.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

#include <array>

namespace Groups {
namespace {

using Controller = Window::SessionController;
namespace Log = Data::GroupAdminLog;

constexpr auto kLoadMoreThreshold = 600;

[[nodiscard]] Data::Groups &Store(Controller *controller) {
	return controller->session().data().groups();
}

[[nodiscard]] const Data::Group *Header(
		Controller *controller,
		const QString &groupId) {
	return Store(controller).group(groupId);
}

[[nodiscard]] QString Name(Controller *controller, const QString &userId) {
	return Support::MemberName(&controller->session(), userId);
}

[[nodiscard]] QString DateTimeText(qint64 ms) {
	const auto time = QDateTime::fromMSecsSinceEpoch(ms);
	return Lang::SolarDate(time.date()) + QLatin1Char(' ') + Lang::Clock(time);
}

[[nodiscard]] Boxes::PeerListRow UserRow(
		Main::Session *session,
		const QString &userId) {
	auto user = session->data().users().peek(userId);
	if (!user.valid()) {
		user.id = userId;
	}
	auto row = Boxes::PeerListRow::FromUser(user);
	if (row.title.isEmpty()) {
		row.title = Support::MemberName(session, userId);
	}
	return row;
}

// ---- who reacted

class ReactionsController final : public Boxes::PeerListController {
public:
	ReactionsController(Controller *controller, QString groupId, qint64 seq)
	: _controller(controller)
	, _groupId(std::move(groupId))
	, _seq(seq) {
	}

	QString title() const override {
		return tr::lng_groups_reactions(tr::now);
	}
	QString emptyText() const override {
		return tr::lng_groups_reactions_none(tr::now);
	}
	void prepare() override {
		delegate()->setLoading(true);
		load(true);
	}
	void loadMore() override {
		if (!_cursor.isEmpty()) {
			load(false);
		}
	}
	void rowClicked(const Boxes::PeerListRow &row) override {
		const auto route = Window::Routes::User(row.id);
		const auto controller = _controller;
		delegate()->closeBox();
		controller->openRoute(route);
	}

private:
	void load(bool first) {
		if (_loading) {
			return;
		}
		_loading = true;
		const auto session = this->session();
		const auto controller = _controller;
		const auto box = QPointer<Ui::BoxContent>(delegate()->peerListBox());
		Store(_controller).loadReactions(
			_groupId,
			_seq,
			QString(),
			first ? QString() : _cursor,
			base::Guard(delegate()->peerListBox()),
			[=, this](const Data::GroupReactionsPage &page) {
				_loading = false;
				_cursor = page.nextCursor;
				auto rows = QVector<Boxes::PeerListRow>();
				for (const auto &reaction : page.reactions) {
					auto row = UserRow(session, reaction.user);
					row.subtitle = reaction.emoji;
					rows.push_back(row);
				}
				if (first) {
					delegate()->setRows(rows);
				} else {
					delegate()->appendRows(rows);
				}
			},
			[=, this](const Api::Error &error) {
				_loading = false;
				if (first) {
					delegate()->setRows({});
				}
				if (error.code == u"group.members_hidden") {
					// The setting was switched on since the header was read.
					controller->showToast(tr::lng_groups_reactions_hidden(tr::now));
					Store(controller).refreshHeader(_groupId);
					if (box) {
						box->closeBox();
					}
				} else {
					Support::ShowError(controller, error);
				}
			});
	}

	Controller *_controller = nullptr;
	QString _groupId;
	qint64 _seq = 0;
	QString _cursor;
	bool _loading = false;

};

// ---- who joined through a link

class InviteMembersController final : public Boxes::PeerListController {
public:
	InviteMembersController(
		Controller *controller,
		QString groupId,
		QString link,
		QString title)
	: _controller(controller)
	, _groupId(std::move(groupId))
	, _link(std::move(link))
	, _title(std::move(title)) {
	}

	QString title() const override {
		return _title;
	}
	QString emptyText() const override {
		return tr::lng_groups_link_joined_none(tr::now);
	}
	void prepare() override {
		delegate()->setLoading(true);
		load(true);
	}
	void loadMore() override {
		if (!_cursor.isEmpty()) {
			load(false);
		}
	}
	void rowClicked(const Boxes::PeerListRow &row) override {
		const auto route = Window::Routes::User(row.id);
		const auto controller = _controller;
		delegate()->closeBox();
		controller->openRoute(route);
	}

private:
	void load(bool first) {
		if (_loading) {
			return;
		}
		_loading = true;
		const auto session = this->session();
		const auto controller = _controller;
		Store(_controller).loadInviteMembers(
			_groupId,
			_link,
			first ? QString() : _cursor,
			base::Guard(delegate()->peerListBox()),
			[=, this](const Data::GroupInviteMembersPage &page) {
				_loading = false;
				_cursor = page.nextCursor;
				auto rows = QVector<Boxes::PeerListRow>();
				for (const auto &member : page.members) {
					auto row = UserRow(session, member.user);
					if (member.joinedAt > 0) {
						row.subtitle = DateTimeText(member.joinedAt);
					}
					rows.push_back(row);
				}
				if (first) {
					delegate()->setRows(rows);
				} else {
					delegate()->appendRows(rows);
				}
			},
			[=, this](const Api::Error &error) {
				_loading = false;
				if (first) {
					delegate()->setRows({});
				}
				Support::ShowError(controller, error);
			});
	}

	Controller *_controller = nullptr;
	QString _groupId;
	QString _link;
	QString _title;
	QString _cursor;
	bool _loading = false;

};

// ---- recent actions

[[nodiscard]] QString EntryText(
		Controller *controller,
		const Data::GroupAdminLogEntry &entry) {
	const auto form = Log::FormOf(entry);
	const auto actor = Lang::Isolated(entry.actor.isEmpty()
		? tr::lng_groups_log_someone(tr::now)
		: Name(controller, entry.actor));
	const auto target = Lang::Isolated(entry.target.isEmpty()
		? tr::lng_groups_log_someone(tr::now)
		: Name(controller, entry.target));
	const auto link = Lang::Isolated(
		Data::Json::CleanName(entry.linkName.simplified()));
	const auto tag = Lang::Isolated(Data::MemberTags::Shown(entry.tag));
	const auto previous = Lang::Isolated(Data::MemberTags::Shown(entry.prevTag));
	using Form = Log::Form;
	switch (form) {
	case Form::Promote:
		return tr::lng_groups_log_promote(tr::now, lt_actor, actor, lt_target, target);
	case Form::AdminRights:
		return tr::lng_groups_log_admin_rights(tr::now, lt_actor, actor, lt_target, target);
	case Form::Demote:
		return tr::lng_groups_log_demote(tr::now, lt_actor, actor, lt_target, target);
	case Form::Restrict:
		return tr::lng_groups_log_restrict(tr::now, lt_actor, actor, lt_target, target);
	case Form::RestrictUntil:
		return tr::lng_groups_log_restrict_until(
			tr::now,
			lt_actor,
			actor,
			lt_target,
			target,
			lt_time,
			DateTimeText(entry.until * 1000));
	case Form::Unrestrict:
		return tr::lng_groups_log_unrestrict(tr::now, lt_actor, actor, lt_target, target);
	case Form::Remove:
		return tr::lng_groups_log_remove(tr::now, lt_actor, actor, lt_target, target);
	case Form::Ban:
		return tr::lng_groups_log_ban(tr::now, lt_actor, actor, lt_target, target);
	case Form::BanDelete:
		return tr::lng_groups_log_ban_delete(tr::now, lt_actor, actor, lt_target, target);
	case Form::Unban:
		return tr::lng_groups_log_unban(tr::now, lt_actor, actor, lt_target, target);
	case Form::Permissions:
		return tr::lng_groups_log_permissions(tr::now, lt_actor, actor);
	case Form::SlowModeOn:
		return tr::lng_groups_log_slow_on(
			tr::now,
			lt_actor,
			actor,
			lt_time,
			SlowModeLabel(entry.slowModeS));
	case Form::SlowModeOff:
		return tr::lng_groups_log_slow_off(tr::now, lt_actor, actor);
	case Form::HideMembersOn:
		return tr::lng_groups_log_hide_on(tr::now, lt_actor, actor);
	case Form::HideMembersOff:
		return tr::lng_groups_log_hide_off(tr::now, lt_actor, actor);
	case Form::Title:
		return tr::lng_groups_log_title_changed(tr::now, lt_actor, actor);
	case Form::Photo:
		return tr::lng_groups_log_photo(tr::now, lt_actor, actor);
	case Form::About:
		return tr::lng_groups_log_about(tr::now, lt_actor, actor);
	case Form::Pin:
		return tr::lng_groups_log_pin(tr::now, lt_actor, actor);
	case Form::Unpin:
		return tr::lng_groups_log_unpin(tr::now, lt_actor, actor);
	case Form::InviteCreate:
		return tr::lng_groups_log_invite_create(tr::now, lt_actor, actor);
	case Form::InviteCreateNamed:
		return tr::lng_groups_log_invite_create_named(tr::now, lt_actor, actor, lt_name, link);
	case Form::InviteRevoke:
		return tr::lng_groups_log_invite_revoke(tr::now, lt_actor, actor);
	case Form::InviteRevokeNamed:
		return tr::lng_groups_log_invite_revoke_named(tr::now, lt_actor, actor, lt_name, link);
	case Form::JoinApprove:
		return tr::lng_groups_log_join_approve(tr::now, lt_actor, actor, lt_target, target);
	case Form::JoinDecline:
		return tr::lng_groups_log_join_decline(tr::now, lt_actor, actor, lt_target, target);
	case Form::DeleteMessage:
		return tr::lng_groups_log_delete_message(tr::now, lt_actor, actor, lt_target, target);
	case Form::DeleteMessages:
		return tr::lng_groups_log_delete_messages(
			tr::now,
			lt_count,
			double(std::max(int(entry.seqs.size()), 1)),
			lt_actor,
			actor);
	case Form::DeleteMessagesOf:
		return tr::lng_groups_log_delete_messages_of(
			tr::now,
			lt_count,
			double(std::max(int(entry.seqs.size()), 1)),
			lt_actor,
			actor,
			lt_target,
			target);
	case Form::DeleteAll:
		return tr::lng_groups_log_delete_all(tr::now, lt_actor, actor, lt_target, target);
	case Form::Transfer:
		return tr::lng_groups_log_transfer(tr::now, lt_actor, actor, lt_target, target);
	case Form::TagChanged:
		return tr::lng_groups_log_tag_changed(
			tr::now,
			lt_actor,
			actor,
			lt_target,
			target,
			lt_previous,
			previous,
			lt_tag,
			tag);
	case Form::TagSet:
		return tr::lng_groups_log_tag_set(
			tr::now,
			lt_actor,
			actor,
			lt_target,
			target,
			lt_tag,
			tag);
	case Form::TagRemoved:
		return tr::lng_groups_log_tag_removed(
			tr::now,
			lt_actor,
			actor,
			lt_target,
			target,
			lt_previous,
			previous);
	case Form::OwnTagChanged:
		return tr::lng_groups_log_own_tag_changed(
			tr::now,
			lt_actor,
			actor,
			lt_previous,
			previous,
			lt_tag,
			tag);
	case Form::OwnTagSet:
		return tr::lng_groups_log_own_tag_set(tr::now, lt_actor, actor, lt_tag, tag);
	case Form::OwnTagRemoved:
		return tr::lng_groups_log_own_tag_removed(
			tr::now,
			lt_actor,
			actor,
			lt_previous,
			previous);
	case Form::Generic:
		break;
	}
	return tr::lng_groups_log_generic(tr::now, lt_actor, actor);
}

[[nodiscard]] QString CategoryLabel(int category) {
	switch (category) {
	case Log::Members: return tr::lng_groups_log_kind_members(tr::now);
	case Log::Admins: return tr::lng_groups_log_kind_admins(tr::now);
	case Log::Settings: return tr::lng_groups_log_kind_settings(tr::now);
	case Log::Messages: return tr::lng_groups_log_kind_messages(tr::now);
	case Log::Tags: return tr::lng_groups_log_kind_tags(tr::now);
	}
	return tr::lng_groups_log_kind_links(tr::now);
}

// One entry: who did it (their picture), what, when.
class LogRow final : public Ui::RpWidget {
public:
	LogRow(
		QWidget *parent,
		Controller *controller,
		const Data::GroupAdminLogEntry &entry)
	: RpWidget(parent)
	, _time(DateTimeText(entry.at)) {
		// The sentence is in the language of the interface: its direction
		// too, whatever script a name or a tag in it starts with.
		const auto mark = style::RightToLeft() ? QChar(0x200F) : QChar(0x200E);
		_text.setText(
			st::groupsLogTextStyle,
			mark + EntryText(controller, entry));
		_userpic = Ui::CreateChild<Support::UserpicButton>(
			this,
			&controller->session(),
			entry.actor,
			st::groupsLogUserpic);
		const auto actor = entry.actor;
		if (!actor.isEmpty()) {
			_userpic->setClickedCallback([=] {
				controller->openRoute(Window::Routes::User(actor));
			});
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::groupsLogPadding;
		_userpic->moveToLeft(padding.left(), padding.top(), newWidth);
		const auto available = textWidth(newWidth);
		const auto textHeight = _text.countHeight(available);
		const auto content = textHeight
			+ st::groupsLogTimeSkip
			+ st::groupsLogTimeFont->height;
		return padding.top()
			+ std::max(content, int(st::groupsLogUserpic))
			+ padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::groupsLogPadding;
		const auto left = padding.left()
			+ st::groupsLogUserpic
			+ st::groupsLogUserpicSkip;
		const auto available = textWidth(width());
		const auto textHeight = _text.countHeight(available);
		auto context = Ui::Text::PaintContext();
		context.position = style::rtlrect(
			left,
			padding.top(),
			available,
			textHeight,
			width()).topLeft();
		context.availableWidth = available;
		p.setPen(st::textPrimary);
		_text.draw(p, context);
		p.setFont(st::groupsLogTimeFont);
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			left,
			padding.top() + textHeight + st::groupsLogTimeSkip,
			width(),
			_time);
	}

private:
	[[nodiscard]] int textWidth(int outer) const {
		const auto &padding = st::groupsLogPadding;
		return std::max(
			outer
				- padding.left()
				- st::groupsLogUserpic
				- st::groupsLogUserpicSkip
				- padding.right(),
			1);
	}

	Ui::Text::String _text;
	QString _time;
	Support::UserpicButton *_userpic = nullptr;

};

// «در حال بارگذاری…», «نمایش بیشتر», what went wrong (with «تلاش دوباره»),
// or that there is nothing.
class StateRow final : public Ui::RpWidget {
public:
	explicit StateRow(QWidget *parent) : RpWidget(parent) {
	}

	void setState(const QString &text, bool link, Fn<void()> clicked) {
		_text = text;
		_link = link;
		_clicked = std::move(clicked);
		setCursor(_clicked ? style::cur_pointer : style::cur_default);
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::groupsLogStateHeight;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.setFont(st::groupsLogStateFont);
		p.setPen(_link ? st::brandText : st::textTertiary);
		p.drawText(rect(), Qt::AlignCenter, _text);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && _clicked && rect().contains(e->pos())) {
			const auto callback = _clicked;
			callback();
		}
	}

private:
	QString _text;
	bool _link = false;
	Fn<void()> _clicked;

};

class AdminLogWidget final : public Window::SectionWidget {
public:
	AdminLogWidget(
		QWidget *parent,
		Controller *controller,
		const QString &groupId)
	: SectionWidget(parent, controller)
	, _groupId(groupId)
	, _bar(new Ui::TopBar(this))
	, _scroll(new Ui::ScrollArea(this)) {
		setFocusPolicy(Qt::StrongFocus);
		_bar->setTitle(tr::lng_groups_log_title(tr::now));
		setTopBar(_bar);
		_content = new Ui::VerticalLayout();
		_scroll->setOwnedWidget(Ui::object_ptr<ColumnWrap>(_scroll, _content));
		connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
			if (!_cursor.isEmpty() && !_loading && !_failed) {
				load(false);
			}
		});
		_scroll->setNearEdgeThreshold(kLoadMoreThreshold);

		_kindsRow = _content->add(Ui::object_ptr<Ui::SettingsButton>(
			_content,
			tr::lng_groups_log_filter_actions(tr::now),
			st::groupsButton));
		_kindsRow->setIcon(&st::groupsIconLog);
		_kindsRow->setClickedCallback([this] { chooseKinds(); });
		_adminRow = _content->add(Ui::object_ptr<Ui::SettingsButton>(
			_content,
			tr::lng_groups_log_filter_admin(tr::now),
			st::groupsButton));
		_adminRow->setIcon(&st::groupsIconAdmins);
		_adminRow->setClickedCallback([this] { chooseAdmin(); });
		_content->add(Ui::object_ptr<Ui::FixedHeightWidget>(
			_content,
			st::groupsDividerHeight));
		_list = _content->add(Ui::object_ptr<Ui::VerticalLayout>(_content));
		_state = _content->add(Ui::object_ptr<StateRow>(_content));
		_content->add(Ui::object_ptr<Ui::FixedHeightWidget>(
			_content,
			st::groupsBottomSkip));
		connect(
			&controller->session().data().chats(),
			&Data::Chats::chatRemoved,
			this,
			[this](const QString &id) {
				if (id == _groupId) {
					close();
				}
			});
		refreshFilters();
		load(true);
	}

	QString sectionKey() const override {
		return Window::Routes::GroupAdminLog(_groupId);
	}
	void setInnerFocus() override {
		setFocus();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		const auto top = _bar->height();
		_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
	}

private:
	void close() {
		const auto weak = QPointer<AdminLogWidget>(this);
		const auto window = controller();
		Ui::PostponeCall(window, [=] {
			if (weak && window->mainSection() == weak.data()) {
				if (!window->showBack()) {
					window->clearMainStack();
				}
			}
		});
	}

	void refreshFilters() {
		const auto picked = Log::PickedCount(_kinds);
		_kindsRow->setValueText(picked
			? tr::lng_groups_log_kinds_picked(tr::now, lt_count, picked)
			: tr::lng_groups_log_filter_all(tr::now));
		_adminRow->setValueText(_admin.isEmpty()
			? tr::lng_groups_log_filter_all(tr::now)
			: Name(controller(), _admin));
	}

	void refreshState() {
		if (_failed) {
			_state->setState(
				_error.isEmpty() ? tr::lng_error_load(tr::now) : _error,
				true,
				[this] { load(_list->count() == 0); });
		} else if (_loading) {
			_state->setState(tr::lng_loading(tr::now), false, nullptr);
		} else if (!_cursor.isEmpty()) {
			_state->setState(
				tr::lng_groups_show_more(tr::now),
				true,
				[this] { load(false); });
		} else if (!_list->count()) {
			_state->setState(
				(Log::PickedCount(_kinds) || !_admin.isEmpty())
					? tr::lng_groups_log_empty_filter(tr::now)
					: tr::lng_groups_log_empty(tr::now),
				false,
				nullptr);
		} else {
			_state->setState(QString(), false, nullptr);
		}
	}

	void load(bool first) {
		if (first) {
			++_generation;
			_loading = false;
			_cursor = QString();
			_known.clear();
			_list->clear();
		}
		if (_loading) {
			return;
		}
		_loading = true;
		_failed = false;
		refreshState();
		const auto generation = _generation;
		Store(controller()).loadAdminLog(
			_groupId,
			_admin,
			Log::ActionsParam(_kinds),
			first ? QString() : _cursor,
			this,
			[=, this](const Data::GroupAdminLogPage &page) {
				if (generation != _generation) {
					return;
				}
				_loading = false;
				_cursor = page.nextCursor;
				for (const auto &entry : page.entries) {
					if (_known.contains(entry.id) && !entry.id.isEmpty()) {
						continue;
					}
					_known.insert(entry.id);
					_list->add(Ui::object_ptr<LogRow>(_list, controller(), entry));
				}
				refreshState();
			},
			[=, this](const Api::Error &error) {
				if (generation != _generation) {
					return;
				}
				_loading = false;
				_failed = true;
				_error = Support::ErrorText(error, tr::lng_error_load(tr::now));
				refreshState();
			});
	}

	void chooseKinds() {
		const auto window = controller();
		const auto weak = QPointer<AdminLogWidget>(this);
		const auto was = _kinds;
		// A server without member tags has no «تغییر تگ‌ها» to filter by.
		const auto tags = Store(window).memberTagsIn(_groupId);
		window->show(Ui::Box([=](Ui::GenericBox *box) {
			box->setTitle(tr::lng_groups_log_filter_actions(tr::now));
			auto checks = std::vector<std::pair<int, Ui::Checkbox*>>();
			for (auto i = 0; i != Log::kCategoryCount; ++i) {
				if (i == Log::Tags && !tags) {
					continue;
				}
				// Nothing picked means every kind: show them all ticked.
				const auto on = !Log::PickedCount(was) || (was & (1 << i));
				checks.emplace_back(i, box->addRow(
					Ui::object_ptr<Ui::Checkbox>(box, CategoryLabel(i), on),
					st::boxCheckboxPadding));
			}
			box->addButton(tr::lng_groups_save(tr::now), [=] {
				auto mask = 0;
				for (const auto &[category, check] : checks) {
					if (check->checked()) {
						mask |= (1 << category);
					}
				}
				box->closeBox();
				if (weak && mask != weak->_kinds) {
					weak->_kinds = mask;
					weak->refreshFilters();
					weak->load(true);
				}
			}, st::defaultRoundButton);
			box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		}));
	}

	void chooseAdmin() {
		const auto window = controller();
		const auto weak = QPointer<AdminLogWidget>(this);
		Store(window).loadMembers(
			_groupId,
			u"admins"_s,
			QString(),
			this,
			[=](const Data::GroupMembersPage &page) {
				if (!weak) {
					return;
				}
				auto ids = QStringList{ QString() };
				for (const auto &member : page.members) {
					ids.push_back(member.user);
				}
				const auto current = std::max(int(ids.indexOf(weak->_admin)), 0);
				window->show(Ui::Box([=](Ui::GenericBox *box) {
					box->setTitle(tr::lng_groups_log_filter_admin(tr::now));
					const auto group = std::make_shared<Ui::RadiobuttonGroup>(current);
					for (auto i = 0; i != int(ids.size()); ++i) {
						box->addRow(
							Ui::object_ptr<Ui::Radiobutton>(
								box,
								group,
								i,
								ids[i].isEmpty()
									? tr::lng_groups_log_filter_all(tr::now)
									: Name(window, ids[i])),
							st::boxCheckboxPadding);
					}
					group->setChangedCallback([=](int value) {
						const auto chosen = ids.value(value);
						box->closeBox();
						if (weak && chosen != weak->_admin) {
							weak->_admin = chosen;
							weak->refreshFilters();
							weak->load(true);
						}
					});
					box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
				}));
			},
			[=](const Api::Error &error) {
				Support::ShowError(window, error);
			});
	}

	QString _groupId;
	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	Ui::SettingsButton *_kindsRow = nullptr;
	Ui::SettingsButton *_adminRow = nullptr;
	Ui::VerticalLayout *_list = nullptr;
	StateRow *_state = nullptr;
	QSet<QString> _known;
	QString _cursor;
	QString _admin;
	QString _error;
	int _kinds = 0;
	int _generation = 0;
	bool _loading = false;
	bool _failed = false;

};

// ---- shared media

enum class MediaTab {
	Media,
	Files,
	Links,
	Voice,
};
constexpr auto kMediaTabs = 4;

[[nodiscard]] QStringList TabKinds(MediaTab tab) {
	switch (tab) {
	case MediaTab::Media: return { u"photo"_s, u"video"_s, u"gif"_s };
	case MediaTab::Files: return { u"file"_s };
	case MediaTab::Links: return { u"link"_s };
	case MediaTab::Voice: return { u"voice"_s, u"video_note"_s, u"audio"_s };
	}
	return {};
}

[[nodiscard]] QString TabEmpty(MediaTab tab) {
	switch (tab) {
	case MediaTab::Media: return tr::lng_groups_media_empty_media(tr::now);
	case MediaTab::Files: return tr::lng_groups_media_empty_files(tr::now);
	case MediaTab::Links: return tr::lng_groups_media_empty_links(tr::now);
	case MediaTab::Voice: return tr::lng_groups_media_empty_voice(tr::now);
	}
	return QString();
}

// The address a message of the «links» tab leads to.
[[nodiscard]] QString MessageLink(const Data::Message &message) {
	if (message.linkPreview && !message.linkPreview->url.isEmpty()) {
		return message.linkPreview->url;
	}
	for (const auto &entity : message.entities) {
		if (entity.type == u"text_link" && !entity.url.isEmpty()) {
			return entity.url;
		}
	}
	static const auto kUrl = QRegularExpression(
		u"https?://[^\\s<>\"]+"_s,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = kUrl.match(message.text);
	return match.hasMatch() ? match.captured(0) : QString();
}

class MediaList final : public Ui::RpWidget {
public:
	MediaList(QWidget *parent, Controller *controller, const QString &groupId)
	: RpWidget(parent)
	, _controller(controller)
	, _groupId(groupId) {
		setMouseTracking(true);
		connect(
			&controller->session().data().users(),
			&Data::Users::changed,
			this,
			[this] { update(); });
		style::OnPaletteChanged(this, [this] { update(); });
	}

	void setTab(MediaTab tab) {
		_tab = tab;
		_over = -1;
		auto &state = this->state();
		if (!state.loaded && !state.loading) {
			load(false);
		}
		changed();
	}

	void setVisibleRange(int top, int bottom) {
		Q_UNUSED(top);
		_visibleBottom = bottom;
		maybeLoadMore();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &state = this->state();
		const auto &padding = st::groupsMediaPadding;
		auto result = padding.top();
		if (_tab == MediaTab::Media) {
			const auto columns = this->columns(newWidth);
			const auto rows = (int(state.items.size()) + columns - 1) / columns;
			const auto tile = tileSize(newWidth);
			result += rows * tile + std::max(rows - 1, 0) * st::groupsMediaTileSkip;
		} else {
			result += int(state.items.size()) * st::groupsMediaRowHeight;
		}
		if (hasStateRow()) {
			result += state.items.isEmpty()
				? int(st::groupsMediaStateHeight)
				: int(st::groupsLogStateHeight);
		}
		return result + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto clip = e->rect();
		const auto &state = this->state();
		for (auto i = 0; i != int(state.items.size()); ++i) {
			const auto rect = itemRect(i);
			if (rect.intersects(clip)) {
				if (_tab == MediaTab::Media) {
					paintTile(p, i, rect);
				} else {
					paintRow(p, i, rect);
				}
			}
		}
		if (hasStateRow()) {
			const auto rect = stateRect();
			p.setFont(st::groupsLogStateFont);
			p.setPen(stateIsLink() ? st::brandText : st::textTertiary);
			p.drawText(rect, Qt::AlignCenter, stateText());
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto index = itemAt(e->pos());
		const auto onState = hasStateRow()
			&& stateIsLink()
			&& stateRect().contains(e->pos());
		setCursor((index >= 0 || onState) ? style::cur_pointer : style::cur_default);
		if (_over != index) {
			_over = index;
			update();
		}
	}
	void leaveEvent(QEvent *e) override {
		Q_UNUSED(e);
		if (_over >= 0) {
			_over = -1;
			update();
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = itemAt(e->pos());
		if (e->button() == Qt::RightButton && index >= 0) {
			showMenu(index, e->globalPosition().toPoint());
		} else if (e->button() != Qt::LeftButton) {
			return;
		} else if (index >= 0) {
			open(index);
		} else if (hasStateRow()
			&& stateIsLink()
			&& stateRect().contains(e->pos())) {
			load(false);
		}
	}

private:
	struct State {
		QVector<Data::Message> items;
		std::vector<std::shared_ptr<Ui::ImageSource>> images;
		bool loading = false;
		bool loaded = false;
		bool failed = false;
		bool more = false;
		int generation = 0;
	};

	[[nodiscard]] State &state() {
		return _states[int(_tab)];
	}
	[[nodiscard]] const State &state() const {
		return _states[int(_tab)];
	}
	[[nodiscard]] int columns(int outer) const {
		const auto &padding = st::groupsMediaPadding;
		const auto inner = std::max(outer - padding.left() - padding.right(), 1);
		const auto skip = int(st::groupsMediaTileSkip);
		return std::max((inner + skip) / (int(st::groupsMediaTileMin) + skip), 3);
	}
	[[nodiscard]] int tileSize(int outer) const {
		const auto &padding = st::groupsMediaPadding;
		const auto inner = std::max(outer - padding.left() - padding.right(), 1);
		const auto count = columns(outer);
		const auto skip = int(st::groupsMediaTileSkip);
		return std::max((inner - (count - 1) * skip) / count, 1);
	}
	[[nodiscard]] QRect itemRect(int index) const {
		const auto &padding = st::groupsMediaPadding;
		const auto outer = width();
		if (_tab == MediaTab::Media) {
			const auto count = columns(outer);
			const auto tile = tileSize(outer);
			const auto skip = int(st::groupsMediaTileSkip);
			const auto row = index / count;
			const auto column = index % count;
			return style::rtlrect(
				padding.left() + column * (tile + skip),
				padding.top() + row * (tile + skip),
				tile,
				tile,
				outer);
		}
		return QRect(
			padding.left(),
			padding.top() + index * st::groupsMediaRowHeight,
			std::max(outer - padding.left() - padding.right(), 1),
			st::groupsMediaRowHeight);
	}
	[[nodiscard]] int itemAt(QPoint position) const {
		const auto &state = this->state();
		for (auto i = 0; i != int(state.items.size()); ++i) {
			if (itemRect(i).contains(position)) {
				return i;
			}
		}
		return -1;
	}
	[[nodiscard]] bool hasStateRow() const {
		const auto &state = this->state();
		return state.loading
			|| state.failed
			|| state.more
			|| (state.loaded && state.items.isEmpty());
	}
	[[nodiscard]] QRect stateRect() const {
		const auto &state = this->state();
		const auto top = height()
			- st::groupsMediaPadding.bottom()
			- (state.items.isEmpty()
				? int(st::groupsMediaStateHeight)
				: int(st::groupsLogStateHeight));
		return QRect(0, top, width(), state.items.isEmpty()
			? int(st::groupsMediaStateHeight)
			: int(st::groupsLogStateHeight));
	}
	[[nodiscard]] bool stateIsLink() const {
		const auto &state = this->state();
		return !state.loading && (state.failed || state.more);
	}
	[[nodiscard]] QString stateText() const {
		const auto &state = this->state();
		return state.loading
			? tr::lng_loading(tr::now)
			: state.failed
			? tr::lng_error_load(tr::now) + u" · "_s + tr::lng_try_again(tr::now)
			: state.more
			? tr::lng_groups_show_more(tr::now)
			: TabEmpty(_tab);
	}

	void changed() {
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}

	void maybeLoadMore() {
		const auto &state = this->state();
		if (state.more
			&& !state.loading
			&& !state.failed
			&& _visibleBottom + kLoadMoreThreshold >= height()) {
			load(false);
		}
	}

	void load(bool reset) {
		const auto tab = _tab;
		auto &state = _states[int(tab)];
		if (reset) {
			state = State();
		}
		if (state.loading) {
			return;
		}
		state.loading = true;
		state.failed = false;
		const auto generation = ++state.generation;
		const auto before = state.items.isEmpty() ? 0 : state.items.back().seq;
		changed();
		Store(_controller).loadMedia(
			_groupId,
			TabKinds(tab),
			before,
			this,
			[=, this](const Data::GroupMediaPage &page) {
				auto &state = _states[int(tab)];
				if (state.generation != generation) {
					return;
				}
				state.loading = false;
				state.loaded = true;
				state.more = page.hasMoreBefore;
				for (const auto &message : page.messages) {
					if (!state.items.isEmpty() && message.seq >= state.items.back().seq) {
						continue; // a repeat (newest first)
					}
					state.items.push_back(message);
					state.images.push_back(nullptr);
				}
				if (page.messages.isEmpty()) {
					state.more = false;
				}
				if (tab == _tab) {
					changed();
					Ui::PostponeCall(this, [this] { maybeLoadMore(); });
				}
			},
			[=, this](const Api::Error &) {
				auto &state = _states[int(tab)];
				if (state.generation != generation) {
					return;
				}
				state.loading = false;
				state.failed = true;
				if (tab == _tab) {
					changed();
				}
			});
	}

	[[nodiscard]] std::shared_ptr<Ui::ImageSource> image(int index) {
		auto &state = this->state();
		auto &result = state.images[index];
		if (!result) {
			result = Support::MakeMessageImageSource(
				&_controller->session(),
				state.items[index]);
			if (result) {
				const auto raw = result.get();
				connect(raw, &Ui::ImageSource::updated, this, [this] {
					update();
				});
			}
		}
		return result;
	}

	void paintTile(Painter &p, int index, const QRect &rect) {
		const auto &message = state().items[index];
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		path.addRoundedRect(rect, st::groupsMediaTileRadius, st::groupsMediaTileRadius);
		p.save();
		p.setClipPath(path);
		p.fillRect(rect, st::skeletonBg);
		if (const auto source = image(index)) {
			const auto ratio = devicePixelRatioF();
			auto picture = source->request(rect.size() * ratio);
			if (picture.isNull()) {
				picture = source->preview();
			}
			if (!picture.isNull()) {
				// Cover: the middle of the picture fills the tile.
				const auto size = picture.size();
				const auto scale = std::max(
					rect.width() / double(std::max(size.width(), 1)),
					rect.height() / double(std::max(size.height(), 1)));
				const auto part = QSizeF(rect.width() / scale, rect.height() / scale);
				const auto from = QRectF(
					(size.width() - part.width()) / 2.,
					(size.height() - part.height()) / 2.,
					part.width(),
					part.height());
				p.drawImage(QRectF(rect), picture, from);
			}
		}
		if (index == _over) {
			p.fillRect(rect, st::stateHover);
		}
		p.restore();
		if (message.kind == Data::MessageKind::Video && message.media) {
			const auto text = Lang::Duration(int(message.media->durationMs / 1000));
			const auto &font = st::groupsMediaVideoFont;
			const auto &padding = st::groupsMediaVideoPadding;
			const auto badge = QRect(
				rect.x() + st::groupsMediaTileSkip,
				rect.y() + rect.height() - st::groupsMediaTileSkip
					- font->height - padding.top() - padding.bottom(),
				font->width(text) + padding.left() + padding.right(),
				font->height + padding.top() + padding.bottom());
			p.setPen(Qt::NoPen);
			p.setBrush(st::overlayScrim);
			p.drawRoundedRect(badge, badge.height() / 2., badge.height() / 2.);
			p.setFont(font);
			p.setPen(st::staticWhite);
			p.drawText(badge, Qt::AlignCenter, text);
		}
	}

	void paintRow(Painter &p, int index, const QRect &rect) {
		const auto &message = state().items[index];
		const auto outer = width();
		if (index == _over) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::stateHover);
			p.drawRoundedRect(rect, st::groupsMediaRowRadius, st::groupsMediaRowRadius);
		}
		const auto size = int(st::groupsMediaRowIcon);
		const auto circle = style::rtlrect(
			rect.x(),
			rect.y() + (rect.height() - size) / 2,
			size,
			size,
			outer);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::brandSoft);
			p.drawEllipse(circle);
		}
		const auto &icon = (_tab == MediaTab::Links)
			? st::groupsMediaIconLink
			: (_tab == MediaTab::Voice)
			? st::groupsMediaIconVoice
			: st::groupsMediaIconFile;
		icon.paintInCenter(p, circle);

		const auto left = rect.x() + size + st::groupsMediaRowIconSkip;
		const auto available = std::max(rect.x() + rect.width() - left, 1);
		const auto date = Lang::SolarDate(
			QDateTime::fromMSecsSinceEpoch(message.createdAt).date());
		auto title = QString();
		auto status = QStringList();
		switch (_tab) {
		case MediaTab::Files:
			title = message.text.trimmed();
			if (message.media && message.media->size > 0) {
				status.push_back(Lang::Bytes(message.media->size));
			}
			break;
		case MediaTab::Links: {
			const auto link = MessageLink(message);
			title = (message.linkPreview && !message.linkPreview->title.isEmpty())
				? message.linkPreview->title
				: link;
			if (!link.isEmpty() && link != title) {
				status.push_back(Lang::Ltr(link));
			}
		} break;
		case MediaTab::Voice:
			title = Support::MemberName(&_controller->session(), message.sender);
			if (message.media && message.media->durationMs > 0) {
				status.push_back(Lang::Duration(int(message.media->durationMs / 1000)));
			}
			break;
		case MediaTab::Media:
			break;
		}
		if (title.isEmpty()) {
			title = Support::MessagePreview(&_controller->session(), message);
		}
		status.push_back(date);
		p.setFont(st::groupsMediaRowTitleFont);
		p.setPen(st::textPrimary);
		p.drawTextLeft(
			left,
			rect.y() + st::groupsMediaRowTitleTop,
			outer,
			st::groupsMediaRowTitleFont->elided(title.simplified(), available));
		p.setFont(st::groupsMediaRowStatusFont);
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			left,
			rect.y() + st::groupsMediaRowStatusTop,
			outer,
			st::groupsMediaRowStatusFont->elided(status.join(u" · "_s), available));
	}

	void showInChat(qint64 seq) {
		_controller->openRoute(Window::Routes::Chat(_groupId, seq));
	}

	void showMenu(int index, QPoint globalPosition) {
		const auto message = state().items[index];
		const auto seq = message.seq;
		const auto menu = new Ui::PopupMenu(this);
		menu->addAction(tr::lng_groups_media_show_in_chat(tr::now), [=, this] {
			showInChat(seq);
		}, &st::groupsMenuGroup);
		if (_tab == MediaTab::Links) {
			const auto link = MessageLink(message);
			if (!link.isEmpty()) {
				const auto controller = _controller;
				menu->addAction(tr::lng_copy_link(tr::now), [=] {
					QGuiApplication::clipboard()->setText(link);
					controller->showToast(tr::lng_link_copied(tr::now));
				}, &st::menuIconLink);
			}
		}
		menu->popup(globalPosition);
	}

	void open(int index) {
		const auto &state = this->state();
		const auto &message = state.items[index];
		if (_tab == MediaTab::Links) {
			const auto link = MessageLink(message);
			if (!link.isEmpty()) {
				Support::OpenLink(_controller, link);
				return;
			}
		} else if (_tab == MediaTab::Media
			&& message.kind == Data::MessageKind::Photo
			&& !message.mediaSpoiler) {
			openPhotos(index);
			return;
		}
		showInChat(message.seq);
	}

	void openPhotos(int clicked) {
		const auto &state = this->state();
		auto items = std::vector<Media::ViewerItem>();
		auto index = 0;
		for (auto i = 0; i != int(state.items.size()); ++i) {
			const auto &message = state.items[i];
			if (message.kind != Data::MessageKind::Photo || message.mediaSpoiler) {
				continue;
			}
			auto options = Support::ImageOptions();
			options.original = true;
			if (message.media) {
				options.blurhash = message.media->blurhash;
				options.aspect = QSize(message.media->width, message.media->height);
			}
			auto item = Media::ViewerItem();
			item.source = Support::MakeImageSource(
				&_controller->session(),
				Data::MediaRef::Attachment(message.chat, message.seq),
				Data::MediaVariant::Full,
				options);
			item.preview = state.images[i];
			item.caption = message.text;
			if (i == clicked) {
				index = int(items.size());
			}
			items.push_back(std::move(item));
		}
		if (!items.empty()) {
			Media::ShowImageViewer(_controller->window(), std::move(items), index);
		}
	}

	Controller *_controller = nullptr;
	QString _groupId;
	MediaTab _tab = MediaTab::Media;
	std::array<State, kMediaTabs> _states;
	int _visibleBottom = 0;
	int _over = -1;

};

// Passes the visible range of the scroll area on to the list.
class MediaWrap final : public Ui::RpWidget {
public:
	MediaWrap(QWidget *parent, MediaList *list)
	: RpWidget(parent)
	, _list(list) {
		_list->setParent(this);
		connect(_list, &Ui::RpWidget::heightChanged, this, [this] {
			if (!_inResize) {
				resizeToWidth(width());
			}
		});
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_inResize = true;
		const auto inner = std::min(newWidth, int(st::groupsColumnWidth));
		_list->resizeToWidth(inner);
		_list->move((newWidth - inner) / 2, 0);
		_inResize = false;
		return _list->height();
	}
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		_list->setVisibleRange(visibleTop, visibleBottom);
	}

private:
	MediaList *_list = nullptr;
	bool _inResize = false;

};

class MediaWidget final : public Window::SectionWidget {
public:
	MediaWidget(
		QWidget *parent,
		Controller *controller,
		const QString &groupId)
	: SectionWidget(parent, controller)
	, _groupId(groupId)
	, _bar(new Ui::TopBar(this))
	, _tabs(new Ui::TabBar(this))
	, _scroll(new Ui::ScrollArea(this)) {
		setFocusPolicy(Qt::StrongFocus);
		_bar->setTitle(tr::lng_groups_media_title(tr::now));
		setTopBar(_bar);
		_tabs->setTabs({
			tr::lng_groups_media_tab_media(tr::now),
			tr::lng_groups_media_tab_files(tr::now),
			tr::lng_groups_media_tab_links(tr::now),
			tr::lng_groups_media_tab_voice(tr::now),
		});
		_tabs->setStretch(true);
		_list = new MediaList(nullptr, controller, groupId);
		_scroll->setOwnedWidget(Ui::object_ptr<MediaWrap>(_scroll, _list));
		connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
			_list->setTab(MediaTab(std::clamp(index, 0, kMediaTabs - 1)));
			_scroll->scrollToY(0);
		});
		connect(
			&controller->session().data().chats(),
			&Data::Chats::chatRemoved,
			this,
			[this](const QString &id) {
				if (id == _groupId) {
					close();
				}
			});
		_list->setTab(MediaTab::Media);
	}

	QString sectionKey() const override {
		return Window::Routes::GroupMedia(_groupId);
	}
	void setInnerFocus() override {
		setFocus();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		const auto &padding = st::groupsMediaTabsPadding;
		const auto tabsWidth = std::min(
			width() - padding.left() - padding.right(),
			int(st::groupsColumnWidth));
		_tabs->resizeToWidth(std::max(tabsWidth, 1));
		_tabs->move((width() - _tabs->width()) / 2, _bar->height() + padding.top());
		const auto top = _tabs->y() + _tabs->height() + padding.bottom();
		_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
	}

private:
	void close() {
		const auto weak = QPointer<MediaWidget>(this);
		const auto window = controller();
		Ui::PostponeCall(window, [=] {
			if (weak && window->mainSection() == weak.data()) {
				if (!window->showBack()) {
					window->clearMainStack();
				}
			}
		});
	}

	QString _groupId;
	Ui::TopBar *_bar = nullptr;
	Ui::TabBar *_tabs = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	MediaList *_list = nullptr;

};

} // namespace

QString SlowModeLabel(int seconds) {
	if (seconds <= 0) {
		return tr::lng_groups_slow_off(tr::now);
	} else if (seconds < 60 || (seconds < 3600 && seconds % 60)) {
		return tr::lng_groups_time_seconds(tr::now, lt_count, seconds);
	} else if (seconds < 3600 || (seconds < 86400 && seconds % 3600)) {
		return tr::lng_groups_time_minutes(tr::now, lt_count, seconds / 60);
	} else if (seconds < 86400 || seconds % 86400) {
		return tr::lng_groups_time_hours(tr::now, lt_count, seconds / 3600);
	}
	return tr::lng_groups_time_days(tr::now, lt_count, seconds / 86400);
}

void ShowSlowModeBox(Controller *controller, const QString &groupId) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	auto steps = Store(controller).slowModeSteps();
	const auto was = header->slowModeS;
	if (!steps.contains(was)) {
		steps.push_back(was);
		std::sort(steps.begin(), steps.end());
	}
	const auto current = std::max(int(steps.indexOf(was)), 0);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_groups_slow_mode(tr::now));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(current);
		for (auto i = 0; i != int(steps.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(
					box,
					group,
					i,
					SlowModeLabel(steps[i])),
				st::boxCheckboxPadding);
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_slow_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_save(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			const auto seconds = steps.value(group->value(), 0);
			if (*saving) {
				return;
			} else if (seconds == was) {
				box->closeBox();
				return;
			}
			*saving = true;
			submit->setLoading(true);
			Store(controller).setSlowMode(
				groupId,
				seconds,
				base::Guard(box),
				[=](const Data::Group &) {
					box->closeBox();
					controller->showToast(tr::lng_groups_slow_saved(tr::now));
				},
				[=](const Api::Error &error) {
					*saving = false;
					submit->setLoading(false);
					Support::ShowError(controller, error);
				});
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowHideMembersBox(Controller *controller, const QString &groupId) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	const auto was = header->hideMembers;
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_groups_hide_members(tr::now));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(was ? 1 : 0);
		box->addRow(
			Ui::object_ptr<Ui::Radiobutton>(
				box,
				group,
				0,
				tr::lng_groups_hide_choice_off(tr::now)),
			st::boxCheckboxPadding);
		box->addRow(
			Ui::object_ptr<Ui::Radiobutton>(
				box,
				group,
				1,
				tr::lng_groups_hide_choice_on(tr::now)),
			st::boxCheckboxPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_hide_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_save(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			const auto hidden = (group->value() == 1);
			if (*saving) {
				return;
			} else if (hidden == was) {
				box->closeBox();
				return;
			}
			*saving = true;
			submit->setLoading(true);
			Store(controller).setHideMembers(
				groupId,
				hidden,
				base::Guard(box),
				[=](const Data::Group &) {
					box->closeBox();
				},
				[=](const Api::Error &error) {
					*saving = false;
					submit->setLoading(false);
					Support::ShowError(controller, error);
				});
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowReactionsBox(
		Controller *controller,
		const QString &groupId,
		qint64 seq) {
	const auto header = Header(controller, groupId);
	if (header && header->membersHidden) {
		controller->showToast(tr::lng_groups_reactions_hidden(tr::now));
		return;
	}
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<ReactionsController>(controller, groupId, seq)));
}

void ShowInviteMembersBox(
		Controller *controller,
		const QString &groupId,
		const QString &link,
		const QString &title) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<InviteMembersController>(
			controller,
			groupId,
			link,
			title.isEmpty() ? tr::lng_groups_link_joined(tr::now) : title)));
}

Window::SectionFactory AdminLogFactory(const QString &groupId) {
	return [=](QWidget *parent, Controller *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new AdminLogWidget(parent, controller, groupId));
	};
}

Window::SectionFactory MediaFactory(const QString &groupId) {
	return [=](QWidget *parent, Controller *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new MediaWidget(parent, controller, groupId));
	};
}

} // namespace Groups
