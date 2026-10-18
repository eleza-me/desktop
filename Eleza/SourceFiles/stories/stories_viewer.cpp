#include "stories/stories_viewer.h"

#include "boxes/report_box.h"
#include "data/data_media.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_video_player.h"
#include "stories/stories_boxes.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/painter.h"
#include "ui/rtl.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_stories.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>

namespace Eli {
// eli/eli_viewer.cpp: the file of a ready media handle (a temporary copy
// when the cache lives in memory).
QString LocalMediaFile(const Data::MediaHandlePtr &handle, const QString &suffix);
} // namespace Eli

namespace Stories {
namespace {

constexpr auto kHoldMs = 220;
constexpr auto kStageAspect = 9. / 16.;

[[nodiscard]] QString NameOf(const Data::User &user) {
	if (user.deleted) {
		return tr::lng_deleted_account(tr::now);
	} else if (!user.displayName.isEmpty()) {
		return user.displayName;
	}
	return user.handle.isEmpty() ? QString() : Lang::Handle(user.handle);
}

class Viewer;

// Everything painted over the media (the video is a child widget of its
// own): shades, progress, header, caption, the post card, reactions.
class Chrome final : public Ui::RpWidget {
public:
	Chrome(QWidget *parent, Viewer *owner);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	Viewer *_owner = nullptr;

};

class Viewer final : public Ui::RpWidget {
public:
	Viewer(
		QWidget *parent,
		Window::SessionController *controller,
		QStringList users,
		int userIndex,
		QString storyId);
	~Viewer();

	void close();

	// Chrome
	void paintChrome(Painter &p);
	void chromePress(QMouseEvent *e);
	void chromeMove(QMouseEvent *e);
	void chromeRelease(QMouseEvent *e);
	void chromeLeave();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	enum class Part {
		None,
		Stage,
		Outside,
		Header,
		Reaction,
		Viewers,
	};
	struct Hit {
		Part part = Part::None;
		int index = -1;
		friend bool operator==(const Hit &, const Hit &) = default;
	};

	[[nodiscard]] Main::Session &session() const {
		return _controller->session();
	}
	[[nodiscard]] Data::Stories &stories() const;
	[[nodiscard]] const Data::Story *current() const;
	[[nodiscard]] bool mine() const;
	[[nodiscard]] bool paused() const;
	[[nodiscard]] QRect stage() const {
		return _stage;
	}
	[[nodiscard]] QRect inStage(int x, int y, int w, int h) const;
	[[nodiscard]] QRect headerRect() const;
	[[nodiscard]] QRect bottomRect() const;
	[[nodiscard]] QRect reactionRect(int index) const;
	[[nodiscard]] QRect viewersRect() const;
	[[nodiscard]] QString viewersText() const;
	[[nodiscard]] QRect postCardRect() const;
	[[nodiscard]] bool replyShown() const;
	[[nodiscard]] Hit hitAt(QPoint point) const;

	void openUser(int index, bool fromEnd, const QString &storyId = QString());
	void applyList(const Data::UserStories &list, bool fromEnd, const QString &storyId);
	void showStory(int index);
	void clearMedia();
	void checkMediaReady();
	void mediaShown();
	void tick(qint64 now);
	void next();
	void previous();
	void togglePause();
	void updatePlayback();
	void updateControls();
	void updateGeometries();
	void showMenu();
	void sendReply();
	void react(int index);
	void openProfile();
	void openPost();
	void confirmDelete();
	void showUnavailable(const QString &text);

	Window::SessionController *_controller = nullptr;
	QStringList _users;
	int _userIndex = 0;
	Data::UserStories _list;
	int _index = -1;
	int _openRequest = 0;
	bool _loadingUser = false;
	QString _unavailable;

	QRect _stage;
	Chrome *_chrome = nullptr;
	Media::VideoPlayerWidget *_video = nullptr;
	Ui::IconButton *_close = nullptr;
	Ui::IconButton *_more = nullptr;
	Ui::IconButton *_pause = nullptr;
	Ui::IconButton *_previousNav = nullptr;
	Ui::IconButton *_nextNav = nullptr;
	Ui::InputField *_reply = nullptr;
	Ui::IconButton *_send = nullptr;
	Ui::RoundButton *_seePost = nullptr;

	std::shared_ptr<Ui::ImageSource> _image;
	std::shared_ptr<Ui::ImageSource> _preview;
	QMetaObject::Connection _imageConnection;
	QMetaObject::Connection _previewConnection;
	Data::MediaHandlePtr _videoFile;
	bool _mediaReady = false;
	bool _mediaFailed = false;
	bool _seenMarked = false;

	Ui::Text::String _caption;
	Ui::Text::String _postText;
	Support::UserpicPainter _userpics;

	Ui::Animations::Basic _ticker;
	Ui::InfiniteRadialAnimation _loading;
	qint64 _elapsed = 0;
	qint64 _lastTick = 0;
	bool _userPaused = false;
	bool _holding = false;
	bool _wasPaused = true;
	QPointer<Ui::PopupMenu> _menu;
	Hit _over;
	Hit _pressed;
	qint64 _pressedAt = 0;
	QPointer<QWidget> _previousFocus;
	bool _closing = false;

};

Chrome::Chrome(QWidget *parent, Viewer *owner)
: RpWidget(parent)
, _owner(owner) {
	setMouseTracking(true);
}

void Chrome::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	_owner->paintChrome(p);
}

void Chrome::mousePressEvent(QMouseEvent *e) {
	_owner->chromePress(e);
}

void Chrome::mouseMoveEvent(QMouseEvent *e) {
	_owner->chromeMove(e);
}

void Chrome::mouseReleaseEvent(QMouseEvent *e) {
	_owner->chromeRelease(e);
}

void Chrome::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	_owner->chromeLeave();
}

Viewer::Viewer(
	QWidget *parent,
	Window::SessionController *controller,
	QStringList users,
	int userIndex,
	QString storyId)
