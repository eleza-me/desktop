// «پرده»: the vertical video pager over the whole window. The first page is
// the video that was clicked; the next ones come from the video feed (or from
// the same author's videos). Only the current page plays, the next one is
// fetched ahead.
//
// Wheel / Up / Down / PageUp / PageDown (J / K) switch videos, a click or
// Space pauses, a double click likes, M mutes, Left / Right seek, L likes,
// Esc closes. The column next to the video carries like / dislike / replies /
// repost / share / more; the author with a follow button, the caption and the
// sound sit over the bottom of the video with the seek bar.
#include "eli/eli_actions.h"

#include "base/timer.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_media.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "eli/eli_post_card.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_video_player.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtCore/QSet>
#include <QtGui/QKeyEvent>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>

namespace Eli {

QString LocalMediaFile(const Data::MediaHandlePtr &handle, const QString &suffix);

namespace {

constexpr auto kWheelPause = 420;
constexpr auto kSeekStepMs = 5000;
constexpr auto kHeartDuration = 700;
constexpr auto kDefaultAspect = 9. / 16.;

auto Muted = false;

enum class Part {
	None,
	Stage,
	Close,
	Mute,
	Previous,
	Next,
	Like,
	Dislike,
	Reply,
	Repost,
	Share,
	More,
	Author,
	Follow,
	Sound,
	Seek,
	Retry,
};

class Pardeh final : public Ui::RpWidget {
public:
	Pardeh(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &postId,
		const QString &authorId);
	~Pardeh();

protected:
	bool eventFilter(QObject *object, QEvent *event) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;

private:
	class Chrome;
	friend class Chrome;
	struct Layout {
		QRect stage;
		QRect close;
		QRect mute;
		QRect previous;
		QRect next;
		QRect like;
		QRect dislike;
		QRect reply;
		QRect repost;
		QRect share;
		QRect more;
		QRect userpic;
		QRect name;
		QRect follow;
		QRect caption;
		QRect sound;
		QRect seek;
		QRect retry;
		bool railInside = false;
	};

	[[nodiscard]] Main::Session &session() const {
		return _controller->session();
	}
	[[nodiscard]] const Data::Post *current() const;
	[[nodiscard]] const Data::PostMedia *videoOf(const Data::Post &post) const;
	[[nodiscard]] Layout layout() const;
	[[nodiscard]] Part partAt(QPoint point) const;

	void boot(const QString &postId);
	void addPost(const QString &id);
	void fill();
	void showPage(int index);
	void startVideo();
	void preloadNext();
	void step(int delta);
	void closeOverlay();
	void activate(Part part, QPoint global);
	void like(bool burst, QPoint center = QPoint());
	void follow();
	void applySeek(int x);
	void refreshTexts();
	void showMoreMenu(QPoint global);

	Window::SessionController *_controller = nullptr;
	QString _authorId;
	QStringList _ids;
	int _index = -1;
	QString _cursor;
	bool _exhausted = false;
	bool _fillLoading = false;
	bool _bootFailed = false;

	Media::VideoPlayerWidget *_video = nullptr;
	Chrome *_chrome = nullptr;
	Data::MediaHandlePtr _file;
	Data::MediaHandlePtr _poster;
	Data::MediaHandlePtr _nextFile;
	Data::MediaHandlePtr _nextPoster;
	QImage _backdrop;
	bool _videoFailed = false;
	bool _videoLoading = false;
	bool _paused = false;
	bool _seeking = false;
	QSet<QString> _followed;

	Ui::Text::String _caption;
	QString _name;
	QString _soundText;
	bool _showFollow = false;

	Support::UserpicPainter _userpics;
	Ui::InfiniteRadialAnimation _loading;
	Ui::Animations::Basic _ticker;
	Ui::Animations::Simple _heart;
	QPoint _heartCenter;
	qint64 _lastWheel = 0;
	bool _closing = false;

};

// Everything drawn over the video and all the mouse handling.
class Pardeh::Chrome final : public Ui::RpWidget {
public:
	Chrome(Pardeh *owner) : RpWidget(owner), _owner(owner) {
		setMouseTracking(true);
	}

protected:
	void paintEvent(QPaintEvent *e) override;

	void mouseMoveEvent(QMouseEvent *e) override {
		if (_owner->_seeking) {
			_owner->applySeek(e->pos().x());
			return;
		}
		const auto part = _owner->partAt(e->pos());
		if (part != _over) {
			_over = part;
			setCursor((part != Part::None && part != Part::Stage)
				? style::cur_pointer
				: style::cur_default);
			update();
		}
	}

