#include "eli/eli_home.h"

#include "data/data_feed.h"
#include "data/data_notifications.h"
#include "data/data_session.h"
#include "eli/eli_actions.h"
#include "eli/eli_post_list.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"
#include "support/connection_label.h"
#include "support/file_dialogs.h"
#include "support/media_image.h"
#include "ui/effects/shadow.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QKeyEvent>

namespace Eli {
namespace {

constexpr auto kForYou = 0;
constexpr auto kFollowing = 1;
constexpr auto kFeedCap = 40;
constexpr auto kRecheckAfterMs = qint64(30) * 1000;
constexpr auto kMaxPillCount = 99;

[[nodiscard]] QString TabKey() {
	return u"eli.tab"_s;
}

} // namespace

// The «چه خبر؟» row on top of the feed.
class HomeSection::Entry final : public Ui::AbstractButton {
public:
	Entry(QWidget *parent, Window::SessionController *controller)
	: AbstractButton(parent)
	, _userpic(new Support::UserpicButton(
		this,
		&controller->session(),
		controller->session().userId(),
		st::eliCardUserpic)) {
		Ui::MakeTransparentForMouse(_userpic);
		resize(width(), st::eliEntryHeight);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto width = std::min(newWidth, int(st::eliCardMaxWidth));
		_left = (newWidth - width) / 2;
		_inner = width;
		const auto x = _left + st::eliCardPadding.left();
		_userpic->moveToLeft(x, (st::eliEntryHeight - st::eliCardUserpic) / 2, newWidth);
		return st::eliEntryHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		if (isOver()) {
			p.fillRect(QRect(_left, 0, _inner, height()), st::eliEntryBgOver);
		}
		const auto textLeft = _left
			+ st::eliCardPadding.left()
			+ st::eliCardUserpic
			+ st::eliCardUserpicSkip;
		p.setPen(st::eliEntryFg);
		p.setFont(st::eliEntryFont);
		p.drawTextLeft(
			textLeft,
			(height() - st::eliEntryFont->height) / 2,
			width(),
			tr::lng_eli_whats_new(tr::now));
		auto right = _left + st::eliCardPadding.left();
		const auto iconTop = (height() - st::eliEntryIconImage.height()) / 2;
		// Icons at the trailing side: x from the trailing edge.
		const auto paintTrailing = [&](const style::icon &icon) {
			const auto x = width() - right - icon.width();
			icon.paint(p, x, iconTop, width());
			right += icon.width() + st::eliEntryIconSkip;
		};
		paintTrailing(st::eliEntryIconPoll);
		paintTrailing(st::eliEntryIconImage);
		p.fillRect(QRect(_left, height() - 1, _inner, 1), st::eliCardLineFg);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		Q_UNUSED(was);
		Q_UNUSED(source);
		update();
	}

private:
	Support::UserpicButton *_userpic = nullptr;
	int _left = 0;
	int _inner = 0;

};

// «N پست تازه» with the faces of up to three people who posted.
class HomeSection::NewPill final : public Ui::AbstractButton {
public:
	NewPill(QWidget *parent, Main::Session *session)
	: AbstractButton(parent)
	, _userpics(session, [this](const QString &) { update(); }, this) {
		hide();
	}

	void setData(int count, const QStringList &authors) {
		_count = count;
		_authors = authors;
		_text = tr::lng_eli_new_posts(
			tr::now,
			lt_count,
			double(std::min(count, kMaxPillCount)));
		const auto faces = int(_authors.size());
		_facesWidth = faces
			? (st::eliPillFace + (faces - 1) * (st::eliPillFace - st::eliPillFaceOverlap))
			: 0;
		const auto &extend = st::eliPillShadow.extend;
		_bodyWidth = st::eliPillPadding
			+ (_facesWidth ? (_facesWidth + st::eliPillSkip) : 0)
			+ st::eliPillFont->width(_text)
			+ st::eliPillSkip
			+ st::eliPillArrow.width()
			+ st::eliPillPadding
			- (_facesWidth ? 6 : 0);
		resize(
			_bodyWidth + extend.left() + extend.right(),
			st::eliPillHeight + extend.top() + extend.bottom());
		update();
	}

	[[nodiscard]] int count() const {
		return _count;
	}
	[[nodiscard]] bool hasFaces() const {
		return !_authors.isEmpty();
	}
	[[nodiscard]] int bodyTopShift() const {
		return st::eliPillShadow.extend.top();
	}