: RpWidget(parent)
, _controller(controller)
, _users(std::move(users))
, _userIndex(std::clamp(userIndex, 0, std::max(int(_users.size()) - 1, 0)))
, _userpics(&controller->session(), [this](const QString &) {
	if (_chrome) {
		_chrome->update();
	}
}, this)
, _ticker([this](qint64 now) {
	tick(now);
	return true;
})
, _loading([this] {
	if (_chrome) {
		_chrome->update();
	}
}) {
	setFocusPolicy(Qt::StrongFocus);
	_previousFocus = QApplication::focusWidget();
	if (parent) {
		setGeometry(parent->rect());
		parent->installEventFilter(this);
	}
	_video = Ui::CreateChild<Media::VideoPlayerWidget>(this);
	_video->setControlsShown(false);
	_video->setFullscreenAllowed(false);
	_video->setRadius(st::storiesStageRadius);
	Ui::MakeTransparentForMouse(_video);
	_video->hide();
	connect(_video, &Media::VideoPlayerWidget::finished, this, [this] {
		next();
	});
	connect(_video, &Media::VideoPlayerWidget::failed, this, [this] {
		_mediaFailed = true;
		_mediaReady = true;
		_video->hide();
		update();
	});
	connect(_video, &Media::VideoPlayerWidget::videoSizeChanged, this, [this] {
		if (!_mediaReady) {
			_mediaReady = true;
			mediaShown();
		}
		update();
	});

	_chrome = Ui::CreateChild<Chrome>(this, this);
	_close = Ui::CreateChild<Ui::IconButton>(_chrome, st::storiesClose);
	_more = Ui::CreateChild<Ui::IconButton>(_chrome, st::storiesMore);
	_pause = Ui::CreateChild<Ui::IconButton>(_chrome, st::storiesPause);
	_previousNav = Ui::CreateChild<Ui::IconButton>(_chrome, st::storiesNavPrevious);
	_nextNav = Ui::CreateChild<Ui::IconButton>(_chrome, st::storiesNavNext);
	_close->setClickedCallback([this] { close(); });
	_more->setClickedCallback([this] { showMenu(); });
	_pause->setClickedCallback([this] { togglePause(); });
	// The arrows are physical: the left one goes back in LTR, on in RTL.
	_previousNav->setClickedCallback([this] {
		style::RightToLeft() ? next() : previous();
	});
	_nextNav->setClickedCallback([this] {
		style::RightToLeft() ? previous() : next();
	});

	_reply = Ui::CreateChild<Ui::InputField>(
		_chrome,
		st::storiesReplyField,
		QString());
	_reply->setMaxLength(4096);
	_send = Ui::CreateChild<Ui::IconButton>(_chrome, st::storiesSend);
	_send->setClickedCallback([this] { sendReply(); });
	connect(_reply, &Ui::InputField::submitted, this, [this] { sendReply(); });
	connect(_reply, &Ui::InputField::cancelled, this, [this] { setFocus(); });
	connect(_reply, &Ui::InputField::focusedChanged, this, [this] {
		updatePlayback();
		_chrome->update();
	});
	connect(_reply, &Ui::InputField::changed, this, [this] { updateControls(); });

	_seePost = Ui::CreateChild<Ui::RoundButton>(
		_chrome,
		tr::lng_stories_see_post(tr::now),
		st::storiesPostButton);
	_seePost->setClickedCallback([this] { openPost(); });
	_seePost->hide();

	connect(&stories(), &Data::Stories::userStoriesChanged, this, [this](
			const QString &userId) {
		if (_list.user.id == userId) {
			// Seen marks, reactions, viewer counts: keep the copy in step.
			if (const auto list = stories().cached(userId)) {
				const auto id = current() ? current()->id : QString();
				auto found = false;
				for (const auto &story : list->stories) {
					found = found || (story.id == id);
				}
				if (found) {
					_list.stories = list->stories;
					for (auto i = 0; i != int(_list.stories.size()); ++i) {
						if (_list.stories[i].id == id) {
							_index = i;
						}
					}
					updateControls();
					_chrome->update();
				}
			}
		}
	});
	if (const auto layers = _controller->window()->layers()) {
		connect(layers, &Ui::LayerStackWidget::layersShownChanged, this, [this](bool shown) {
			updatePlayback();
			if (!shown && !_closing) {
				setFocus();
			}
		});
	}

	_ticker.start();
	openUser(_userIndex, false, storyId);
}

Viewer::~Viewer() {
	QObject::disconnect(_imageConnection);
	QObject::disconnect(_previewConnection);
}

Data::Stories &Viewer::stories() const {
	return session().data().stories();
}

const Data::Story *Viewer::current() const {
	return (_index >= 0 && _index < _list.stories.size())
		? &_list.stories[_index]
		: nullptr;
}

bool Viewer::mine() const {
	return !_list.user.id.isEmpty() && (_list.user.id == session().userId());
}

bool Viewer::paused() const {
	return _userPaused
		|| _holding
		|| !_mediaReady
		|| (_reply && _reply->hasFocus())
		|| _menu
		|| _controller->window()->layers()->hasLayers()
		|| !_unavailable.isEmpty();
}

void Viewer::close() {
	if (_closing) {
		return;
	}
	_closing = true;
	_ticker.stop();
	_video->stop();
	stories().flushSeen();
	hide();
	if (_previousFocus) {
		_previousFocus->setFocus();
	}
	deleteLater();
}

bool Viewer::eventFilter(QObject *object, QEvent *event) {
	if (object == parentWidget() && event->type() == QEvent::Resize) {
		setGeometry(parentWidget()->rect());
	}
	return RpWidget::eventFilter(object, event);
}

// ---- loading people and stories

void Viewer::openUser(int index, bool fromEnd, const QString &storyId) {
	if (index < 0 || index >= _users.size()) {
		close();
		return;
	}
	_userIndex = index;
	const auto userId = _users[index];
	const auto request = ++_openRequest;
	clearMedia();
	_unavailable = QString();
	_index = -1;
	_list = Data::UserStories();
	_list.user = session().data().users().peek(userId);
	_list.user.id = userId;
	if (const auto cached = stories().cached(userId); cached && !cached->stories.isEmpty()) {
		applyList(*cached, fromEnd, storyId);
	}
	_loadingUser = true;
	_loading.start();
	updateControls();
	_chrome->update();
	stories().loadUser(userId, this, [=, this](const Data::UserStories &list) {
		if (request != _openRequest) {
			return;
		}
		_loadingUser = false;
		_loading.stop();
		const auto shown = current() ? current()->id : QString();
		if (shown.isEmpty()) {
			applyList(list, fromEnd, storyId);
		} else {
			// Already showing the cached copy: keep the story on screen.
			_list = list;
			_index = -1;
			for (auto i = 0; i != int(list.stories.size()); ++i) {
				if (list.stories[i].id == shown) {
					_index = i;
				}
			}
			if (_index < 0) {
				applyList(list, fromEnd, storyId);
			}
		}
		updateControls();
		_chrome->update();
	}, [=, this](const Api::Error &error) {
		if (request != _openRequest) {
			return;
		}
		_loadingUser = false;
		_loading.stop();
		if (!current()) {
			if (_users.size() > 1 && _userIndex + 1 < _users.size()) {
				openUser(_userIndex + 1, false);
			} else {
				showUnavailable(error.notFound()
					? tr::lng_stories_unavailable(tr::now)
					: Support::ErrorText(error, tr::lng_stories_media_failed(tr::now)));
			}
		}
	});
}

