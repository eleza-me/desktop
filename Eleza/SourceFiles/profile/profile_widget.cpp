#include "profile/profile_widget.h"

#include "data/data_calls.h"
#include "data/data_chats.h"
#include "data/data_graph.h"
#include "data/data_notifications.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_image_viewer.h"
#include "profile/profile_boxes.h"
#include "profile/profile_common.h"
#include "profile/profile_cover.h"
#include "profile/profile_edit.h"
#include "profile/profile_follow.h"
#include "profile/profile_follow_list.h"
#include "profile/profile_posts.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "window/window_calls.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>

#include <map>

namespace Profile {
namespace {

[[nodiscard]] QString SourceName(PostsTab tab) {
	switch (tab) {
	case PostsTab::Posts: return u"user"_s;
	case PostsTab::Replies: return u"user_replies"_s;
	case PostsTab::Media: return u"user_media"_s;
	case PostsTab::Bookmarks: return u"bookmarks"_s;
	}
	return QString();
}

// The note over the bookmarks list: «فقط خودت نشان‌شده‌هایت را می‌بینی».
class NoteRow final : public Ui::RpWidget {
public:
	NoteRow(QWidget *parent, const QString &text)
	: RpWidget(parent)
	, _text(text) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::profileNoteHeight;
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &font = st::profilePostMetaFont;
		const auto &icon = st::profileNoteIcon;
		const auto left = int(st::profilePostPadding.left());
		icon.paint(p, left, (height() - icon.height()) / 2, width());
		p.setFont(font->f);
		p.setPen(st::textTertiary);
		const auto textLeft = left + icon.width() + st::profileBadgeSkip;
		p.drawTextLeft(
			textLeft,
			(height() - font->height) / 2,
			width(),
			font->elided(_text, std::max(width() - textLeft - left, 0)));
	}

private:
	QString _text;

};

} // namespace

// Everything below the top bar; the scroll area's widget.
class Inner final : public Ui::RpWidget {
public:
	Inner(QWidget *parent, Window::SessionController *controller);

	[[nodiscard]] Cover *cover() const {
		return _cover;
	}
	void setProfile(const Data::Profile &profile, const ButtonModel &model);
	void setConversation(const std::optional<Data::OpenConversation> &value);
	void setTab(PostsTab tab);
	void setMinHeight(int height);
	void loadMore();
	void setUnblock(Fn<void()> unblock, Fn<void()> viewProfile) {
		_unblock = std::move(unblock);
		_viewProfile = std::move(viewProfile);
	}
	// Where the tab bar starts (to scroll the lists to their top).
	[[nodiscard]] int tabsTop() const {
		return _tabsTop;
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	[[nodiscard]] QVector<PostsTab> tabs() const;
	void rebuildContent();
	void showCurrentList();
	void relayout();
	void updateVisible();
	[[nodiscard]] Ui::object_ptr<Ui::RpWidget> makeEmpty(PostsTab tab);

	Window::SessionController *_controller = nullptr;
	Cover *_cover = nullptr;
	ConversationCard *_card = nullptr;
	Ui::TabBar *_tabs = nullptr;
	NoteRow *_note = nullptr;
	StateBlock *_state = nullptr;
	Ui::EmptyPlaceholder *_empty = nullptr;
	std::map<PostsTab, Window::EmbeddedList*> _lists;
	std::map<PostsTab, bool> _emptyLists;
	Data::Profile _profile;
	ButtonModel _model;
	QString _contentKey;
	PostsTab _tab = PostsTab::Posts;
	Fn<void()> _unblock;
	Fn<void()> _viewProfile;
	int _minHeight = 0;
	int _tabsTop = 0;
	int _columnLeft = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	bool _inResize = false;

};

Inner::Inner(QWidget *parent, Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _cover(Ui::CreateChild<Cover>(this, controller))
, _card(Ui::CreateChild<ConversationCard>(this))
, _tabs(Ui::CreateChild<Ui::TabBar>(this, st::profileTabs)) {
	_card->hide();
	_tabs->hide();
	_tabs->setStretch(true);
	_card->setClickedCallback([this] {
		_controller->openRoute(
			Window::Routes::Conversation(_card->conversationId()));
	});
	connect(_cover, &Ui::RpWidget::heightChanged, this, [this] { relayout(); });
	connect(_card, &Ui::RpWidget::heightChanged, this, [this] { relayout(); });
	connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		const auto list = tabs();
		if (index >= 0 && index < list.size()) {
			setTab(list[index]);
		}
	});
}

QVector<PostsTab> Inner::tabs() const {
	auto result = QVector<PostsTab>{
		PostsTab::Posts,
		PostsTab::Replies,
		PostsTab::Media,
	};
	if (_model.bookmarksTab) {
		result.push_back(PostsTab::Bookmarks);
	}
	return result;
}