	void setShown(bool shown) {
		if (_shown == shown) {
			return;
		}
		_shown = shown;
		if (shown) {
			show();
			raise();
		}
		_animation.start(
			[this] {
				if (!_animation.animating() && !_shown) {
					hide();
				}
				update();
			},
			shown ? 0. : 1.,
			shown ? 1. : 0.,
			st::eliPillDuration,
			anim::easeOutCubic);
		if (!_animation.animating() && !shown) {
			hide();
		}
	}

	[[nodiscard]] bool shown() const {
		return _shown;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto progress = _animation.value(_shown ? 1. : 0.);
		const auto &extend = st::eliPillShadow.extend;
		p.setOpacity(progress);
		p.translate(0, int((1. - progress) * -10));
		const auto body = QRect(
			extend.left(),
			extend.top(),
			_bodyWidth,
			st::eliPillHeight);
		PainterHighQualityEnabler hq(p);
		Ui::Shadow::Paint(p, body, st::eliPillHeight / 2, st::eliPillShadow);
		p.setPen(Qt::NoPen);
		p.setBrush(isOver() ? st::eliPillBgOver : st::eliPillBg);
		p.drawRoundedRect(body, st::eliPillHeight / 2, st::eliPillHeight / 2);

		auto x = body.x() + st::eliPillPadding - (_facesWidth ? 6 : 0); // logical
		const auto outer = width();
		const auto face = st::eliPillFace;
		// Later faces lie under the earlier ones.
		for (auto i = int(_authors.size()) - 1; i >= 0; --i) {
			const auto left = x + i * (face - st::eliPillFaceOverlap);
			const auto rect = style::rtlrect(
				left,
				body.y() + (body.height() - face) / 2,
				face,
				face,
				outer);
			p.setPen(Qt::NoPen);
			p.setBrush(isOver() ? st::eliPillBgOver : st::eliPillBg);
			const auto border = st::eliPillFaceBorder;
			p.drawEllipse(rect.adjusted(-border, -border, border, border));
			_userpics.paint(p, rect, _authors[i]);
		}
		if (_facesWidth) {
			x += _facesWidth + st::eliPillSkip;
		}
		p.setPen(st::eliPillFg);
		p.setFont(st::eliPillFont);
		const auto textWidth = st::eliPillFont->width(_text);
		p.drawTextLeft(
			x,
			body.y() + (body.height() - st::eliPillFont->height) / 2,
			outer,
			_text,
			textWidth);
		x += textWidth + st::eliPillSkip;
		st::eliPillArrow.paint(
			p,
			x,
			body.y() + (body.height() - st::eliPillArrow.height()) / 2,
			outer);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		Q_UNUSED(was);
		Q_UNUSED(source);
		update();
	}

private:
	Support::UserpicPainter _userpics;
	QStringList _authors;
	QString _text;
	int _count = 0;
	int _facesWidth = 0;
	int _bodyWidth = 0;
	bool _shown = false;
	Ui::Animations::Simple _animation;

};

// A thin running line under the tabs while the shown feed reloads.
class HomeSection::LoadingLine final : public Ui::RpWidget {
public:
	explicit LoadingLine(QWidget *parent)
	: RpWidget(parent)
	, _animation([this](qint64) {
		update();
		return true;
	}) {
		Ui::MakeTransparentForMouse(this);
		hide();
	}