	void leaveEvent(QEvent *e) override {
		RpWidget::leaveEvent(e);
		_over = Part::None;
		update();
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		_pressed = _owner->partAt(e->pos());
		if (_pressed == Part::Seek) {
			_owner->_seeking = true;
			_owner->applySeek(e->pos().x());
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto pressed = std::exchange(_pressed, Part::None);
		if (_owner->_seeking) {
			_owner->_seeking = false;
			update();
			return;
		}
		const auto part = _owner->partAt(e->pos());
		if (part == pressed) {
			_owner->activate(part, e->globalPosition().toPoint());
		}
	}

	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton && _owner->partAt(e->pos()) == Part::Stage) {
			// The first click of the pair paused the video: undo that.
			_owner->activate(Part::Stage, e->globalPosition().toPoint());
			_owner->like(true, e->pos());
		} else {
			mousePressEvent(e);
		}
	}

	void contextMenuEvent(QContextMenuEvent *e) override {
		if (_owner->current()) {
			_owner->showMoreMenu(e->globalPos());
		}
	}

private:
	friend class Pardeh;

	Pardeh *_owner = nullptr;
	Part _over = Part::None;
	Part _pressed = Part::None;

};

Pardeh::Pardeh(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &postId,
	const QString &authorId)
: RpWidget(parent)
, _controller(controller)
, _authorId(authorId)
, _video(new Media::VideoPlayerWidget(this))
, _chrome(new Chrome(this))
, _userpics(&controller->session(), [this](const QString &) { _chrome->update(); }, this)
, _loading([this] { _chrome->update(); })
, _ticker([this](qint64) {
	const auto seek = layout().seek;
	_chrome->update(seek.adjusted(0, -8, 0, 8));
	return true;
}) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);
	_video->setControlsShown(false);
	_video->setFullscreenAllowed(false);
	_video->setLooped(true);
	_video->setMuted(Muted);
	_video->setRadius(st::eliPardehStageRadius);
	Ui::MakeTransparentForMouse(_video);
	connect(_video, &Media::VideoPlayerWidget::failed, this, [this] {
		_videoFailed = true;
		_videoLoading = false;
		_loading.stop();
		_chrome->update();
	});
	connect(_video, &Media::VideoPlayerWidget::playingChanged, this, [this](bool playing) {
		if (playing) {
			_videoLoading = false;
			_loading.stop();
			_ticker.start();
		} else {
			_ticker.stop();
		}
		_chrome->update();
	});
	auto &data = controller->session().data();
	connect(&data.feed(), &Data::Feed::postChanged, this, [this](const QString &id) {
		if (_index >= 0 && _index < _ids.size() && _ids[_index] == id) {
			_chrome->update();
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this] {
		refreshTexts();
	});
	parent->installEventFilter(this);
	setGeometry(parent->rect());
	boot(postId);
}

Pardeh::~Pardeh() {
	_video->pause();
}

const Data::Post *Pardeh::current() const {
	return (_index >= 0 && _index < _ids.size())
		? session().data().feed().post(_ids[_index])
		: nullptr;
}

const Data::PostMedia *Pardeh::videoOf(const Data::Post &post) const {
	for (const auto &media : post.media) {
		if (media.kind == u"video") {
			return &media;
		}
	}
	return nullptr;
}

void Pardeh::boot(const QString &postId) {
	_bootFailed = false;
	_videoLoading = true;
	_loading.start();
	const auto start = [=, this] {
		addPost(postId);
		if (_ids.isEmpty()) {
			_bootFailed = true;
			_videoLoading = false;
			_loading.stop();
			_chrome->update();
			return;
		}
		showPage(0);
		fill();
	};
	if (const auto known = session().data().feed().post(postId); known && !known->media.isEmpty()) {
		start();
		return;
	}
	session().data().feed().fetchPost(
		postId,
		base::Guard(this),
		[=](const Data::Post &) { start(); },
		[this](const Api::Error &) {
			_bootFailed = true;
			_videoLoading = false;
			_loading.stop();
			_chrome->update();
		});
}

void Pardeh::addPost(const QString &id) {
	if (_ids.contains(id)) {
		return;
	}
	const auto post = session().data().feed().post(id);
	if (post && !post->deleted && videoOf(*post)) {
		_ids.push_back(id);
	}
}

void Pardeh::fill() {
	if (_fillLoading || _exhausted) {
		return;
	}
	_fillLoading = true;
	auto &feed = session().data().feed();
	const auto fail = [this](const Api::Error &) {
		_fillLoading = false;
		_exhausted = true;
	};
	if (!_authorId.isEmpty()) {
		feed.userPosts(
			_authorId,
			QString(),
			_cursor,
			u"video"_s,
			base::Guard(this),
			[this](const Data::PostsPage &page) {
				_fillLoading = false;
				for (const auto &id : page.posts) {
					addPost(id);
				}
				_cursor = page.nextBefore;
				_exhausted = _cursor.isEmpty() || page.posts.isEmpty();
				preloadNext();
				_chrome->update();
			},
			fail);
	} else {
		feed.loadVideoFeed(
			_cursor,
			base::Guard(this),
			[this](const Data::FeedPage &page) {
				_fillLoading = false;
				for (const auto &item : page.items) {
					addPost(item.postId);
				}
				_cursor = page.nextCursor;
				_exhausted = _cursor.isEmpty() || page.items.isEmpty();
				preloadNext();
				_chrome->update();
			},
			fail);
	}
}