void Inner::setProfile(const Data::Profile &profile, const ButtonModel &model) {
	_profile = profile;
	_model = model;
	_cover->setProfile(profile, model);
	// The lists are rebuilt only when what they show changes.
	const auto key = profile.id
		+ QLatin1Char('/')
		+ QString::number(int(model.content))
		+ (model.bookmarksTab ? u"/b"_s : QString());
	if (_contentKey != key) {
		_contentKey = key;
		rebuildContent();
	}
	_card->setVisible(model.conversationCard && !_card->conversationId().isEmpty());
	relayout();
}

void Inner::setConversation(
		const std::optional<Data::OpenConversation> &value) {
	if (value) {
		_card->setConversation(*value);
	}
	_card->setVisible(value.has_value() && _model.conversationCard);
	relayout();
}

void Inner::rebuildContent() {
	for (const auto &[tab, list] : _lists) {
		delete list;
	}
	_lists.clear();
	_emptyLists.clear();
	delete _state;
	_state = nullptr;
	delete _empty;
	_empty = nullptr;
	delete _note;
	_note = nullptr;

	const auto content = _model.content;
	_tabs->setVisible(content == Content::Tabs);
	if (content == Content::Tabs) {
		const auto list = tabs();
		auto titles = QStringList();
		for (const auto tab : list) {
			switch (tab) {
			case PostsTab::Posts: titles.push_back(tr::lng_search_posts(tr::now)); break;
			case PostsTab::Replies: titles.push_back(tr::lng_eli_replies(tr::now)); break;
			case PostsTab::Media: titles.push_back(tr::lng_profile_media(tr::now)); break;
			case PostsTab::Bookmarks: titles.push_back(tr::lng_eli_bookmarks_title(tr::now)); break;
			}
		}
		_tabs->setTabs(titles);
		if (!list.contains(_tab)) {
			_tab = PostsTab::Posts;
		}
		_tabs->setActive(int(list.indexOf(_tab)), anim::type::instant);
		_note = Ui::CreateChild<NoteRow>(
			this,
			tr::lng_eli_only_can_see_saved_posts(tr::now));
		_note->hide();
		showCurrentList();
		return;
	}
	auto descriptor = StateBlock::Descriptor();
	switch (content) {
	case Content::Locked:
	case Content::LockedRequested:
		descriptor.icon = &st::profileStateLockIcon;
		descriptor.title = tr::lng_profile_account_private(tr::now);
		descriptor.text = (content == Content::LockedRequested)
			? tr::lng_profile_locked_requested(
				tr::now,
				lt_name,
				_profile.displayName.section(QLatin1Char(' '), 0, 0))
			: tr::lng_profile_locked_text(tr::now);
		break;
	case Content::Blocked:
		descriptor.icon = &st::profileStateBlockIcon;
		descriptor.title = tr::lng_profile_blocked_title(
			tr::now,
			lt_name,
			_profile.displayName);
		descriptor.text = tr::lng_profile_blocked_text(tr::now);
		descriptor.buttonText = tr::lng_unblock(tr::now);
		descriptor.buttonLight = false;
		descriptor.buttonCallback = [this] {
			if (_unblock) {
				_unblock();
			}
		};
		break;
	case Content::Unblocked:
		descriptor.icon = &st::profileStateBlockIcon;
		descriptor.title = tr::lng_profile_unblocked_toast(
			tr::now,
			lt_name,
			_profile.displayName);
		descriptor.text = tr::lng_profile_unblocked_text(tr::now);
		descriptor.buttonText = tr::lng_profile_view_profile(tr::now);
		descriptor.buttonCallback = [this] {
			if (_viewProfile) {
				_viewProfile();
			}
		};
		break;
	case Content::Tabs:
	case Content::Deleted:
	case Content::None: return;
	}
	_state = Ui::CreateChild<StateBlock>(this, std::move(descriptor));
	_state->show();
}

void Inner::setTab(PostsTab tab) {
	if (!tabs().contains(tab)) {
		_tab = tab; // applied when the tabs appear
		return;
	}
	const auto changed = (_tab != tab);
	_tab = tab;
	if (_model.content != Content::Tabs) {
		return;
	}
	const auto index = int(tabs().indexOf(tab));
	if (_tabs->active() != index) {
		_tabs->setActive(index);
	}
	if (changed || !_lists.contains(tab)) {
		showCurrentList();
	}
}