	void setLoading(bool loading) {
		if (loading == !isHidden()) {
			return;
		}
		setVisible(loading);
		if (loading) {
			_started = Ui::Now();
			_animation.start();
		} else {
			_animation.stop();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		const auto period = 1100;
		const auto t = Ui::AnimationsDisabled()
			? 0.4
			: double((Ui::Now() - _started) % period) / period;
		const auto segment = width() / 3;
		const auto left = int(t * (width() + segment)) - segment;
		p.fillRect(QRect(left, 0, segment, height()), st::eliLoadingLineFg);
	}

private:
	Ui::Animations::Basic _animation;
	qint64 _started = 0;

};

HomeSection::HomeSection(
	QWidget *parent,
	Window::SessionController *controller)
: SectionWidget(parent, controller)
, _bar(new Ui::TopBar(this))
, _tabs(new Ui::TabBar(this, st::eliHomeTabs))
, _loadingLine(new LoadingLine(this))
, _pill(new NewPill(this, &controller->session()))
, _openedAt(QDateTime::currentMSecsSinceEpoch()) {
	setAcceptDrops(true);
	setFocusPolicy(Qt::StrongFocus);
	auto &session = controller->session();
	auto &data = session.data();

	_title = new Support::ConnectionTitle(_bar, &session, tr::lng_tab_eli(tr::now));
	setTopBar(_bar);
	const auto create = _bar->addAction(st::topBarCreateButton);
	const auto refresh = _bar->addAction(st::eliTopBarRefresh);
	_bell = _bar->addAction(st::topBarBellButton);
	const auto more = _bar->addAction(st::topBarMoreButton);
	Ui::InstallTooltip(create, [] { return tr::lng_eli_new_post(tr::now); });
	Ui::InstallTooltip(refresh, [] { return tr::lng_eli_refresh(tr::now); });
	Ui::InstallTooltip(_bell, [] { return tr::lng_settings_notifications(tr::now); });
	Ui::InstallTooltip(more, [] { return tr::lng_more(tr::now); });
	create->setClickedCallback([=] { OpenCompose(controller); });
	refresh->setClickedCallback([this] { refreshCurrent(); });
	_bell->setClickedCallback([=] {
		controller->openRoute(Window::Routes::EliNotifications());
	});
	more->setClickedCallback([=, this] {
		const auto menu = new Ui::PopupMenu(this);
		menu->addAction(
			tr::lng_eli_bookmarks_title(tr::now),
			[=] { OpenBookmarks(controller); },
			&st::menuIconBookmark);
		menu->addAction(
			tr::lng_eli_trending(tr::now),
			[=] { OpenTrends(controller); },
			&st::eliMenuIconEye);
		menu->popupAnchored(more);
	});
	_badge = new Ui::UnreadBadge(_bar, st::smallBadge);
	Ui::MakeTransparentForMouse(_badge);
	connect(&data.notifications(), &Data::Notifications::unreadChanged, this, [this] {
		updateBadge();
	});
	updateBadge();

	_tabs->setStretch(true);
	_tabs->setTabs({
		tr::lng_eli_for_you(tr::now),
		tr::lng_eli_following_tab(tr::now),
	});
	_tabs->installEventFilter(this);
	connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		setTab(index, true);
	});

	const auto stored = session.account().settings().string(TabKey());
	_tab = (stored == u"following") ? kFollowing : kForYou;
	_tabs->setActive(_tab, anim::type::instant);
	createPane(kForYou);
	createPane(kFollowing);
	setTab(_tab, false);

	_pill->setClickedCallback([this] { applyNewPosts(); });

	connect(&data.feed(), &Data::Feed::newPosts, this, [this](int count, qint64) {
		newPostsArrived(count);
	});
	connect(&data.feed(), &Data::Feed::postPublished, this, [this](const QString &id) {
		const auto post = this->controller()->session().data().feed().post(id);
		if (!post || !post->replyTo.isEmpty()) {
			return;
		}
		const auto createdAt = post->createdAt;
		_panes[kForYou].list->prepend(id);
		_panes[kFollowing].list->prepend(id);
		if (createdAt > _newestFollowing) {
			_newestFollowing = createdAt;
			watchNew();
		}
	});
	connect(controller, &Window::SessionController::mainSectionChanged, this, [this] {
		updateActivePost();
	});
	connect(controller, &Window::SessionController::tabChanged, this, [this](Window::Tab tab) {
		if (tab == Window::Tab::Eli) {
			showFinished();
		}
	});
	updateActivePost();
	watchNew();
}

HomeSection::~HomeSection() {
	controller()->session().data().feed().watchNew(0);
}

