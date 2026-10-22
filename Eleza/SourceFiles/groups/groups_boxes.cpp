#include "groups/groups_boxes.h"

#include "groups/groups_admin.h"
#include "groups/groups_tags.h"

#include "api/api_auth.h"
#include "boxes/peer_list_box.h"
#include "boxes/photo_crop_box.h"
#include "boxes/report_box.h"
#include "data/data_chats.h"
#include "data/data_graph.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_upload.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/image/userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_groups.h"

#include <QtCore/QDateTime>
#include <QtCore/QSet>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>

namespace Groups {
namespace {

namespace Rights = Data::GroupRights;

constexpr auto kRecentLimit = 20;
constexpr auto kHour = qint64(3600);
constexpr auto kDay = 24 * kHour;
constexpr auto kWeek = 7 * kDay;

[[nodiscard]] Data::Groups &Store(Window::SessionController *controller) {
	return controller->session().data().groups();
}

[[nodiscard]] const Data::Group *Header(
		Window::SessionController *controller,
		const QString &groupId) {
	return Store(controller).group(groupId);
}

[[nodiscard]] QString Title(
		Window::SessionController *controller,
		const QString &groupId) {
	const auto header = Header(controller, groupId);
	return (header && !header->title.isEmpty())
		? header->title
		: tr::lng_groups_group(tr::now);
}

[[nodiscard]] QString Name(
		Window::SessionController *controller,
		const QString &userId) {
	return Support::MemberName(&controller->session(), userId);
}

// Me, as the rights table sees me.
[[nodiscard]] Rights::Member Me(
		Window::SessionController *controller,
		const Data::Group &header) {
	return Rights::Member::Of(
		controller->session().userId(),
		header.role,
		Rights::RightsMask(header.rights));
}

// The names a box lists, in their order: the ones of member tags only for a
// server that has them (an older one refuses a name it does not know).
[[nodiscard]] QStringList ShownNames(
		Window::SessionController *controller,
		const QString &groupId,
		const QStringList &names) {
	if (Store(controller).memberTagsIn(groupId)) {
		return names;
	}
	auto result = names;
	result.removeAll(u"manage_tags"_s);
	result.removeAll(u"edit_tag"_s);
	return result;
}

// What a member is known as: the store's latest, else the row's own.
[[nodiscard]] TagSender TagOf(
		Window::SessionController *controller,
		const QString &groupId,
		const Data::GroupMember &member) {
	if (const auto known = Store(controller).senderTag(groupId, member.user)) {
		return *known;
	}
	return Data::MemberTags::FromMember(
		member.status,
		member.role,
		member.tag,
		member.adminTitle);
}

[[nodiscard]] Rights::Member Target(const Data::GroupMember &member) {
	auto result = Rights::Member();
	result.user = member.user;
	result.status = member.status.isEmpty() ? u"member"_s : member.status;
	result.role = member.role.isEmpty() ? u"member"_s : member.role;
	result.granted = Rights::RightsMask(member.rights);
	result.denied = Rights::PermissionsMask(member.denied);
	result.restricted = member.restricted();
	return result;
}

// The refusals that have words of their own; anything else is worded by the
// server (the problem's title comes in the client's language).
void ShowGroupError(
		Window::SessionController *controller,
		const Api::Error &error) {
	const auto &code = error.code;
	if (code == u"group.owner_must_transfer") {
		controller->showToast(tr::lng_groups_owner_must_transfer(tr::now));
	} else if (code == u"group.full") {
		controller->showToast(tr::lng_groups_full(tr::now));
	} else if (code == u"group.too_many_admins") {
		controller->showToast(tr::lng_groups_admins_full(tr::now));
	} else if (code == u"group.invite_invalid" || code == u"group.invite_expired") {
		controller->showToast(tr::lng_groups_invite_invalid(tr::now));
	} else if (code == u"group.join_requests_full") {
		controller->showToast(tr::lng_groups_invite_later(tr::now));
	} else if (code == u"group.banned") {
		controller->showToast(tr::lng_groups_invite_banned(tr::now));
	} else if (code == u"group.disabled") {
		controller->showToast(tr::lng_groups_not_available(tr::now));
	} else {
		Support::ShowError(controller, error);
	}
}

[[nodiscard]] Data::Groups::Fail FailHandler(
		Window::SessionController *controller,
		Fn<void()> also = nullptr) {
	return [=](const Api::Error &error) {
		if (also) {
			also();
		}
		ShowGroupError(controller, error);
	};
}

[[nodiscard]] QString NotAddedReason(const QString &reason) {
	if (reason == u"privacy") {
		return tr::lng_groups_not_added_privacy(tr::now);
	} else if (reason == u"left") {
		return tr::lng_groups_not_added_left(tr::now);
	} else if (reason == u"limit") {
		return tr::lng_groups_not_added_limit(tr::now);
	} else if (reason == u"unavailable") {
		return tr::lng_groups_not_added_unavailable(tr::now);
	} else if (reason == u"banned") {
		return tr::lng_groups_not_added_banned(tr::now);
	}
	return tr::lng_groups_not_added_other(tr::now);
}

// «۲ نفر اضافه نشدند»: who and why.
void ShowNotAdded(
		Window::SessionController *controller,
		const QVector<Data::GroupNotAdded> &list) {
	if (list.isEmpty()) {
		return;
	}
	auto lines = QStringList();
	for (const auto &entry : list) {
		lines.push_back(tr::lng_groups_not_added_line(
			tr::now,
			lt_name,
			Name(controller, entry.user),
			lt_reason,
			NotAddedReason(entry.reason)));
	}
	controller->show(Ui::MakeInformBox(
		lines.join(u'\n'),
		tr::lng_groups_not_added_title(tr::now, lt_count, int(list.size()))));
}

// ---- picking people (a new group, adding members)

class PickPeopleController final : public Boxes::PeerListController {
public:
	PickPeopleController(
		Window::SessionController *controller,
		QString title,
		QString submitText,
		int limit,
		bool allowEmpty,
		Fn<void(const QStringList &ids, Ui::BoxContent *box)> done)
	: _controller(controller)
	, _title(std::move(title))
	, _submitText(std::move(submitText))
	, _limit(limit)
	, _allowEmpty(allowEmpty)
	, _done(std::move(done)) {
	}

	QString title() const override {
		return _title;
	}
	QString searchPlaceholder() const override {
		return tr::lng_groups_pick_search(tr::now);
	}
	QString emptyText() const override {
		return tr::lng_nothing_found(tr::now);
	}
	bool searchRemotely() const override {
		return true;
	}
	bool multiSelect() const override {
		return true;
	}
	int selectionLimit() const override {
		return _limit;
	}
	QString submitText() const override {
		return _submitText;
	}
	bool submitWithoutSelection() const override {
		return _allowEmpty;
	}

	void prepare() override {
		const auto session = this->session();
		// The people of the recent chats…
		auto count = 0;
		for (const auto &chat : session->data().chats().list(
				Data::Chats::Section::Messages)) {
			if (chat.saved
				|| chat.isGroup
				|| chat.isBot
				|| chat.anonymous
				|| chat.peerDeleted
				|| chat.peerUserId.isEmpty()) {
				continue;
			}
			auto user = session->data().users().get(chat.peerUserId);
			if (!user.valid()) {
				user.id = chat.peerUserId;
				user.displayName = chat.title;
				user.verified = chat.verified;
			}
			add(user, chat.title);
			if (++count >= kRecentLimit) {
				break;
			}
		}
		delegate()->setRows(_base);

		// …then the ones I follow.
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().graph().following(
			session->userId(),
			QString(),
			guard,
			[this](const Data::GraphList &list) {
				for (const auto &profile : list.users) {
					if (!profile.deleted && !profile.me) {
						add(profile, profile.displayName);
					}
				}
				if (delegate()->searchQuery().trimmed().isEmpty()) {
					showBase();
				}
			},
			[](const Api::Error &) {
			});
	}