void Viewer::applyList(
		const Data::UserStories &list,
		bool fromEnd,
		const QString &storyId) {
	_list = list;
	if (_list.stories.isEmpty()) {
		if (!storyId.isEmpty() || _users.size() == 1) {
			showUnavailable(storyId.isEmpty()
				? tr::lng_stories_none(tr::now)
				: tr::lng_stories_unavailable(tr::now));
		} else if (fromEnd) {
			openUser(_userIndex - 1, true);
		} else {
			openUser(_userIndex + 1, false);
		}
		return;
	}
	auto index = fromEnd
		? int(_list.stories.size()) - 1
		: Data::FirstUnseenIndex(_list.stories);
	if (!storyId.isEmpty()) {
		index = -1;
		for (auto i = 0; i != int(_list.stories.size()); ++i) {
			if (_list.stories[i].id == storyId) {
				index = i;
			}
		}
		if (index < 0) {
			showUnavailable(tr::lng_stories_unavailable(tr::now));
			return;
		}
	}
	showStory(index);
}

void Viewer::showUnavailable(const QString &text) {
	clearMedia();
	_unavailable = text;
	_index = -1;
	_list.stories.clear();
	updateControls();
	_chrome->update();
	update();
}

// ---- one story

void Viewer::clearMedia() {
	QObject::disconnect(_imageConnection);
	QObject::disconnect(_previewConnection);
	_image = nullptr;
	_preview = nullptr;
	_videoFile = nullptr;
	_video->stop();
	_video->hide();
	_mediaReady = false;
	_mediaFailed = false;
	_seenMarked = false;
	_elapsed = 0;
	_caption.clear();
	_postText.clear();
}

void Viewer::showStory(int index) {
	if (index < 0 || index >= _list.stories.size()) {
		return;
	}
	clearMedia();
	_index = index;
	_userPaused = false;
	const auto story = _list.stories[index];
	auto &session = this->session();
	const auto ref = Data::MediaRef::Story(story.id);
	if (!story.caption.isEmpty()) {
		_caption.setText(st::storiesCaptionStyle, story.caption);
	}
	switch (story.kind) {
	case Data::StoryKind::Image: {
		auto options = Support::ImageOptions();
		options.original = true;
		if (story.media) {
			options.blurhash = story.media->blurhash;
			options.aspect = QSize(story.media->width, story.media->height);
		}
		_preview = Support::MakeImageSource(&session, ref, Data::MediaVariant::Thumb, options);
		_image = Support::MakeImageSource(&session, ref, Data::MediaVariant::Full, options);
	} break;
	case Data::StoryKind::Video: {
		auto options = Support::ImageOptions();
		if (story.media) {
			options.blurhash = story.media->blurhash;
			options.aspect = QSize(story.media->width, story.media->height);
		}
		_preview = Support::MakeImageSource(&session, ref, Data::MediaVariant::Poster, options);
		auto fileOptions = Data::Media::Options();
		fileOptions.decode = false;
		_videoFile = session.data().media().request(ref, Data::MediaVariant::Mp4, fileOptions);
		const auto start = [=, this] {
			const auto path = Eli::LocalMediaFile(_videoFile, u".mp4"_s);
			if (path.isEmpty()) {
				_mediaFailed = true;
				_mediaReady = true;
				update();
				return;
			}
			_video->setSource(path);
			_video->show();
			_chrome->raise();
			updateGeometries();
			_video->play();
			updatePlayback();
		};
		if (_videoFile->isReady()) {
			start();
		} else if (_videoFile->isFailed()) {
			_mediaFailed = true;
			_mediaReady = true;
		} else {
			const auto handle = _videoFile.get();
			connect(handle, &Data::MediaHandle::ready, this, [=, this] {
				if (_videoFile.get() == handle) {
					start();
				}
			});
			connect(handle, &Data::MediaHandle::failed, this, [=, this] {
				if (_videoFile.get() == handle) {
					_mediaFailed = true;
					_mediaReady = true;
					update();
				}
			});
		}
	} break;
	case Data::StoryKind::Post:
		if (story.post && story.post->available) {
			if (!story.post->thumbUrl.isEmpty()) {
				_image = Support::MakeImageSource(&session, ref, Data::MediaVariant::Thumb);
			}
			_postText.setText(st::storiesPostCardTextStyle, story.post->textExcerpt);
		}
		_mediaReady = true;
		break;
	}
	const auto watch = [this](const std::shared_ptr<Ui::ImageSource> &source) {
		return source
			? connect(source.get(), &Ui::ImageSource::updated, this, [this] {
				checkMediaReady();
				update();
				_chrome->update();
			})
			: QMetaObject::Connection();
	};
	_imageConnection = watch(_image);
	_previewConnection = watch(_preview);
	_lastTick = Ui::Now();
	updateControls();
	updateGeometries();
	checkMediaReady();
	if (_mediaReady) {
		mediaShown();
	}
	update();
	_chrome->update();
}

void Viewer::checkMediaReady() {
	const auto story = current();
	if (!story || _mediaReady || story->kind != Data::StoryKind::Image || !_image) {
		return;
	}
	if (_image->failed()) {
		_mediaFailed = true;
		_mediaReady = true;
	} else if (!_image->request(_stage.size() * devicePixelRatioF()).isNull()) {
		_mediaReady = true;
	}
	if (_mediaReady) {
		mediaShown();
	}
}

void Viewer::mediaShown() {
	const auto story = current();
	if (!story || _seenMarked) {
		return;
	}
	_seenMarked = true;
	_lastTick = Ui::Now();
	stories().markSeen(story->id);
	updatePlayback();
}

void Viewer::tick(qint64 now) {
	const auto delta = std::max(now - _lastTick, qint64(0));
	_lastTick = now;
	const auto isPaused = paused();
	if (isPaused != _wasPaused) {
		updatePlayback();
	}
	const auto story = current();
	if (!story || isPaused) {
		return;
	}
	if (story->kind == Data::StoryKind::Video && !_mediaFailed) {
		// The video's own clock; finished() goes on.
		_chrome->update(_stage.x(), _stage.y(), _stage.width(), st::storiesHeaderTop);
		return;
	}
	_elapsed += delta;
	if (_elapsed >= story->showMs()) {
		next();
		return;
	}
	_chrome->update(_stage.x(), _stage.y(), _stage.width(), st::storiesHeaderTop);
}

