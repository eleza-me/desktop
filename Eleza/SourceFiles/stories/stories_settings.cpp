#include "stories/stories_settings.h"

#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/settings_logic.h"
#include "stories/stories_boxes.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/top_bar.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_stories.h"

#include <QtCore/QPointer>
#include <QtCore/QSet>

namespace Stories {
namespace {

constexpr auto kListPagesMax = 40;    // 40 × 50 people
constexpr auto kBulkMax = 200;        // stories.close_friends_bulk_max

enum class ListKind {
	CloseFriends,
	Hidden,
};

[[nodiscard]] QString RepliesLabel(const QString &value) {
	if (value == u"following") {
		return tr::lng_stories_replies_following(tr::now);
	} else if (value == u"off") {
		return tr::lng_stories_replies_off(tr::now);
	}
	return tr::lng_stories_replies_everyone(tr::now);
}

[[nodiscard]] QString NameOf(const Data::User &user) {
	if (user.deleted) {
		return tr::lng_deleted_account(tr::now);
	}
	return user.displayName.isEmpty() ? Lang::Handle(user.handle) : user.displayName;
}

// One person of a list: the face, the name, the handle and «×».
class PersonRow final : public Ui::RpWidget {
public:
	PersonRow(
		QWidget *parent,
		Main::Session *session,
		const Data::User &user,
		Fn<void()> remove)
	: RpWidget(parent)
	, _session(session)
	, _user(user)
	, _userpics(session, [this](const QString &) { update(); }, this)
	, _remove(Ui::CreateChild<Ui::IconButton>(this, st::storiesListRemove)) {
		_remove->setClickedCallback(std::move(remove));
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::storiesListPadding;
		_remove->moveToRight(
			padding.right(),
			(st::storiesListRowHeight - _remove->height()) / 2,
			newWidth);
		return st::storiesListRowHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::storiesListPadding;
		const auto size = st::storiesListUserpic;
		const auto top = (height() - size) / 2;
		const auto card = _session ? _session->data().users().peek(_user.id) : Data::User();
		const auto &user = card.valid() ? card : _user;
		_userpics.paint(p, style::rtlrect(padding.left(), top, size, size, width()), user);
		const auto left = padding.left() + size + 12;
		const auto available = width() - left - padding.right() - _remove->width() - 8;
		const auto name = st::storiesListNameFont->elided(NameOf(user), available);
		const auto handle = user.handle.isEmpty() ? QString() : Lang::Handle(user.handle);
		const auto lines = handle.isEmpty() ? 1 : 2;
		auto y = (height()
			- st::storiesListNameFont->height * lines) / 2;
		p.setPen(st::storiesListNameFg);
		p.setFont(st::storiesListNameFont);
		p.drawTextLeft(left, y, width(), name);
		if (!handle.isEmpty()) {
			y += st::storiesListNameFont->height;
			p.setPen(st::storiesListHandleFg);
			p.setFont(st::storiesListHandleFont);
			p.drawTextLeft(left, y, width(), st::storiesListHandleFont->elided(handle, available));
		}
	}

private:
	QPointer<Main::Session> _session;
	Data::User _user;
	Support::UserpicPainter _userpics;
	Ui::IconButton *_remove = nullptr;

};

// The close-friends or hide-from list.
class ListPage final : public Settings::Page {
public:
	ListPage(QWidget *parent, Window::SessionController *controller, ListKind kind)
	: Page(parent, controller, Settings::Section::Privacy)
	, _kind(kind) {
		bar()->setTitle(title());
		load();
	}

	QString sectionKey() const override {
		return (_kind == ListKind::CloseFriends)
			? u"settings/privacy/stories/close-friends"_s
			: u"settings/privacy/stories/hidden"_s;
	}
	QString restoreRoute() const override {
		return Window::Routes::Settings(u"privacy"_s);
	}

private:
	[[nodiscard]] QString title() const {
		return (_kind == ListKind::CloseFriends)
			? tr::lng_stories_close_friends(tr::now)
			: tr::lng_stories_hide_from(tr::now);
	}

	void load() {
		showLoading();
		_users.clear();
		loadPage(QString(), 0);
	}

	void loadPage(const QString &cursor, int page) {
		auto &stories = controller()->session().data().stories();
		const auto done = [=, this](const Data::StoryUsersPage &result) {
			_users += result.users;
			if (!result.nextCursor.isEmpty() && page + 1 < kListPagesMax) {
				loadPage(result.nextCursor, page + 1);
			} else {
				rebuild();
			}
		};
		const auto fail = [=, this](const Api::Error &error) {
			showError(error, [this] { load(); });
		};
		if (_kind == ListKind::CloseFriends) {
			stories.closeFriends(cursor, this, done, fail);
		} else {
			stories.hidden(cursor, this, done, fail);
		}
	}