	void searchQueryChanged(const QString &query) override {
		const auto request = ++_request;
		auto text = query.trimmed();
		while (text.startsWith(QLatin1Char('@'))) {
			text.remove(0, 1);
		}
		if (text.isEmpty()) {
			showBase();
			return;
		}
		delegate()->setLoading(true);
		const auto session = this->session();
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().graph().searchPeople(text, guard, [=, this](
				const QVector<Data::Profile> &people) {
			if (request != _request) {
				return;
			}
			auto rows = QVector<Boxes::PeerListRow>();
			for (const auto &profile : people) {
				if (!profile.deleted && profile.id != session->userId()) {
					rows.push_back(Boxes::PeerListRow::FromUser(profile));
					// A person found by search stays pickable after the
					// query is cleared.
					add(profile, profile.displayName);
				}
			}
			setShown(rows);
		});
	}

	void rowClicked(const Boxes::PeerListRow &row) override {
		Q_UNUSED(row);
	}

	void submit(const QStringList &ids) override {
		if (_done) {
			_done(ids, delegate()->peerListBox());
		}
	}

private:
	void add(const Data::User &user, const QString &fallbackName) {
		if (!user.valid() || _known.contains(user.id)) {
			return;
		}
		_known.insert(user.id);
		auto row = Boxes::PeerListRow::FromUser(user);
		if (row.title.isEmpty()) {
			row.title = fallbackName;
		}
		_base.push_back(row);
	}
	void showBase() {
		setShown(_base);
	}
	// Replacing the rows keeps what was ticked.
	void setShown(const QVector<Boxes::PeerListRow> &rows) {
		const auto selected = delegate()->selectedIds();
		delegate()->setRows(rows);
		for (const auto &id : selected) {
			delegate()->setSelected(id, true);
		}
	}

	Window::SessionController *_controller = nullptr;
	QString _title;
	QString _submitText;
	int _limit = 0;
	bool _allowEmpty = false;
	Fn<void(const QStringList &ids, Ui::BoxContent *box)> _done;
	QVector<Boxes::PeerListRow> _base;
	QSet<QString> _known;
	int _request = 0;

};

void OpenGroupChat(
		Window::SessionController *controller,
		const QString &groupId) {
	controller->openRoute(Window::Routes::Chat(groupId));
}

// The second step of «گروه تازه»: the name (and what the group is about).
void ShowNameBox(
		Window::SessionController *controller,
		const QStringList &members) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		auto &store = Store(controller);
		const auto titleMax = store.titleMaxChars();
		box->setTitle(tr::lng_groups_new_group(tr::now));
		const auto name = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_groups_name_placeholder(tr::now)));
		// (Code points are counted by the rules; units are a loose bound.)
		name->setMaxLength(titleMax * 2);
		const auto about = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_groups_about_placeholder(tr::now)));
		about->setMaxLength(store.aboutMaxChars() * 2);
		about->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				members.isEmpty()
					? tr::lng_groups_create_alone(tr::now)
					: tr::lng_groups_create_with(
						tr::now,
						lt_count,
						int(members.size())),
				st::groupsBoxCaption),
			st::boxRowPadding);
		box->setFocusCallback([=] { name->setFocusFast(); });
		const auto creating = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_create(tr::now),
			nullptr,
			st::defaultRoundButton);
		const auto create = [=] {
			if (*creating) {
				return;
			}
			const auto title = Data::GroupRules::CleanTitle(name->text());
			if (!Data::GroupRules::TitleOk(title, titleMax)) {
				name->showError();
				controller->showToast(title.isEmpty()
					? tr::lng_groups_name_needed(tr::now)
					: tr::lng_groups_name_too_long(tr::now));
				return;
			}
			*creating = true;
			submit->setLoading(true);
			auto request = Data::GroupCreateRequest();
			request.title = title;
			request.about = about->text().trimmed();
			request.members = members;
			Store(controller).create(request, base::Guard(box), [=](
					const Data::Group &group,
					const QVector<Data::GroupNotAdded> &notAdded) {
				const auto id = group.id;
				box->closeBox();
				OpenGroupChat(controller, id);
				ShowNotAdded(controller, notAdded);
			}, FailHandler(controller, [=] {
				*creating = false;
				submit->setLoading(false);
			}));
		};
		submit->setClickedCallback(create);
		QObject::connect(name, &Ui::InputField::submitted, box, create);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

} // namespace

QString RightLabel(const QString &name) {
	if (name == u"change_info") {
		return tr::lng_groups_right_change_info(tr::now);
	} else if (name == u"delete_messages") {
		return tr::lng_groups_right_delete_messages(tr::now);
	} else if (name == u"ban_users") {
		return tr::lng_groups_right_ban_users(tr::now);
	} else if (name == u"invite_users") {
		return tr::lng_groups_right_invite_users(tr::now);
	} else if (name == u"pin_messages") {
		return tr::lng_groups_right_pin_messages(tr::now);
	} else if (name == u"add_admins") {
		return tr::lng_groups_right_add_admins(tr::now);
	} else if (name == u"manage_tags") {
		return tr::lng_groups_right_manage_tags(tr::now);
	}
	return name;
}

QString PermissionLabel(const QString &name) {
	if (name == u"send_messages") {
		return tr::lng_groups_perm_send_messages(tr::now);
	} else if (name == u"send_media") {
		return tr::lng_groups_perm_send_media(tr::now);
	} else if (name == u"send_links") {
		return tr::lng_groups_perm_send_links(tr::now);
	} else if (name == u"add_members") {
		return tr::lng_groups_perm_add_members(tr::now);
	} else if (name == u"pin_messages") {
		return tr::lng_groups_perm_pin_messages(tr::now);
	} else if (name == u"change_info") {
		return tr::lng_groups_perm_change_info(tr::now);
	} else if (name == u"edit_tag") {
		return tr::lng_groups_perm_edit_tag(tr::now);
	}
	return name;
}

QString MemberTag(const Data::GroupMember &member) {
	const auto tag = Data::MemberTags::Shown(member.currentTag());
	if (member.role == u"owner") {
		return tag.isEmpty() ? tr::lng_groups_role_owner(tr::now) : tag;
	} else if (member.role == u"admin") {
		return tag.isEmpty() ? tr::lng_groups_role_admin(tr::now) : tag;
	} else if (member.restricted()) {
		return tr::lng_groups_role_restricted(tr::now);
	}
	return QString();
}

void ShowNewGroup(Window::SessionController *controller) {
	auto &store = Store(controller);
	if (!store.enabled()) {
		controller->showToast(tr::lng_groups_not_available(tr::now));
		return;
	}
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<PickPeopleController>(
			controller,
			tr::lng_groups_pick_title(tr::now),
			tr::lng_groups_next(tr::now),
			store.maxInitialMembers(),
			true,
			[=](const QStringList &ids, Ui::BoxContent *box) {
				box->closeBox();
				ShowNameBox(controller, ids);
			})));
}

void ShowAddMembers(
		Window::SessionController *controller,
		const QString &groupId) {
	auto &store = Store(controller);
	const auto header = store.group(groupId);
	if (!header) {
		return;
	}
	const auto room = Data::GroupRules::AddRoom(
		header->memberCount,
		store.maxMembers(),
		store.addBatchMax());
	if (room <= 0) {
		controller->showToast(tr::lng_groups_full(tr::now));
		return;
	}
	const auto adding = std::make_shared<bool>(false);
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<PickPeopleController>(
			controller,
			tr::lng_groups_add_members(tr::now),
			tr::lng_groups_add(tr::now),
			room,
			false,
			[=](const QStringList &ids, Ui::BoxContent *box) {
				if (*adding || ids.isEmpty()) {
					return;
				}
				*adding = true;
				Store(controller).addMembers(groupId, ids, base::Guard(box), [=](
						const Data::GroupAddResult &result) {
					box->closeBox();
					if (!result.added.isEmpty()) {
						controller->showToast(tr::lng_groups_added(
							tr::now,
							lt_count,
							int(result.added.size())));
					}
					ShowNotAdded(controller, result.notAdded);
				}, FailHandler(controller, [=] { *adding = false; }));
			})));
}