void Inner::showCurrentList() {
	if (!_lists.contains(_tab)) {
		auto list = CreatePostsList(
			this,
			_controller,
			SourceName(_tab),
			_profile.id);
		const auto raw = list.release();
		const auto tab = _tab;
		_lists.emplace(tab, raw);
		connect(raw, &Window::EmbeddedList::heightChanged, this, [this] {
			relayout();
			// A page that does not fill the window yet (the media filter
			// keeps few rows) never brings a scroll: ask for the next one.
			if (height() <= _minHeight) {
				Ui::PostponeCall(this, [this] {
					if (height() <= _minHeight) {
						loadMore();
					}
				});
			}
		});
		connect(raw, &Window::EmbeddedList::emptyChanged, this, [=, this](
				bool empty) {
			_emptyLists[tab] = empty;
			if (_tab == tab) {
				showCurrentList();
			}
		});
	}
	for (const auto &[tab, list] : _lists) {
		list->setVisible(tab == _tab && !_emptyLists[tab]);
	}
	delete _empty;
	_empty = nullptr;
	if (_emptyLists[_tab]) {
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		switch (_tab) {
		case PostsTab::Posts:
			descriptor.icon = &st::emptyIconEli;
			descriptor.title = tr::lng_profile_no_posts_yet(tr::now);
			break;
		case PostsTab::Replies:
			descriptor.icon = &st::profileEmptyRepliesIcon;
			descriptor.title = tr::lng_profile_no_replies_yet(tr::now);
			break;
		case PostsTab::Media:
			descriptor.icon = &st::profileEmptyMediaIcon;
			descriptor.title = tr::lng_profile_no_media_yet(tr::now);
			break;
		case PostsTab::Bookmarks:
			descriptor.icon = &st::emptyIconBookmarks;
			descriptor.title = tr::lng_eli_havent_saved_anything(tr::now);
			break;
		}
		_empty = Ui::CreateChild<Ui::EmptyPlaceholder>(
			this,
			std::move(descriptor));
		_empty->show();
	}
	if (_note) {
		_note->setVisible(_tab == PostsTab::Bookmarks);
	}
	relayout();
}

void Inner::setMinHeight(int height) {
	if (_minHeight != height) {
		_minHeight = height;
		relayout();
	}
}

void Inner::loadMore() {
	if (_model.content != Content::Tabs) {
		return;
	}
	const auto i = _lists.find(_tab);
	if (i != _lists.end() && !_emptyLists[_tab]) {
		i->second->loadMore();
	}
}

void Inner::relayout() {
	if (!_inResize && width() > 0) {
		resizeToWidth(width());
		updateVisible();
	}
}

int Inner::resizeGetHeight(int newWidth) {
	_inResize = true;
	const auto column = CenteredColumn(newWidth, st::profileColumnWidth);
	_columnLeft = column.left;
	auto y = 0;
	_cover->resizeToWidth(column.width);
	_cover->move(column.left, y);
	y += _cover->height();
	if (!_card->isHidden()) {
		const auto &margin = st::profileCardMargin;
		_card->resizeToWidth(
			std::max(column.width - margin.left() - margin.right(), 0));
		_card->moveToLeft(
			column.left + margin.left(),
			y + margin.top(),
			newWidth);
		y += margin.top() + _card->height() + margin.bottom();
	}
	_tabsTop = y;
	if (_model.content == Content::Tabs) {
		_tabs->resizeToWidth(column.width);
		_tabs->move(column.left, std::max(_tabsTop, _visibleTop));
		y += _tabs->height();
		if (_note && !_note->isHidden()) {
			_note->resizeToWidth(column.width);
			_note->move(column.left, y);
			y += _note->height();
		}
		const auto i = _lists.find(_tab);
		if (_empty) {
			const auto height = std::max(
				_empty->contentHeight(column.width),
				int(st::profileEmptyHeight));
			_empty->setGeometry(column.left, y, column.width, height);
			y += height;
		} else if (i != _lists.end()) {
			i->second->resizeToWidth(column.width);
			i->second->move(column.left, y);
			y += i->second->height();
		}
		_tabs->raise();
	} else if (_state) {
		_state->resizeToWidth(column.width);
		_state->move(column.left, y);
		y += _state->height();
	}
	_inResize = false;
	return std::max(y + int(st::profileBottomSkip), _minHeight);
}

void Inner::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
	updateVisible();
}

void Inner::updateVisible() {
	if (_model.content != Content::Tabs) {
		return;
	}
	// The tab bar sticks to the top of the viewport once the header is gone.
	_tabs->move(_columnLeft, std::max(_tabsTop, _visibleTop));
	const auto i = _lists.find(_tab);
	if (i == _lists.end() || i->second->isHidden()) {
		return;
	}
	const auto list = i->second;
	const auto top = std::clamp(
		_visibleTop + _tabs->height() - list->y(),
		0,
		list->height());
	const auto bottom = std::clamp(
		_visibleBottom - list->y(),
		top,
		list->height());
	list->setVisibleRange(top, bottom);
	list->setVisibleTopBottom(top, bottom);
}

Window::SectionFactory Factory(const Source &source, PostsTab tab) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new Widget(parent, controller, source, tab));
	};
}

