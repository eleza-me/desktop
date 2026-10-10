#include "profile/profile_follow_list.h"

#include "data/data_graph.h"
#include "data/data_notifications.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "profile/profile_common.h"
#include "profile/profile_people_list.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/top_bar.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

#include <QtGui/QKeyEvent>
#include <QtWidgets/QTextEdit>

namespace Profile {
namespace {

constexpr auto kFilterEnough = 20;
constexpr auto kFilterMaxPages = 10;

} // namespace

Window::SectionFactory FollowListFactory(
		const Data::Profile &profile,
		FollowKind kind) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new FollowListSection(parent, controller, profile, kind));
	};
}

Window::SectionFactory FollowRequestsFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new FollowRequestsSection(parent, controller));
	};
}

FollowListSection::FollowListSection(
	QWidget *parent,
	Window::SessionController *controller,
	const Data::Profile &profile,
	FollowKind kind)
: SectionWidget(parent, controller)
, _profile(profile)
, _kind(kind)
, _bar(Ui::CreateChild<Ui::TopBar>(this))
, _tabs(Ui::CreateChild<Ui::TabBar>(this, st::profileListTabs))
, _search(Ui::CreateChild<Ui::SearchField>(this))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this)) {
	setTopBar(_bar);
	_tabs->setStretch(true);
	_wrap = _scroll->setOwnedWidget(
		Ui::object_ptr<CenteredWrap>(_scroll, st::profileColumnWidth));
	_list = _wrap->setChild(Ui::object_ptr<PeopleList>(
		_wrap,
		controller,
		PeopleList::Mode::Follow));
	_scroll->setNearEdgeThreshold(st::profileLoadMoreThreshold);
	_skeleton = Ui::CreateChild<Ui::SkeletonList>(
		this,
		Ui::SkeletonList::Kind::Chats);

	refreshTitles();
	_tabs->setActive(
		(_kind == FollowKind::Followers) ? 0 : 1,
		anim::type::instant);

	connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		const auto kind = (index == 0)
			? FollowKind::Followers
			: FollowKind::Following;
		if (_kind != kind) {
			_kind = kind;
			refreshTitles();
			reload();
		}
	});
	connect(_search, &Ui::SearchField::changed, this, [this](const QString &text) {
		_query = text.trimmed().toLower();
		_list->setFilter(_query);
		refreshState();
		checkLoadMore();
	});
	connect(_search, &Ui::SearchField::submitted, this, [this] {
		if (!_list->chooseSelected()) {
			_list->selectSkip(1);
			_list->chooseSelected();
		}
	});
	_search->field()->rawTextEdit()->installEventFilter(this);
	connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
		if (!_loading && !_cursor.isEmpty()) {
			loadPage();
		}
	});
	connect(
		_list,
		&PeopleList::scrollToRequested,
		this,
		[this](int top, int bottom) { _scroll->scrollToY(top, bottom); });
	connect(
		_list,
		&PeopleList::removeFollowerRequested,
		this,
		[this](const QString &userId, const QString &name) {
			confirmRemove(userId, name);
		});
	connect(
		&controller->session().data().users(),
		&Data::Users::meChanged,
		this,
		[this] {
			if (mine()) {
				const auto &me = this->controller()->session().data().users().me();
				_profile.followers = me.followers;
				_profile.following = me.following;
				refreshTitles();
			}
		});
	reload();
}

QString FollowListSection::sectionKey() const {
	return u"profile/follows/"_s
		+ (_profile.id.isEmpty() ? _profile.handle : _profile.id);
}

void FollowListSection::setInnerFocus() {
	_search->setFocusFast();
}

bool FollowListSection::onBackRequest() {
	if (!_search->text().isEmpty()) {
		_search->clear();
		return true;
	}
	return false;
}

bool FollowListSection::mine() const {
	return !_profile.id.isEmpty()
		&& (_profile.id == controller()->session().data().users().myId());
}