void Pardeh::refreshTexts() {
	const auto post = current();
	if (!post) {
		return;
	}
	_name = AuthorName(session(), post->author);
	_caption.setMarkedText(
		st::eliPardehCaptionStyle,
		PostText(post->text, false));
	const auto sound = post->sound
		? (post->sound->title.isEmpty()
			? QString()
			: (post->sound->artist.isEmpty()
				? post->sound->title
				: (post->sound->title + tr::lng_eli_meta_separator(tr::now) + post->sound->artist)))
		: QString();
	_soundText = sound.isEmpty()
		? (tr::lng_eli_original_sound(tr::now) + tr::lng_eli_meta_separator(tr::now) + _name)
		: sound;
	_showFollow = (post->author != session().userId());
	_chrome->update();
}

void Pardeh::showPage(int index) {
	if (index < 0 || index >= _ids.size()) {
		return;
	}
	_index = index;
	_paused = false;
	_videoFailed = false;
	const auto post = current();
	const auto media = post ? videoOf(*post) : nullptr;
	if (!post || !media) {
		return;
	}
	refreshTexts();
	session().data().feed().reportViews({ post->id });
	auto &store = session().data().media();
	const auto ref = Data::MediaRef::PostMedia(post->id, media->id);
	_video->stop();
	_video->setPoster(QImage());
	_backdrop = QImage();
	// The next page's handles become this page's when we step forward.
	_poster = (_nextPoster && _nextPoster->ref().version == media->id)
		? _nextPoster
		: store.request(ref, Data::MediaVariant::Poster);
	const auto applyPoster = [this] {
		if (_poster && _poster->isReady() && !_poster->image().isNull()) {
			const auto &image = _poster->image();
			_video->setPoster(image);
			const auto small = image.scaled(
				64,
				64,
				Qt::KeepAspectRatioByExpanding,
				Qt::SmoothTransformation);
			_backdrop = Ui::Images::Blur(
				small.convertToFormat(QImage::Format_ARGB32_Premultiplied),
				6);
			update();
		}
	};
	applyPoster();
	connect(_poster.get(), &Data::MediaHandle::ready, this, applyPoster);
	auto options = Data::Media::Options();
	options.decode = false;
	_file = (_nextFile && _nextFile->ref().version == media->id)
		? _nextFile
		: store.request(ref, Data::MediaVariant::Mp4, options);
	_nextFile = nullptr;
	_nextPoster = nullptr;
	resizeEvent(nullptr);
	startVideo();
	preloadNext();
	if (_index >= _ids.size() - 2) {
		fill();
	}
	update();
}

void Pardeh::startVideo() {
	_videoFailed = false;
	if (!_file) {
		return;
	}
	// Raw pointers in the callbacks: a functor stored in the handle must not
	// keep the handle itself alive.
	const auto raw = _file.get();
	const auto fail = [=, this] {
		if (_file.get() == raw) {
			_videoFailed = true;
			_videoLoading = false;
			_loading.stop();
			_chrome->update();
		}
	};
	const auto start = [=, this] {
		if (_file.get() != raw) {
			return;
		}
		const auto path = LocalMediaFile(_file, u".mp4"_s);
		if (path.isEmpty()) {
			fail();
			return;
		}
		_video->setMuted(Muted);
		_video->setSource(path);
		if (!_paused) {
			_video->play();
		}
	};
	if (_file->isReady()) {
		start();
		return;
	} else if (_file->isFailed()) {
		fail();
		return;
	}
	_videoLoading = true;
	_loading.start();
	connect(raw, &Data::MediaHandle::ready, this, start);
	connect(raw, &Data::MediaHandle::failed, this, fail);
}

void Pardeh::preloadNext() {
	if (_nextFile || _index + 1 >= _ids.size()) {
		return;
	}
	const auto post = session().data().feed().post(_ids[_index + 1]);
	const auto media = post ? videoOf(*post) : nullptr;
	if (!media) {
		return;
	}
	auto &store = session().data().media();
	const auto ref = Data::MediaRef::PostMedia(post->id, media->id);
	auto options = Data::Media::Options();
	options.decode = false;
	_nextFile = store.request(ref, Data::MediaVariant::Mp4, options);
	_nextPoster = store.request(ref, Data::MediaVariant::Poster);
}

void Pardeh::step(int delta) {
	const auto index = _index + delta;
	if (index < 0 || index >= _ids.size()) {
		if (delta > 0) {
			fill();
		}
		return;
	}
	showPage(index);
}

void Pardeh::closeOverlay() {
	if (_closing) {
		return;
	}
	_closing = true;
	_video->pause();
	hide();
	deleteLater();
}