// ---- an invite link

namespace {

void ShowInvite(
		Window::SessionController *controller,
		const QString &token,
		const Data::GroupInvitePreview &preview) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		const auto session = &controller->session();
		box->setTitle(tr::lng_groups_invite_title(tr::now));
		const auto title = preview.title.isEmpty()
			? tr::lng_groups_group(tr::now)
			: Data::Json::CleanName(preview.title.simplified());
		const auto group = Data::GroupInviteLinks::GroupOf(token);
		const auto userpic = box->addRow(
			Ui::object_ptr<Ui::UserpicWidget>(box, st::groupsInviteUserpic),
			st::groupsInvitePadding,
			style::al_top);
		userpic->setPlaceholder(group, title);
		if (!preview.photo.isEmpty()) {
			auto options = Support::ImageOptions();
			options.original = true;
			userpic->setSource(Support::MakeImageSource(
				session,
				Data::MediaRef::InvitePhoto(token, preview.photo),
				Data::MediaVariant::Feed,
				std::move(options)));
		}
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, title, st::groupsInviteTitle),
			st::boxRowPadding);
		if (preview.memberCount > 0) {
			box->addRow(
				Ui::object_ptr<Ui::FlatLabel>(
					box,
					tr::lng_groups_members(tr::now, lt_count, preview.memberCount),
					st::groupsInviteCount),
				st::boxRowPadding);
		}
		if (!preview.about.trimmed().isEmpty()) {
			const auto about = box->addRow(
				Ui::object_ptr<Ui::FlatLabel>(box, preview.about.trimmed(), st::boxLabel),
				st::boxRowPadding);
			about->setAutoDirection(true);
			about->setMaxLines(6);
		}
		const auto member = (preview.status == u"member");
		const auto pending = (preview.status == u"pending");
		const auto banned = (preview.status == u"banned");
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				member
					? tr::lng_groups_invite_member(tr::now)
					: pending
					? tr::lng_groups_invite_pending_note(tr::now)
					: banned
					? tr::lng_groups_invite_banned(tr::now)
					: preview.requiresApproval
					? tr::lng_groups_invite_approval_note(tr::now)
					: tr::lng_groups_invite_join_note(tr::now),
				st::groupsInviteNote),
			st::boxRowPadding);
		if (member) {
			const auto id = preview.group.isEmpty() ? group : preview.group;
			box->addButton(tr::lng_groups_invite_open(tr::now), [=] {
				box->closeBox();
				OpenGroupChat(controller, id);
			}, st::defaultRoundButton);
			box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
			return;
		} else if (pending || banned) {
			box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
			return;
		}
		const auto joining = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			preview.requiresApproval
				? tr::lng_groups_invite_request(tr::now)
				: tr::lng_groups_invite_join(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			if (*joining) {
				return;
			}
			*joining = true;
			submit->setLoading(true);
			Store(controller).joinByInvite(token, base::Guard(box), [=](
					const Data::GroupJoinResult &result) {
				const auto id = result.group ? result.group->id : QString();
				box->closeBox();
				if (!id.isEmpty()) {
					controller->showToast(tr::lng_groups_invite_joined(tr::now));
					OpenGroupChat(controller, id);
				} else {
					controller->showToast(tr::lng_groups_invite_pending(tr::now));
				}
			}, FailHandler(controller, [=] {
				*joining = false;
				submit->setLoading(false);
			}));
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

} // namespace

void ShowInvitePreview(
		Window::SessionController *controller,
		const QString &token) {
	auto &store = Store(controller);
	if (!store.enabled() || !store.administration()) {
		controller->showToast(tr::lng_groups_not_available(tr::now));
		return;
	} else if (!Data::GroupInviteLinks::IsToken(token)) {
		controller->showToast(tr::lng_groups_invite_invalid(tr::now));
		return;
	}
	store.invitePreview(token, base::Guard(controller), [=](
			const Data::GroupInvitePreview &preview) {
		ShowInvite(controller, token, preview);
	}, FailHandler(controller));
}

// ---- my own place

void ShowLeaveBox(
		Window::SessionController *controller,
		const QString &groupId) {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_groups_leave(tr::now);
	args.text = tr::lng_groups_leave_sure(
		tr::now,
		lt_name,
		Title(controller, groupId));
	args.confirmText = tr::lng_groups_leave(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		Store(controller).leave(groupId, base::Guard(controller), [=] {
			controller->showToast(tr::lng_groups_left_toast(tr::now));
		}, FailHandler(controller));
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowClearHistoryBox(
		Window::SessionController *controller,
		const QString &groupId) {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_groups_clear_history(tr::now);
	args.text = tr::lng_groups_clear_sure(
		tr::now,
		lt_name,
		Title(controller, groupId));
	args.confirmText = tr::lng_groups_clear_history(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		Store(controller).clearHistory(groupId);
		controller->showToast(tr::lng_groups_cleared_toast(tr::now));
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowNotifyBox(
		Window::SessionController *controller,
		const QString &groupId) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	static const auto kLevels = QStringList{ u"all"_s, u"mentions"_s, u"none"_s };
	const auto current = std::max(
		int(kLevels.indexOf(header->notify.isEmpty() ? u"all"_s : header->notify)),
		0);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_groups_notifications(tr::now));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(current);
		const auto labels = QStringList{
			tr::lng_groups_notify_all(tr::now),
			tr::lng_groups_notify_mentions(tr::now),
			tr::lng_groups_notify_none(tr::now),
		};
		for (auto i = 0; i != int(labels.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(box, group, i, labels[i]),
				st::boxCheckboxPadding);
		}
		group->setChangedCallback([=](int value) {
			Store(controller).setNotify(groupId, kLevels.value(value, u"all"_s));
			box->closeBox();
		});
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowEditBox(
		Window::SessionController *controller,
		const QString &groupId) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	const auto was = *header;
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		auto &store = Store(controller);
		const auto session = &controller->session();
		const auto titleMax = store.titleMaxChars();
		box->setTitle(tr::lng_groups_edit(tr::now));
		// nullopt: the photo is not touched; "": removed; else a new media id.
		const auto photo = std::make_shared<std::optional<QString>>();
		const auto uploading = std::make_shared<bool>(false);
		const auto userpic = box->addRow(
			Ui::object_ptr<Ui::UserpicWidget>(box, st::groupsInviteUserpic),
			st::groupsInvitePadding,
			style::al_top);
		userpic->setPlaceholder(groupId, was.title);
		userpic->setSource(Support::GroupPhotoSource(
			session,
			groupId,
			was.photo,
			Data::MediaVariant::Feed));
		const auto pick = [=] {
			Boxes::PickAndCropPhoto(box, [=](Ui::object_ptr<Ui::BoxContent> crop) {
				controller->show(std::move(crop));
			}, [=](const QImage &square) {
				userpic->setSource(nullptr);
				userpic->setImage(square);
				*uploading = true;
				Boxes::SaveCroppedPhoto(square, box, [=](const QString &path) {
					if (path.isEmpty()) {
						*uploading = false;
						return;
					}
					auto request = Data::UploadRequest();
					request.filePath = path;
					request.kind = Data::UploadKind::Image;
					request.purpose = Data::UploadPurpose::Chat;
					auto callbacks = Data::UploadCallbacks();
					callbacks.done = [=](const Data::UploadResult &result) {
						*uploading = false;
						*photo = result.mediaId;
					};
					callbacks.fail = [=](const Api::Error &error) {
						*uploading = false;
						Support::ShowError(controller, error);
					};
					session->data().upload().start(
						request,
						base::Guard(box),
						std::move(callbacks));
				});
			});
		};
		userpic->setClickedCallback(pick);
		userpic->setPointerCursor(true);
		const auto change = box->addRow(
			Ui::object_ptr<Ui::LinkButton>(
				box,
				was.photo.isEmpty()
					? tr::lng_groups_photo_set(tr::now)
					: tr::lng_groups_photo_change(tr::now)),
			st::boxRowPadding,
			style::al_top);
		change->setClickedCallback(pick);
		if (!was.photo.isEmpty()) {
			const auto remove = box->addRow(
				Ui::object_ptr<Ui::LinkButton>(
					box,
					tr::lng_groups_photo_remove(tr::now)),
				st::boxRowPadding,
				style::al_top);
			remove->setClickedCallback([=] {
				*photo = QString();
				userpic->setSource(nullptr);
				userpic->setImage(QImage());
			});
		}
		const auto name = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_groups_name_placeholder(tr::now),
			was.title));
		name->setMaxLength(titleMax * 2);
		const auto about = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_groups_about_placeholder(tr::now),
			was.about));
		about->setMaxLength(store.aboutMaxChars() * 2);
		about->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
		box->setFocusCallback([=] { name->setFocusFast(); });
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_save(tr::now),
			nullptr,
			st::defaultRoundButton);
		const auto save = [=] {
			if (*saving || *uploading) {
				return;
			}
			const auto title = Data::GroupRules::CleanTitle(name->text());
			if (!Data::GroupRules::TitleOk(title, titleMax)) {
				name->showError();
				return;
			}
			const auto newAbout = about->text().trimmed();
			const auto titleChange = (title != was.title)
				? std::make_optional(title)
				: std::nullopt;
			const auto aboutChange = (newAbout != was.about.trimmed())
				? std::make_optional(newAbout)
				: std::nullopt;
			if (!titleChange && !aboutChange && !*photo) {
				box->closeBox();
				return;
			}
			*saving = true;
			submit->setLoading(true);
			Store(controller).edit(
				groupId,
				titleChange,
				aboutChange,
				*photo,
				base::Guard(box),
				[=](const Data::Group &) {
					box->closeBox();
					controller->showToast(tr::lng_groups_saved(tr::now));
				},
				FailHandler(controller, [=] {
					*saving = false;
					submit->setLoading(false);
				}));
		};
		submit->setClickedCallback(save);
		QObject::connect(name, &Ui::InputField::submitted, box, save);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowDeleteGroupBox(
		Window::SessionController *controller,
		const QString &groupId) {
	const auto title = Title(controller, groupId);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_groups_delete_group(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_delete_explain(tr::now, lt_name, title),
				st::boxLabel),
			st::boxRowPadding);
		const auto name = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_groups_name_placeholder(tr::now)));
		box->setFocusCallback([=] { name->setFocusFast(); });
		const auto deleting = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_delete_group(tr::now),
			nullptr,
			st::dangerRoundButton);
		const auto remove = [=] {
			if (*deleting) {
				return;
			}
			if (Data::GroupRules::CleanTitle(name->text())
				!= Data::GroupRules::CleanTitle(title)) {
				name->showError();
				controller->showToast(tr::lng_groups_delete_mismatch(tr::now));
				return;
			}
			*deleting = true;
			submit->setLoading(true);
			Store(controller).deleteGroup(groupId, base::Guard(controller), [=] {
				controller->showToast(tr::lng_groups_delete_done(tr::now));
			}, FailHandler(controller));
			box->closeBox();
		};
		submit->setClickedCallback(remove);
		QObject::connect(name, &Ui::InputField::submitted, box, remove);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowReportGroup(
		Window::SessionController *controller,
		const QString &groupId) {
	auto args = Boxes::ReportArgs();
	args.kind = u"group"_s;
	args.id = groupId;
	Boxes::ShowReportBox(controller, std::move(args));
}