void FollowListSection::refreshTitles() {
	_bar->setTitle(!_profile.displayName.isEmpty()
		? _profile.displayName
		: !_profile.handle.isEmpty()
		? Lang::Handle(_profile.handle)
		: (_kind == FollowKind::Followers)
		? tr::lng_profile_followers_title(tr::now)
		: tr::lng_profile_following_title(tr::now));
	const auto known = !_profile.id.isEmpty() && !_profile.displayName.isEmpty();
	const auto active = _tabs->active();
	_tabs->setTabs({
		known
			? tr::lng_profile_followers_count(
				tr::now,
				lt_n,
				Lang::CompactCount(_profile.followers))
			: tr::lng_profile_followers_title(tr::now),
		known
			? tr::lng_profile_following_count(
				tr::now,
				lt_n,
				Lang::CompactCount(_profile.following))
			: tr::lng_profile_following_title(tr::now),
	});
	_tabs->setActive(
		(_tabs->count() > active) ? active : 0,
		anim::type::instant);
	_search->field()->setPlaceholder((_kind == FollowKind::Followers)
		? tr::lng_profile_search_followers(tr::now)
		: tr::lng_profile_search_following(tr::now));
	_list->setRemovable(mine() && (_kind == FollowKind::Followers));
	if (width() > 0) {
		_tabs->resizeToWidth(_tabs->width());
	}
}

void FollowListSection::reload() {
	++_generation;
	_cursor = QString();
	_pages = 0;
	_loading = false;
	_loadedOnce = false;
	_error = std::nullopt;
	_list->setPeople({});
	_list->setLoadingMore(false);
	_scroll->scrollToY(0);
	if (_profile.id.isEmpty() || _profile.displayName.isEmpty()) {
		loadProfile(_generation);
	} else {
		loadPage();
	}
	refreshState();
}

void FollowListSection::loadProfile(int generation) {
	_loading = true;
	auto &graph = controller()->session().data().graph();
	const auto done = [=, this](const Data::Profile &profile) {
		if (generation != _generation) {
			return;
		}
		_profile = profile;
		_loading = false;
		refreshTitles();
		loadPage();
		refreshState();
	};
	const auto fail = [=, this](const Api::Error &error) {
		if (generation == _generation) {
			failed(error);
		}
	};
	if (!_profile.id.isEmpty()) {
		graph.profile(_profile.id, this, done, fail);
	} else {
		graph.profileByHandle(_profile.handle, this, done, fail);
	}
}

void FollowListSection::loadPage() {
	if (_loading || _profile.id.isEmpty()) {
		return;
	}
	_loading = true;
	_error = std::nullopt;
	const auto generation = _generation;
	auto &graph = controller()->session().data().graph();
	const auto done = [=, this](const Data::GraphList &page) {
		if (generation == _generation) {
			pageLoaded(page);
		}
	};
	const auto fail = [=, this](const Api::Error &error) {
		if (generation == _generation) {
			failed(error);
		}
	};
	if (_kind == FollowKind::Followers) {
		graph.followers(_profile.id, _cursor, this, done, fail);
	} else {
		graph.following(_profile.id, _cursor, this, done, fail);
	}
	_list->setLoadingMore(_list->totalCount() > 0);
	refreshState();
}

void FollowListSection::pageLoaded(const Data::GraphList &page) {
	_loading = false;
	_loadedOnce = true;
	++_pages;
	_cursor = page.nextCursor;
	_list->setLoadingMore(false);
	_list->appendPeople(page.users);
	refreshState();
	checkLoadMore();
}

void FollowListSection::failed(const Api::Error &error) {
	_loading = false;
	_error = error;
	_list->setLoadingMore(false);
	if (_list->totalCount() > 0) {
		// Rows stay; the next scroll to the end tries again.
		Support::ShowError(controller(), error, tr::lng_error_load(tr::now));
	}
	refreshState();
}

// A filter that matches only a few of the loaded rows keeps paging (up to ten
// pages), and so does a list that does not fill the window yet.
void FollowListSection::checkLoadMore() {
	if (_loading || _cursor.isEmpty() || _error) {
		return;
	}
	const auto filtering = !_query.isEmpty()
		&& (_list->shownCount() < kFilterEnough)
		&& (_pages < kFilterMaxPages);
	const auto shortList = _query.isEmpty()
		&& (_wrap->height() <= _scroll->height());
	if (filtering || shortList) {
		loadPage();
	}
}

