// Viewers of a post's media: the album in the full-window image viewer, and
// the round video note played large over the window, which stays visible
// behind it — blurred and dimmed, never a solid background.
#include "eli/eli_actions.h"

#include "base/platform_file.h"
#include "data/data_feed.h"
#include "data/data_media.h"
#include "data/data_session.h"
#include "eli/eli_post_card.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_image_viewer.h"
#include "media/media_video_player.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "window/window_main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"
#include "styles/style_media.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

namespace Eli {

// The file a downloaded media lives in (the cache's own file, or a temporary
// copy when the cache keeps bytes in memory only).
QString LocalMediaFile(const Data::MediaHandlePtr &handle, const QString &suffix) {
	if (!handle || !handle->isReady()) {
		return QString();
	}
	if (!handle->filePath().isEmpty()) {
		return handle->filePath();
	}
	if (handle->bytes().isEmpty()) {
		return QString();
	}
	const auto directory = base::platform::PrivateTempDirectory();
	if (directory.isEmpty()) {
		return QString();
	}
	const auto name = u"eleza-"_s
		+ QString::number(qHash(handle->ref().cacheKey(handle->variant())), 16)
		+ suffix;
	const auto path = QDir(directory).filePath(name);
	if (!QFile::exists(path)) {
		auto file = QFile(path);
		if (!file.open(QIODevice::WriteOnly) || file.write(handle->bytes()) < 0) {
			return QString();
		}
	}
	return path;
}

namespace {

class VideoNoteOverlay final : public Ui::RpWidget {
public:
	VideoNoteOverlay(
		QWidget *parent,
		Window::SessionController *controller,
		const Data::Post &post,
		const Data::PostMedia &media)
	: RpWidget(parent)
	, _controller(controller)
	, _postId(post.id)
	, _author(post.author)
	, _media(media)
	, _video(new Media::VideoPlayerWidget(this))
	, _close(new Ui::IconButton(this, st::viewerClose))
	, _loading([this] { update(); })
	, _progress([this](qint64) {
		update(ringRect().adjusted(-4, -4, 4, 4));
		return true;
	}) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);

		// What is under us, blurred: the feed stays in sight.
		_backdrop = Ui::GrabWidgetToImage(parent);
		if (!_backdrop.isNull()) {
			const auto small = _backdrop.scaled(
				_backdrop.size() / 4,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
			_backdrop = Ui::Images::Blur(
				small.convertToFormat(QImage::Format_ARGB32_Premultiplied),
				st::eliNoteBlur / 4);
		}

		_video->setControlsShown(false);
		_video->setFullscreenAllowed(false);
		_video->setLooped(true);
		connect(_video, &Media::VideoPlayerWidget::failed, this, [this] {
			_failed = true;
			_loading.stop();
			update();
		});
		connect(_video, &Media::VideoPlayerWidget::playingChanged, this, [this](bool playing) {
			if (playing) {
				_loading.stop();
				_progress.start();
			} else {
				_progress.stop();
			}
			update();
		});
		_close->setClickedCallback([this] { closeOverlay(); });

		auto &media_ = controller->session().data().media();
		const auto ref = Data::MediaRef::PostMedia(_postId, _media.id);
		_poster = media_.request(ref, Data::MediaVariant::Poster);
		const auto applyPoster = [this] {
			if (_poster->isReady() && !_poster->image().isNull()) {
				_video->setPoster(_poster->image());
			}
		};
		applyPoster();
		connect(_poster.get(), &Data::MediaHandle::ready, this, applyPoster);

		auto options = Data::Media::Options();
		options.decode = false;
		_file = media_.request(ref, Data::MediaVariant::Mp4, options);
		const auto start = [this] {
			const auto path = LocalMediaFile(_file, u".mp4"_s);
			if (path.isEmpty()) {
				_failed = true;
				_loading.stop();
				update();
				return;
			}
			_video->setSource(path);
			_video->play();
		};
		if (_file->isReady()) {
			start();
		} else {
			_loading.start();
			connect(_file.get(), &Data::MediaHandle::ready, this, start);
			connect(_file.get(), &Data::MediaHandle::failed, this, [this] {
				_failed = true;
				_loading.stop();
				update();
			});
		}
		controller->session().data().feed().reportViews({ _postId });

		parent->installEventFilter(this);
		setGeometry(parent->rect());
		_show.start([this] { update(); }, 0., 1., st::motionFast, anim::easeOutCubic);
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (object == parentWidget() && event->type() == QEvent::Resize) {
			setGeometry(parentWidget()->rect());
		}
		return RpWidget::eventFilter(object, event);
	}

	[[nodiscard]] QRect circleRect() const {
		const auto size = std::clamp(
			std::min(width(), height()) * 3 / 5,
			160,
			int(st::eliNoteMaxSize));
		return QRect((width() - size) / 2, (height() - size) / 2 - 12, size, size);
	}

