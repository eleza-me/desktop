#include "profile/profile_posts.h"

#include "data/data_feed.h"
#include "data/data_session.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "profile/profile_logic.h"
#include "support/error_text.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

#include <QtCore/QDateTime>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>

namespace Profile {
namespace {

constexpr auto kMediaPagesPerLoad = 5;
constexpr auto kTextLines = 3;

} // namespace

Ui::object_ptr<Window::EmbeddedList> CreatePostsList(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &source,
		const QString &userId) {
	auto params = QVariantMap{ { u"source"_s, source } };
	if (source != u"bookmarks") {
		params.insert(u"user"_s, userId);
	}
	if (auto embed = Window::CreateEmbed(
			u"eli.posts"_s,
			parent,
			controller,
			params)) {
		return embed;
	}
	return Ui::Owned<Window::EmbeddedList>(
		new CompactPosts(parent, controller, source, userId));
}

bool IsMediaPost(const Data::Post &post) {
	if (!post.media.isEmpty()) {
		return true;
	} else if (post.type == u"audio"
		&& post.clip
		&& post.clip->source != u"record") {
		return true;
	}
	return (post.type == u"media"
		|| post.type == u"video_note"
		|| post.type == u"audio") && post.clip.has_value();
}

CompactPosts::CompactPosts(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &source,
	const QString &userId)
: EmbeddedList(parent)
, _controller(controller)
, _source(source)
, _userId(userId)
, _spinner(std::make_unique<Ui::InfiniteRadialAnimation>([this] {
	update();
})) {
	setMouseTracking(true);
	auto &feed = controller->session().data().feed();
	connect(&feed, &Data::Feed::postChanged, this, [this](const QString &id) {
		const auto post = _controller->session().data().feed().post(id);
		for (auto i = 0; i != int(_rows.size()); ++i) {
			if (_rows[i]->id != id) {
				continue;
			} else if (!post
				|| post->deleted
				|| (_source == u"bookmarks" && !post->my.bookmarked)) {
				removeRow(id);
			} else {
				fillRow(*_rows[i], *post);
				recount();
				refreshHeight();
				update();
			}
			return;
		}
	});
	connect(&feed, &Data::Feed::postRemoved, this, [this](const QString &id) {
		removeRow(id);
	});
	connect(&feed, &Data::Feed::postPublished, this, [this](const QString &id) {
		const auto &data = _controller->session().data();
		const auto post = data.feed().post(id);
		if (!post || post->author != _userId || !_loadedOnce) {
			return;
		}
		const auto reply = !post->replyTo.isEmpty();
		const auto fits = (_source == u"user" && !reply)
			|| (_source == u"user_replies" && reply)
			|| (_source == u"user_media" && !reply && IsMediaPost(*post));
		if (!fits) {
			return;
		}
		auto row = std::make_unique<Row>();
		row->id = id;
		fillRow(*row, *post);
		_rows.insert(_rows.begin(), std::move(row));
		_over = _pressed = -1;
		if (_empty) {
			_empty = false;
			Q_EMIT emptyChanged(false);
		}
		recount();
		refreshHeight();
		update();
	});
	load();
}

CompactPosts::~CompactPosts() = default;

void CompactPosts::setVisibleRange(int top, int bottom) {
	_visibleTop = top;
	_visibleBottom = bottom;
}

void CompactPosts::loadMore() {
	if (!_loading && !_allLoaded && _error.isEmpty()) {
		load();
	}
}

void CompactPosts::reload() {
	++_generation;
	_rows.clear();
	_next = QString();
	_mediaPages = 0;
	_loading = false;
	_loadedOnce = false;
	_allLoaded = false;
	_error = QString();
	_over = _pressed = -1;
	if (_empty) {
		_empty = false;
		Q_EMIT emptyChanged(false);
	}
	recount();
	load();
}

void CompactPosts::load() {
	_loading = true;
	_error = QString();
	_spinner->start();
	const auto generation = _generation;
	auto &feed = _controller->session().data().feed();
	const auto done = [=, this](const Data::PostsPage &page) {
		if (generation == _generation) {
			pageLoaded(page);
		}
	};
	const auto fail = [=, this](const Api::Error &error) {
		if (generation == _generation) {
			failed(error);
		}
	};
	if (_source == u"bookmarks") {
		feed.bookmarks(_next, this, done, fail);
	} else {
		feed.userPosts(
			_userId,
			(_source == u"user_replies") ? u"replies"_s : u"posts"_s,
			_next,
			QString(),
			this,
			done,
			fail);
	}
	refreshHeight();
	update();
}

void CompactPosts::pageLoaded(const Data::PostsPage &page) {
	_loading = false;
	_spinner->stop();
	const auto &feed = _controller->session().data().feed();
	auto ids = QStringList();
	for (const auto &id : page.posts) {
		const auto post = feed.post(id);
		if (!post || post->deleted) {
			continue;
		} else if (_source == u"user_media" && !IsMediaPost(*post)) {
			continue;
		}
		ids.push_back(id);
	}
	_next = (_source == u"bookmarks") ? page.nextCursor : page.nextBefore;
	_allLoaded = _next.isEmpty();
	appendPosts(ids);
	// The media tab filters on this side: a page without media asks for the
	// next one right away (a few at a time).
	if (_source == u"user_media"
		&& ids.isEmpty()
		&& !_allLoaded
		&& ++_mediaPages < kMediaPagesPerLoad) {
		load();
		return;
	}
	_mediaPages = 0;
	_loadedOnce = true;
	const auto empty = _rows.empty() && _allLoaded;
	if (_empty != empty) {
		_empty = empty;
		Q_EMIT emptyChanged(empty);
	}
	refreshHeight();
	update();
}

void CompactPosts::failed(const Api::Error &error) {
	_loading = false;
	_spinner->stop();
	_error = Support::ErrorText(error, tr::lng_error_load(tr::now));
	refreshHeight();
	update();
}

void CompactPosts::appendPosts(const QStringList &ids) {
	const auto &feed = _controller->session().data().feed();
	for (const auto &id : ids) {
		const auto exists = std::any_of(
			_rows.begin(),
			_rows.end(),
			[&](const auto &row) { return row->id == id; });
		const auto post = feed.post(id);
		if (exists || !post) {
			continue;
		}
		auto row = std::make_unique<Row>();
		row->id = id;
		fillRow(*row, *post);
		_rows.push_back(std::move(row));
	}
	recount();
}

void CompactPosts::fillRow(Row &row, const Data::Post &post) {
	auto text = post.text.trimmed();
	const auto hasVideo = std::any_of(
		post.media.begin(),
		post.media.end(),
		[](const Data::PostMedia &media) { return media.kind != u"image"; });
	row.kind = post.poll
		? &st::profilePostPollIcon
		: (post.type == u"audio")
		? &st::profilePostAudioIcon
		: hasVideo
		? &st::profilePostVideoIcon
		: !post.media.isEmpty()
		? &st::profilePostImageIcon
		: (post.type == u"thread")
		? &st::profilePostThreadIcon
		: nullptr;
	if (text.isEmpty()) {
		text = post.poll
			? post.poll->question
			: (post.conversation && !post.conversation->topic.isEmpty())
			? post.conversation->topic
			: tr::lng_profile_media(tr::now);
	}
	row.text.setMarkedText(st::profilePostTextStyle, ParseBio(text));
	row.time = Lang::RelativeTime(
		QDateTime::fromMSecsSinceEpoch(post.createdAt));
	row.likes = Lang::CompactCount(post.counts.likes);
	row.replies = Lang::CompactCount(post.counts.replies);
	row.pinned = post.pinned && (_source == u"user");
	row.height = 0;
}

void CompactPosts::removeRow(const QString &id) {
	const auto i = std::find_if(_rows.begin(), _rows.end(), [&](const auto &row) {
		return row->id == id;
	});
	if (i == _rows.end()) {
		return;
	}
	_rows.erase(i);
	_over = _pressed = -1;
	recount();
	const auto empty = _rows.empty() && _allLoaded && _loadedOnce;
	if (_empty != empty) {
		_empty = empty;
		Q_EMIT emptyChanged(empty);
	}
	refreshHeight();
	update();
}

int CompactPosts::textWidth(int forWidth) const {
	const auto &padding = st::profilePostPadding;
	return std::max(forWidth - padding.left() - padding.right(), 0);
}

void CompactPosts::recount() {
	recount(width());
}

void CompactPosts::recount(int forWidth) {
	const auto &padding = st::profilePostPadding;
	const auto available = textWidth(forWidth);
	auto top = 0;
	for (const auto &row : _rows) {
		row->top = top;
		row->height = padding.top()
			+ (row->pinned
				? (st::profilePostMetaFont->height + st::profilePostMetaSkip)
				: 0)
			+ row->text.countHeight(available, kTextLines)
			+ st::profilePostMetaSkip
			+ st::profilePostMetaFont->height
			+ padding.bottom();
		top += row->height;
	}
	_rowsHeight = top;
	_countedWidth = forWidth;
}

void CompactPosts::refreshHeight() {
	const auto wanted = resizeGetHeight(width());
	if (wanted != height()) {
		resize(width(), wanted);
		Q_EMIT heightChanged();
	}
}

int CompactPosts::resizeGetHeight(int newWidth) {
	if (_countedWidth != newWidth) {
		recount(newWidth);
	}
	if (_empty) {
		return 0;
	}
	const auto footer = (_loading || !_error.isEmpty())
		? int(st::profilePostFooterHeight)
		: 0;
	return _rowsHeight + footer;
}

int CompactPosts::rowAt(QPoint position) const {
	if (position.y() < 0 || position.y() >= _rowsHeight) {
		return -1;
	}
	const auto i = std::upper_bound(
		_rows.begin(),
		_rows.end(),
		position.y(),
		[](int y, const auto &row) { return y < row->top; });
	return int(i - _rows.begin()) - 1;
}

void CompactPosts::updateRow(int index) {
	if (index >= 0 && index < int(_rows.size())) {
		update(0, _rows[index]->top, width(), _rows[index]->height);
	}
}

void CompactPosts::setOver(int index) {
	if (_over == index) {
		return;
	}
	updateRow(_over);
	_over = index;
	updateRow(_over);
}

void CompactPosts::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto clip = e->rect();
	if (_visibleBottom > _visibleTop) {
		clip = clip.intersected(
			QRect(0, _visibleTop, width(), _visibleBottom - _visibleTop));
	}
	const auto from = std::lower_bound(
		_rows.begin(),
		_rows.end(),
		clip.top(),
		[](const auto &row, int y) { return row->top + row->height <= y; });
	for (auto i = from; i != _rows.end(); ++i) {
		const auto &row = **i;
		if (row.top > clip.bottom()) {
			break;
		}
		const auto index = int(i - _rows.begin());
		paintRow(
			p,
			row,
			(_over == index),
			(_pressed == index) && (_over == index));
	}
	const auto footer = QRect(
		0,
		_rowsHeight,
		width(),
		st::profilePostFooterHeight);
	if (!footer.intersects(e->rect())) {
		return;
	}
	if (_loading) {
		const auto size = int(st::profileTailSpinner);
		_spinner->draw(
			p,
			QRect(
				(width() - size) / 2,
				footer.y() + (footer.height() - size) / 2,
				size,
				size),
			2.5,
			st::brandPrimary->c);
	} else if (!_error.isEmpty()) {
		const auto &font = st::profilePostMetaFont;
		const auto text = _error + u" · "_s + tr::lng_retry(tr::now);
		p.setFont(font->f);
		p.setPen(st::textSecondary);
		p.drawText(
			(width() - font->width(text)) / 2,
			footer.y() + (footer.height() - font->height) / 2 + font->ascent,
			text);
	}
}