void Viewer::updatePlayback() {
	const auto isPaused = paused();
	_wasPaused = isPaused;
	if (const auto story = current(); story && story->kind == Data::StoryKind::Video) {
		if (isPaused && _video->playing()) {
			_video->pause();
		} else if (!isPaused && !_video->playing() && !_mediaFailed && _videoFile && _videoFile->isReady()) {
			_video->play();
		}
	}
	const auto &icon = _userPaused ? st::storiesPlayIcon : st::storiesPauseIcon;
	_pause->setIconOverride(&icon, &icon);
	_lastTick = Ui::Now();
}

void Viewer::next() {
	if (_closing) {
		return;
	}
	if (_index + 1 < _list.stories.size()) {
		showStory(_index + 1);
	} else if (_userIndex + 1 < _users.size()) {
		openUser(_userIndex + 1, false);
	} else {
		close();
	}
}

void Viewer::previous() {
	if (_closing) {
		return;
	}
	if (_index > 0) {
		showStory(_index - 1);
	} else if (_userIndex > 0) {
		openUser(_userIndex - 1, true);
	} else if (_index == 0) {
		showStory(0);
	}
}

void Viewer::togglePause() {
	_userPaused = !_userPaused;
	updatePlayback();
	_chrome->update();
}

// ---- layout

QRect Viewer::inStage(int x, int y, int w, int h) const {
	// `x` from the leading edge of the stage.
	return style::RightToLeft()
		? QRect(_stage.x() + _stage.width() - x - w, y, w, h)
		: QRect(_stage.x() + x, y, w, h);
}

QRect Viewer::headerRect() const {
	return QRect(
		_stage.x(),
		_stage.y() + st::storiesHeaderTop,
		_stage.width(),
		st::storiesHeaderUserpic);
}

bool Viewer::replyShown() const {
	const auto story = current();
	return story && !mine() && story->canReply;
}

QRect Viewer::bottomRect() const {
	const auto &padding = st::storiesReplyPadding;
	auto height = padding.top() + padding.bottom();
	if (mine()) {
		height += st::storiesViewersHeight;
	} else if (replyShown()) {
		height += st::storiesReactionSize + st::storiesReactionsSkip + st::storiesReplyField.heightMin;
	} else if (current()) {
		height += st::storiesReplyClosedFont->height + 8;
	}
	return QRect(_stage.x(), _stage.y() + _stage.height() - height, _stage.width(), height);
}

QRect Viewer::reactionRect(int index) const {
	const auto &padding = st::storiesReplyPadding;
	const auto count = int(Data::StoryReactions().size());
	const auto size = st::storiesReactionSize;
	const auto skip = st::storiesReactionsSkip;
	const auto full = count * size + (count - 1) * skip;
	const auto left = std::max((_stage.width() - full) / 2, int(padding.left()));
	const auto bottom = bottomRect();
	return inStage(left + index * (size + skip), bottom.y() + padding.top(), size, size);
}

QString Viewer::viewersText() const {
	const auto story = current();
	const auto count = (story && story->viewerCount > 0) ? story->viewerCount : 0;
	return count
		? tr::lng_stories_viewers_count(tr::now, lt_count, double(count))
		: tr::lng_stories_viewers_none(tr::now);
}

QRect Viewer::viewersRect() const {
	const auto &padding = st::storiesReplyPadding;
	const auto text = viewersText();
	const auto width = st::storiesViewersIcon.width() + 8
		+ st::storiesViewersButtonFont->width(text) + 24;
	const auto bottom = bottomRect();
	return inStage(padding.left(), bottom.y() + padding.top(), width, st::storiesViewersHeight);
}

QRect Viewer::postCardRect() const {
	const auto story = current();
	if (!story || story->kind != Data::StoryKind::Post) {
		return QRect();
	}
	const auto &padding = st::storiesPostCardPadding;
	const auto width = std::min(_stage.width() - 48, 360);
	const auto inner = width - padding.left() - padding.right();
	auto height = padding.top() + padding.bottom();
	if (story->post && story->post->available) {
		height += st::storiesPostCardUserpic + 10;
		if (_image) {
			height += st::storiesPostCardThumb + 10;
		}
		if (!_postText.isEmpty()) {
			height += _postText.countHeight(inner, st::storiesPostCardTextLines);
		}
	} else {
		height += st::storiesPostCardTextStyle.font->height;
	}
	return QRect(
		_stage.x() + (_stage.width() - width) / 2,
		_stage.y() + (_stage.height() - height) / 2 - 24,
		width,
		height);
}

void Viewer::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void Viewer::updateGeometries() {
	const auto margin = st::storiesStageMargin;
	const auto nav = st::storiesNavPrevious.width + st::storiesNavSkip;
	auto height = std::max(this->height() - 2 * margin, 1);
	auto width = int(height * kStageAspect);
	const auto maxWidth = std::max(this->width() - 2 * nav, int(st::storiesStageMinWidth));
	if (width > maxWidth) {
		width = std::min(maxWidth, this->width());
		height = std::min(int(width / kStageAspect), this->height());
	}
	_stage = QRect(
		(this->width() - width) / 2,
		(this->height() - height) / 2,
		width,
		height);
	_chrome->setGeometry(rect());
	_video->setGeometry(_stage);

	const auto header = headerRect();
	const auto buttonTop = header.y() + (header.height() - st::storiesClose.height) / 2;
	auto right = st::storiesHeaderSide - 6;
	const auto placeTrailing = [&](Ui::IconButton *button) {
		const auto rect = inStage(
			_stage.width() - right - button->width(),
			buttonTop,
			button->width(),
			button->height());
		button->move(rect.topLeft());
		right += button->width();
	};
	placeTrailing(_close);
	placeTrailing(_more);
	placeTrailing(_pause);

	const auto navTop = _stage.y() + (_stage.height() - _previousNav->height()) / 2;
	_previousNav->move(_stage.x() - st::storiesNavSkip - _previousNav->width(), navTop);
	_nextNav->move(_stage.x() + _stage.width() + st::storiesNavSkip, navTop);

	const auto &padding = st::storiesReplyPadding;
	const auto bottom = bottomRect();
	const auto fieldHeight = st::storiesReplyField.heightMin;
	const auto fieldTop = bottom.y() + bottom.height() - padding.bottom() - fieldHeight;
	const auto sendWidth = _send->width();
	const auto fieldWidth = std::max(_stage.width() - padding.left() - padding.right() - sendWidth - 4, 40);
	_reply->resizeToWidth(fieldWidth);
	const auto fieldRect = inStage(padding.left(), fieldTop, fieldWidth, fieldHeight);
	_reply->move(fieldRect.topLeft());
	const auto sendRect = inStage(
		padding.left() + fieldWidth + 4,
		fieldTop + (fieldHeight - _send->height()) / 2,
		sendWidth,
		_send->height());
	_send->move(sendRect.topLeft());

	const auto card = postCardRect();
	if (!card.isEmpty()) {
		_seePost->move(
			card.x() + (card.width() - _seePost->width()) / 2,
			card.y() + card.height() + 14);
	}
	updateControls();
}