	[[nodiscard]] QRect ringRect() const {
		const auto gap = st::eliNoteRingWidth + 4;
		return circleRect().adjusted(-gap, -gap, gap, gap);
	}

	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		const auto circle = circleRect();
		_video->setGeometry(circle);
		_video->setRadius(circle.width() / 2);
		_close->moveToRight(st::viewerNavSkip, st::viewerNavSkip, width());
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto shown = _show.value(1.);
		if (!_backdrop.isNull()) {
			p.setRenderHint(QPainter::SmoothPixmapTransform);
			p.drawImage(rect(), _backdrop);
		} else {
			p.fillRect(rect(), st::bgCanvas);
		}
		p.setOpacity(shown);
		p.fillRect(rect(), st::eliNoteDim);
		p.setOpacity(1.);
		PainterHighQualityEnabler hq(p);
		const auto ring = QRectF(ringRect()).adjusted(
			st::eliNoteRingWidth / 2.,
			st::eliNoteRingWidth / 2.,
			-st::eliNoteRingWidth / 2.,
			-st::eliNoteRingWidth / 2.);
		auto pen = QPen(st::eliNoteRingBg->c);
		pen.setWidthF(st::eliNoteRingWidth);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(ring);
		pen.setColor(st::eliNoteRingFg->c);
		p.setPen(pen);
		const auto duration = _video->durationMs();
		if (_loading.animating()) {
			_loading.draw(p, ring.toRect(), st::eliNoteRingWidth, st::eliNoteRingFg->c);
		} else if (duration > 0) {
			const auto progress = std::clamp(
				double(_video->positionMs()) / duration,
				0.,
				1.);
			p.drawArc(ring, 90 * 16, -int(progress * 360 * 16));
		}
		const auto circle = circleRect();
		auto top = ringRect().bottom() + 18;
		p.setPen(st::eliPardehFg);
		p.setFont(st::eliNoteNameFont);
		p.drawTextCentered(
			QRect(0, top, width(), st::eliNoteNameFont->height),
			AuthorName(_controller->session(), _author));
		top += st::eliNoteNameFont->height + 4;
		p.setPen(st::eliPardehFgDim);
		p.setFont(st::smallFont);
		const auto time = _failed
			? tr::lng_eli_video_couldnt_load_check_connection(tr::now)
			: (duration > 0)
			? (QChar(0x2066)
				+ Lang::Duration(int(_video->positionMs() / 1000))
				+ tr::lng_eli_meta_separator(tr::now)
				+ Lang::Duration(int(duration / 1000))
				+ QChar(0x2069))
			: Lang::Duration(int(_media.durationMs / 1000));
		p.drawTextCentered(QRect(0, top, width(), st::smallFont->height), time);
		Q_UNUSED(circle);
	}

	void mousePressEvent(QMouseEvent *e) override {
		// The video itself takes its own clicks (pause / play).
		if (e->button() == Qt::LeftButton) {
			closeOverlay();
		}
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (e->key() == Qt::Key_Escape) {
			closeOverlay();
		} else if (e->key() == Qt::Key_Space) {
			_video->toggle();
		} else if (e->key() == Qt::Key_M) {
			_video->setMuted(!_video->muted());
		}
	}

	void wheelEvent(QWheelEvent *e) override {
		e->accept(); // the list behind does not scroll under the overlay
	}

private:
	void closeOverlay() {
		if (_closing) {
			return;
		}
		_closing = true;
		_video->pause();
		hide();
		deleteLater();
	}

	Window::SessionController *_controller = nullptr;
	QString _postId;
	QString _author;
	Data::PostMedia _media;
	Media::VideoPlayerWidget *_video = nullptr;
	Ui::IconButton *_close = nullptr;
	QImage _backdrop;
	Data::MediaHandlePtr _poster;
	Data::MediaHandlePtr _file;
	Ui::InfiniteRadialAnimation _loading;
	Ui::Animations::Basic _progress;
	Ui::Animations::Simple _show;
	bool _failed = false;
	bool _closing = false;

};

} // namespace

void OpenImageViewer(
		Window::SessionController *controller,
		const Data::Post &post,
		int index) {
	auto items = std::vector<Media::ViewerItem>();
	auto number = 0;
	for (const auto &media : post.media) {
		if (media.kind != u"image") {
			continue;
		}
		const auto ref = Data::MediaRef::PostMedia(post.id, media.id);
		auto preview = Support::ImageOptions();
		preview.blurhash = media.blurhash;
		preview.aspect = QSize(media.width, media.height);
		auto full = preview;
		full.original = true;
		auto item = Media::ViewerItem();
		item.source = Support::MakeImageSource(
			&controller->session(),
			ref,
			Data::MediaVariant::Full,
			std::move(full));
		item.preview = Support::MakeImageSource(
			&controller->session(),
			ref,
			Data::MediaVariant::Feed,
			std::move(preview));
		item.caption = media.alt.isEmpty() ? post.text : media.alt;
		item.saveName = u"eleza-%1-%2.jpg"_s.arg(post.id).arg(++number);
		items.push_back(std::move(item));
	}
	if (items.empty()) {
		return;
	}
	const auto shown = std::clamp(index, 0, int(items.size()) - 1);
	Media::ShowImageViewer(controller->window(), std::move(items), shown);
}

void OpenVideoNote(
		Window::SessionController *controller,
		const QString &postId) {
	auto &feed = controller->session().data().feed();
	const auto post = feed.post(postId);
	if (!post) {
		feed.fetchPost(postId, base::Guard(controller), [=](const Data::Post &) {
			if (controller->session().data().feed().post(postId)) {
				OpenVideoNote(controller, postId);
			}
		});
		return;
	} else if (post->media.isEmpty()) {
		return;
	}
	auto media = post->media.front();
	for (const auto &entry : post->media) {
		if (entry.kind == u"video_note") {
			media = entry;
			break;
		}
	}
	const auto overlay = new VideoNoteOverlay(
		controller->window()->bodyWidget(),
		controller,
		*post,
		media);
	overlay->show();
	overlay->raise();
	overlay->setFocus();
}

} // namespace Eli