void HomeSection::createPane(int index) {
	auto &pane = _panes[index];
	pane.scroll = new Ui::ScrollArea(this);
	pane.content = pane.scroll->setOwnedWidget(
		Ui::object_ptr<Ui::VerticalLayout>(pane.scroll));
	const auto controller = this->controller();
	pane.entry = pane.content->add(
		Ui::object_ptr<Entry>(pane.content, controller));
	pane.entry->setClickedCallback([=] { OpenCompose(controller); });

	auto descriptor = ListDescriptor();
	const auto tab = (index == kFollowing)
		? Data::Feed::Tab::Following
		: Data::Feed::Tab::ForYou;
	descriptor.loader = [=](
			const QString &cursor,
			base::Guard guard,
			Fn<void(const ListPage &)> done,
			Data::FailCallback fail) {
		// «برای تو» has no cursor: one page per refresh.
		controller->session().data().feed().loadFeed(
			tab,
			(tab == Data::Feed::Tab::Following) ? cursor : QString(),
			guard,
			[=](const Data::FeedPage &page) {
				auto result = ListPage::From(page);
				if (tab != Data::Feed::Tab::Following) {
					result.next = QString();
				}
				done(result);
			},
			fail);
	};
	descriptor.cap = kFeedCap;
	if (index == kFollowing) {
		descriptor.empty = [=] {
			auto result = Ui::EmptyPlaceholderDescriptor();
			result.icon = &st::eliEmptyIconFollow;
			result.title = tr::lng_eli_dont_follow_anyone_yet(tr::now);
			result.text = tr::lng_eli_find_people_whose_words_matter(tr::now);
			result.buttonText = tr::lng_eli_find_people(tr::now);
			result.buttonCallback = [=] {
				controller->openRoute(Window::Routes::Search());
			};
			return result;
		};
		descriptor.onPage = [this](const ListPage &page, bool first) {
			auto newest = qint64(0);
			auto &feed = this->controller()->session().data().feed();
			for (const auto &item : page.items) {
				if (const auto post = feed.post(item.postId)) {
					newest = std::max(newest, post->createdAt);
				}
			}
			_newestFollowing = first ? newest : std::max(_newestFollowing, newest);
			watchNew();
		};
	} else {
		descriptor.empty = [] {
			auto result = Ui::EmptyPlaceholderDescriptor();
			result.icon = &st::emptyIconEli;
			result.title = tr::lng_eli_nothing_show_yet(tr::now);
			result.text = tr::lng_eli_check_back_little_later(tr::now);
			return result;
		};
	}
	pane.list = pane.content->add(
		Ui::object_ptr<PostList>(pane.content, controller, std::move(descriptor)));
	const auto scroll = pane.scroll;
	const auto list = pane.list;
	connect(list, &PostList::scrollToRequested, this, [=](int top, int bottom) {
		const auto shift = list->y();
		scroll->scrollToY(top + shift, bottom + shift);
	});
	connect(list, &PostList::loadingChanged, this, [this] {
		updateLoading();
	});
	connect(scroll, &Ui::ScrollArea::scrollTopChanged, this, [=, this](int top) {
		auto &pane = _panes[index];
		const auto was = pane.lastScrollTop;
		pane.lastScrollTop = top;
		if (index != _tab || !_newCount) {
			return;
		}
		// Reading on: the pill steps aside; scrolling back up brings it back.
		if (top > was + 2 && top > 240) {
			_pillHiddenByScroll = true;
		} else if (top < was - 2 || top < 120) {
			_pillHiddenByScroll = false;
		}
		_pill->setShown(_newCount > 0 && !_pillHiddenByScroll);
	});
	pane.scroll->hide();
}

void HomeSection::setTab(int index, bool save) {
	index = (index == kFollowing) ? kFollowing : kForYou;
	_tab = index;
	for (auto i = 0; i != 2; ++i) {
		_panes[i].scroll->setVisible(i == _tab);
	}
	if (save) {
		controller()->session().account().settings().set(
			TabKey(),
			(index == kFollowing) ? u"following"_s : u"for-you"_s);
		current().list->setFocus();
	}
	_pillHiddenByScroll = false;
	_pill->setShown(_newCount > 0);
	_pill->raise();
	updateLoading();
}

void HomeSection::updateLoading() {
	const auto list = current().list;
	_loadingLine->setLoading(list->loading() && list->count() > 0);
	_loadingLine->raise();
}

void HomeSection::refreshCurrent() {
	if (_tab == kFollowing) {
		clearNewPosts();
	}
	current().scroll->scrollToY(0);
	current().list->reload();
}

void HomeSection::updateBadge() {
	_badge->setCount(controller()->session().data().notifications().unread());
	placeBadge();
}

void HomeSection::placeBadge() {
	if (!_bell || !_badge) {
		return;
	}
	const auto bell = _bell->geometry();
	const auto shift = st::eliBellBadgeShift;
	// Over the top trailing corner of the bell.
	_badge->move(
		style::RightToLeft()
			? (bell.x() + shift.x())
			: (bell.x() + bell.width() - _badge->width() - shift.x()),
		bell.y() + shift.y());
	_badge->raise();
}

void HomeSection::updateActivePost() {
	auto id = QString();
	if (const auto section = controller()->mainSection()) {
		const auto route = Window::ParseRoute(section->sectionKey());
		if (route.type == Window::RouteType::Post) {
			id = route.id;
		}
	}
	for (auto &pane : _panes) {
		if (pane.list) {
			pane.list->setActivePost(id);
		}
	}
}

// ---- the new posts pill

qint64 HomeSection::newSince() const {
	return _newestFollowing ? _newestFollowing : _openedAt;
}

void HomeSection::watchNew() {
	controller()->session().data().feed().watchNew(newSince());
}

void HomeSection::showNewPosts(int count) {
	newPostsArrived(count);
}