void FollowListSection::refreshState() {
	delete _placeholder;
	_placeholder = nullptr;
	const auto empty = (_list->shownCount() == 0);
	const auto loading = _loading
		|| (!_loadedOnce && !_error)
		|| (!_cursor.isEmpty()
			&& !_query.isEmpty()
			&& _pages < kFilterMaxPages
			&& !_error);
	_skeleton->setVisible(empty && loading && _query.isEmpty());
	if (empty && !(loading && _query.isEmpty())) {
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		const auto controller = this->controller();
		if (_error) {
			descriptor.icon = &st::emptyIconError;
			descriptor.title = tr::lng_error_load(tr::now);
			descriptor.text = Support::ErrorText(*_error, QString());
			descriptor.buttonText = tr::lng_retry(tr::now);
			descriptor.buttonCallback = [this] { reload(); };
		} else if (!_query.isEmpty()) {
			descriptor.icon = &st::emptyIconSearch;
			descriptor.title = loading
				? tr::lng_profile_searching(tr::now)
				: tr::lng_profile_nobody_found(tr::now);
		} else if (mine()) {
			descriptor.icon = &st::profileEmptyFollowIcon;
			descriptor.title = (_kind == FollowKind::Followers)
				? tr::lng_profile_nobody_follows_yet(tr::now)
				: tr::lng_eli_dont_follow_anyone_yet(tr::now);
			descriptor.text = (_kind == FollowKind::Followers)
				? tr::lng_profile_followers_empty_text(tr::now)
				: tr::lng_profile_following_empty_text(tr::now);
			descriptor.buttonText = tr::lng_profile_find_people(tr::now);
			descriptor.buttonCallback = [controller] {
				controller->openRoute(Window::Routes::Search());
			};
		} else {
			descriptor.icon = &st::profileEmptyFollowIcon;
			descriptor.title = tr::lng_profile_nobody_yet(tr::now);
		}
		_placeholder = Ui::CreateChild<Ui::EmptyPlaceholder>(
			this,
			std::move(descriptor));
		_placeholder->show();
	}
	updateGeometries();
}

void FollowListSection::confirmRemove(
		const QString &userId,
		const QString &name) {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_profile_remove_follower_title(tr::now, lt_name, name);
	args.text = tr::lng_profile_remove_follower_text(tr::now);
	args.confirmText = tr::lng_delete(tr::now);
	args.cancelText = tr::lng_never_mind(tr::now);
	args.destructive = true;
	const auto weak = QPointer<FollowListSection>(this);
	const auto controller = this->controller();
	args.confirmed = [=](std::function<void()> close) {
		close();
		if (!weak) {
			return;
		}
		controller->session().data().graph().removeFollower(
			userId,
			weak.data(),
			[=](const Data::GraphState &) {
				weak->_list->removePerson(userId);
				weak->_profile.followers = std::max(
					weak->_profile.followers - 1,
					qint64(0));
				weak->refreshTitles();
				weak->refreshState();
				controller->showToast(tr::lng_profile_remove_follower_done(
					tr::now,
					lt_name,
					name));
				controller->session().data().users().refreshMe();
			},
			[=](const Api::Error &error) {
				Support::ShowError(
					controller,
					error,
					tr::lng_profile_action_failed(tr::now));
			});
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void FollowListSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void FollowListSection::updateGeometries() {
	if (width() <= 0) {
		return;
	}
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto column = CenteredColumn(width(), st::profileColumnWidth);
	auto top = _bar->height();
	_tabs->resizeToWidth(column.width);
	_tabs->move(column.left, top);
	top += _tabs->height();
	const auto &padding = st::profileSearchPadding;
	_search->resizeToWidth(column.width - padding.left() - padding.right());
	_search->moveToLeft(column.left + padding.left(), top + padding.top());
	top += padding.top() + _search->height() + padding.bottom();
	const auto rest = std::max(height() - top, 0);
	_scroll->setGeometry(0, top, width(), rest);
	_skeleton->setGeometry(column.left, top, column.width, rest);
	if (_placeholder) {
		_placeholder->setGeometry(column.left, top, column.width, rest);
		_placeholder->raise();
	}
}

void FollowListSection::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Down) {
		_list->selectSkip(1);
	} else if (e->key() == Qt::Key_Up) {
		_list->selectSkip(-1);
	} else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		_list->chooseSelected();
	} else {
		SectionWidget::keyPressEvent(e);
	}
}