// ---- administration

void ShowAdminBox(
		Window::SessionController *controller,
		const QString &groupId,
		const Data::GroupMember &member,
		Fn<void()> changed) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	const auto me = Me(controller, *header);
	const auto grantable = Rights::Grantable(me);
	const auto promoting = (member.role != u"admin");
	const auto name = Name(controller, member.user);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(promoting
			? tr::lng_groups_promote(tr::now)
			: tr::lng_groups_admin_edit(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_admin_rights_header(tr::now, lt_name, name),
				st::boxLabel),
			st::boxRowPadding);
		const auto names = ShownNames(controller, groupId, Rights::ShownRights());
		const auto current = Rights::RightsMask(member.rights);
		auto checks = std::vector<Ui::Checkbox*>();
		for (auto i = 0; i != int(names.size()); ++i) {
			const auto bit = Rights::RightBit(names[i]);
			const auto mine = (grantable & bit) != 0;
			// A new admin starts with everything I may give but «add admins».
			const auto checked = promoting
				? (mine && bit != Rights::kAddAdmins)
				: ((current & bit) != 0);
			const auto check = box->addRow(
				Ui::object_ptr<Ui::Checkbox>(box, RightLabel(names[i]), checked),
				st::boxCheckboxPadding);
			// What I do not hold myself stays as it is.
			check->setDisabled(!mine);
			checks.push_back(check);
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_admin_rights_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		// The admin's title is their member tag: the same field, «تگ».
		const auto tags = Store(controller).memberTagsIn(groupId);
		const auto title = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tags
				? tr::lng_groups_tag_placeholder(tr::now)
				: tr::lng_groups_admin_title_placeholder(tr::now),
			tags ? TagOf(controller, groupId, member).tag : member.adminTitle));
		title->setMaxLength(Store(controller).adminTitleMaxChars() * 2);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tags
					? tr::lng_groups_admin_tag_note(tr::now)
					: tr::lng_groups_admin_title_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			promoting ? tr::lng_groups_promote(tr::now) : tr::lng_groups_save(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			if (*saving) {
				return;
			}
			auto mask = 0;
			for (auto i = 0; i != int(checks.size()); ++i) {
				if (checks[i]->checked()) {
					mask |= Rights::RightBit(names[i]);
				}
			}
			*saving = true;
			submit->setLoading(true);
			Store(controller).setAdmin(
				groupId,
				member.user,
				Rights::RightsList(mask),
				std::make_optional(Data::MemberTags::Typed(title->text())),
				base::Guard(box),
				[=](const Data::GroupMember &) {
					box->closeBox();
					controller->showToast(promoting
						? tr::lng_groups_admin_added(tr::now, lt_name, name)
						: tr::lng_groups_admin_saved(tr::now, lt_name, name));
					if (changed) {
						changed();
					}
				},
				FailHandler(controller, [=] {
					*saving = false;
					submit->setLoading(false);
				}));
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowRestrictBox(
		Window::SessionController *controller,
		const QString &groupId,
		const Data::GroupMember &member,
		Fn<void()> changed) {
	const auto name = Name(controller, member.user);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(member.restricted()
			? tr::lng_groups_restrict_edit(tr::now)
			: tr::lng_groups_restrict_title(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_admin_rights_header(tr::now, lt_name, name),
				st::boxLabel),
			st::boxRowPadding);
		const auto names = ShownNames(controller, groupId, Rights::ShownPermissions());
		const auto denied = Rights::PermissionsMask(member.denied);
		auto checks = std::vector<Ui::Checkbox*>();
		for (auto i = 0; i != int(names.size()); ++i) {
			const auto bit = Rights::PermissionBit(names[i]);
			// A fresh restriction takes sending away; the rest stays.
			const auto allowed = member.restricted()
				? ((denied & bit) == 0)
				: ((Rights::kPermissionsSend & bit) == 0);
			checks.push_back(box->addRow(
				Ui::object_ptr<Ui::Checkbox>(box, PermissionLabel(names[i]), allowed),
				st::boxCheckboxPadding));
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_restrict_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_restrict_length(tr::now),
				st::groupsSectionTitle),
			st::boxRowPadding);
		const auto length = std::make_shared<Ui::RadiobuttonGroup>(0);
		const auto lengths = std::vector<std::pair<qint64, QString>>{
			{ 0, tr::lng_groups_restrict_forever(tr::now) },
			{ kHour, tr::lng_groups_duration_hour(tr::now) },
			{ kDay, tr::lng_groups_duration_day(tr::now) },
			{ kWeek, tr::lng_groups_duration_week(tr::now) },
		};
		for (auto i = 0; i != int(lengths.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(box, length, i, lengths[i].second),
				st::boxCheckboxPadding);
		}
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_restrict(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			if (*saving) {
				return;
			}
			auto mask = 0;
			for (auto i = 0; i != int(checks.size()); ++i) {
				if (!checks[i]->checked()) {
					mask |= Rights::PermissionBit(names[i]);
				}
			}
			const auto seconds = lengths[std::clamp(
				length->value(),
				0,
				int(lengths.size()) - 1)].first;
			const auto until = seconds
				? (QDateTime::currentSecsSinceEpoch() + seconds)
				: qint64(0);
			*saving = true;
			submit->setLoading(true);
			Store(controller).restrict(
				groupId,
				member.user,
				Rights::PermissionsList(mask),
				until,
				base::Guard(box),
				[=](const Data::GroupMember &) {
					box->closeBox();
					controller->showToast(mask
						? tr::lng_groups_restrict_done(tr::now, lt_name, name)
						: tr::lng_groups_restrict_lifted(tr::now, lt_name, name));
					if (changed) {
						changed();
					}
				},
				FailHandler(controller, [=] {
					*saving = false;
					submit->setLoading(false);
				}));
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowPermissionsBox(
		Window::SessionController *controller,
		const QString &groupId) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	const auto was = *header;
	const auto me = Me(controller, was);
	const auto defaults = Rights::PermissionsMask(was.defaultPermissions);
	const auto mayHistory = Rights::Can(defaults, me, Rights::kActHistory);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_groups_perms_title(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_perms_header(tr::now),
				st::boxLabel),
			st::boxRowPadding);
		// «ویرایش تگ خود» after «pin» and before «change info» (Telegram's order).
		const auto names = ShownNames(controller, groupId, Rights::ShownPermissions());
		auto checks = std::vector<Ui::Checkbox*>();
		for (auto i = 0; i != int(names.size()); ++i) {
			checks.push_back(box->addRow(
				Ui::object_ptr<Ui::Checkbox>(
					box,
					PermissionLabel(names[i]),
					(defaults & Rights::PermissionBit(names[i])) != 0),
				st::boxCheckboxPadding));
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_perms_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		const auto history = mayHistory
			? box->addRow(
				Ui::object_ptr<Ui::Checkbox>(
					box,
					tr::lng_groups_history_visible(tr::now),
					was.historyVisible),
				st::boxCheckboxPadding)
			: nullptr;
		if (history) {
			box->addRow(
				Ui::object_ptr<Ui::FlatLabel>(
					box,
					tr::lng_groups_history_note(tr::now),
					st::groupsBoxCaption),
				st::boxRowPadding);
		}
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_save(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			if (*saving) {
				return;
			}
			// What the box does not list stays as it is.
			auto mask = defaults;
			for (auto i = 0; i != int(checks.size()); ++i) {
				const auto bit = Rights::PermissionBit(names[i]);
				mask = checks[i]->checked() ? (mask | bit) : (mask & ~bit);
			}
			const auto permissions = (mask != defaults)
				? std::make_optional(Rights::PermissionsList(mask))
				: std::nullopt;
			const auto visible = (history && history->checked() != was.historyVisible)
				? std::make_optional(history->checked())
				: std::nullopt;
			if (!permissions && !visible) {
				box->closeBox();
				return;
			}
			*saving = true;
			submit->setLoading(true);
			Store(controller).setPermissions(
				groupId,
				permissions,
				visible,
				base::Guard(box),
				[=](const Data::Group &) {
					box->closeBox();
					controller->showToast(tr::lng_groups_perms_saved(tr::now));
				},
				FailHandler(controller, [=] {
					*saving = false;
					submit->setLoading(false);
				}));
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

void ShowMemberMenu(
		Window::SessionController *controller,
		const QString &groupId,
		const Data::GroupMember &member,
		QPoint globalPosition,
		Fn<void()> changed) {
	const auto header = Header(controller, groupId);
	if (!header) {
		return;
	}
	auto &store = Store(controller);
	const auto me = Me(controller, *header);
	const auto target = Target(member);
	const auto defaults = Rights::PermissionsMask(header->defaultPermissions);
	const auto user = member.user;
	const auto self = (user == controller->session().userId());
	const auto name = Name(controller, user);
	const auto done = [=] {
		if (changed) {
			changed();
		}
	};
	const auto menu = new Ui::PopupMenu(controller->window());
	menu->addAction(tr::lng_dialogs_view_profile(tr::now), [=] {
		controller->hideLayers();
		controller->openRoute(Window::Routes::User(user));
	}, &st::groupsMenuUser);
	// The tag: my own («افزودن / ویرایش تگ»), or a member's with `manage_tags`.
	if (member.status.isEmpty() || member.status == u"member") {
		const auto tag = TagOf(controller, groupId, member);
		const auto mayTag = self
			? MayEditOwnTag(controller, groupId)
			: MayEditTagOf(controller, groupId, user, tag.role);
		if (mayTag) {
			menu->addAction(
				self
					? (tag.tag.isEmpty()
						? tr::lng_groups_tag_add(tr::now)
						: tr::lng_groups_tag_edit(tr::now))
					: (tag.tag.isEmpty()
						? tr::lng_groups_tag_add_member(tr::now)
						: tr::lng_groups_tag_edit_member(tr::now)),
				// The rows that show the tag follow Data::Groups::senderTagsChanged.
				[=] { ShowEditTagBox(controller, groupId, user, tag); },
				&st::groupsMenuTag);
		}
	}
	const auto danger = [&](const QString &text, Fn<void()> callback, const style::icon *icon) {
		auto item = Ui::MenuItem();
		item.text = text;
		item.callback = std::move(callback);
		item.icon = icon;
		item.destructive = true;
		menu->addItem(std::move(item));
	};
	if (self) {
		menu->popup(globalPosition);
		return;
	}
	const auto banned = (member.status == u"banned");
	if (banned) {
		if (Rights::Can(defaults, me, Rights::kActBan)) {
			menu->addAction(tr::lng_groups_unban(tr::now), [=] {
				Store(controller).unban(groupId, user, base::Guard(controller), [=] {
					controller->showToast(
						tr::lng_groups_unbanned_toast(tr::now, lt_name, name));
					done();
				}, FailHandler(controller));
			}, &st::groupsMenuApprove);
		}
		menu->popup(globalPosition);
		return;
	}
	const auto remove = [=] {
		auto args = Ui::ConfirmBoxArgs();
		args.title = tr::lng_groups_remove(tr::now);
		args.text = tr::lng_groups_remove_sure(tr::now, lt_name, name);
		args.confirmText = tr::lng_groups_remove(tr::now);
		args.destructive = true;
		args.confirmed = [=](std::function<void()> close) {
			close();
			Store(controller).removeMember(groupId, user, base::Guard(controller), [=] {
				controller->showToast(
					tr::lng_groups_removed_toast(tr::now, lt_name, name));
				done();
			}, FailHandler(controller));
		};
		controller->show(Ui::MakeConfirmBox(std::move(args)));
	};
	if (!store.administration()) {
		// The first release of groups: the owner removes members and hands
		// the group over; nothing else exists.
		if (me.role == u"owner") {
			menu->addSeparator();
			danger(tr::lng_groups_remove(tr::now), remove, &st::menuIconDelete);
		}
	} else {
		const auto promote = Rights::CanPromote(me, target, 0, store.maxAdmins());
		const auto editAdmin = Rights::CanEditAdmin(me, target);
		const auto restrict = Rights::CanRestrict(me, target);
		const auto ban = Rights::CanBan(me, target);
		if (promote || editAdmin || restrict || ban) {
			menu->addSeparator();
		}
		if (promote) {
			menu->addAction(tr::lng_groups_promote(tr::now), [=] {
				ShowAdminBox(controller, groupId, member, done);
			}, &st::groupsMenuAdmin);
		}
		if (editAdmin) {
			menu->addAction(tr::lng_groups_admin_edit(tr::now), [=] {
				ShowAdminBox(controller, groupId, member, done);
			}, &st::groupsMenuAdmin);
			menu->addAction(tr::lng_groups_admin_remove(tr::now), [=] {
				auto args = Ui::ConfirmBoxArgs();
				args.title = tr::lng_groups_admin_remove(tr::now);
				args.text = tr::lng_groups_admin_remove_sure(tr::now, lt_name, name);
				args.confirmText = tr::lng_groups_admin_remove(tr::now);
				args.destructive = true;
				args.confirmed = [=](std::function<void()> close) {
					close();
					Store(controller).unsetAdmin(
						groupId,
						user,
						base::Guard(controller),
						[=](const Data::GroupMember &) {
							controller->showToast(
								tr::lng_groups_admin_removed(tr::now, lt_name, name));
							done();
						},
						FailHandler(controller));
				};
				controller->show(Ui::MakeConfirmBox(std::move(args)));
			}, &st::groupsMenuDecline);
		}
		if (restrict) {
			menu->addAction(
				member.restricted()
					? tr::lng_groups_restrict_edit(tr::now)
					: tr::lng_groups_restrict(tr::now),
				[=] { ShowRestrictBox(controller, groupId, member, done); },
				&st::groupsMenuRestrict);
			if (member.restricted()) {
				menu->addAction(tr::lng_groups_restrict_lift(tr::now), [=] {
					Store(controller).restrict(
						groupId,
						user,
						{},
						0,
						base::Guard(controller),
						[=](const Data::GroupMember &) {
							controller->showToast(
								tr::lng_groups_restrict_lifted(tr::now, lt_name, name));
							done();
						},
						FailHandler(controller));
				}, &st::groupsMenuApprove);
			}
		}
		if (ban) {
			danger(tr::lng_groups_remove(tr::now), remove, &st::menuIconDelete);
			danger(tr::lng_groups_ban(tr::now), [=] {
				const auto wipe = std::make_shared<bool>(false);
				auto args = Ui::ConfirmBoxArgs();
				args.title = tr::lng_groups_ban(tr::now);
				args.text = tr::lng_groups_ban_sure(tr::now, lt_name, name);
				args.confirmText = tr::lng_groups_ban(tr::now);
				args.destructive = true;
				args.checkboxText = tr::lng_groups_ban_delete_all(tr::now);
				args.checkboxChanged = [=](bool checked) { *wipe = checked; };
				args.confirmed = [=](std::function<void()> close) {
					close();
					Store(controller).ban(groupId, user, *wipe, base::Guard(controller), [=] {
						controller->showToast(
							tr::lng_groups_banned_toast(tr::now, lt_name, name));
						done();
					}, FailHandler(controller));
				};
				controller->show(Ui::MakeConfirmBox(std::move(args)));
			}, &st::menuIconBlock);
		}
	}
	if (me.role == u"owner") {
		menu->addAction(tr::lng_groups_transfer(tr::now), [=] {
			auto args = Ui::ConfirmBoxArgs();
			args.title = tr::lng_groups_transfer(tr::now);
			args.text = tr::lng_groups_transfer_sure(tr::now, lt_name, name);
			args.confirmText = tr::lng_groups_transfer_confirm(tr::now);
			args.confirmed = [=](std::function<void()> close) {
				close();
				Store(controller).transfer(
					groupId,
					user,
					base::Guard(controller),
					[=](const Data::Group &) {
						controller->showToast(
							tr::lng_groups_transfer_done(tr::now, lt_name, name));
						done();
					},
					FailHandler(controller));
			};
			controller->show(Ui::MakeConfirmBox(std::move(args)));
		}, &st::groupsMenuGroup);
	}
	menu->popup(globalPosition);
}

// ---- lists: admins, restricted, banned

namespace {

class MembersController final : public Boxes::PeerListController {
public:
	MembersController(
		Window::SessionController *controller,
		QString groupId,
		QString filter)
	: _controller(controller)
	, _groupId(std::move(groupId))
	, _filter(std::move(filter)) {
	}

	QString title() const override {
		return (_filter == u"admins")
			? tr::lng_groups_admins(tr::now)
			: (_filter == u"restricted")
			? tr::lng_groups_restricted_title(tr::now)
			: (_filter == u"banned")
			? tr::lng_groups_banned_title(tr::now)
			: tr::lng_groups_members_title(tr::now);
	}
	QString emptyText() const override {
		return (_filter == u"admins")
			? tr::lng_groups_admins_empty(tr::now)
			: (_filter == u"restricted")
			? tr::lng_groups_restricted_empty(tr::now)
			: (_filter == u"banned")
			? tr::lng_groups_banned_empty(tr::now)
			: tr::lng_nothing_found(tr::now);
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
		const auto i = _members.constFind(row.id);
		if (i == _members.cend()) {
			return;
		}
		const auto weak = QPointer<Ui::BoxContent>(delegate()->peerListBox());
		ShowMemberMenu(_controller, _groupId, *i, QCursor::pos(), [=, this] {
			// (`this` lives as long as the box.)
			if (weak) {
				reload();
			}
		});
	}

private:
	void reload() {
		_cursor = QString();
		++_generation;
		_loading = false;
		load(true);
	}
	void load(bool first) {
		if (_loading) {
			return;
		}
		_loading = true;
		const auto generation = _generation;
		const auto session = this->session();
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().groups().loadMembers(
			_groupId,
			_filter,
			first ? QString() : _cursor,
			guard,
			[=, this](const Data::GroupMembersPage &page) {
				if (generation != _generation) {
					return;
				}
				auto ids = QStringList();
				for (const auto &member : page.members) {
					ids.push_back(member.user);
				}
				session->data().users().ensure(ids, guard, [=, this] {
					if (generation != _generation) {
						return;
					}
					_loading = false;
					_cursor = page.nextCursor;
					auto rows = QVector<Boxes::PeerListRow>();
					if (first) {
						_members.clear();
					}
					for (const auto &member : page.members) {
						_members.insert(member.user, member);
						rows.push_back(rowFor(member));
					}
					if (first) {
						delegate()->setRows(rows);
					} else {
						delegate()->appendRows(rows);
					}
				});
			},
			[=, this](const Api::Error &error) {
				_loading = false;
				if (first) {
					delegate()->setRows({});
				}
				ShowGroupError(_controller, error);
			});
	}
	[[nodiscard]] Boxes::PeerListRow rowFor(const Data::GroupMember &member) const {
		auto user = session()->data().users().peek(member.user);
		if (!user.valid()) {
			user.id = member.user;
		}
		auto row = Boxes::PeerListRow::FromUser(user);
		if (row.title.isEmpty()) {
			row.title = Support::MemberName(session(), member.user);
		}
		const auto now = QDateTime::currentMSecsSinceEpoch();
		if (member.restricted()) {
			auto denied = QStringList();
			for (const auto &name : member.denied) {
				denied.push_back(PermissionLabel(name));
			}
			const auto left = Support::GroupTimeLeft(member.restrictedUntil, now);
			row.subtitle = left.isEmpty()
				? denied.join(u"، "_s)
				: tr::lng_groups_member_restricted_until(tr::now, lt_time, left);
		} else {
			row.subtitle = MemberTag(member);
		}
		return row;
	}

	Window::SessionController *_controller = nullptr;
	QString _groupId;
	QString _filter;
	QString _cursor;
	QHash<QString, Data::GroupMember> _members;
	int _generation = 0;
	bool _loading = false;

};

class JoinRequestsController final : public Boxes::PeerListController {
public:
	JoinRequestsController(
		Window::SessionController *controller,
		QString groupId)
	: _controller(controller)
	, _groupId(std::move(groupId)) {
	}

	QString title() const override {
		return tr::lng_groups_requests_title(tr::now);
	}
	QString emptyText() const override {
		return tr::lng_groups_requests_empty(tr::now);
	}

	void prepare() override {
		delegate()->setLoading(true);
		load(true);
		// A new request, or one decided by another admin: read the list
		// again (the group_admin signal).
		QObject::connect(
			&_controller->session().data().groups(),
			&Data::Groups::joinRequestsChanged,
			delegate()->peerListBox(),
			[=, this](const QString &group) {
				if (group == _groupId) {
					reload();
				}
			});
		if (Store(_controller).thirdRelease()) {
			const auto box = delegate()->peerListBox();
			box->addLeftButton(
				tr::lng_groups_requests_dismiss_all(tr::now),
				[=, this] { decideAll(false); });
			box->addButton(
				tr::lng_groups_requests_add_all(tr::now),
				[=, this] { decideAll(true); },
				st::defaultRoundButton);
		}
	}
	void loadMore() override {
		if (!_cursor.isEmpty()) {
			load(false);
		}
	}
	void rowClicked(const Boxes::PeerListRow &row) override {
		const auto user = row.id;
		const auto name = row.title;
		const auto controller = _controller;
		const auto groupId = _groupId;
		const auto weak = QPointer<Ui::BoxContent>(delegate()->peerListBox());
		const auto decide = [=, this](bool approve) {
			controller->session().data().groups().decideJoinRequest(
				groupId,
				user,
				approve,
				base::Guard(controller),
				[=, this] {
					controller->showToast(approve
						? tr::lng_groups_requests_approved(tr::now, lt_name, name)
						: tr::lng_groups_requests_declined(tr::now, lt_name, name));
					if (weak) {
						_users.removeAll(user);
						delegate()->removeRow(user);
					}
				},
				FailHandler(controller));
		};
		const auto menu = new Ui::PopupMenu(controller->window());
		menu->addAction(tr::lng_groups_requests_approve(tr::now), [=] {
			decide(true);
		}, &st::groupsMenuApprove);
		menu->addAction(tr::lng_groups_requests_decline(tr::now), [=] {
			decide(false);
		}, &st::groupsMenuDecline);
		menu->addSeparator();
		menu->addAction(tr::lng_dialogs_view_profile(tr::now), [=] {
			controller->hideLayers();
			controller->openRoute(Window::Routes::User(user));
		}, &st::groupsMenuUser);
		menu->popup(QCursor::pos());
	}

private:
	// «افزودن همه» / «رد همه»: every request, the pages not read yet too.
	void decideAll(bool approve) {
		if (_users.isEmpty() && _cursor.isEmpty()) {
			return;
		}
		const auto controller = _controller;
		const auto weak = QPointer<Ui::BoxContent>(delegate()->peerListBox());
		auto args = Ui::ConfirmBoxArgs();
		args.text = approve
			? tr::lng_groups_requests_add_all_sure(tr::now)
			: tr::lng_groups_requests_dismiss_all_sure(tr::now);
		args.confirmText = approve
			? tr::lng_groups_requests_add_all(tr::now)
			: tr::lng_groups_requests_dismiss_all(tr::now);
		args.destructive = !approve;
		args.confirmed = [=, this](std::function<void()> close) {
			close();
			if (weak) {
				collectAll(_cursor, _users, [=, this](const QStringList &users) {
					decide(users, approve);
				});
			}
		};
		controller->show(Ui::MakeConfirmBox(std::move(args)));
	}
	void collectAll(
			const QString &cursor,
			const QStringList &collected,
			Fn<void(QStringList)> done,
			int pages = 0) {
		if (cursor.isEmpty() || pages >= kMaxCollectPages) {
			done(collected);
			return;
		}
		const auto controller = _controller;
		Store(controller).loadJoinRequests(
			_groupId,
			cursor,
			base::Guard(delegate()->peerListBox()),
			[=, this](const Data::GroupJoinRequestsPage &page) {
				auto more = collected;
				for (const auto &request : page.requests) {
					if (!more.contains(request.user)) {
						more.push_back(request.user);
					}
				}
				collectAll(page.nextCursor, more, done, pages + 1);
			},
			FailHandler(controller));
	}
	void decide(const QStringList &users, bool approve) {
		const auto controller = _controller;
		const auto weak = QPointer<Ui::BoxContent>(delegate()->peerListBox());
		Store(controller).decideJoinRequests(
			_groupId,
			users,
			approve,
			base::Guard(controller),
			[=, this](const Data::GroupDecideResult &result) {
				const auto done = int(result.done.size());
				const auto failed = int(result.failed.size());
				auto text = approve
					? tr::lng_groups_requests_added(tr::now, lt_count, done)
					: tr::lng_groups_requests_dismissed(tr::now, lt_count, done);
				if (failed > 0) {
					text += u"\n"_s
						+ tr::lng_groups_requests_failed(tr::now, lt_count, failed);
				}
				controller->showToast(text);
				if (weak) {
					reload();
				}
			},
			FailHandler(controller));
	}
	void reload() {
		_cursor = QString();
		_users.clear();
		_loading = false;
		delegate()->setLoading(true);
		load(true);
	}
	void load(bool first) {
		if (_loading) {
			return;
		}
		_loading = true;
		const auto session = this->session();
		const auto guard = base::Guard(delegate()->peerListBox());
		session->data().groups().loadJoinRequests(
			_groupId,
			first ? QString() : _cursor,
			guard,
			[=, this](const Data::GroupJoinRequestsPage &page) {
				auto ids = QStringList();
				for (const auto &request : page.requests) {
					ids.push_back(request.user);
				}
				session->data().users().ensure(ids, guard, [=, this] {
					_loading = false;
					_cursor = page.nextCursor;
					auto rows = QVector<Boxes::PeerListRow>();
					for (const auto &request : page.requests) {
						auto user = session->data().users().peek(request.user);
						if (!user.valid()) {
							user.id = request.user;
						}
						auto row = Boxes::PeerListRow::FromUser(user);
						if (row.title.isEmpty()) {
							row.title = Support::MemberName(session, request.user);
						}
						if (!_users.contains(request.user)) {
							_users.push_back(request.user);
						}
						if (!request.linkName.trimmed().isEmpty()) {
							row.subtitle = tr::lng_groups_requests_via(
								tr::now,
								lt_name,
								Data::Json::CleanName(request.linkName.simplified()));
						}
						rows.push_back(row);
					}
					if (first) {
						delegate()->setRows(rows);
					} else {
						delegate()->appendRows(rows);
					}
				});
			},
			[=, this](const Api::Error &error) {
				_loading = false;
				if (first) {
					delegate()->setRows({});
				}
				ShowGroupError(_controller, error);
			});
	}

	static constexpr auto kMaxCollectPages = 50;

	Window::SessionController *_controller = nullptr;
	QString _groupId;
	QString _cursor;
	QStringList _users;
	bool _loading = false;

};

[[nodiscard]] QString InviteStatus(const Data::GroupInvite &invite) {
	auto parts = QStringList();
	if (invite.expired) {
		parts.push_back(tr::lng_groups_links_expired(tr::now));
	}
	parts.push_back((invite.maxUses > 0)
		? tr::lng_groups_links_uses_of(
			tr::now,
			lt_used,
			Lang::Num(invite.uses),
			lt_limit,
			Lang::Num(invite.maxUses))
		: tr::lng_groups_links_uses(tr::now, lt_count, invite.uses));
	if (invite.requiresApproval) {
		parts.push_back(tr::lng_groups_links_approval(tr::now));
	}
	return parts.join(u" · "_s);
}

void ShowCreateInviteBox(
		Window::SessionController *controller,
		const QString &groupId) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		auto &store = Store(controller);
		box->setTitle(tr::lng_groups_links_new(tr::now));
		const auto name = box->addRow(Ui::object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_groups_links_name_placeholder(tr::now)));
		name->setMaxLength(store.inviteNameMaxChars() * 2);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_links_expiry(tr::now),
				st::groupsSectionTitle),
			st::boxRowPadding);
		const auto maxExpiry = store.inviteMaxExpiry();
		auto expiries = std::vector<std::pair<qint64, QString>>{
			{ 0, tr::lng_groups_links_never(tr::now) },
		};
		for (const auto &[seconds, label] : std::vector<std::pair<qint64, QString>>{
				{ kHour, tr::lng_groups_duration_hour(tr::now) },
				{ kDay, tr::lng_groups_duration_day(tr::now) },
				{ kWeek, tr::lng_groups_duration_week(tr::now) } }) {
			if (maxExpiry <= 0 || seconds <= maxExpiry) {
				expiries.push_back({ seconds, label });
			}
		}
		const auto expiry = std::make_shared<Ui::RadiobuttonGroup>(0);
		for (auto i = 0; i != int(expiries.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(box, expiry, i, expiries[i].second),
				st::boxCheckboxPadding);
		}
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_links_max_uses(tr::now),
				st::groupsSectionTitle),
			st::boxRowPadding);
		const auto limits = std::vector<int>{ 0, 1, 10, 100 };
		const auto uses = std::make_shared<Ui::RadiobuttonGroup>(0);
		for (auto i = 0; i != int(limits.size()); ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(
					box,
					uses,
					i,
					limits[i]
						? tr::lng_groups_links_people(tr::now, lt_count, limits[i])
						: tr::lng_groups_links_no_limit(tr::now)),
				st::boxCheckboxPadding);
		}
		const auto approval = box->addRow(
			Ui::object_ptr<Ui::Checkbox>(
				box,
				tr::lng_groups_links_needs_approval(tr::now)),
			st::boxCheckboxPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_links_approval_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		const auto creating = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_links_create(tr::now),
			nullptr,
			st::defaultRoundButton);
		submit->setClickedCallback([=] {
			if (*creating) {
				return;
			}
			*creating = true;
			submit->setLoading(true);
			auto request = Data::GroupInviteRequest();
			request.name = name->text().simplified();
			request.requiresApproval = approval->checked();
			request.expiresIn = expiries[std::clamp(
				expiry->value(),
				0,
				int(expiries.size()) - 1)].first;
			request.maxUses = limits[std::clamp(
				uses->value(),
				0,
				int(limits.size()) - 1)];
			Store(controller).createInvite(groupId, request, base::Guard(box), [=](
					const Data::GroupInvite &invite) {
				const auto url = invite.url.isEmpty()
					? Data::GroupInviteLinks::Url(
						invite.link,
						Store(controller).inviteLinkBase())
					: invite.url;
				QGuiApplication::clipboard()->setText(url);
				box->closeBox();
				controller->showToast(tr::lng_groups_links_created(tr::now));
				ShowInviteLinksBox(controller, groupId);
			}, FailHandler(controller, [=] {
				*creating = false;
				submit->setLoading(false);
			}));
		});
		box->addButton(tr::lng_cancel(tr::now), [=] {
			box->closeBox();
			ShowInviteLinksBox(controller, groupId);
		});
	}));
}