void Pardeh::like(bool burst, QPoint center) {
	const auto post = current();
	if (!post) {
		return;
	}
	if (burst) {
		_heartCenter = center;
		_heart.start([this] { _chrome->update(); }, 0., 1., kHeartDuration);
		if (post->my.reaction != u"like") {
			session().data().feed().react(post->id, Data::Feed::Reaction::Like);
		}
	} else {
		ToggleLike(_controller, post->id);
	}
}

void Pardeh::follow() {
	const auto post = current();
	if (!post || _followed.contains(post->author)) {
		return;
	}
	const auto author = post->author;
	_followed.insert(author);
	_chrome->update();
	const auto controller = _controller;
	session().data().graph().follow(
		author,
		base::Guard(this),
		nullptr,
		[=, this](const Api::Error &error) {
			_followed.remove(author);
			_chrome->update();
			Support::ShowError(controller, error);
		});
}

void Pardeh::applySeek(int x) {
	const auto seek = layout().seek;
	if (seek.width() <= 0) {
		return;
	}
	// A timeline always runs left to right.
	_video->seek(std::clamp(double(x - seek.x()) / seek.width(), 0., 1.));
	_chrome->update();
}

void Pardeh::showMoreMenu(QPoint global) {
	const auto post = current();
	if (!post) {
		return;
	}
	const auto id = post->id;
	const auto author = post->author;
	const auto mine = (author == session().userId());
	const auto controller = _controller;
	const auto menu = new Ui::PopupMenu(this);
	menu->addAction(
		post->my.bookmarked ? tr::lng_eli_saved(tr::now) : tr::lng_eli_save(tr::now),
		[=] { ToggleBookmark(controller, id); },
		&st::menuIconBookmark);
	menu->addAction(
		tr::lng_copy_link(tr::now),
		[=] { CopyPostLink(controller, id); },
		&st::menuIconLink);
	menu->addAction(
		tr::lng_eli_send_in_chat(tr::now),
		[=] { SendPostToChats(controller, id); },
		&st::eliMenuIconSend);
	menu->addAction(
		Muted ? tr::lng_chat_unmute(tr::now) : tr::lng_chat_mute(tr::now),
		[this] { activate(Part::Mute, QPoint()); },
		Muted ? &st::eliMenuIconVolume : &st::menuIconMute);
	menu->addAction(
		tr::lng_eli_open_post(tr::now),
		[=, this] {
			closeOverlay();
			OpenPost(controller, id);
		},
		&st::eliMenuIconOpen);
	if (!mine) {
		auto report = Ui::MenuItem();
		report.text = tr::lng_eli_report_post(tr::now);
		report.icon = &st::menuIconReport;
		report.destructive = true;
		report.callback = [=] {
			OpenReport(controller, { u"post"_s, id, author });
		};
		menu->addItem(std::move(report));
	}
	menu->popup(global);
}

void Pardeh::activate(Part part, QPoint global) {
	const auto post = current();
	switch (part) {
	case Part::None:
		closeOverlay();
		break;
	case Part::Stage:
		if (_videoFailed || _bootFailed) {
			break;
		}
		_paused = !_paused;
		if (_paused) {
			_video->pause();
		} else {
			_video->play();
		}
		_chrome->update();
		break;
	case Part::Close:
		closeOverlay();
		break;
	case Part::Mute:
		Muted = !Muted;
		_video->setMuted(Muted);
		_chrome->update();
		break;
	case Part::Previous:
		step(-1);
		break;
	case Part::Next:
		step(1);
		break;
	case Part::Like:
		like(false);
		break;
	case Part::Dislike:
		if (post) {
			ToggleDislike(_controller, post->id);
		}
		break;
	case Part::Reply:
		if (post) {
			// The conversation lives on the post's page.
			const auto id = post->id;
			const auto controller = _controller;
			closeOverlay();
			OpenPost(controller, id);
		}
		break;
	case Part::Repost:
		if (post && (post->canRepost || post->my.reposted)) {
			ShowRepostMenu(_controller, this, post->id, global);
		}
		break;
	case Part::Share:
		if (post) {
			ShowShareMenu(_controller, this, post->id, global);
		}
		break;
	case Part::More:
		showMoreMenu(global);
		break;
	case Part::Author:
		if (post) {
			const auto author = post->author;
			const auto controller = _controller;
			closeOverlay();
			OpenProfile(controller, author);
		}
		break;
	case Part::Follow:
		follow();
		break;
	case Part::Sound:
		if (post && post->sound && !post->sound->id.isEmpty()) {
			const auto id = post->sound->id;
			const auto controller = _controller;
			closeOverlay();
			OpenSound(controller, id);
		}
		break;
	case Part::Seek:
		break;
	case Part::Retry:
		if (_bootFailed) {
			if (!_ids.isEmpty()) {
				showPage(std::max(_index, 0));
			}
		} else if (post) {
			if (const auto media = videoOf(*post)) {
				auto options = Data::Media::Options();
				options.decode = false;
				options.force = true;
				_file = session().data().media().request(
					Data::MediaRef::PostMedia(post->id, media->id),
					Data::MediaVariant::Mp4,
					options);
				startVideo();
			}
		}
		break;
	}
}