void HomeSection::newPostsArrived(int count) {
	if (count <= 0) {
		clearNewPosts();
		return;
	}
	const auto changed = (count != _newCount);
	_newCount = count;
	if ((changed || !_pill->hasFaces()) && !_authorsLoading) {
		// The first page is read once per change, only for the faces: it is
		// never applied to the list under the reader.
		_authorsLoading = true;
		controller()->session().data().feed().newAuthors(
			newSince(),
			base::Guard(this),
			[this](const QStringList &authors) {
				_authorsLoading = false;
				if (_newCount > 0) {
					_pill->setData(_newCount, authors);
					resizeEvent(nullptr);
				}
			});
	}
	_pill->setData(count, QStringList());
	resizeEvent(nullptr);
	if (changed) {
		_pillHiddenByScroll = false;
	}
	_pill->setShown(!_pillHiddenByScroll);
}

void HomeSection::clearNewPosts() {
	_newCount = 0;
	_pill->setShown(false);
}

void HomeSection::applyNewPosts() {
	clearNewPosts();
	if (_tab != kFollowing) {
		_tabs->setActive(kFollowing);
		setTab(kFollowing, true);
	}
	_panes[kFollowing].scroll->scrollToY(0);
	_panes[kFollowing].list->reload();
}

void HomeSection::showFinished() {
	// Came back after a while: one count, not a timer.
	const auto now = Ui::Now();
	if (_lastCheck && now - _lastCheck < kRecheckAfterMs) {
		return;
	}
	const auto first = !_lastCheck;
	_lastCheck = now;
	if (first) {
		return;
	}
	controller()->session().data().feed().newCount(
		newSince(),
		base::Guard(this),
		[this](int count) {
			if (count > 0) {
				newPostsArrived(count);
			}
		});
}

// ---- events

void HomeSection::setInnerFocus() {
	current().list->setFocus();
}

void HomeSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_tabs->resizeToWidth(width());
	_tabs->move(0, _bar->height());
	const auto top = _bar->height() + _tabs->height();
	_loadingLine->setGeometry(0, top - st::eliLoadingLineHeight, width(), st::eliLoadingLineHeight);
	const auto height = std::max(this->height() - top, 0);
	for (auto &pane : _panes) {
		if (!pane.scroll) {
			continue;
		}
		pane.scroll->setGeometry(0, top, width(), height);
		pane.list->setMinHeight(std::max(height - st::eliEntryHeight, 0));
	}
	_pill->move(
		(width() - _pill->width()) / 2,
		top + st::eliPillTop - _pill->bodyTopShift());
	_pill->raise();
	placeBadge();
}

bool HomeSection::eventFilter(QObject *object, QEvent *event) {
	if (object == _tabs) {
		if (event->type() == QEvent::MouseButtonPress) {
			_tabOnPress = _tabs->active();
		} else if (event->type() == QEvent::MouseButtonRelease) {
			const auto was = std::exchange(_tabOnPress, -1);
			Ui::PostponeCall(this, [=, this] {
				// The active tab was clicked again: to the top, then refresh.
				if (was >= 0 && was == _tabs->active() && was == _tab) {
					if (current().scroll->scrollTop() > 40) {
						current().scroll->scrollToYAnimated(0);
					} else {
						refreshCurrent();
					}
				}
			});
		}
	}
	return SectionWidget::eventFilter(object, event);
}

void HomeSection::keyPressEvent(QKeyEvent *e) {
	const auto plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
	if (e->key() == Qt::Key_F5
		|| (e->key() == Qt::Key_R && (e->modifiers() & Qt::ControlModifier))) {
		refreshCurrent();
	} else if (plain && (e->key() == Qt::Key_N || e->text() == u"د")) {
		OpenCompose(controller());
	} else if (plain && e->key() == Qt::Key_Period && _newCount > 0) {
		applyNewPosts();
	} else if (!current().list->handleKey(e)) {
		SectionWidget::keyPressEvent(e);
	}
}

void HomeSection::dragEnterEvent(QDragEnterEvent *e) {
	if (Support::CanAcceptDrop(e->mimeData())) {
		e->acceptProposedAction();
	}
}

void HomeSection::dropEvent(QDropEvent *e) {
	const auto data = Support::ReadMimeData(e->mimeData());
	if (!data.hasMedia()) {
		return;
	}
	e->acceptProposedAction();
	const auto controller = this->controller();
	if (!data.files.isEmpty()) {
		auto args = ComposeArgs();
		args.files = data.files;
		OpenCompose(controller, args);
	} else {
		Support::SaveImageToTemp(data.image, this, [=](const QString &path) {
			if (!path.isEmpty()) {
				auto args = ComposeArgs();
				args.files = QStringList{ path };
				OpenCompose(controller, args);
			}
		});
	}
}

} // namespace Eli