Widget::Widget(
	QWidget *parent,
	Window::SessionController *controller,
	const Source &source,
	PostsTab tab)
: SectionWidget(parent, controller)
, _source(source)
, _initialTab(tab)
, _bar(Ui::CreateChild<Ui::TopBar>(this))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this))
, _skeleton(Ui::CreateChild<Ui::SkeletonList>(
	this,
	Ui::SkeletonList::Kind::Posts)) {
	setTopBar(_bar);
	auto &data = controller->session().data();
	// A link to myself is my own profile.
	const auto &me = data.users().me();
	if ((_source.type == Source::Type::UserId && _source.value == data.users().myId())
		|| (_source.type == Source::Type::Handle
			&& !me.handle.isEmpty()
			&& _source.value.compare(me.handle, Qt::CaseInsensitive) == 0)) {
		_source = Source::Me();
	}

	_inner = _scroll->setOwnedWidget(Ui::object_ptr<Inner>(_scroll, controller));
	_scroll->setNearEdgeThreshold(st::profileLoadMoreThreshold);
	_inner->setTab(_initialTab);
	_inner->setUnblock([this] { toggleBlock(); }, [this] {
		_justUnblocked = false;
		load();
	});
	connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
		_inner->loadMore();
	});

	const auto cover = _inner->cover();
	connect(cover, &Cover::photoClicked, this, [this] { openPhoto(); });
	connect(cover, &Cover::followClicked, this, [this] { toggleFollow(); });
	connect(cover, &Cover::messageClicked, this, [this] { openChat(); });
	connect(cover, &Cover::moreClicked, this, [this] { showMenu(); });
	connect(cover, &Cover::handleClicked, this, [this] { copyHandle(); });
	connect(cover, &Cover::editClicked, this, [this] {
		this->controller()->showMainSection(EditFactory());
	});
	connect(cover, &Cover::shareClicked, this, [this] {
		ShowShareBox(this->controller(), _profile);
	});
	connect(cover, &Cover::followersClicked, this, [this] {
		this->controller()->showMainSection(
			FollowListFactory(_profile, FollowKind::Followers));
	});
	connect(cover, &Cover::followingClicked, this, [this] {
		this->controller()->showMainSection(
			FollowListFactory(_profile, FollowKind::Following));
	});
	connect(cover, &Cover::requestsClicked, this, [this] {
		this->controller()->showMainSection(FollowRequestsFactory());
	});

	if (_source.type == Source::Type::Me) {
		_bell = _bar->addAction(st::topBarBellButton);
		_bell->setClickedCallback([this] {
			this->controller()->openRoute(Window::Routes::EliNotifications());
		});
		Ui::InstallTooltip(_bell, [] {
			return tr::lng_settings_notifications(tr::now);
		});
		const auto settings = _bar->addAction(st::topBarSettingsButton);
		settings->setClickedCallback([this] {
			this->controller()->openRoute(Window::Routes::Settings());
		});
		Ui::InstallTooltip(settings, [] { return tr::lng_settings(tr::now); });
		connect(&data.users(), &Data::Users::meChanged, this, [this] {
			const auto &me = this->controller()->session().data().users().me();
			if (me.valid()) {
				loaded(me);
			}
		});
		connect(
			&data.notifications(),
			&Data::Notifications::unreadChanged,
			this,
			[this] { refreshRequests(); });
	}
	connect(
		&data.graph(),
		&Data::Graph::relationshipChanged,
		this,
		[this](const QString &userId, const Data::GraphState &state) {
			if (_loaded && !isMe() && userId == _profile.id) {
				applyRelationship(state);
			}
		});
	connect(
		&data.users(),
		&Data::Users::changed,
		this,
		[this](const QStringList &ids) {
			if (!_loaded || isMe() || !ids.contains(_profile.id)) {
				return;
			}
			const auto card = this->controller()->session().data().users().peek(
				_profile.id);
			if (card.valid()
				&& !card.deleted
				&& (card.displayName != _profile.displayName
					|| card.avatarId != _profile.avatarId
					|| card.verified != _profile.verified)) {
				_profile.displayName = card.displayName;
				_profile.avatarId = card.avatarId;
				_profile.verified = card.verified;
				apply();
			}
		});
	connect(
		&data.chats(),
		&Data::Chats::presenceChanged,
		this,
		[this](const QString &userId) {
			if (_loaded && userId == _profile.id) {
				refreshPresence();
			}
		});
	refreshBar();
	load();
}

Widget::~Widget() {
	if (_watchingPresence) {
		controller()->session().data().chats().unwatchPresence({ _profile.id });
	}
}

QString Widget::sectionKey() const {
	switch (_source.type) {
	case Source::Type::Me: return u"profile/me"_s;
	case Source::Type::UserId: return u"profile/user/"_s + _source.value;
	case Source::Type::Handle:
		return u"profile/handle/"_s + _source.value.toLower();
	}
	return QString();
}