void Viewer::updateControls() {
	const auto story = current();
	const auto reply = replyShown();
	_reply->setVisible(reply);
	_send->setVisible(reply && !_reply->text().trimmed().isEmpty());
	if (reply) {
		_reply->setPlaceholder(tr::lng_stories_reply_hint(
			tr::now,
			lt_name,
			Lang::Isolated(NameOf(session().data().users().peek(_list.user.id).valid()
				? session().data().users().peek(_list.user.id)
				: _list.user))));
	} else if (_reply->hasFocus()) {
		setFocus();
	}
	_more->setVisible(story != nullptr);
	_pause->setVisible(story != nullptr);
	const auto postShown = story
		&& story->kind == Data::StoryKind::Post
		&& story->post
		&& story->post->available;
	_seePost->setVisible(postShown);
	// Physical buttons: back is on the right in RTL.
	const auto back = style::RightToLeft() ? _nextNav : _previousNav;
	const auto forward = style::RightToLeft() ? _previousNav : _nextNav;
	back->setVisible(story && (_index > 0 || _userIndex > 0));
	forward->setVisible(story != nullptr);
	if (postShown) {
		const auto card = postCardRect();
		_seePost->move(
			card.x() + (card.width() - _seePost->width()) / 2,
			card.y() + card.height() + 14);
	}
}

// ---- painting

void Viewer::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::storiesViewerBg);
	auto hq = PainterHighQualityEnabler(p);
	auto clip = QPainterPath();
	clip.addRoundedRect(QRectF(_stage), st::storiesStageRadius, st::storiesStageRadius);
	p.setClipPath(clip);
	const auto story = current();
	if (story && story->kind == Data::StoryKind::Post) {
		auto gradient = QLinearGradient(_stage.topLeft(), _stage.bottomLeft());
		gradient.setColorAt(0., st::storiesStageGradientTop->c);
		gradient.setColorAt(1., st::storiesStageGradientBottom->c);
		p.fillRect(_stage, gradient);
		return;
	}
	p.fillRect(_stage, st::storiesStageBg);
	if (!story) {
		return;
	}
	const auto ratio = devicePixelRatioF();
	auto image = _image ? _image->request(_stage.size() * ratio) : QImage();
	if (image.isNull() && _preview) {
		image = _preview->request(_stage.size() * ratio);
		if (image.isNull()) {
			image = _preview->preview();
		}
	}
	if (story->kind == Data::StoryKind::Video && _video->isVisible() && !_video->videoSize().isEmpty()) {
		return;
	}
	if (!image.isNull()) {
		// Contained; a picture close to the stage's shape fills it.
		auto size = image.size().scaled(_stage.size(), Qt::KeepAspectRatio);
		const auto stageAspect = double(_stage.width()) / _stage.height();
		const auto imageAspect = double(image.width()) / std::max(image.height(), 1);
		if (std::abs(imageAspect - stageAspect) < 0.08) {
			size = image.size().scaled(_stage.size(), Qt::KeepAspectRatioByExpanding);
		}
		const auto target = QRect(
			_stage.x() + (_stage.width() - size.width()) / 2,
			_stage.y() + (_stage.height() - size.height()) / 2,
			size.width(),
			size.height());
		p.drawImage(target, image);
	}
}

