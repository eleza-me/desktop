#include "eli/eli_post_list.h"

#include "data/data_feed.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "eli/eli_actions.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_audio_player.h"
#include "stories/stories_viewer.h"
#include "support/error_text.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/tooltip.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

#include <map>

namespace Eli {
namespace {

constexpr auto kLoadMoreDistance = 800;
constexpr auto kSkeletonRows = 4;
constexpr auto kTooltipDelay = 700;
constexpr auto kHighlightDuration = 1800;
constexpr auto kMinuteMs = 60 * 1000;
constexpr auto kMaxIdlePages = 5;

[[nodiscard]] bool SameLetter(const QKeyEvent *e, int key, QStringView persian) {
	if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
		return false;
	}
	// The same physical key on a Persian layout types another letter.
	return (e->key() == key) || (!e->text().isEmpty() && e->text() == persian);
}

} // namespace

ListPage ListPage::From(const Data::FeedPage &page) {
	auto result = ListPage();
	result.items = page.items;
	result.next = page.nextCursor;
	return result;
}

ListPage ListPage::From(const Data::PostsPage &page) {
	auto result = ListPage();
	for (const auto &id : page.posts) {
		auto item = Data::FeedItem();
		item.postId = id;
		if (const auto actor = page.repostedBy.value(id); !actor.isEmpty()) {
			// «↻ name reposted», as in the following feed.
			item.reason = u"repost"_s;
			item.actor = actor;
		}
		result.items.push_back(item);
	}
	result.next = page.nextBefore.isEmpty() ? page.nextCursor : page.nextBefore;
	return result;
}

PostList::PostList(
	QWidget *parent,
	Window::SessionController *controller,
	ListDescriptor descriptor)
: RpWidget(parent)
, _controller(controller)
, _descriptor(std::move(descriptor))
, _userpics(std::make_unique<Support::UserpicPainter>(
	&controller->session(),
	[this](const QString &userId) { peopleChanged({ userId }); },
	this))
, _skeleton([this] { update(); })
, _tooltipTimer([this] { showTooltip(); })
, _viewTimer([this] { checkViews(); })
, _minuteTimer([this] {
	for (auto i = 0; i != int(_rows.size()); ++i) {
		auto &row = _rows[i];
		if (row.card && row.top + row.height > _visibleTop && row.top < _visibleBottom) {
			row.card->refreshTime();
			updateRow(i);
		}
	}
}) {
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);

	auto &data = controller->session().data();
	connect(&data.feed(), &Data::Feed::postChanged, this, [this](const QString &id) {
		postChanged(id);
	});
	connect(&data.feed(), &Data::Feed::postRemoved, this, [this](const QString &id) {
		postRemoved(id);
	});
	connect(&data.feed(), &Data::Feed::authorBlocked, this, [this](const QString &userId) {
		if (_descriptor.keepRemoved) {
			return;
		}
		auto removed = false;
		for (auto i = _rows.begin(); i != _rows.end();) {
			if (i->card && i->card->post().author == userId) {
				i = _rows.erase(i);
				removed = true;
			} else {
				++i;
			}
		}
		if (removed) {
			rebuildIndex();
			relayout();
		}
	});
	connect(&data.stories(), &Data::Stories::ringsChanged, this, [this] {
		update();
	});
	connect(&data.users(), &Data::Users::changed, this, [this](const QStringList &ids) {
		peopleChanged(ids);
	});
	connect(&data.users(), &Data::Users::meChanged, this, [this] {
		peopleChanged({ _controller->session().userId() });
	});
	const auto audioChanged = [this] {
		const auto &current = Media::Player().current().id;
		for (auto i = 0; i != int(_rows.size()); ++i) {
			const auto &row = _rows[i];
			if (row.card
				&& row.card->hasAudio()
				&& row.top + row.height > _visibleTop
				&& row.top < _visibleBottom
				&& (current.isEmpty() || row.card->audioTrackId() == current)) {
				updateRow(i);
			}
		}
	};
	connect(&Media::Player(), &Media::AudioPlayer::stateChanged, this, audioChanged);
	connect(&Media::Player(), &Media::AudioPlayer::trackChanged, this, [this] {
		update();
	});
	connect(&Media::Player(), &Media::AudioPlayer::positionChanged, this, audioChanged);
	style::OnPaletteChanged(this, [this] {
		_userpics->clear();
		update();
	});
	_minuteTimer.callEach(kMinuteMs);

	if (_descriptor.loader) {
		load(true);
	} else {
		_loadedOnce = true;
		_done = true;
	}
}

PostList::~PostList() {
	Ui::Tooltip::Hide();
}

Main::Session &PostList::cardSession() const {
	return _controller->session();
}

Support::UserpicPainter &PostList::cardUserpics() {
	return *_userpics;
}