QString Widget::restoreRoute() const {
	switch (_source.type) {
	case Source::Type::Me: return QString();
	case Source::Type::UserId: return Window::Routes::User(_source.value);
	case Source::Type::Handle: return Window::Routes::Handle(_source.value);
	}
	return QString();
}

void Widget::showFinished() {
	_scroll->setFocus();
}

void Widget::showTab(PostsTab tab) {
	_initialTab = tab;
	_inner->setTab(tab);
	if (_loaded && _scroll->scrollTop() > _inner->tabsTop()) {
		_scroll->scrollToY(_inner->tabsTop());
	}
}

bool Widget::isMe() const {
	return (_source.type == Source::Type::Me)
		|| (_loaded
			&& _profile.id == controller()->session().data().users().myId());
}

QString Widget::callableUserId() const {
	if (!Window::CallsSupported()
		|| !_loaded
		|| isMe()
		|| _blocked
		|| _profile.deleted
		|| _profile.isBot
		|| _profile.id.isEmpty()) {
		return QString();
	}
	const auto &chats = controller()->session().data().chats();
	for (const auto &chat : chats.list(Data::Chats::Section::All)) {
		if (chat.peerUserId == _profile.id
			&& (Data::CallAvailabilityFor(chat)
				== Data::CallAvailability::Available)) {
			return _profile.id;
		}
	}
	return QString();
}

ViewerState Widget::viewerState() const {
	auto result = ViewerState();
	result.loaded = _loaded;
	result.me = isMe();
	result.deleted = _profile.deleted;
	result.blocked = _blocked;
	result.justUnblocked = _justUnblocked;
	result.isPrivate = _profile.isPrivate;
	result.followsYou = _profile.followsYou;
	result.muted = _muted;
	result.postNotifications = _postNotifications;
	result.hasHandle = !_profile.handle.isEmpty();
	result.follow = _follow;
	return result;
}

void Widget::load() {
	if (_loading) {
		return;
	}
	_loading = true;
	_error = std::nullopt;
	_unavailable = false;
	auto &data = controller()->session().data();
	const auto done = [this](const Data::Profile &profile) {
		_loading = false;
		loaded(profile);
	};
	const auto fail = [this](const Api::Error &error) {
		_loading = false;
		failed(error);
	};
	switch (_source.type) {
	case Source::Type::Me: {
		// The store has my profile already; one fetch brings fresh counters.
		const auto &me = data.users().me();
		if (me.valid()) {
			loaded(me);
		}
		data.users().refreshMe(this, [this](const Data::Profile &) {
			_loading = false;
		}, fail);
	} break;
	case Source::Type::UserId:
		data.graph().profile(_source.value, this, done, fail);
		break;
	case Source::Type::Handle:
		data.graph().profileByHandle(_source.value, this, done, fail);
		break;
	}
	apply();
}

void Widget::loaded(const Data::Profile &profile) {
	const auto first = !_loaded;
	const auto wasVisibility = _profile.visibility;
	_profile = profile;
	_loaded = true;
	_error = std::nullopt;
	if (!isMe()) {
		_follow = FollowUiOf(profile);
		_followBusy = false;
		_blocked = profile.blockedByYou;
		if (first || wasVisibility != profile.visibility) {
			_extrasLoaded = false;
		}
		loadExtras();
	}
	apply();
}

void Widget::failed(const Api::Error &error) {
	if (_loaded) {
		// What is shown stays; my own profile came from the store.
		if (!isMe()) {
			Support::ShowError(controller(), error);
		}
		return;
	}
	_unavailable = error.notFound();
	_error = error;
	apply();
}

void Widget::loadExtras() {
	if (_extrasLoaded || _profile.deleted || isMe()) {
		return;
	}
	_extrasLoaded = true;
	const auto id = _profile.id;
	auto &data = controller()->session().data();
	data.graph().openConversationsOf(id, this, [this](
			const QVector<Data::OpenConversation> &list) {
		_inner->setConversation(list.isEmpty()
			? std::nullopt
			: std::make_optional(list.front()));
	});
	if (!_blocked) {
		data.graph().postNotifications(id, this, [this](bool enabled) {
			_postNotifications = enabled;
		});
	}
	data.graph().filters(this, [=, this](const Data::Filters &filters) {
		_muted = filters.muted.contains(id);
	});
	if (!_watchingPresence) {
		_watchingPresence = true;
		data.chats().watchPresence({ id });
	}
}