	void rebuild() {
		clearContent();
		const auto add = Settings::AddButton(
			content(),
			tr::lng_stories_add_people(tr::now),
			&st::storiesSettingsIconAdd,
			[this] { pick(); });
		Q_UNUSED(add);
		Settings::AddFootnote(content(), (_kind == ListKind::CloseFriends)
			? tr::lng_stories_close_friends_footer(tr::now)
			: tr::lng_stories_hide_from_footer(tr::now));
		Settings::AddDivider(content());
		if (_users.isEmpty()) {
			Settings::AddFootnote(content(), tr::lng_stories_list_empty(tr::now));
		}
		for (const auto &user : std::as_const(_users)) {
			const auto userId = user.id;
			content()->add(Ui::object_ptr<PersonRow>(
				content(),
				&controller()->session(),
				user,
				[=, this] { remove(userId); }));
		}
		showContent();
	}

	[[nodiscard]] QStringList ids() const {
		auto result = QStringList();
		for (const auto &user : _users) {
			result.push_back(user.id);
		}
		return result;
	}

	void pick() {
		const auto before = ids();
		PickPeople(controller(), title(), before, Settings::Guarded(this, [=, this](
				const QStringList &after) {
			auto add = QStringList();
			auto remove = QStringList();
			for (const auto &id : after) {
				if (!before.contains(id)) {
					add.push_back(id);
				}
			}
			for (const auto &id : before) {
				if (!after.contains(id)) {
					remove.push_back(id);
				}
			}
			apply(add, remove);
		}));
	}

	void apply(QStringList add, QStringList remove) {
		if (add.isEmpty() && remove.isEmpty()) {
			return;
		}
		auto &stories = controller()->session().data().stories();
		const auto controller = this->controller();
		const auto fail = [=, this](const Api::Error &error) {
			Support::ShowError(controller, error);
			load();
		};
		if (_kind == ListKind::CloseFriends) {
			// In calls of at most kBulkMax ids, removals first.
			auto nowRemove = remove.mid(0, kBulkMax);
			auto nowAdd = add.mid(0, std::max(kBulkMax - int(nowRemove.size()), 0));
			const auto restRemove = remove.mid(nowRemove.size());
			const auto restAdd = add.mid(nowAdd.size());
			stories.updateCloseFriends(nowAdd, nowRemove, this, [=, this] {
				if (restAdd.isEmpty() && restRemove.isEmpty()) {
					controller->showToast(tr::lng_stories_saved(tr::now));
					load();
				} else {
					apply(restAdd, restRemove);
				}
			}, fail);
		} else {
			const auto remaining = std::make_shared<int>(int(add.size() + remove.size()));
			const auto failed = std::make_shared<bool>(false);
			const auto one = [=, this] {
				if (--*remaining == 0 && !*failed) {
					controller->showToast(tr::lng_stories_saved(tr::now));
					load();
				}
			};
			const auto oneFailed = [=](const Api::Error &error) {
				if (!*failed) {
					*failed = true;
					fail(error);
				}
			};
			for (const auto &id : remove) {
				stories.setHidden(id, false, this, one, oneFailed);
			}
			for (const auto &id : add) {
				stories.setHidden(id, true, this, one, oneFailed);
			}
		}
	}

	void remove(const QString &userId) {
		auto &stories = controller()->session().data().stories();
		const auto controller = this->controller();
		auto user = controller->session().data().users().peek(userId);
		if (!user.valid()) {
			user.id = userId;
		}
		const auto name = Lang::Isolated(NameOf(user));
		const auto done = [=, this] {
			if (_kind == ListKind::Hidden) {
				controller->showToast(tr::lng_stories_hidden_removed(tr::now, lt_name, name));
			}
			_users.erase(std::remove_if(_users.begin(), _users.end(), [&](const Data::User &user) {
				return user.id == userId;
			}), _users.end());
			rebuild();
		};
		const auto fail = [=](const Api::Error &error) {
			Support::ShowError(controller, error);
		};
		if (_kind == ListKind::CloseFriends) {
			stories.setCloseFriend(userId, false, this, done, fail);
		} else {
			stories.setHidden(userId, false, this, done, fail);
		}
	}

	ListKind _kind = ListKind::CloseFriends;
	QVector<Data::User> _users;

};

[[nodiscard]] Window::SectionFactory ListFactory(ListKind kind) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(new ListPage(parent, controller, kind));
	};
}