void Viewer::paintChrome(Painter &p) {
	const auto story = current();
	auto hq = PainterHighQualityEnabler(p);
	if (!_unavailable.isEmpty() || (!story && !_loadingUser)) {
		p.setFont(st::storiesErrorFont);
		p.setPen(st::storiesOverFgDim);
		p.drawText(_stage, Qt::AlignCenter | Qt::TextWordWrap, _unavailable);
		return;
	}
	{
		// The shades that keep the white text readable over any picture.
		auto top = QLinearGradient(0, _stage.y(), 0, _stage.y() + st::storiesShadeTop);
		top.setColorAt(0., st::storiesShadeFg->c);
		top.setColorAt(1., Qt::transparent);
		auto clip = QPainterPath();
		clip.addRoundedRect(QRectF(_stage), st::storiesStageRadius, st::storiesStageRadius);
		p.save();
		p.setClipPath(clip);
		p.fillRect(QRect(_stage.x(), _stage.y(), _stage.width(), st::storiesShadeTop), top);
		const auto shadeBottom = std::max(int(st::storiesShadeBottom), bottomRect().height() + 40);
		auto bottom = QLinearGradient(0, _stage.y() + _stage.height() - shadeBottom, 0, _stage.y() + _stage.height());
		bottom.setColorAt(0., Qt::transparent);
		bottom.setColorAt(1., st::storiesShadeFg->c);
		p.fillRect(
			QRect(_stage.x(), _stage.y() + _stage.height() - shadeBottom, _stage.width(), shadeBottom),
			bottom);
		p.restore();
	}

	// Progress: one segment per story.
	const auto count = std::max(int(_list.stories.size()), 1);
	const auto side = st::storiesProgressSide;
	const auto skip = st::storiesProgressSkip;
	const auto barsWidth = _stage.width() - 2 * side;
	const auto segment = std::max((barsWidth - (count - 1) * skip) / count, 2);
	auto progress = 0.;
	if (story) {
		if (story->kind == Data::StoryKind::Video && !_mediaFailed && _video->durationMs() > 0) {
			progress = double(_video->positionMs()) / _video->durationMs();
		} else if (story->kind != Data::StoryKind::Video || _mediaFailed) {
			progress = double(_elapsed) / std::max(story->showMs(), qint64(1));
		}
	}
	progress = std::clamp(progress, 0., 1.);
	for (auto i = 0; i != count; ++i) {
		const auto bar = inStage(
			side + i * (segment + skip),
			_stage.y() + st::storiesProgressTop,
			segment,
			st::storiesProgressHeight);
		p.setPen(Qt::NoPen);
		p.setBrush(st::storiesProgressBg);
		const auto radius = st::storiesProgressHeight / 2.;
		p.drawRoundedRect(bar, radius, radius);
		const auto filled = (i < _index) ? 1. : (i == _index) ? progress : 0.;
		if (filled > 0.) {
			const auto width = int(std::round(bar.width() * filled));
			const auto fill = style::RightToLeft()
				? QRect(bar.x() + bar.width() - width, bar.y(), width, bar.height())
				: QRect(bar.x(), bar.y(), width, bar.height());
			p.setBrush(st::storiesProgressFg);
			p.drawRoundedRect(fill, radius, radius);
		}
	}

	// Header: the face, the name, the time, «دوستان نزدیک».
	const auto header = headerRect();
	const auto userpicSize = st::storiesHeaderUserpic;
	const auto userpic = inStage(st::storiesHeaderSide, header.y(), userpicSize, userpicSize);
	_userpics.paint(p, userpic, _list.user.id);
	const auto card = session().data().users().peek(_list.user.id);
	const auto name = NameOf(card.valid() ? card : _list.user);
	const auto textLeft = st::storiesHeaderSide + userpicSize + st::storiesHeaderSkip;
	const auto buttons = _close->width() + _more->width() + _pause->width() + st::storiesHeaderSide;
	const auto available = std::max(_stage.width() - textLeft - buttons, 0);
	const auto nameFont = st::storiesHeaderNameFont;
	const auto timeFont = st::storiesHeaderTimeFont;
	const auto nameShown = nameFont->elided(name, available);
	const auto nameWidth = nameFont->width(nameShown);
	const auto lineTop = header.y() + (header.height() - nameFont->height - timeFont->height) / 2;
	p.setPen(st::storiesOverFg);
	p.setFont(nameFont);
	const auto nameRect = inStage(textLeft, lineTop, nameWidth, nameFont->height);
	p.drawText(nameRect.x(), nameRect.y() + nameFont->ascent, nameShown);
	if (story) {
		auto time = Lang::RelativeTime(QDateTime::fromSecsSinceEpoch(story->createdAt));
		const auto timeShown = timeFont->elided(time, available);
		const auto timeWidth = timeFont->width(timeShown);
		p.setFont(timeFont);
		p.setPen(st::storiesOverFgDim);
		const auto timeRect = inStage(textLeft, lineTop + nameFont->height, timeWidth, timeFont->height);
		p.drawText(timeRect.x(), timeRect.y() + timeFont->ascent, timeShown);
		if (story->closeFriends) {
			const auto label = tr::lng_stories_close_friends(tr::now);
			const auto chipFont = st::storiesChipFont;
			const auto chipWidth = chipFont->width(label) + 2 * st::storiesChipPadding;
			const auto chipLeft = textLeft + timeWidth + 8;
			if (chipLeft + chipWidth <= textLeft + available) {
				const auto chip = inStage(
					chipLeft,
					timeRect.y() + (timeFont->height - st::storiesChipHeight) / 2,
					chipWidth,
					st::storiesChipHeight);
				auto gradient = QLinearGradient(chip.topLeft(), chip.topRight());
				gradient.setColorAt(0., st::storiesRingCloseStart->c);
				gradient.setColorAt(1., st::storiesRingCloseEnd->c);
				p.setPen(Qt::NoPen);
				p.setBrush(gradient);
				p.drawRoundedRect(chip, chip.height() / 2., chip.height() / 2.);
				p.setPen(st::storiesChipFg);
				p.setFont(chipFont);
				p.drawText(chip, Qt::AlignCenter, label);
			}
		}
	}

	if (!story) {
		const auto size = int(st::storiesSpinnerSize);
		_loading.draw(
			p,
			QRect(_stage.x() + (_stage.width() - size) / 2, _stage.y() + (_stage.height() - size) / 2, size, size),
			3.,
			st::storiesOverFg->c);
		return;
	}

	// The shared post.
	if (story->kind == Data::StoryKind::Post) {
		const auto cardRect = postCardRect();
		const auto &padding = st::storiesPostCardPadding;
		p.setPen(Qt::NoPen);
		p.setBrush(st::storiesPostCardBg);
		p.drawRoundedRect(cardRect, st::storiesPostCardRadius, st::storiesPostCardRadius);
		const auto inner = cardRect.marginsRemoved(padding);
		if (story->post && story->post->available) {
			auto top = inner.y();
			const auto pic = st::storiesPostCardUserpic;
			const auto author = story->post->author.value_or(Data::User());
			const auto authorRect = style::RightToLeft()
				? QRect(inner.x() + inner.width() - pic, top, pic, pic)
				: QRect(inner.x(), top, pic, pic);
			if (author.valid()) {
				_userpics.paint(p, authorRect, author);
			}
			p.setFont(st::storiesPostCardNameFont);
			p.setPen(st::storiesPostCardFg);
			const auto authorName = st::storiesPostCardNameFont->elided(NameOf(author), inner.width() - pic - 8);
			const auto authorWidth = st::storiesPostCardNameFont->width(authorName);
			const auto nameX = style::RightToLeft()
				? (authorRect.x() - 8 - authorWidth)
				: (authorRect.x() + pic + 8);
			p.drawText(
				nameX,
				top + (pic - st::storiesPostCardNameFont->height) / 2 + st::storiesPostCardNameFont->ascent,
				authorName);
			top += pic + 10;
			if (_image) {
				const auto size = st::storiesPostCardThumb;
				const auto thumb = QRect(inner.x() + (inner.width() - size) / 2, top, size, size);
				const auto image = _image->request(thumb.size() * devicePixelRatioF());
				p.setBrush(st::storiesStageBg);
				p.drawRoundedRect(thumb, 10, 10);
				if (!image.isNull()) {
					auto path = QPainterPath();
					path.addRoundedRect(QRectF(thumb), 10, 10);
					p.save();
					p.setClipPath(path);
					const auto scaled = image.size().scaled(thumb.size(), Qt::KeepAspectRatioByExpanding);
					p.drawImage(
						QRect(
							thumb.x() + (thumb.width() - scaled.width()) / 2,
							thumb.y() + (thumb.height() - scaled.height()) / 2,
							scaled.width(),
							scaled.height()),
						image);
					p.restore();
				}
				top += size + 10;
			}
			if (!_postText.isEmpty()) {
				auto context = Ui::Text::PaintContext();
				context.position = QPoint(inner.x(), top);
				context.availableWidth = inner.width();
				context.elisionLines = st::storiesPostCardTextLines;
				context.autoDirection = true;
				p.setPen(st::storiesPostCardFg);
				_postText.draw(p, context);
			}
		} else {
			p.setFont(st::storiesPostCardTextStyle.font);
			p.setPen(st::storiesPostCardFgDim);
			p.drawText(inner, Qt::AlignCenter, tr::lng_stories_post_unavailable(tr::now));
		}
	} else if (_mediaFailed) {
		p.setFont(st::storiesErrorFont);
		p.setPen(st::storiesOverFgDim);
		p.drawText(_stage, Qt::AlignCenter, tr::lng_stories_media_failed(tr::now));
	} else if (!_mediaReady) {
		const auto size = int(st::storiesSpinnerSize);
		_loading.draw(
			p,
			QRect(_stage.x() + (_stage.width() - size) / 2, _stage.y() + (_stage.height() - size) / 2, size, size),
			3.,
			st::storiesOverFg->c);
	}

	// The caption over the bottom shade.
	const auto bottom = bottomRect();
	if (!_caption.isEmpty()) {
		const auto &padding = st::storiesCaptionPadding;
		const auto width = _stage.width() - padding.left() - padding.right();
		const auto height = _caption.countHeight(width, st::storiesCaptionLines);
		auto context = Ui::Text::PaintContext();
		context.position = QPoint(
			_stage.x() + padding.left(),
			bottom.y() - padding.bottom() - height);
		context.availableWidth = width;
		context.elisionLines = st::storiesCaptionLines;
		context.autoDirection = true;
		p.setPen(st::storiesOverFg);
		_caption.draw(p, context);
	}

	// What the bottom holds: my viewers, the reactions, or why not.
	if (mine()) {
		const auto rect = viewersRect();
		if (_over.part == Part::Viewers) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::storiesReactionBgOver);
			p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
		}
		const auto iconSize = st::storiesViewersIcon.size();
		const auto icon = QRect(
			style::RightToLeft()
				? (rect.x() + rect.width() - 12 - iconSize.width())
				: (rect.x() + 12),
			rect.y() + (rect.height() - iconSize.height()) / 2,
			iconSize.width(),
			iconSize.height());
		st::storiesViewersIcon.paintInCenter(p, icon);
		const auto text = viewersText();
		p.setFont(st::storiesViewersButtonFont);
		p.setPen(st::storiesOverFg);
		const auto textWidth = st::storiesViewersButtonFont->width(text);
		const auto textX = style::RightToLeft()
			? (icon.x() - 8 - textWidth)
			: (icon.x() + icon.width() + 8);
		p.drawText(
			textX,
			rect.y() + (rect.height() - st::storiesViewersButtonFont->height) / 2
				+ st::storiesViewersButtonFont->ascent,
			text);
	} else if (replyShown()) {
		const auto &reactions = Data::StoryReactions();
		for (auto i = 0; i != int(reactions.size()); ++i) {
			const auto rect = reactionRect(i);
			const auto active = (story->myReaction == reactions[i]);
			if (active || (_over.part == Part::Reaction && _over.index == i)) {
				p.setPen(Qt::NoPen);
				p.setBrush(active ? st::storiesReactionActiveBg : st::storiesReactionBgOver);
				p.drawEllipse(rect);
			}
			p.setFont(st::storiesReactionFont);
			p.setPen(st::storiesOverFg);
			p.drawText(rect, Qt::AlignCenter, reactions[i]);
		}
	} else {
		p.setFont(st::storiesReplyClosedFont);
		p.setPen(st::storiesOverFgDim);
		p.drawText(
			bottom.marginsRemoved(st::storiesReplyPadding),
			Qt::AlignCenter,
			tr::lng_stories_replies_closed(tr::now));
	}
}