void Widget::apply() {
	refreshBar();
	const auto showProfile = _loaded && !_profile.deleted;
	delete _placeholder;
	_placeholder = nullptr;
	_skeleton->setVisible(!_loaded && !_error);
	_scroll->setVisible(showProfile);
	if (showProfile) {
		const auto model = MakeButtonModel(viewerState());
		_inner->setProfile(_profile, model);
		_inner->cover()->setFollow(_follow, _followBusy);
		refreshPresence();
		refreshRequests();
	} else if (_loaded || _error) {
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		descriptor.icon = &st::emptyIconMe;
		if (_loaded) {
			descriptor.title = tr::lng_deleted_account(tr::now);
		} else if (_unavailable) {
			descriptor.title = tr::lng_profile_unavailable_title(tr::now);
			descriptor.text = tr::lng_profile_unavailable_text(tr::now);
		} else {
			descriptor.title = tr::lng_profile_couldnt_open_profile(tr::now);
			descriptor.text = Support::ErrorText(*_error, QString());
			descriptor.buttonText = tr::lng_retry(tr::now);
			descriptor.buttonCallback = [this] { load(); };
		}
		_placeholder = Ui::CreateChild<Ui::EmptyPlaceholder>(
			this,
			std::move(descriptor));
		_placeholder->show();
	}
	updateGeometries();
}

void Widget::refreshBar() {
	const auto title = (_source.type == Source::Type::Me)
		? tr::lng_tab_me(tr::now)
		: (_loaded && _profile.deleted)
		? tr::lng_deleted_account(tr::now)
		: (_loaded && !_profile.displayName.isEmpty())
		? _profile.displayName
		: (_source.type == Source::Type::Handle)
		? LtrText(QLatin1Char('@') + _source.value)
		: QString();
	_bar->setTitle(title);
	if (_bell) {
		_bell->setDotShown(
			controller()->session().data().notifications().unread() > 0);
	}
}

void Widget::refreshPresence() {
	const auto model = MakeButtonModel(viewerState());
	if (!model.presence || _blocked) {
		_inner->cover()->setPresence(QString(), false);
		return;
	}
	const auto presence = controller()->session().data().chats().presence(
		_profile.id);
	if (!presence) {
		_inner->cover()->setPresence(QString(), false);
		return;
	}
	const auto status = (presence->status == u"last_week")
		? u"within_week"_s
		: (presence->status == u"last_month")
		? u"within_month"_s
		: (presence->status == u"offline")
		? QString()
		: presence->status;
	_inner->cover()->setPresence(
		Lang::LastSeen(status, presence->lastSeenAt),
		presence->online());
}

void Widget::refreshRequests() {
	if (!_loaded || !isMe()) {
		return;
	}
	const auto &notifications = controller()->session().data().notifications();
	const auto count = notifications.kind(u"follow_request"_s);
	_inner->cover()->setRequests(_profile.isPrivate || count > 0, count);
	if (_bell) {
		_bell->setDotShown(notifications.unread() > 0);
	}
}

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void Widget::updateGeometries() {
	if (width() <= 0) {
		return;
	}
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto top = _bar->height();
	const auto rest = std::max(height() - top, 0);
	_scroll->setGeometry(0, top, width(), rest);
	_inner->setMinHeight(rest);
	const auto column = CenteredColumn(width(), st::profileColumnWidth);
	_skeleton->setGeometry(column.left, top, column.width, rest);
	if (_placeholder) {
		_placeholder->setGeometry(column.left, top, column.width, rest);
	}
}

void Widget::keyPressEvent(QKeyEvent *e) {
	// Arrows / PageUp / PageDown / Home / End scroll wherever the focus is.
	switch (e->key()) {
	case Qt::Key_Up:
	case Qt::Key_Down:
	case Qt::Key_PageUp:
	case Qt::Key_PageDown:
	case Qt::Key_Home:
	case Qt::Key_End:
		if (_scroll->isVisible() && !_scroll->hasFocus()) {
			_scroll->setFocus();
			QCoreApplication::sendEvent(_scroll, e);
			return;
		}
		break;
	default: break;
	}
	SectionWidget::keyPressEvent(e);
}

void Widget::applyRelationship(const Data::GraphState &state) {
	const auto was = _follow;
	const auto wasBlocked = _blocked;
	_follow = FollowUiOf(state);
	_profile.followState = state.follow;
	_profile.followsYou = state.followsYou;
	_blocked = state.blockedByYou;
	_profile.blockedByYou = state.blockedByYou;
	if (wasBlocked == _blocked) {
		_profile.followers = std::max(
			_profile.followers + FollowersDelta(was, _follow),
			qint64(0));
	}
	apply();
}