Pardeh::Layout Pardeh::layout() const {
	auto result = Layout();
	const auto padding = st::eliPardehStagePadding;
	const auto post = current();
	const auto media = post ? videoOf(*post) : nullptr;
	auto aspect = kDefaultAspect;
	if (media && media->width > 0 && media->height > 0) {
		aspect = std::clamp(double(media->width) / media->height, 0.45, 1.9);
	}
	const auto rail = st::eliPardehRailWidth + st::eliPardehRailSkip;
	const auto availableHeight = std::max(height() - 2 * padding, 120);
	auto stageHeight = availableHeight;
	auto stageWidth = int(stageHeight * aspect);
	// The column of actions stands next to the video when the window has
	// room on both sides (so the video stays centered), over it otherwise.
	const auto roomOutside = width() - 2 * padding - 2 * rail;
	result.railInside = (stageWidth > roomOutside);
	const auto maxWidth = result.railInside ? (width() - 2 * padding) : roomOutside;
	if (stageWidth > maxWidth) {
		stageWidth = std::max(maxWidth, 120);
		stageHeight = std::min(int(stageWidth / aspect), availableHeight);
	}
	result.stage = QRect(
		(width() - stageWidth) / 2,
		(height() - stageHeight) / 2,
		stageWidth,
		stageHeight);
	const auto &stage = result.stage;
	const auto corner = st::eliPardehCornerButton;
	const auto rtl = style::RightToLeft();
	// Close at the top trailing corner of the window, mute at the leading one.
	result.close = QRect(
		rtl ? padding : (width() - padding - corner),
		padding,
		corner,
		corner);
	result.mute = QRect(
		rtl ? (width() - padding - corner) : padding,
		padding,
		corner,
		corner);
	const auto button = st::eliPardehRailButton;
	const auto railLeft = result.railInside
		? (rtl
			? (stage.x() + 10)
			: (stage.right() + 1 - 10 - button))
		: (rtl
			? (stage.x() - st::eliPardehRailSkip - button)
			: (stage.right() + 1 + st::eliPardehRailSkip));
	const auto counted = button + st::eliPardehRailCountFont->height + 2;
	const auto skip = st::eliPardehRailButtonSkip - 8;
	auto y = stage.bottom() + 1 - (result.railInside ? 96 : 8);
	const auto place = [&](QRect &rect, bool withCount) {
		y -= withCount ? counted : button;
		rect = QRect(railLeft, y, button, button);
		y -= skip;
	};
	place(result.more, false);
	place(result.share, false);
	place(result.repost, true);
	place(result.reply, true);
	place(result.dislike, true);
	place(result.like, true);
	if (!result.railInside) {
		result.previous = QRect(railLeft, stage.y() + corner + 20, button, button);
		result.next = QRect(railLeft, stage.y() + corner + 20 + button + 8, button, button);
		if (result.next.bottom() + 12 > result.like.y()) {
			result.previous = QRect();
			result.next = QRect();
		}
	}

	const auto &info = st::eliPardehInfoPadding;
	const auto innerLeft = stage.x() + info.left();
	const auto innerWidth = stage.width()
		- info.left()
		- info.right()
		- (result.railInside ? (button + 12) : 0);
	const auto textLeft = (rtl && result.railInside)
		? (innerLeft + button + 12)
		: innerLeft;
	auto bottom = stage.bottom() + 1 - info.bottom();
	result.seek = QRect(
		stage.x() + info.left(),
		stage.bottom() + 1 - 12,
		stage.width() - info.left() - info.right(),
		st::eliPardehSeekHeight);
	const auto soundHeight = st::eliPardehSoundFont->height + 4;
	result.sound = QRect(textLeft, bottom - soundHeight, innerWidth, soundHeight);
	bottom -= soundHeight + 6;
	if (!_caption.isEmpty()) {
		const auto captionHeight = _caption.countHeight(innerWidth, st::eliPardehCaptionLines);
		result.caption = QRect(textLeft, bottom - captionHeight, innerWidth, captionHeight);
		bottom -= captionHeight + 8;
	}
	const auto userpic = st::eliCardUserpic;
	const auto rowTop = bottom - userpic;
	const auto userpicLeft = rtl ? (textLeft + innerWidth - userpic) : textLeft;
	result.userpic = QRect(userpicLeft, rowTop, userpic, userpic);
	const auto followText = (post && _followed.contains(post->author))
		? tr::lng_following(tr::now)
		: tr::lng_follow(tr::now);
	const auto followWidth = _showFollow
		? (st::eliPardehFollowFont->width(followText) + 2 * st::eliPardehFollowPadding)
		: 0;
	const auto nameAvailable = innerWidth - userpic - 10 - (followWidth ? followWidth + 10 : 0);
	const auto nameWidth = std::min(st::eliPardehNameFont->width(_name), std::max(nameAvailable, 0));
	const auto nameLeft = rtl
		? (userpicLeft - 10 - nameWidth)
		: (userpicLeft + userpic + 10);
	result.name = QRect(nameLeft, rowTop, nameWidth, userpic);
	if (followWidth) {
		result.follow = QRect(
			rtl ? (nameLeft - 10 - followWidth) : (nameLeft + nameWidth + 10),
			rowTop + (userpic - st::eliPardehFollowHeight) / 2,
			followWidth,
			st::eliPardehFollowHeight);
	}
	result.retry = QRect(
		stage.center().x() - 70,
		stage.center().y() + 24,
		140,
		st::eliPardehRetryHeight);
	return result;
}