double PostList::cardPixelRatio() const {
	return Ui::DevicePixelRatio(this);
}

void PostList::cardRepaint(const QString &postId) {
	const auto index = indexOf(postId);
	if (index >= 0) {
		updateRow(index);
	}
}

QObject *PostList::cardGuard() {
	return this;
}

int PostList::cardWidth() const {
	return std::min(width(), int(st::eliCardMaxWidth));
}

int PostList::cardLeft() const {
	return (width() - cardWidth()) / 2;
}

// ---- loading

void PostList::reload() {
	if (!_descriptor.loader) {
		return;
	}
	++_generation;
	_loading = false;
	load(true);
}

void PostList::loadMore() {
	if (_loading || _done || !_descriptor.loader || _failedFirst) {
		return;
	}
	load(false);
}

void PostList::load(bool reset) {
	if (_loading) {
		return;
	}
	_loading = true;
	_reloading = reset && !_rows.empty();
	_failedMore = false;
	if (reset) {
		_failedFirst = false;
	}
	const auto generation = _generation;
	const auto cursor = reset ? QString() : _next;
	refreshPlaceholder();
	relayout();
	Q_EMIT loadingChanged();
	_descriptor.loader(cursor, base::Guard(this), [=, this](const ListPage &page) {
		if (generation != _generation) {
			return;
		}
		_loading = false;
		_reloading = false;
		applyPage(page, reset, cursor);
		Q_EMIT loadingChanged();
	}, [=, this](const Api::Error &error) {
		if (generation != _generation) {
			return;
		}
		_loading = false;
		const auto reloading = _reloading;
		_reloading = false;
		_errorText = Support::ErrorText(error, tr::lng_error_load(tr::now));
		if (reloading) {
			// The posts on screen stay; the failure is only told.
			Support::ShowError(_controller, error, tr::lng_error_load(tr::now));
		} else if (_rows.empty()) {
			_failedFirst = true;
		} else {
			_failedMore = true;
		}
		refreshPlaceholder();
		relayout();
		Q_EMIT loadingChanged();
	});
}

CardOptions PostList::optionsFor(const Data::FeedItem &item) const {
	auto options = CardOptions();
	options.mode = _descriptor.mode;
	options.reason = item.reason;
	options.actor = item.actor;
	options.pinned = !_descriptor.pinnedId.isEmpty()
		&& (_descriptor.pinnedId == item.postId);
	return options;
}

void PostList::appendRow(const Data::FeedItem &item) {
	auto row = Row();
	row.id = item.postId;
	row.card = std::make_unique<PostCard>(this, item.postId, optionsFor(item));
	_index.insert(row.id, int(_rows.size()));
	_rows.push_back(std::move(row));
}

void PostList::applyPage(
		const ListPage &page,
		bool reset,
		const QString &requested) {
	const auto first = reset;
	if (reset) {
		_rows.clear();
		_index.clear();
		_halfVisible.clear();
		_overRow = -1;
		_pressedRow = -1;
		_overHit = {};
	}
	auto fresh = QStringList();
	auto added = 0;
	auto &feed = _controller->session().data().feed();
	for (const auto &item : page.items) {
		if (_index.contains(item.postId) || !feed.post(item.postId)) {
			continue;
		}
		if (_descriptor.cap > 0 && int(_rows.size()) >= _descriptor.cap) {
			break;
		}
		appendRow(item);
		fresh.push_back(item.postId);
		++added;
	}
	_freshness.noteFresh(fresh, Ui::Now());
	_next = page.next;
	// A page can come back short, even empty, while the server has more
	// (posts it does not show me): go on, unless it names the same page
	// again or nothing turned up for a while.
	_idlePages = added ? 0 : ((reset ? 0 : _idlePages) + 1);
	_done = _next.isEmpty()
		|| (page.items.isEmpty() && _next == requested)
		|| (_idlePages > kMaxIdlePages)
		|| (_descriptor.cap > 0 && int(_rows.size()) >= _descriptor.cap);
	_loadedOnce = true;
	_failedFirst = false;
	_failedMore = false;
	refreshPlaceholder();
	relayout();
	if (_descriptor.onPage) {
		_descriptor.onPage(page, first);
	}
	Q_EMIT emptyChanged(_rows.empty());
	// A page of posts that were all here already: go on.
	if (!added && !_done) {
		loadMore();
	}
}

void PostList::prepend(const QString &postId) {
	if (_index.contains(postId)
		|| !_controller->session().data().feed().post(postId)) {
		return;
	}
	auto item = Data::FeedItem();
	item.postId = postId;
	auto row = Row();
	row.id = postId;
	row.card = std::make_unique<PostCard>(this, postId, optionsFor(item));
	_rows.insert(_rows.begin(), std::move(row));
	_freshness.noteFresh({ postId }, Ui::Now());
	rebuildIndex();
	refreshPlaceholder();
	relayout();
	Q_EMIT emptyChanged(false);
}