// ---- input

Viewer::Hit Viewer::hitAt(QPoint point) const {
	if (!_stage.contains(point)) {
		return { Part::Outside, -1 };
	}
	const auto story = current();
	if (!story) {
		return { Part::Stage, -1 };
	}
	const auto header = headerRect();
	const auto userpicSize = st::storiesHeaderUserpic;
	const auto nameWidth = std::min(
		_stage.width() / 2,
		userpicSize + st::storiesHeaderSkip + st::storiesHeaderNameFont->width(NameOf(_list.user)));
	if (inStage(st::storiesHeaderSide, header.y(), nameWidth, userpicSize).contains(point)) {
		return { Part::Header, -1 };
	}
	if (mine()) {
		if (viewersRect().contains(point)) {
			return { Part::Viewers, -1 };
		}
	} else if (replyShown()) {
		for (auto i = 0; i != int(Data::StoryReactions().size()); ++i) {
			if (reactionRect(i).contains(point)) {
				return { Part::Reaction, i };
			}
		}
	}
	if (bottomRect().contains(point)) {
		return { Part::None, -1 };
	}
	return { Part::Stage, -1 };
}

void Viewer::chromePress(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	if (_reply->hasFocus()) {
		setFocus();
	}
	_pressed = hitAt(e->pos());
	_pressedAt = Ui::Now();
	if (_pressed.part == Part::Stage) {
		const auto pressedAt = _pressedAt;
		Ui::CallDelayed(kHoldMs, this, [=, this] {
			if (_pressed.part == Part::Stage && _pressedAt == pressedAt) {
				_holding = true;
				updatePlayback();
			}
		});
	}
}

void Viewer::chromeMove(QMouseEvent *e) {
	const auto hit = hitAt(e->pos());
	if (_over != hit) {
		_over = hit;
		const auto pointer = (hit.part == Part::Header)
			|| (hit.part == Part::Reaction)
			|| (hit.part == Part::Viewers);
		_chrome->setCursor(pointer ? style::cur_pointer : style::cur_default);
		_chrome->update();
	}
}

void Viewer::chromeLeave() {
	if (_over.part != Part::None) {
		_over = Hit();
		_chrome->update();
	}
}

void Viewer::chromeRelease(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, Hit());
	const auto wasHolding = std::exchange(_holding, false);
	if (wasHolding) {
		updatePlayback();
		return;
	}
	const auto hit = hitAt(e->pos());
	if (hit != pressed) {
		return;
	}
	switch (hit.part) {
	case Part::Outside:
		close();
		break;
	case Part::Stage: {
		if (!current()) {
			if (!_unavailable.isEmpty()) {
				close();
			}
			break;
		}
		// The leading half goes back, the trailing half goes on.
		const auto x = e->pos().x() - _stage.x();
		const auto leftHalf = (x < _stage.width() / 2);
		const auto back = (leftHalf != style::RightToLeft());
		back ? previous() : next();
	} break;
	case Part::Header:
		openProfile();
		break;
	case Part::Reaction:
		react(hit.index);
		break;
	case Part::Viewers:
		if (const auto story = current()) {
			ShowViewersBox(_controller, story->id);
		}
		break;
	case Part::None:
		break;
	}
}

void Viewer::keyPressEvent(QKeyEvent *e) {
	const auto rtl = style::RightToLeft();
	switch (e->key()) {
	case Qt::Key_Escape:
		close();
		break;
	case Qt::Key_Space:
		togglePause();
		break;
	case Qt::Key_Left:
		rtl ? next() : previous();
		break;
	case Qt::Key_Right:
		rtl ? previous() : next();
		break;
	default:
		RpWidget::keyPressEvent(e);
		break;
	}
}

// ---- actions