// The page itself.
class StoriesPage final : public Settings::Page {
public:
	StoriesPage(QWidget *parent, Window::SessionController *controller)
	: Page(parent, controller, Settings::Section::Privacy) {
		bar()->setTitle(tr::lng_stories_settings_title(tr::now));
		load();
	}

	QString sectionKey() const override {
		return u"settings/privacy/stories"_s;
	}
	QString restoreRoute() const override {
		return Window::Routes::Settings(u"privacy"_s);
	}
	void showFinished() override {
		Page::showFinished();
		// Back from a list: the counts may have changed.
		if (_built) {
			reload();
		}
	}

private:
	void load() {
		showLoading();
		reload();
	}

	void reload() {
		controller()->session().data().stories().settings(this, [this](
				const Data::StorySettings &settings) {
			_settings = settings;
			build();
		}, [this](const Api::Error &error) {
			if (!_built) {
				showError(error, [this] { load(); });
			}
		});
	}

	void build() {
		clearContent();
		_built = true;
		const auto controller = this->controller();
		Settings::AddSubsectionTitle(content(), tr::lng_stories_who_sees(tr::now));
		const auto close = Settings::AddLinkButton(
			content(),
			tr::lng_stories_close_friends(tr::now),
			&st::storiesSettingsIconClose,
			[=] { controller->showMainSection(ListFactory(ListKind::CloseFriends)); });
		close->setValueText(Lang::Num(_settings.closeFriendsCount));
		const auto hidden = Settings::AddLinkButton(
			content(),
			tr::lng_stories_hide_from(tr::now),
			&st::storiesSettingsIconHidden,
			[=] { controller->showMainSection(ListFactory(ListKind::Hidden)); });
		hidden->setValueText(Lang::Num(_settings.hiddenCount));
		Settings::AddFootnote(content(), tr::lng_stories_settings_footer(tr::now));

		Settings::AddDivider(content());
		const auto current = _settings.replies;
		const auto replies = Settings::AddLinkButton(
			content(),
			tr::lng_stories_replies_title(tr::now),
			&st::storiesSettingsIconReplies,
			[=, this] {
				Settings::ShowPickerBox(
					controller,
					tr::lng_stories_replies_title(tr::now),
					{
						{ u"everyone"_s, tr::lng_stories_replies_everyone(tr::now) },
						{ u"following"_s, tr::lng_stories_replies_following(tr::now) },
						{ u"off"_s, tr::lng_stories_replies_off(tr::now) },
					},
					current,
					Settings::Guarded(this, [=, this](const QString &value) {
						save(value);
					}));
			});
		replies->setValueText(RepliesLabel(current));
		Settings::AddFootnote(content(), tr::lng_stories_replies_footer(tr::now));

		// A server that cannot send stories on does not say allow_sharing.
		if (_settings.allowSharing) {
			Settings::AddDivider(content());
			Settings::AddSubsectionTitle(content(), tr::lng_stories_sharing_title(tr::now));
			Settings::AddToggle(
				content(),
				tr::lng_stories_allow_sharing(tr::now),
				*_settings.allowSharing,
				[=, this](bool allow) { saveSharing(allow); },
				&st::storiesSettingsIconShare);
			Settings::AddFootnote(content(), tr::lng_stories_allow_sharing_footer(tr::now));
		}
		showContent();
	}

	void saveSharing(bool allow) {
		// The switch shows the new state already: no rebuild from inside
		// its own signal, only back when the server refuses.
		const auto controller = this->controller();
		const auto was = _settings.allowSharing;
		_settings.allowSharing = allow;
		controller->session().data().stories().setAllowSharing(allow, this, [this](
				const Data::StorySettings &settings) {
			_settings = settings;
		}, [=, this](const Api::Error &error) {
			_settings.allowSharing = was;
			Ui::PostponeCall(this, [this] { build(); });
			Support::ShowError(controller, error);
		});
	}

	void save(const QString &replies) {
		const auto controller = this->controller();
		const auto was = _settings.replies;
		_settings.replies = replies;
		build();
		controller->session().data().stories().updateSettings(replies, this, [this](
				const Data::StorySettings &settings) {
			_settings = settings;
			build();
		}, [=, this](const Api::Error &error) {
			_settings.replies = was;
			build();
			Support::ShowError(controller, error);
		});
	}

	Data::StorySettings _settings;
	bool _built = false;

};

} // namespace

Window::SectionFactory SettingsFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(new StoriesPage(parent, controller));
	};
}

} // namespace Stories