void PostList::remove(const QString &postId) {
	const auto index = indexOf(postId);
	if (index < 0) {
		return;
	}
	_rows.erase(_rows.begin() + index);
	_freshness.forget(postId);
	_overRow = -1;
	_overHit = {};
	_pressedRow = -1;
	rebuildIndex();
	refreshPlaceholder();
	relayout();
	Q_EMIT emptyChanged(_rows.empty());
}

void PostList::setRows(const QVector<ListRow> &rows) {
	auto old = std::map<QString, std::unique_ptr<PostCard>>();
	for (auto &row : _rows) {
		if (row.card) {
			old.emplace(row.id, std::move(row.card));
		}
	}
	_rows.clear();
	for (const auto &entry : rows) {
		auto row = Row();
		row.id = entry.id;
		if (!entry.moreText.isEmpty()) {
			row.moreText = entry.moreText;
			row.moreIndent = (entry.options.depth > 0) ? st::eliReplyIndent : 0;
		} else {
			auto options = entry.options;
			const auto i = old.find(entry.id);
			if (i != old.end() && i->second) {
				row.card = std::move(i->second);
				row.card->setOptions(options);
			} else {
				row.card = std::make_unique<PostCard>(this, entry.id, options);
			}
		}
		_rows.push_back(std::move(row));
	}
	_overRow = -1;
	_overHit = {};
	_pressedRow = -1;
	rebuildIndex();
	refreshPlaceholder();
	relayout();
	Q_EMIT emptyChanged(_rows.empty());
}

QStringList PostList::ids() const {
	auto result = QStringList();
	for (const auto &row : _rows) {
		if (row.card) {
			result.push_back(row.id);
		}
	}
	return result;
}

void PostList::rebuildIndex() {
	_index.clear();
	for (auto i = 0; i != int(_rows.size()); ++i) {
		_index.insert(_rows[i].id, i);
	}
}

int PostList::indexOf(const QString &postId) const {
	return _index.value(postId, -1);
}

PostCard *PostList::card(const QString &postId) const {
	const auto index = indexOf(postId);
	return (index >= 0) ? _rows[index].card.get() : nullptr;
}

// ---- layout

int PostList::tailHeight() const {
	if (_rows.empty() || !_descriptor.loader) {
		return 0;
	} else if (_loading && !_reloading) {
		return 2 * st::eliListSkeletonHeight;
	} else if (_failedMore) {
		return st::eliListTailHeight;
	}
	return st::eliListBottomSkip;
}

int PostList::resizeGetHeight(int newWidth) {
	if (newWidth <= 0) {
		return height();
	}
	const auto width = std::min(newWidth, int(st::eliCardMaxWidth));
	auto top = 0;
	for (auto &row : _rows) {
		row.top = top;
		row.height = row.card
			? (row.card->resizeGetHeight(width) + 1)
			: st::eliReplyMoreRowHeight;
		top += row.height;
	}
	_layoutWidth = newWidth;
	_rowsHeight = top;
	auto result = top + tailHeight();
	const auto left = (newWidth - width) / 2;
	if (_rows.empty()) {
		const auto filler = std::max(_minHeight, 0);
		if (_placeholder) {
			const auto wanted = std::max(
				_placeholder->contentHeight(width) + 2 * st::space2xl,
				filler);
			_placeholder->setGeometry(left, 0, width, wanted);
			result = wanted;
		} else if (_loading || !_loadedOnce) {
			result = std::max(kSkeletonRows * st::eliListSkeletonHeight, filler);
		} else {
			result = filler;
		}
	}
	if (_retry) {
		_retry->resizeToNaturalWidth(width);
		_retry->move(
			left + (width - _retry->width()) / 2,
			_rowsHeight + (st::eliListTailHeight - _retry->height()) / 2 + 10);
	}
	return result;
}

void PostList::relayout() {
	if (width() <= 0) {
		return;
	}
	const auto was = height();
	resizeToWidth(width());
	if (_loading && !_reloading) {
		_skeleton.start();
	} else {
		_skeleton.stop();
	}
	if (height() == was) {
		rangeChanged();
	}
	update();
}

void PostList::setMinHeight(int height) {
	if (_minHeight != height) {
		_minHeight = height;
		if (_rows.empty() && width() > 0) {
			resizeToWidth(width());
		}
	}
}