void CompactPosts::paintRow(
		Painter &p,
		const Row &row,
		bool over,
		bool pressed) {
	const auto outer = width();
	const auto &padding = st::profilePostPadding;
	const auto available = textWidth(outer);
	if (pressed) {
		p.fillRect(0, row.top, outer, row.height, st::statePressed);
	} else if (over) {
		p.fillRect(0, row.top, outer, row.height, st::stateHover);
	}
	p.fillRect(
		style::rtlrect(
			padding.left(),
			row.top + row.height - st::lineWidth,
			available,
			st::lineWidth,
			outer),
		st::borderSubtle);
	const auto &font = st::profilePostMetaFont;
	auto y = row.top + padding.top();
	if (row.pinned) {
		const auto &icon = st::profilePostPinIcon;
		icon.paint(p, padding.left(), y + (font->height - icon.height()) / 2, outer);
		p.setFont(font->f);
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			padding.left() + icon.width() + st::profileBadgeSkip,
			y,
			outer,
			tr::lng_profile_pinned(tr::now));
		y += font->height + st::profilePostMetaSkip;
	}
	p.setPen(st::textPrimary);
	auto context = Ui::Text::PaintContext();
	context.position = QPoint(padding.left(), y);
	context.availableWidth = available;
	context.elisionLines = kTextLines;
	context.autoDirection = true;
	row.text.draw(p, context);
	y += row.text.countHeight(available, kTextLines) + st::profilePostMetaSkip;

	p.setFont(font->f);
	p.setPen(st::textTertiary);
	auto x = padding.left();
	const auto item = [&](const style::icon *icon, const QString &text) {
		if (icon) {
			icon->paint(p, x, y + (font->height - icon->height()) / 2, outer);
			x += icon->width() + st::profileBadgeSkip;
		}
		if (!text.isEmpty()) {
			p.drawTextLeft(x, y, outer, text);
			x += font->width(text);
		}
		x += st::profilePostMetaItemSkip;
	};
	if (row.kind) {
		item(row.kind, QString());
	}
	item(nullptr, row.time);
	item(&st::profilePostLikeIcon, row.likes);
	item(&st::profilePostReplyIcon, row.replies);
}