void ShowInviteLinks(
		Window::SessionController *controller,
		const QString &groupId,
		const QVector<Data::GroupInvite> &invites) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		auto &store = Store(controller);
		box->setTitle(tr::lng_groups_links_title(tr::now));
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_groups_links_note(tr::now),
				st::groupsBoxCaption),
			st::boxRowPadding);
		auto active = 0;
		for (const auto &invite : invites) {
			if (!invite.revoked) {
				++active;
			}
		}
		const auto max = store.maxInvites();
		const auto full = (max > 0) && (active >= max);
		const auto create = box->addRow(
			Ui::object_ptr<Ui::SettingsButton>(
				box,
				tr::lng_groups_links_new(tr::now),
				st::groupsButton),
			style::margins());
		create->setIcon(&st::groupsIconLink);
		create->setClickedCallback([=] {
			if (full) {
				controller->showToast(
					tr::lng_groups_links_full(tr::now, lt_count, max));
				return;
			}
			box->closeBox();
			ShowCreateInviteBox(controller, groupId);
		});
		if (!active) {
			box->addRow(
				Ui::object_ptr<Ui::FlatLabel>(
					box,
					tr::lng_groups_links_empty(tr::now),
					st::boxLabel),
				st::boxRowPaddingNoTitle);
		}
		const auto base = store.inviteLinkBase();
		const auto third = store.thirdRelease();
		for (const auto &invite : invites) {
			if (invite.revoked) {
				continue;
			}
			const auto url = invite.url.isEmpty()
				? Data::GroupInviteLinks::Url(invite.link, base)
				: invite.url;
			const auto row = box->addRow(
				Ui::object_ptr<Ui::SettingsButton>(
					box,
					invite.name.trimmed().isEmpty()
						? tr::lng_groups_links_unnamed(tr::now)
						: Data::Json::CleanName(invite.name.simplified()),
					st::groupsButton),
				style::margins());
			row->setIcon(&st::groupsIconLink);
			row->setValueText(InviteStatus(invite));
			const auto link = invite.link;
			const auto title = invite.name.trimmed().isEmpty()
				? tr::lng_groups_link_joined(tr::now)
				: Data::Json::CleanName(invite.name.simplified());
			row->setClickedCallback([=] {
				const auto menu = new Ui::PopupMenu(controller->window());
				menu->addAction(tr::lng_copy_link(tr::now), [=] {
					QGuiApplication::clipboard()->setText(url);
					controller->showToast(tr::lng_groups_links_copied(tr::now));
				}, &st::menuIconLink);
				if (third) {
					menu->addAction(tr::lng_groups_link_joined(tr::now), [=] {
						ShowInviteMembersBox(controller, groupId, link, title);
					}, &st::groupsMenuJoined);
				}
				auto revoke = Ui::MenuItem();
				revoke.text = tr::lng_groups_links_revoke(tr::now);
				revoke.icon = &st::menuIconDelete;
				revoke.destructive = true;
				revoke.callback = [=] {
					auto args = Ui::ConfirmBoxArgs();
					args.title = tr::lng_groups_links_revoke(tr::now);
					args.text = tr::lng_groups_links_revoke_sure(tr::now);
					args.confirmText = tr::lng_groups_links_revoke(tr::now);
					args.destructive = true;
					args.confirmed = [=](std::function<void()> close) {
						close();
						Store(controller).revokeInvite(
							groupId,
							link,
							base::Guard(controller),
							[=] {
								controller->showToast(
									tr::lng_groups_links_revoked(tr::now));
								controller->hideLayers();
								ShowInviteLinksBox(controller, groupId);
							},
							FailHandler(controller));
					};
					controller->show(Ui::MakeConfirmBox(std::move(args)));
				};
				menu->addItem(std::move(revoke));
				menu->popup(QCursor::pos());
			});
		}
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
	}));
}

} // namespace