void PostList::refreshPlaceholder() {
	const auto wantError = _rows.empty() && _failedFirst;
	const auto wantEmpty = _rows.empty()
		&& !_failedFirst
		&& !_loading
		&& _loadedOnce
		&& (_descriptor.empty != nullptr);
	if (_placeholder && (_placeholderIsError != wantError || (!wantError && !wantEmpty))) {
		delete _placeholder;
		_placeholder = nullptr;
	}
	if (!_placeholder && (wantError || wantEmpty)) {
		auto descriptor = Ui::EmptyPlaceholderDescriptor();
		if (wantError) {
			descriptor.icon = &st::emptyIconError;
			descriptor.title = tr::lng_error_load(tr::now);
			descriptor.text = _errorText;
			descriptor.buttonText = tr::lng_try_again(tr::now);
			descriptor.buttonCallback = [this] {
				_failedFirst = false;
				load(true);
			};
		} else {
			descriptor = _descriptor.empty();
		}
		_placeholder = new Ui::EmptyPlaceholder(this, std::move(descriptor));
		_placeholderIsError = wantError;
		_placeholder->show();
	}
	const auto wantRetry = !_rows.empty() && _failedMore;
	if (_retry && !wantRetry) {
		delete _retry;
		_retry = nullptr;
	} else if (!_retry && wantRetry) {
		_retry = new Ui::LinkButton(this, tr::lng_try_again(tr::now));
		_retry->setClickedCallback([this] {
			_failedMore = false;
			load(false);
		});
		_retry->show();
	}
}

QRect PostList::cardRect(int index) const {
	const auto &row = _rows[index];
	return QRect(cardLeft(), row.top, cardWidth(), row.height);
}

QRect PostList::rowRect(const QString &postId) const {
	const auto index = indexOf(postId);
	return (index >= 0) ? cardRect(index) : QRect();
}

void PostList::updateRow(int index) {
	if (index >= 0 && index < int(_rows.size())) {
		update(cardRect(index));
	}
}

int PostList::rowAt(int y) const {
	if (_rows.empty() || y < 0 || y >= _rowsHeight) {
		return -1;
	}
	const auto i = std::upper_bound(
		_rows.begin(),
		_rows.end(),
		y,
		[](int value, const Row &row) { return value < row.top; });
	return int(i - _rows.begin()) - 1;
}

void PostList::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
	rangeChanged();
}

void PostList::rangeChanged() {
	manageMedia();
	// The cards at least half on screen.
	auto half = QSet<QString>();
	auto list = QStringList();
	if (isVisible() && _visibleBottom > _visibleTop) {
		for (auto i = std::max(rowAt(std::max(_visibleTop, 0)), 0); i < int(_rows.size()); ++i) {
			const auto &row = _rows[i];
			if (row.top >= _visibleBottom) {
				break;
			} else if (!row.card || row.card->post().deleted) {
				continue;
			}
			const auto shown = std::min(row.top + row.height, _visibleBottom)
				- std::max(row.top, _visibleTop);
			// A card taller than the viewport counts when it fills half of it.
			if (shown * 2 >= std::min(row.height, _visibleBottom - _visibleTop)) {
				half.insert(row.id);
				list.push_back(row.id);
			}
		}
	}
	const auto now = Ui::Now();
	auto appeared = QStringList();
	for (const auto &id : list) {
		if (!_halfVisible.contains(id)) {
			appeared.push_back(id);
		}
	}
	_halfVisible = half;
	_views.setVisible(list, now);
	const auto next = _views.nextCheckIn(now);
	if (next >= 0) {
		_viewTimer.callOnce(next + 1);
	} else {
		_viewTimer.cancel();
	}
	if (!appeared.isEmpty()) {
		const auto stale = _freshness.takeStale(appeared, now);
		if (!stale.isEmpty()) {
			_controller->session().data().feed().refreshCounts(stale);
		}
	}
	if (!_loading
		&& !_done
		&& _descriptor.loader
		&& !_failedMore
		&& !_rows.empty()
		&& _visibleBottom > _rowsHeight - kLoadMoreDistance) {
		loadMore();
	}
}

void PostList::checkViews() {
	const auto now = Ui::Now();
	const auto seen = _views.takeSeen(now);
	if (!seen.isEmpty()) {
		_controller->session().data().feed().reportViews(seen);
	}
	const auto next = _views.nextCheckIn(now);
	if (next >= 0) {
		_viewTimer.callOnce(next + 1);
	}
}

void PostList::manageMedia() {
	const auto screen = std::max(_visibleBottom - _visibleTop, 0);
	const auto from = _visibleTop - screen;
	const auto till = _visibleBottom + screen;
	const auto shown = isVisible() && (screen > 0);
	for (auto &row : _rows) {
		if (!row.card) {
			continue;
		}
		const auto near = shown && (row.top + row.height > from) && (row.top < till);
		if (near && !row.card->mediaLoaded()) {
			row.card->loadMedia();
		} else if (!near && row.card->mediaLoaded()) {
			row.card->unloadMedia();
		}
	}
}