void Widget::toggleFollow() {
	if (!_loaded || _followBusy || isMe()) {
		return;
	}
	auto target = FollowTarget();
	target.id = _profile.id;
	target.name = _profile.displayName;
	target.isPrivate = _profile.isPrivate;
	target.followsYou = _profile.followsYou;
	target.state = _follow;
	PerformFollowToggle(controller(), this, target, [this](
			FollowUi state,
			bool busy) {
		const auto was = _follow;
		const auto wasBusy = _followBusy;
		_follow = state;
		_followBusy = busy;
		_profile.followState = (state == FollowUi::Following)
			? u"following"_s
			: (state == FollowUi::Requested)
			? u"requested"_s
			: u"none"_s;
		_profile.followers = std::max(
			_profile.followers + FollowersDelta(was, state),
			qint64(0));
		apply();
		// A private account that let me in right away: its posts and the
		// full profile are readable now.
		if (wasBusy
			&& !busy
			&& state == FollowUi::Following
			&& _profile.visibility == u"limited") {
			load();
		}
	});
}

void Widget::openChat() {
	if (!_loaded || _openingChat) {
		return;
	}
	_openingChat = true;
	const auto controller = this->controller();
	controller->session().data().chats().openDirect(
		_profile.id,
		this,
		[=, this](const Data::Chat &chat) {
			_openingChat = false;
			// Switches the tab: nothing after this line.
			controller->openRoute(Window::Routes::Chat(chat.id));
		},
		[=, this](const Api::Error &error) {
			_openingChat = false;
			Support::ShowError(controller, error);
		});
}

void Widget::openPhoto() {
	if (!_loaded || _profile.avatarId.isEmpty()) {
		return;
	}
	auto &session = controller()->session();
	auto item = Media::ViewerItem();
	auto options = Support::ImageOptions();
	options.original = true;
	item.source = Support::MakeImageSource(
		&session,
		Data::MediaRef::Avatar(_profile.id, _profile.avatarId),
		Data::MediaVariant::Full,
		options);
	item.preview = Support::UserpicSource(&session, _profile);
	item.caption = _profile.displayName;
	item.saveName = (_profile.handle.isEmpty() ? _profile.id : _profile.handle)
		+ u".jpg"_s;
	auto items = std::vector<Media::ViewerItem>();
	items.push_back(std::move(item));
	Media::ShowImageViewer(controller()->window(), std::move(items), 0);
}

void Widget::copyHandle() {
	if (_profile.handle.isEmpty()) {
		return;
	}
	QGuiApplication::clipboard()->setText(QLatin1Char('@') + _profile.handle);
	controller()->showToast(tr::lng_profile_handle_copied(tr::now));
}

void Widget::copyLink() {
	QGuiApplication::clipboard()->setText(ProfileLink(_profile.handle));
	controller()->showToast(tr::lng_link_copied(tr::now));
}

void Widget::showMenu() {
	if (!_loaded) {
		return;
	}
	if (_menu) {
		_menu->hideMenu(true);
	}
	const auto model = MakeButtonModel(viewerState());
	_menu = new Ui::PopupMenu(this);
	const auto add = [&](
			const QString &text,
			Fn<void()> callback,
			const style::icon *icon,
			bool destructive = false) {
		auto item = Ui::MenuItem();
		item.text = text;
		item.callback = std::move(callback);
		item.icon = icon;
		item.destructive = destructive;
		_menu->addItem(std::move(item));
	};
	// «تماس صوتی»: where the chat's header would offer it too (a direct
	// chat of two exists and the server would not refuse the call).
	if (const auto callee = callableUserId(); !callee.isEmpty()) {
		add(
			tr::lng_calls_voice_call(tr::now),
			[=, this] { Window::StartCall(controller(), callee); },
			&st::profileMenuCallIcon);
		if (Window::VideoCallsOffered(controller())) {
			add(
				tr::lng_calls_video_call(tr::now),
				[=, this] { Window::StartCall(controller(), callee, true); },
				&st::profileMenuVideoCallIcon);
		}
	}
	for (const auto entry : model.menu) {
		switch (entry) {
		case MenuEntry::PostNotificationsOn:
			add(
				tr::lng_profile_post_notifications_on(tr::now),
				[this] { togglePostNotifications(); },
				&st::profileMenuBellIcon);
			break;
		case MenuEntry::PostNotificationsOff:
			add(
				tr::lng_profile_post_notifications_off(tr::now),
				[this] { togglePostNotifications(); },
				&st::profileMenuBellIcon);
			break;
		case MenuEntry::ShareProfile:
			add(
				tr::lng_profile_share_profile(tr::now),
				[this] { ShowShareBox(controller(), _profile); },
				&st::menuIconShare);
			break;
		case MenuEntry::CopyLink:
			add(
				tr::lng_copy_link(tr::now),
				[this] { copyLink(); },
				&st::menuIconLink);
			break;
		case MenuEntry::Mute:
			add(
				tr::lng_profile_mute(tr::now),
				[this] { toggleMute(); },
				&st::menuIconMute);
			break;
		case MenuEntry::Unmute:
			add(
				tr::lng_profile_unmute(tr::now),
				[this] { toggleMute(); },
				&st::menuIconUnmute);
			break;
		case MenuEntry::RemoveFollower:
			add(
				tr::lng_profile_remove_follower(tr::now),
				[this] { removeFollower(); },
				&st::profileMenuRemoveIcon);
			break;
		case MenuEntry::Report:
			_menu->addSeparator();
			add(
				tr::lng_profile_report_account(tr::now),
				[this] { report(); },
				&st::menuIconReport,
				true);
			break;
		case MenuEntry::Block:
			add(
				tr::lng_block_user(tr::now),
				[this] { toggleBlock(); },
				&st::menuIconBlock,
				true);
			break;
		case MenuEntry::Unblock:
			add(
				tr::lng_unblock(tr::now),
				[this] { toggleBlock(); },
				&st::menuIconBlock);
			break;
		}
	}
	_menu->popupAnchored(_inner->cover()->moreButton());
}