Part Pardeh::partAt(QPoint point) const {
	const auto geometry = layout();
	const auto has = [&](const QRect &rect) {
		return !rect.isEmpty() && rect.contains(point);
	};
	if (has(geometry.close)) {
		return Part::Close;
	} else if (has(geometry.mute)) {
		return Part::Mute;
	}
	if (!current()) {
		return has(geometry.retry) && _bootFailed
			? Part::Retry
			: has(geometry.stage)
			? Part::Stage
			: Part::None;
	}
	if ((_videoFailed || _bootFailed) && has(geometry.retry)) {
		return Part::Retry;
	} else if (has(geometry.previous) && _index > 0) {
		return Part::Previous;
	} else if (has(geometry.next) && _index + 1 < _ids.size()) {
		return Part::Next;
	} else if (has(geometry.like)) {
		return Part::Like;
	} else if (has(geometry.dislike)) {
		return Part::Dislike;
	} else if (has(geometry.reply)) {
		return Part::Reply;
	} else if (has(geometry.repost)) {
		return Part::Repost;
	} else if (has(geometry.share)) {
		return Part::Share;
	} else if (has(geometry.more)) {
		return Part::More;
	} else if (has(geometry.follow)) {
		return Part::Follow;
	} else if (has(geometry.userpic) || has(geometry.name)) {
		return Part::Author;
	} else if (has(geometry.sound)) {
		const auto post = current();
		return (post && post->sound && !post->sound->id.isEmpty())
			? Part::Sound
			: Part::Stage;
	} else if (has(geometry.seek.adjusted(0, -st::eliPardehSeekArea / 2, 0, st::eliPardehSeekArea / 2))) {
		return Part::Seek;
	} else if (has(geometry.stage)) {
		return Part::Stage;
	}
	return Part::None;
}

bool Pardeh::eventFilter(QObject *object, QEvent *event) {
	if (object == parentWidget() && event->type() == QEvent::Resize) {
		setGeometry(parentWidget()->rect());
	}
	return RpWidget::eventFilter(object, event);
}

void Pardeh::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_video->setGeometry(layout().stage);
	_chrome->setGeometry(rect());
	_chrome->raise();
}

void Pardeh::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.fillRect(rect(), st::eliPardehBg);
	if (!_backdrop.isNull()) {
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.setOpacity(0.55);
		const auto cover = _backdrop.size().scaled(
			this->size(),
			Qt::KeepAspectRatioByExpanding);
		p.drawImage(
			QRect(
				(width() - cover.width()) / 2,
				(height() - cover.height()) / 2,
				cover.width(),
				cover.height()),
			_backdrop);
		p.setOpacity(1.);
		p.fillRect(rect(), st::eliPardehDim);
	}
}

void Pardeh::keyPressEvent(QKeyEvent *e) {
	const auto letter = [&](int key, QStringView persian) {
		return (e->key() == key) || (!e->text().isEmpty() && e->text() == persian);
	};
	if (e->key() == Qt::Key_Escape) {
		closeOverlay();
	} else if (e->key() == Qt::Key_Down || e->key() == Qt::Key_PageDown || letter(Qt::Key_J, u"ت")) {
		step(1);
	} else if (e->key() == Qt::Key_Up || e->key() == Qt::Key_PageUp || letter(Qt::Key_K, u"ن")) {
		step(-1);
	} else if (e->key() == Qt::Key_Space) {
		activate(Part::Stage, QPoint());
	} else if (letter(Qt::Key_M, u"پ")) {
		activate(Part::Mute, QPoint());
	} else if (letter(Qt::Key_L, u"م")) {
		like(false);
	} else if (e->key() == Qt::Key_Left) {
		_video->seekBy(-kSeekStepMs);
	} else if (e->key() == Qt::Key_Right) {
		_video->seekBy(kSeekStepMs);
	}
}

void Pardeh::wheelEvent(QWheelEvent *e) {
	e->accept();
	const auto delta = e->angleDelta().y();
	const auto now = Ui::Now();
	// One video per wheel gesture.
	if (!delta || now - _lastWheel < kWheelPause) {
		return;
	}
	_lastWheel = now;
	step((delta < 0) ? 1 : -1);
}