void PostList::showEvent(QShowEvent *e) {
	RpWidget::showEvent(e);
	rangeChanged();
}

void PostList::hideEvent(QHideEvent *e) {
	RpWidget::hideEvent(e);
	Ui::Tooltip::Hide();
	_tooltipTimer.cancel();
	rangeChanged();
}

// ---- store signals

void PostList::postChanged(const QString &id) {
	const auto index = indexOf(id);
	if (index < 0 || !_rows[index].card) {
		return;
	}
	if (_rows[index].card->refresh()) {
		relayout();
	} else {
		updateRow(index);
	}
}

void PostList::postRemoved(const QString &id) {
	if (_descriptor.keepRemoved) {
		postChanged(id);
	} else {
		remove(id);
	}
}

void PostList::peopleChanged(const QStringList &ids) {
	const auto has = [&](const QString &id) {
		return !id.isEmpty() && ids.contains(id);
	};
	auto changed = false;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		auto &row = _rows[i];
		if (!row.card) {
			continue;
		}
		const auto &post = row.card->post();
		if (has(post.author)
			|| has(row.card->options().actor)
			|| has(row.card->options().replyingTo)
			|| (post.quote && has(post.quote->author))) {
			const auto was = row.card->height();
			row.card->refreshPeople();
			if (row.card->height() != was) {
				changed = true;
			} else {
				updateRow(i);
			}
		}
	}
	if (changed) {
		relayout();
	}
}

// ---- painting

void PostList::paintSkeleton(QPainter &p, int top, const QRect &clip) const {
	const auto height = st::eliListSkeletonHeight;
	if (top + height <= clip.top() || top >= clip.top() + clip.height()) {
		return;
	}
	const auto outer = rect();
	const auto width = cardWidth();
	const auto left = cardLeft();
	const auto &padding = st::eliCardPadding;
	const auto userpic = st::eliListSkeletonUserpic;
	const auto rtl = [&](int x, int y, int w, int h) {
		return QRectF(style::rtlrect(x, y, w, h, width).translated(left, top));
	};
	_skeleton.paint(p, rtl(padding.left(), padding.top(), userpic, userpic), userpic / 2., outer);
	const auto lineLeft = padding.left() + userpic + st::eliCardUserpicSkip;
	const auto lineWidth = width - lineLeft - padding.left();
	_skeleton.paint(p, rtl(lineLeft, padding.top() + 2, lineWidth * 2 / 5, 12), 6., outer);
	_skeleton.paint(p, rtl(lineLeft, padding.top() + 30, lineWidth * 9 / 10, 12), 6., outer);
	_skeleton.paint(p, rtl(lineLeft, padding.top() + 54, lineWidth * 7 / 10, 12), 6., outer);
	_skeleton.paint(p, rtl(lineLeft, padding.top() + 86, lineWidth * 3 / 10, 12), 6., outer);
	p.fillRect(QRect(left, top + height - 1, width, 1), st::eliCardLineFg);
}