void ShowMembersBox(
		Window::SessionController *controller,
		const QString &groupId,
		const QString &filter) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<MembersController>(controller, groupId, filter)));
}

void ShowJoinRequestsBox(
		Window::SessionController *controller,
		const QString &groupId) {
	controller->show(Ui::Box<Boxes::PeerListBox>(
		&controller->session(),
		std::make_unique<JoinRequestsController>(controller, groupId)));
}

void ShowInviteLinksBox(
		Window::SessionController *controller,
		const QString &groupId) {
	Store(controller).loadInvites(groupId, base::Guard(controller), [=](
			const QVector<Data::GroupInvite> &invites) {
		ShowInviteLinks(controller, groupId, invites);
	}, FailHandler(controller));
}

// ---- the menu of a chat row / of the chat screen

void FillChatMenu(
		Ui::PopupMenu *menu,
		Window::SessionController *controller,
		const QString &chatId) {
	if (!Data::IsGroupId(chatId) || !Header(controller, chatId)) {
		return;
	}
	menu->addAction(tr::lng_groups_info(tr::now), [=] {
		controller->openRoute(Window::Routes::Group(chatId));
	}, &st::groupsMenuGroup);
	menu->addAction(tr::lng_groups_clear_history(tr::now), [=] {
		ShowClearHistoryBox(controller, chatId);
	}, &st::groupsMenuClear);
	auto leave = Ui::MenuItem();
	leave.text = tr::lng_groups_leave(tr::now);
	leave.icon = &st::groupsMenuLeave;
	leave.destructive = true;
	leave.callback = [=] {
		ShowLeaveBox(controller, chatId);
	};
	menu->addItem(std::move(leave));
}

} // namespace Groups