bool FollowListSection::eventFilter(QObject *object, QEvent *event) {
	if (event->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(event)->key();
		if (key == Qt::Key_Down || key == Qt::Key_Up) {
			_list->selectSkip((key == Qt::Key_Down) ? 1 : -1);
			return true;
		}
	}
	return SectionWidget::eventFilter(object, event);
}

FollowRequestsSection::FollowRequestsSection(
	QWidget *parent,
	Window::SessionController *controller)
: SectionWidget(parent, controller)
, _bar(Ui::CreateChild<Ui::TopBar>(this))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this)) {
	setTopBar(_bar);
	_bar->setTitle(tr::lng_eli_follow_requests(tr::now));
	_wrap = _scroll->setOwnedWidget(
		Ui::object_ptr<CenteredWrap>(_scroll, st::profileColumnWidth));
	_list = _wrap->setChild(Ui::object_ptr<PeopleList>(
		_wrap,
		controller,
		PeopleList::Mode::Requests));
	_skeleton = Ui::CreateChild<Ui::SkeletonList>(
		this,
		Ui::SkeletonList::Kind::Chats);
	connect(
		_list,
		&PeopleList::scrollToRequested,
		this,
		[this](int top, int bottom) { _scroll->scrollToY(top, bottom); });
	connect(
		_list,
		&PeopleList::requestAnswered,
		this,
		[this](const QString &userId, bool accepted) {
			const auto name = this->controller()->session().data().users().peek(
				userId).displayName;
			_list->removePerson(userId);
			refreshState();
			this->controller()->showToast((accepted && !name.isEmpty())
				? tr::lng_profile_request_accepted(tr::now, lt_name, name)
				: accepted
				? tr::lng_profile_request_accepted_plain(tr::now)
				: tr::lng_profile_request_declined(tr::now));
			auto &data = this->controller()->session().data();
			if (accepted) {
				data.users().refreshMe();
			}
			// The badge of the requests entry follows the server's counter.
			data.notifications().refreshUnread();
		});
	reload();
}

QString FollowRequestsSection::sectionKey() const {
	return u"profile/requests"_s;
}

void FollowRequestsSection::reload() {
	_loading = true;
	_error = std::nullopt;
	refreshState();
	auto &data = controller()->session().data();
	data.graph().followRequests(this, [this](const QVector<Data::Profile> &list) {
		_loading = false;
		this->controller()->session().data().users().seed(list);
		_list->setPeople(list);
		refreshState();
	}, [this](const Api::Error &error) {
		_loading = false;
		_error = error;
		refreshState();
	});
}

void FollowRequestsSection::refreshState() {
	delete _placeholder;
	_placeholder = nullptr;
	const auto empty = (_list->shownCount() == 0);
	_skeleton->setVisible(empty && _loading);
	if (empty && !_loading) {
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		if (_error) {
			descriptor.icon = &st::emptyIconError;
			descriptor.title = tr::lng_error_load(tr::now);
			descriptor.text = Support::ErrorText(*_error, QString());
			descriptor.buttonText = tr::lng_retry(tr::now);
			descriptor.buttonCallback = [this] { reload(); };
		} else {
			descriptor.icon = &st::profileEmptyRequestsIcon;
			descriptor.title = tr::lng_eli_no_new_requests(tr::now);
		}
		_placeholder = Ui::CreateChild<Ui::EmptyPlaceholder>(
			this,
			std::move(descriptor));
		_placeholder->show();
	}
	updateGeometries();
}

void FollowRequestsSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void FollowRequestsSection::updateGeometries() {
	if (width() <= 0) {
		return;
	}
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto column = CenteredColumn(width(), st::profileColumnWidth);
	const auto top = _bar->height();
	const auto rest = std::max(height() - top, 0);
	_scroll->setGeometry(0, top, width(), rest);
	_skeleton->setGeometry(column.left, top, column.width, rest);
	if (_placeholder) {
		_placeholder->setGeometry(column.left, top, column.width, rest);
		_placeholder->raise();
	}
}

void FollowRequestsSection::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Down) {
		_list->selectSkip(1);
	} else if (e->key() == Qt::Key_Up) {
		_list->selectSkip(-1);
	} else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		_list->chooseSelected();
	} else {
		SectionWidget::keyPressEvent(e);
	}
}

} // namespace Profile