void PostList::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto left = cardLeft();
	const auto width = cardWidth();
	if (_rows.empty()) {
		if (!_placeholder && (_loading || !_loadedOnce)) {
			for (auto i = 0; i != kSkeletonRows; ++i) {
				paintSkeleton(p, i * st::eliListSkeletonHeight, clip);
			}
		}
		return;
	}
	const auto highlight = _highlight.value(0.);
	const auto burst = _burst.animating() ? _burst.value(1.) : -1.;
	for (auto i = std::max(rowAt(std::max(clip.top(), 0)), 0); i < int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		if (row.top >= clip.top() + clip.height()) {
			break;
		}
		const auto rect = QRect(left, row.top, width, row.height);
		if (!row.card) {
			const auto over = (_overRow == i);
			p.setPen(st::eliReplyMoreFg);
			p.setFont(over ? st::eliReplyMoreFont->underline() : st::eliReplyMoreFont);
			p.drawTextLeft(
				st::eliCardPadding.left() + row.moreIndent + st::eliCardUserpic + st::eliCardUserpicSkip,
				row.top + (row.height - st::eliReplyMoreFont->height) / 2,
				width,
				row.moreText);
			continue;
		}
		const auto feedLook = (row.card->options().mode == CardMode::Feed);
		if (row.id == _activeId && feedLook) {
			p.fillRect(rect, st::stateActive);
		} else if (_overRow == i && feedLook && _overHit.type != HitType::None) {
			p.fillRect(rect, st::eliCardOverBg);
		}
		if (highlight > 0. && row.id == _highlightId) {
			p.setOpacity(highlight);
			p.fillRect(rect, st::eliCardHighlightBg);
			p.setOpacity(1.);
		}
		auto state = CardPaintState();
		state.clip = clip.translated(-left, -row.top);
		if (_overRow == i) {
			state.hover = _overHit;
		}
		if (_pressedRow == i) {
			state.pressed = _pressedHit;
		}
		state.selected = hasFocus() && !_selectedId.isEmpty() && (row.id == _selectedId);
		if (burst >= 0. && row.id == _burstId) {
			state.burst = burst;
			state.burstCenter = _burstPoint;
		}
		p.translate(left, row.top);
		row.card->paint(p, state);
		p.translate(-left, -row.top);
		p.setOpacity(1.);
		if (row.card->options().mode != CardMode::Reply || !row.card->options().line) {
			p.fillRect(
				QRect(left, row.top + row.height - 1, width, 1),
				st::eliCardLineFg);
		}
	}
	if (_loading && !_reloading) {
		paintSkeleton(p, _rowsHeight, clip);
		paintSkeleton(p, _rowsHeight + st::eliListSkeletonHeight, clip);
	} else if (_failedMore) {
		p.setPen(st::eliListTailFg);
		p.setFont(st::eliListTailFont);
		const auto text = st::eliListTailFont->elided(_errorText, width - 2 * st::spaceLg);
		p.drawTextCentered(
			QRect(left, _rowsHeight + 6, width, st::eliListTailFont->height + 8),
			text);
	} else if (_done && _loadedOnce && _descriptor.loader && int(_rows.size()) > 3) {
		// The end of the list: a quiet dot.
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::eliListEndFg);
		p.drawEllipse(QPoint(left + width / 2, _rowsHeight + st::eliListBottomSkip / 2), 2, 2);
	}
}

// ---- mouse

void PostList::setHover(int row, Hit hit) {
	if (_overRow == row && _overHit == hit) {
		return;
	}
	const auto was = _overRow;
	_overRow = row;
	_overHit = hit;
	updateRow(was);
	updateRow(row);
	const auto pointer = (row >= 0)
		&& (!_rows[row].card
			|| (hit.type != HitType::None && hit.type != HitType::Body));
	setCursor(pointer ? style::cur_pointer : style::cur_default);
	Ui::Tooltip::Hide();
	_tooltipTimer.cancel();
	if (row >= 0 && _rows[row].card && !_rows[row].card->tooltip(hit).isEmpty()) {
		_tooltipTimer.callOnce(kTooltipDelay);
	}
}

void PostList::showTooltip() {
	if (_overRow < 0 || _overRow >= int(_rows.size()) || !_rows[_overRow].card) {
		return;
	}
	const auto text = _rows[_overRow].card->tooltip(_overHit);
	if (!text.isEmpty() && underMouse()) {
		Ui::Tooltip::ShowAt(mapToGlobal(_lastMouse) + QPoint(0, 18), text);
	}
}

void PostList::updateHover(QPoint position) {
	_lastMouse = position;
	const auto index = rowAt(position.y());
	const auto left = cardLeft();
	if (index < 0 || position.x() < left || position.x() >= left + cardWidth()) {
		setHover(-1, {});
		return;
	}
	const auto &row = _rows[index];
	if (!row.card) {
		setHover(index, {});
		return;
	}
	setHover(index, row.card->hitTest(position - QPoint(left, row.top)));
}

void PostList::mouseMoveEvent(QMouseEvent *e) {
	updateHover(e->pos());
}

void PostList::leaveEvent(QEvent *e) {
	RpWidget::leaveEvent(e);
	setHover(-1, {});
}

void PostList::mousePressEvent(QMouseEvent *e) {
	updateHover(e->pos());
	setFocus();
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressedRow = _overRow;
	_pressedHit = _overHit;
	updateRow(_pressedRow);
}

void PostList::mouseReleaseEvent(QMouseEvent *e) {
	updateHover(e->pos());
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto row = std::exchange(_pressedRow, -1);
	const auto hit = std::exchange(_pressedHit, {});
	updateRow(row);
	if (row < 0 || row != _overRow || !(hit == _overHit) || row >= int(_rows.size())) {
		return;
	}
	if (!_rows[row].card) {
		Q_EMIT moreRequested(_rows[row].id);
		return;
	}
	activate(row, hit, e->globalPosition().toPoint());
}

void PostList::mouseDoubleClickEvent(QMouseEvent *e) {
	updateHover(e->pos());
	if (e->button() != Qt::LeftButton
		|| _overRow < 0
		|| !_rows[_overRow].card
		|| _overHit.type != HitType::Body) {
		mousePressEvent(e);
		return;
	}
	const auto &row = _rows[_overRow];
	const auto &post = row.card->post();
	_burstId = row.id;
	_burstPoint = e->pos() - QPoint(cardLeft(), row.top);
	const auto index = _overRow;
	_burst.start([=, this] { updateRow(index); }, 0., 1., st::eliHeartBurstDuration);
	if (post.my.reaction != u"like") {
		_controller->session().data().feed().react(post.id, Data::Feed::Reaction::Like);
	}
}