void Widget::togglePostNotifications() {
	const auto controller = this->controller();
	controller->session().data().graph().setPostNotifications(
		_profile.id,
		!_postNotifications,
		this,
		[=, this](bool enabled) {
			_postNotifications = enabled;
			controller->showToast(enabled
				? tr::lng_profile_post_notifications_enabled(tr::now)
				: tr::lng_profile_post_notifications_disabled(tr::now));
		});
}

void Widget::toggleMute() {
	// Fire-and-forget in the store: a failure is reported by the session's
	// error reporter, the next profile load brings the real state.
	auto &graph = controller()->session().data().graph();
	const auto name = _profile.displayName;
	if (_muted) {
		graph.unmute(_profile.id);
		controller()->showToast(tr::lng_profile_unmuted_toast(
			tr::now,
			lt_name,
			name));
	} else {
		graph.mute(_profile.id);
		controller()->showToast(tr::lng_profile_muted_toast(
			tr::now,
			lt_name,
			name));
	}
	_muted = !_muted;
}

void Widget::toggleBlock() {
	const auto controller = this->controller();
	const auto weak = QPointer<Widget>(this);
	const auto id = _profile.id;
	const auto name = _profile.displayName;
	const auto fail = [=](const Api::Error &error) {
		Support::ShowError(
			controller,
			error,
			tr::lng_profile_action_failed(tr::now));
	};
	auto args = Ui::ConfirmBoxArgs();
	if (_blocked) {
		args.title = tr::lng_profile_unblock_title(tr::now, lt_name, name);
		args.text = tr::lng_profile_unblock_text(tr::now);
		args.confirmText = tr::lng_unblock(tr::now);
		args.confirmed = [=](std::function<void()> close) {
			close();
			if (!weak) {
				return;
			}
			controller->session().data().graph().unblock(
				id,
				weak.data(),
				[=](const Data::GraphState &) {
					weak->_blocked = false;
					weak->_profile.blockedByYou = false;
					weak->_justUnblocked = true;
					weak->apply();
					controller->showToast(tr::lng_profile_unblocked_toast(
						tr::now,
						lt_name,
						name));
				},
				fail);
		};
	} else {
		args.title = tr::lng_profile_block_title(tr::now, lt_name, name);
		args.text = tr::lng_profile_block_text(tr::now);
		args.confirmText = tr::lng_block_user(tr::now);
		args.cancelText = tr::lng_never_mind(tr::now);
		args.destructive = true;
		args.confirmed = [=](std::function<void()> close) {
			close();
			if (!weak) {
				return;
			}
			controller->session().data().graph().block(
				id,
				weak.data(),
				[=](const Data::GraphState &) {
					weak->_blocked = true;
					weak->_profile.blockedByYou = true;
					weak->_justUnblocked = false;
					weak->_follow = FollowUi::None;
					weak->_profile.followState = u"none"_s;
					weak->_profile.followsYou = false;
					weak->apply();
					controller->showToast(tr::lng_profile_blocked_toast(
						tr::now,
						lt_name,
						name));
				},
				fail);
		};
	}
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void Widget::removeFollower() {
	const auto controller = this->controller();
	const auto weak = QPointer<Widget>(this);
	const auto id = _profile.id;
	const auto name = _profile.displayName;
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_profile_remove_follower_title(tr::now, lt_name, name);
	args.text = tr::lng_profile_remove_follower_text(tr::now);
	args.confirmText = tr::lng_delete(tr::now);
	args.cancelText = tr::lng_never_mind(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		close();
		if (!weak) {
			return;
		}
		controller->session().data().graph().removeFollower(
			id,
			weak.data(),
			[=](const Data::GraphState &) {
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

void Widget::report() {
	const auto weak = QPointer<Widget>(this);
	ShowReportBox(controller(), _profile, [=] {
		if (weak) {
			weak->_blocked = true;
			weak->_profile.blockedByYou = true;
			weak->_follow = FollowUi::None;
			weak->apply();
		}
	});
}

} // namespace Profile