void Pardeh::Chrome::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	PainterHighQualityEnabler hq(p);
	const auto owner = _owner;
	const auto geometry = owner->layout();
	const auto &stage = geometry.stage;
	const auto post = owner->current();

	const auto roundButton = [&](const QRect &rect, const style::icon &icon, Part part, bool onMedia) {
		if (rect.isEmpty()) {
			return;
		}
		p.setPen(Qt::NoPen);
		p.setBrush((_over == part)
			? st::eliPardehRailBgOver
			: onMedia
			? st::eliMediaPlayBg
			: st::eliPardehRailBg);
		p.drawEllipse(rect);
		icon.paintInCenter(p, rect);
	};
	roundButton(geometry.close, st::eliPardehIconClose, Part::Close, false);
	roundButton(
		geometry.mute,
		Muted ? st::eliPardehIconMute : st::eliPardehIconVolume,
		Part::Mute,
		false);

	if (owner->_bootFailed || (owner->_videoFailed && post)) {
		p.setPen(st::eliPardehFg);
		p.setFont(st::eliPardehErrorFont);
		p.drawText(
			stage.adjusted(16, 0, -16, -40),
			Qt::AlignCenter | Qt::TextWordWrap,
			tr::lng_eli_video_couldnt_load_check_connection(tr::now));
		const auto retry = geometry.retry;
		p.setPen(Qt::NoPen);
		p.setBrush((_over == Part::Retry) ? st::eliPardehRailBgOver : st::eliPardehRailBg);
		p.drawRoundedRect(retry, retry.height() / 2, retry.height() / 2);
		p.setPen(st::eliPardehFg);
		p.setFont(st::eliPardehRetryFont);
		p.drawTextCentered(retry, tr::lng_retry(tr::now));
	} else if (owner->_videoLoading) {
		const auto size = 44;
		owner->_loading.draw(
			p,
			QRect(stage.center().x() - size / 2, stage.center().y() - size / 2, size, size),
			3.,
			st::eliPardehFg->c);
	}
	if (!post) {
		return;
	}

	// A shade under the texts.
	{
		auto clip = QPainterPath();
		clip.addRoundedRect(stage, st::eliPardehStageRadius, st::eliPardehStageRadius);
		const auto shadeTop = std::max(
			(geometry.caption.isEmpty() ? geometry.userpic.y() : geometry.userpic.y()) - 60,
			stage.y());
		auto gradient = QLinearGradient(0, shadeTop, 0, stage.bottom() + 1);
		gradient.setColorAt(0., QColor(0, 0, 0, 0));
		gradient.setColorAt(1., QColor(0, 0, 0, 190));
		auto shade = QPainterPath();
		shade.addRect(QRect(stage.x(), shadeTop, stage.width(), stage.bottom() + 1 - shadeTop));
		p.fillPath(clip.intersected(shade), gradient);
	}

	const auto counted = [&](
			const QRect &rect,
			const style::icon &icon,
			Part part,
			qint64 count) {
		roundButton(rect, icon, part, geometry.railInside);
		if (count > 0) {
			p.setPen(st::eliPardehFg);
			p.setFont(st::eliPardehRailCountFont);
			p.drawTextCentered(
				QRect(
					rect.x() - 16,
					rect.bottom() + 3,
					rect.width() + 32,
					st::eliPardehRailCountFont->height),
				Lang::CompactCount(count));
		}
	};
	const auto liked = (post->my.reaction == u"like");
	const auto disliked = (post->my.reaction == u"dislike");
	counted(
		geometry.like,
		liked ? st::eliPardehIconLikeActive : st::eliPardehIconLike,
		Part::Like,
		post->counts.likes);
	counted(
		geometry.dislike,
		disliked ? st::eliPardehIconDislikeActive : st::eliPardehIconDislike,
		Part::Dislike,
		post->counts.hasDislikes ? post->counts.dislikes : 0);
	counted(geometry.reply, st::eliPardehIconReply, Part::Reply, post->counts.replies);
	counted(
		geometry.repost,
		post->my.reposted ? st::eliPardehIconRepostActive : st::eliPardehIconRepost,
		Part::Repost,
		post->counts.reposts + post->counts.quotes);
	roundButton(geometry.share, st::eliPardehIconShare, Part::Share, geometry.railInside);
	roundButton(geometry.more, st::eliPardehIconMore, Part::More, geometry.railInside);
	if (owner->_index > 0) {
		roundButton(geometry.previous, st::eliPardehIconUp, Part::Previous, false);
	}
	if (owner->_index + 1 < owner->_ids.size()) {
		roundButton(geometry.next, st::eliPardehIconDown, Part::Next, false);
	}

	// The author, the caption, the sound.
	owner->_userpics.paint(p, geometry.userpic, post->author);
	p.setPen(st::eliPardehFg);
	p.setFont((_over == Part::Author)
		? st::eliPardehNameFont->underline()
		: st::eliPardehNameFont);
	p.drawText(
		geometry.name.x(),
		geometry.name.y()
			+ (geometry.name.height() - st::eliPardehNameFont->height) / 2
			+ st::eliPardehNameFont->ascent,
		st::eliPardehNameFont->elided(owner->_name, geometry.name.width()));
	if (!geometry.follow.isEmpty()) {
		const auto following = owner->_followed.contains(post->author);
		const auto &rect = geometry.follow;
		if (following) {
			p.setPen(st::eliPardehFgDim);
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(
				QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
				rect.height() / 2,
				rect.height() / 2);
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush((_over == Part::Follow) ? st::staticWhite : st::eliPardehFg);
			p.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
		}
		p.setPen(following ? st::eliPardehFg : st::eliPardehBg);
		p.setFont(st::eliPardehFollowFont);
		p.drawTextCentered(
			rect,
			following ? tr::lng_following(tr::now) : tr::lng_follow(tr::now));
	}
	if (!geometry.caption.isEmpty()) {
		p.setPen(st::eliPardehFg);
		auto context = Ui::Text::PaintContext();
		context.position = geometry.caption.topLeft();
		context.availableWidth = geometry.caption.width();
		context.elisionLines = st::eliPardehCaptionLines;
		context.autoDirection = true;
		owner->_caption.draw(p, context);
	}
	{
		const auto &rect = geometry.sound;
		const auto rtl = style::RightToLeft();
		const auto &icon = st::eliPardehIconMusic;
		const auto iconLeft = rtl ? (rect.right() + 1 - icon.width()) : rect.x();
		icon.paintInCenter(
			p,
			QRect(iconLeft, rect.y(), icon.width(), rect.height()));
		p.setPen(st::eliPardehFgDim);
		p.setFont((_over == Part::Sound)
			? st::eliPardehSoundFont->underline()
			: st::eliPardehSoundFont);
		const auto text = st::eliPardehSoundFont->elided(
			owner->_soundText,
			rect.width() - icon.width() - 6);
		const auto textWidth = st::eliPardehSoundFont->width(text);
		p.drawText(
			rtl ? (iconLeft - 6 - textWidth) : (iconLeft + icon.width() + 6),
			rect.y() + (rect.height() - st::eliPardehSoundFont->height) / 2 + st::eliPardehSoundFont->ascent,
			text);
	}

	// The seek bar.
	{
		const auto over = (_over == Part::Seek) || owner->_seeking;
		const auto barHeight = over ? st::eliPardehSeekHeightOver : st::eliPardehSeekHeight;
		const auto bar = QRect(
			geometry.seek.x(),
			geometry.seek.center().y() - barHeight / 2,
			geometry.seek.width(),
			barHeight);
		const auto duration = owner->_video->durationMs();
		const auto progress = (duration > 0)
			? std::clamp(double(owner->_video->positionMs()) / duration, 0., 1.)
			: 0.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::eliPardehSeekBg);
		p.drawRoundedRect(bar, barHeight / 2., barHeight / 2.);
		p.setBrush(st::eliPardehSeekFg);
		p.drawRoundedRect(
			QRect(bar.x(), bar.y(), int(bar.width() * progress), bar.height()),
			barHeight / 2.,
			barHeight / 2.);
		if (over) {
			p.drawEllipse(
				QPoint(bar.x() + int(bar.width() * progress), bar.center().y() + 1),
				6,
				6);
		}
		if (duration > 0 && (over || owner->_paused)) {
			p.setPen(st::eliPardehFg);
			p.setFont(st::eliPardehCounterFont);
			const auto text = QChar(0x2066)
				+ Lang::Duration(int(owner->_video->positionMs() / 1000))
				+ tr::lng_eli_meta_separator(tr::now)
				+ Lang::Duration(int(duration / 1000))
				+ QChar(0x2069);
			p.drawText(
				bar.x(),
				bar.y() - 8,
				text);
		}
	}

	const auto heart = owner->_heart.animating() ? owner->_heart.value(1.) : -1.;
	if (heart >= 0. && heart < 1.) {
		const auto scale = (heart < 0.3) ? (0.4 + 2. * heart) : 1.;
		p.save();
		p.setOpacity((heart < 0.6) ? 1. : (1. - (heart - 0.6) / 0.4));
		p.translate(owner->_heartCenter);
		p.scale(scale, scale);
		const auto size = st::eliPardehHeartIcon.size();
		st::eliPardehHeartIcon.paintInCenter(
			p,
			QRect(-size.width() / 2, -size.height() / 2, size.width(), size.height()));
		p.restore();
	}
}

} // namespace

void OpenPardeh(
		Window::SessionController *controller,
		const QString &postId,
		const QString &authorId) {
	const auto overlay = new Pardeh(
		controller->window()->bodyWidget(),
		controller,
		postId,
		authorId);
	controller->window()->registerDarkOverlay(overlay);
	overlay->show();
	overlay->raise();
	overlay->setFocus();
}

} // namespace Eli