void PostList::contextMenuEvent(QContextMenuEvent *e) {
	updateHover(e->pos());
	if (_overRow < 0 || !_rows[_overRow].card) {
		return;
	}
	const auto &card = *_rows[_overRow].card;
	const auto &post = card.post();
	if (post.deleted || post.id.isEmpty()) {
		return;
	}
	if (_overHit.type == HitType::Like || _overHit.type == HitType::Dislike) {
		OpenReactors(_controller, post.id, _overHit.type == HitType::Dislike);
		return;
	}
	if (card.options().mode == CardMode::Reply) {
		// The root is the owner's business: it gave us the rows.
		ShowReplyMenu(
			_controller,
			this,
			post.threadRoot.isEmpty() ? post.replyTo : post.threadRoot,
			post.id,
			e->globalPos());
		return;
	}
	auto linkToCopy = QString();
	if (_overHit.type == HitType::Link) {
		const auto link = card.link(_overHit.index);
		linkToCopy = Ui::Text::LinkUrl(link);
	}
	ShowContextMenu(_controller, this, post.id, e->globalPos(), linkToCopy);
}

void PostList::activate(int rowIndex, Hit hit, QPoint globalPosition) {
	auto &row = _rows[rowIndex];
	auto &card = *row.card;
	const auto post = card.post(); // a copy: actions may change the store
	const auto id = post.id;
	auto &feed = _controller->session().data().feed();
	const auto anchor = [&] {
		const auto rect = card.hitRect(hit).translated(cardLeft(), row.top);
		return mapToGlobal(style::RightToLeft()
			? QPoint(rect.right() + 1, rect.bottom() + 1)
			: rect.bottomLeft() + QPoint(0, 1));
	};
	switch (hit.type) {
	case HitType::None:
		break;
	case HitType::Body:
		if (card.options().mode == CardMode::Feed) {
			OpenPost(_controller, id);
		}
		break;
	case HitType::Userpic:
		if (_controller->session().data().stories().ring(post.author)
			!= Data::StoryRing::None) {
			// A ringed face opens the stories.
			Stories::OpenUserStories(_controller, post.author);
		} else {
			OpenProfile(_controller, post.author);
		}
		break;
	case HitType::Author:
		OpenProfile(_controller, post.author);
		break;
	case HitType::ContextActor:
		OpenProfile(_controller, card.options().actor);
		break;
	case HitType::ReplyingTo:
		OpenProfile(_controller, card.options().replyingTo);
		break;
	case HitType::More:
		if (card.options().mode == CardMode::Reply) {
			ShowReplyMenu(
				_controller,
				this,
				post.threadRoot.isEmpty() ? post.replyTo : post.threadRoot,
				id,
				anchor());
		} else {
			ShowMoreMenu(_controller, this, id, anchor());
		}
		break;
	case HitType::Link:
		Support::ActivateTextLink(_controller, card.link(hit.index));
		break;
	case HitType::ShowMore:
		card.setExpanded(true);
		relayout();
		break;
	case HitType::Media:
		if (card.tileIsVideo(hit.index)) {
			OpenPardeh(_controller, id);
		} else {
			OpenImageViewer(_controller, post, card.imageIndexOfTile(hit.index));
		}
		break;
	case HitType::QuoteMedia:
		if (post.quote) {
			if (card.tileIsVideo(hit.index)) {
				OpenPardeh(_controller, post.quote->id);
			} else {
				OpenImageViewer(_controller, *post.quote, card.imageIndexOfTile(hit.index));
			}
		}
		break;
	case HitType::VideoNote:
		OpenVideoNote(_controller, id);
		break;
	case HitType::PollOption:
		feed.vote(id, hit.index);
		break;
	case HitType::PollRetract:
		feed.vote(id, -1);
		break;
	case HitType::Quote:
		if (post.quote) {
			OpenPost(_controller, post.quote->id);
		}
		break;
	case HitType::InviteJoin:
		if (post.conversation) {
			_controller->openRoute(
				Window::Routes::Conversation(post.conversation->id));
		}
		break;
	case HitType::Audio:
		if (post.clip) {
			auto track = Media::AudioTrack();
			track.id = card.audioTrackId();
			track.ref = Data::MediaRef::PostMedia(id, post.clip->media);
			track.variant = Data::MediaVariant::Audio;
			track.title = post.clip->title;
			track.performer = post.clip->artist;
			track.durationMs = post.clip->endMs - post.clip->startMs;
			track.voice = (post.clip->source == u"record");
			Media::Player().toggle(track, &_controller->session());
		}
		break;
	case HitType::Thread:
		OpenThread(
			_controller,
			(post.threadRoot.isEmpty() || post.threadRoot == id) ? id : post.threadRoot);
		break;
	case HitType::Reply:
		if (!post.canReply) {
			if (card.options().mode == CardMode::Feed) {
				OpenPost(_controller, id);
			}
		} else if (_descriptor.replyToOwner) {
			Q_EMIT replyRequested(id);
		} else {
			auto args = ComposeArgs();
			args.kind = ComposeArgs::Kind::Reply;
			args.postId = id;
			OpenCompose(_controller, args);
		}
		break;
	case HitType::Repost:
		if (post.canRepost || post.my.reposted) {
			ShowRepostMenu(_controller, this, id, anchor());
		}
		break;
	case HitType::Like:
		ToggleLike(_controller, id);
		break;
	case HitType::Dislike:
		ToggleDislike(_controller, id);
		break;
	case HitType::Bookmark:
		ToggleBookmark(_controller, id);
		break;
	case HitType::Share:
		ShowShareMenu(_controller, this, id, anchor());
		break;
	case HitType::LikesStat:
		OpenReactors(_controller, id, false);
		break;
	case HitType::DislikesStat:
		OpenReactors(_controller, id, true);
		break;
	case HitType::Follow:
		Q_EMIT followRequested(id);
		break;
	}
	Q_UNUSED(globalPosition);
}