void Viewer::showMenu() {
	const auto story = current();
	if (!story) {
		return;
	}
	const auto controller = _controller;
	const auto storyId = story->id;
	const auto ownerId = _list.user.id;
	const auto menu = new Ui::PopupMenu(this);
	_menu = menu;
	connect(menu, &QObject::destroyed, this, [this] {
		Ui::PostponeCall(this, [this] {
			updatePlayback();
			if (!_closing && !_reply->hasFocus()) {
				setFocus();
			}
		});
	});
	if (mine()) {
		menu->addAction(tr::lng_stories_viewers(tr::now), [=] {
			ShowViewersBox(controller, storyId);
		}, &st::storiesMenuIconEye);
		auto remove = Ui::MenuItem();
		remove.text = tr::lng_stories_delete(tr::now);
		remove.icon = &st::menuIconDelete;
		remove.destructive = true;
		remove.callback = [this] { confirmDelete(); };
		menu->addItem(std::move(remove));
	} else {
		auto muted = false;
		for (const auto &item : stories().tray()) {
			if (item.user.id == ownerId) {
				muted = item.muted;
			}
		}
		const auto card = session().data().users().peek(ownerId);
		const auto name = Lang::Isolated(NameOf(card.valid() ? card : _list.user));
		menu->addAction(
			muted
				? tr::lng_stories_unmute(tr::now, lt_name, name)
				: tr::lng_stories_mute(tr::now, lt_name, name),
			[=] {
				controller->session().data().stories().setMuted(ownerId, !muted);
				controller->showToast(muted
					? tr::lng_stories_unmuted(tr::now, lt_name, name)
					: tr::lng_stories_muted(tr::now, lt_name, name));
			},
			muted ? &st::storiesMenuIconEye : &st::storiesMenuIconHide);
		auto report = Ui::MenuItem();
		report.text = tr::lng_stories_report(tr::now);
		report.icon = &st::menuIconReport;
		report.destructive = true;
		report.callback = [=] {
			auto args = Boxes::ReportArgs();
			args.kind = u"story"_s;
			args.id = storyId;
			args.offerBlockUserId = ownerId;
			Boxes::ShowReportBox(controller, std::move(args));
		};
		menu->addItem(std::move(report));
	}
	menu->popupAnchored(_more);
	updatePlayback();
}

void Viewer::confirmDelete() {
	const auto story = current();
	if (!story) {
		return;
	}
	const auto storyId = story->id;
	const auto controller = _controller;
	const auto weak = QPointer<Viewer>(this);
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_stories_delete_confirm(tr::now);
	args.text = tr::lng_stories_delete_about(tr::now);
	args.confirmText = tr::lng_delete(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> closeBox) {
		closeBox();
		controller->session().data().stories().deleteStory(storyId, controller, [=] {
			controller->showToast(tr::lng_stories_deleted(tr::now));
			if (!weak) {
				return;
			}
			auto &list = weak->_list.stories;
			for (auto i = 0; i != int(list.size()); ++i) {
				if (list[i].id == storyId) {
					list.erase(list.begin() + i);
					if (list.isEmpty()) {
						weak->close();
					} else {
						weak->showStory(std::min(i, int(list.size()) - 1));
					}
					break;
				}
			}
		}, [=](const Api::Error &error) {
			Support::ShowError(controller, error);
		});
	};
	_controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void Viewer::sendReply() {
	const auto story = current();
	const auto text = _reply->text().trimmed();
	if (!story || text.isEmpty() || !replyShown()) {
		return;
	}
	const auto controller = _controller;
	const auto name = Lang::Isolated(NameOf(_list.user));
	_reply->clear();
	setFocus();
	stories().reply(story->id, text, this, [=](const QString &, qint64) {
		controller->showToast(tr::lng_stories_reply_sent(tr::now));
	}, [=](const Api::Error &error) {
		if (error.code == u"story.replies_off") {
			controller->showToast(tr::lng_stories_replies_closed(tr::now));
		} else if (error.code == u"story.reply_not_allowed") {
			controller->showToast(tr::lng_stories_reply_not_allowed(tr::now, lt_name, name));
		} else if (error.code == u"story.not_found") {
			controller->showToast(tr::lng_stories_unavailable(tr::now));
		} else {
			Support::ShowError(controller, error);
		}
	});
}

void Viewer::react(int index) {
	const auto story = current();
	const auto &reactions = Data::StoryReactions();
	if (!story || index < 0 || index >= reactions.size()) {
		return;
	}
	const auto controller = _controller;
	const auto name = Lang::Isolated(NameOf(_list.user));
	const auto emoji = reactions[index];
	const auto fail = [=](const Api::Error &error) {
		if (error.code == u"story.replies_off") {
			controller->showToast(tr::lng_stories_replies_closed(tr::now));
		} else if (error.code == u"story.reply_not_allowed") {
			controller->showToast(tr::lng_stories_reply_not_allowed(tr::now, lt_name, name));
		} else {
			Support::ShowError(controller, error);
		}
	};
	if (story->myReaction == emoji) {
		stories().unreact(story->id, this, [=] {
			controller->showToast(tr::lng_stories_reaction_removed(tr::now));
		}, fail);
	} else {
		stories().react(story->id, emoji, this, [=] {
			controller->showToast(tr::lng_stories_reaction_sent(tr::now));
		}, fail);
	}
	_chrome->update();
}

void Viewer::openProfile() {
	const auto userId = _list.user.id;
	const auto controller = _controller;
	close();
	controller->openRoute(Window::Routes::User(userId));
}

void Viewer::openPost() {
	const auto story = current();
	if (!story || !story->post || !story->post->available) {
		return;
	}
	const auto postId = story->post->id;
	const auto controller = _controller;
	close();
	controller->openRoute(Window::Routes::Post(postId));
}

Viewer *Show(
		Window::SessionController *controller,
		QStringList users,
		int index,
		const QString &storyId) {
	const auto window = controller->window();
	const auto viewer = new Viewer(
		window->bodyWidget(),
		controller,
		std::move(users),
		index,
		storyId);
	window->registerDarkOverlay(viewer);
	viewer->show();
	viewer->raise();
	viewer->setFocus();
	return viewer;
}

} // namespace

void OpenUserStories(
		Window::SessionController *controller,
		const QString &userId,
		const QStringList &queue) {
	if (userId.isEmpty() || !controller->session().data().stories().enabled()) {
		return;
	}
	auto users = queue;
	auto index = int(users.indexOf(userId));
	if (index < 0) {
		users = QStringList{ userId };
		index = 0;
	}
	Show(controller, users, index, QString());
}

void OpenStory(
		Window::SessionController *controller,
		const QString &ownerId,
		const QString &storyId) {
	if (ownerId.isEmpty() || storyId.isEmpty()) {
		return;
	}
	Show(controller, QStringList{ ownerId }, 0, storyId);
}

} // namespace Stories