void CompactPosts::mouseMoveEvent(QMouseEvent *e) {
	const auto index = rowAt(e->pos());
	setOver(index);
	const auto footer = !_error.isEmpty() && (e->pos().y() >= _rowsHeight);
	setCursor((index >= 0 || footer) ? style::cur_pointer : style::cur_default);
}

void CompactPosts::mousePressEvent(QMouseEvent *e) {
	setOver(rowAt(e->pos()));
	if (e->button() == Qt::LeftButton) {
		_pressed = _over;
		updateRow(_pressed);
	}
}

void CompactPosts::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = _pressed;
	_pressed = -1;
	updateRow(pressed);
	setOver(rowAt(e->pos()));
	if (e->button() != Qt::LeftButton) {
		return;
	}
	if (!_error.isEmpty() && e->pos().y() >= _rowsHeight) {
		load();
	} else if (pressed >= 0 && pressed == _over) {
		// Opening the post may replace the section: nothing follows.
		_controller->openRoute(Window::Routes::Post(_rows[pressed]->id));
	}
}

void CompactPosts::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	setOver(-1);
}

void CompactPosts::contextMenuEvent(QContextMenuEvent *e) {
	const auto index = rowAt(e->pos());
	if (index < 0) {
		return;
	}
	if (_menu) {
		_menu->hideMenu(true);
	}
	const auto id = _rows[index]->id;
	// Without the direction marks the list added for painting.
	auto text = _rows[index]->text.toString();
	text.remove(QChar(0x200E));
	text.remove(QChar(0x200F));
	const auto controller = _controller;
	_menu = new Ui::PopupMenu(this);
	_menu->addAction(tr::lng_profile_open_post(tr::now), [=] {
		controller->openRoute(Window::Routes::Post(id));
	}, &st::profileMenuOpenIcon);
	_menu->addAction(tr::lng_profile_copy_text(tr::now), [=] {
		QGuiApplication::clipboard()->setText(text);
		controller->showToast(tr::lng_text_copied(tr::now));
	}, &st::menuIconCopy);
	_menu->popup(e->globalPos());
}

} // namespace Profile