// ---- keyboard

void PostList::select(int index) {
	if (_rows.empty()) {
		return;
	}
	index = std::clamp(index, 0, int(_rows.size()) - 1);
	const auto was = indexOf(_selectedId);
	_selectedId = _rows[index].id;
	updateRow(was);
	updateRow(index);
	Q_EMIT scrollToRequested(_rows[index].top, _rows[index].top + _rows[index].height);
}

QString PostList::selectedId() const {
	return _selectedId;
}

bool PostList::handleKey(QKeyEvent *e) {
	if (_rows.empty()) {
		return false;
	}
	const auto current = indexOf(_selectedId);
	const auto firstVisible = [&] {
		for (auto i = std::max(rowAt(std::max(_visibleTop, 0)), 0); i < int(_rows.size()); ++i) {
			if (_rows[i].card && _rows[i].top + _rows[i].height / 2 >= _visibleTop) {
				return i;
			}
		}
		return 0;
	};
	const auto step = [&](int delta) {
		auto index = (current < 0) ? firstVisible() : (current + delta);
		while (index >= 0 && index < int(_rows.size()) && !_rows[index].card) {
			index += delta;
		}
		if (index >= 0 && index < int(_rows.size())) {
			select(index);
		}
	};
	if (SameLetter(e, Qt::Key_J, u"ت")) {
		step(1);
		return true;
	} else if (SameLetter(e, Qt::Key_K, u"ن")) {
		step(-1);
		return true;
	}
	if (current < 0 || !_rows[current].card) {
		return false;
	}
	const auto &post = _rows[current].card->post();
	if (post.deleted) {
		return false;
	}
	if (SameLetter(e, Qt::Key_L, u"م")) {
		ToggleLike(_controller, post.id);
		return true;
	} else if (SameLetter(e, Qt::Key_B, u"ذ")) {
		ToggleBookmark(_controller, post.id);
		return true;
	} else if (SameLetter(e, Qt::Key_R, u"ق")) {
		if (post.canReply) {
			if (_descriptor.replyToOwner) {
				Q_EMIT replyRequested(post.id);
			} else {
				auto args = ComposeArgs();
				args.kind = ComposeArgs::Kind::Reply;
				args.postId = post.id;
				OpenCompose(_controller, args);
			}
		}
		return true;
	} else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
		&& !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
		if (_rows[current].card->options().mode == CardMode::Feed) {
			OpenPost(_controller, post.id);
		}
		return true;
	}
	return false;
}

void PostList::keyPressEvent(QKeyEvent *e) {
	if (!handleKey(e)) {
		e->ignore();
	}
}

void PostList::setActivePost(const QString &postId) {
	if (_activeId == postId) {
		return;
	}
	const auto was = indexOf(_activeId);
	_activeId = postId;
	updateRow(was);
	updateRow(indexOf(_activeId));
}

void PostList::highlight(const QString &postId) {
	const auto index = indexOf(postId);
	if (index < 0) {
		return;
	}
	_highlightId = postId;
	_highlight.start(
		[=, this] { updateRow(indexOf(postId)); },
		1.,
		0.,
		kHighlightDuration,
		anim::easeOutCubic);
	updateRow(index);
}

} // namespace Eli
