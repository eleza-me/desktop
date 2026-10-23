#include "media/media_video_player.h"

#include "core/core_application.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "media/media_audio_player.h"
#include "ui/painter.h"
#include "styles/style_media.h"

#include <QtCore/QUrl>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

namespace Media {
namespace {

constexpr auto kSeekStep = 5000;
constexpr auto kControlsFade = 160;

[[nodiscard]] QRect FitRect(QSize content, QRect outer) {
	if (content.isEmpty() || outer.isEmpty()) {
		return outer;
	}
	const auto scaled = content.scaled(outer.size(), Qt::KeepAspectRatio);
	return QRect(
		outer.x() + (outer.width() - scaled.width()) / 2,
		outer.y() + (outer.height() - scaled.height()) / 2,
		scaled.width(),
		scaled.height());
}

void PaintBar(
		QPainter &p,
		const QRect &rect,
		double value,
		bool knob) {
	const auto line = QRectF(
		rect.x(),
		rect.y() + (rect.height() - st::videoSeekHeight) / 2.,
		rect.width(),
		st::videoSeekHeight);
	const auto radius = line.height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(st::videoSeekBg);
	p.drawRoundedRect(line, radius, radius);
	const auto filled = line.width() * std::clamp(value, 0., 1.);
	p.setBrush(st::videoSeekFg);
	p.drawRoundedRect(
		QRectF(line.x(), line.y(), filled, line.height()),
		radius,
		radius);
	if (knob) {
		const auto size = st::videoSeekKnob / 2.;
		p.drawEllipse(
			QPointF(line.x() + filled, line.y() + line.height() / 2.),
			size,
			size);
	}
}

// Four corner brackets: "enter full screen" points outwards.
void PaintFullscreenIcon(QPainter &p, const QRect &rect, bool exit) {
	const auto size = 14.;
	const auto arm = 5.;
	const auto left = rect.x() + (rect.width() - size) / 2.;
	const auto top = rect.y() + (rect.height() - size) / 2.;
	auto pen = QPen(st::mediaOverFg->c, 1.75);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto corner = [&](double x, double y, double dx, double dy) {
		auto path = QPainterPath();
		if (exit) {
			// The bracket's corner points to the center.
			path.moveTo(x + dx * arm, y);
			path.lineTo(x + dx * arm, y + dy * arm);
			path.lineTo(x, y + dy * arm);
		} else {
			path.moveTo(x + dx * arm, y);
			path.lineTo(x, y);
			path.lineTo(x, y + dy * arm);
		}
		p.drawPath(path);
	};
	corner(left, top, 1, 1);
	corner(left + size, top, -1, 1);
	corner(left, top + size, 1, -1);
	corner(left + size, top + size, -1, -1);
}

} // namespace

VideoPlayerWidget::VideoPlayerWidget(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
	_controlsTimer.setSingleShot(true);
	_controlsTimer.setInterval(st::videoControlsHideDelay);
	QObject::connect(&_controlsTimer, &QTimer::timeout, this, [this] {
		controlsTimeout();
	});
}

VideoPlayerWidget::~VideoPlayerWidget() {
	if (_player) {
		_player->stop();
		_player->setVideoSink(nullptr);
	}
}

double VideoPlayerWidget::volume() const {
	return Core::Application::Exists()
		? Core::App().settings().playbackVolume()
		: 1.;
}

void VideoPlayerWidget::ensurePlayer() {
	if (_player) {
		return;
	}
	_output = std::make_unique<QAudioOutput>();
	_output->setVolume(float(volume()));
	_output->setMuted(_muted);
	_sink = std::make_unique<QVideoSink>();
	_player = std::make_unique<QMediaPlayer>();
	_player->setAudioOutput(_output.get());
	_player->setVideoSink(_sink.get());
	_player->setLoops(_looped ? QMediaPlayer::Infinite : 1);
	QObject::connect(
		_sink.get(),
		&QVideoSink::videoFrameChanged,
		this,
		[this](const QVideoFrame &frame) {
			if (!frame.isValid()) {
				return;
			}
			const auto image = frame.toImage();
			if (image.isNull()) {
				return;
			}
			const auto sizeChanged = (image.size() != _frame.size());
			_frame = image;
			if (sizeChanged) {
				emit videoSizeChanged(_frame.size());
			}
			update();
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::positionChanged,
		this,
		[this] {
			if (_controlsVisible || _controlsOpacity.animating()) {
				update(controlsLayout().panel);
			}
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::playbackStateChanged,
		this,
		[this](QMediaPlayer::PlaybackState state) {
			setPlaying(state == QMediaPlayer::PlayingState);
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::mediaStatusChanged,
		this,
		[this](QMediaPlayer::MediaStatus status) {
			if (status == QMediaPlayer::EndOfMedia) {
				_ended = true;
				showControls();
				emit finished();
			} else if (status == QMediaPlayer::InvalidMedia) {
				_failed = true;
				update();
				emit failed();
			}
		});
	QObject::connect(
		_player.get(),
		&QMediaPlayer::errorOccurred,
		this,
		[this](QMediaPlayer::Error error, const QString &text) {
			if (error == QMediaPlayer::NoError) {
				return;
			}
			// The text may name the file or its address.
#ifdef ELEZA_DEBUG_TOOLS
			qWarning("Media: video playback failed: %s", qPrintable(text));
#else // ELEZA_DEBUG_TOOLS
			Q_UNUSED(text);
			qWarning("Media: video playback failed: %d", int(error));
#endif // ELEZA_DEBUG_TOOLS
			_failed = true;
			update();
			emit failed();
		});
}

void VideoPlayerWidget::setSource(const QString &filePath) {
	if (_source == filePath) {
		return;
	}
	_source = filePath;
	_failed = false;
	_ended = false;
	_frame = QImage();
	if (_player) {
		_player->stop();
		_player->setSource(QUrl::fromLocalFile(filePath));
	}
	update();
}

void VideoPlayerWidget::setPoster(const QImage &poster) {
	_poster = poster;
	if (_frame.isNull()) {
		update();
	}
}

void VideoPlayerWidget::play() {
	if (_source.isEmpty() || _failed) {
		return;
	}
	ensurePlayer();
	if (_player->source().isEmpty()) {
		_player->setSource(QUrl::fromLocalFile(_source));
	}
	if (_ended) {
		_ended = false;
		_player->setPosition(0);
	}
	if (!_muted) {
		// One thing sounds at a time.
		Player().pause();
	}
	_player->play();
	showControls();
}

void VideoPlayerWidget::pause() {
	if (_player) {
		_player->pause();
	}
	showControls();
}

void VideoPlayerWidget::toggle() {
	if (_playing) {
		pause();
	} else {
		play();
	}
}

void VideoPlayerWidget::stop() {
	if (_player) {
		_player->pause();
		_player->setPosition(0);
	}
	_ended = false;
	showControls();
}

void VideoPlayerWidget::seek(double progress) {
	const auto duration = durationMs();
	if (_player && duration > 0) {
		_ended = false;
		_player->setPosition(qint64(std::clamp(progress, 0., 1.) * duration));
		update();
	}
}

void VideoPlayerWidget::seekBy(qint64 deltaMs) {
	const auto duration = durationMs();
	if (_player && duration > 0) {
		_ended = false;
		_player->setPosition(std::clamp(
			_player->position() + deltaMs,
			qint64(0),
			duration));
		showControls();
	}
}

void VideoPlayerWidget::setMuted(bool muted) {
	if (_muted == muted) {
		return;
	}
	_muted = muted;
	if (_output) {
		_output->setMuted(muted);
	}
	update();
	emit mutedChanged(muted);
}

void VideoPlayerWidget::setLooped(bool looped) {
	_looped = looped;
	if (_player) {
		_player->setLoops(looped ? QMediaPlayer::Infinite : 1);
	}
}

void VideoPlayerWidget::setControlsShown(bool shown) {
	_controlsShown = shown;
	update();
}

void VideoPlayerWidget::setFullscreenAllowed(bool allowed) {
	_fullscreenAllowed = allowed;
	update();
}

void VideoPlayerWidget::setRadius(int radius) {
	_radius = radius;
	update();
}

qint64 VideoPlayerWidget::positionMs() const {
	return _player ? _player->position() : 0;
}

qint64 VideoPlayerWidget::durationMs() const {
	return _player ? std::max(_player->duration(), qint64(0)) : 0;
}

void VideoPlayerWidget::setPlaying(bool playing) {
	if (_playing == playing) {
		return;
	}
	_playing = playing;
	showControls();
	emit playingChanged(playing);
}

void VideoPlayerWidget::setFullscreen(bool fullscreen) {
	if (_fullscreen == fullscreen || (fullscreen && !_fullscreenAllowed)) {
		return;
	}
	_fullscreen = fullscreen;
	_reparenting = true;
	if (fullscreen) {
		_normalParent = parentWidget();
		_normalGeometry = geometry();
		_normalFlags = windowFlags();
		const auto screen = this->screen();
		setParent(nullptr);
		setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
		if (screen) {
			setGeometry(screen->geometry());
		}
		showFullScreen();
		activateWindow();
		setFocus();
	} else {
		hide();
		setWindowFlags(_normalFlags);
		if (_normalParent) {
			setParent(_normalParent);
			setGeometry(_normalGeometry);
			show();
			setFocus();
		} else {
			// The place we came from is gone.
			deleteLater();
		}
	}
	_reparenting = false;
	showControls();
	emit fullscreenChanged(fullscreen);
}

QRect VideoPlayerWidget::videoRect() const {
	const auto size = !_frame.isNull()
		? _frame.size()
		: !_poster.isNull()
		? _poster.size()
		: QSize();
	return FitRect(size, rect());
}

QString VideoPlayerWidget::timeText() const {
	const auto duration = durationMs();
	const auto position = (_seekingProgress >= 0.)
		? qint64(_seekingProgress * duration)
		: positionMs();
	return Lang::Duration(int(position / 1000))
		+ QStringLiteral(" / ")
		+ Lang::Duration(int(duration / 1000));
}

VideoPlayerWidget::Layout VideoPlayerWidget::controlsLayout() const {
	// The player reads left to right in every interface language (like the
	// timeline of a video itself).
	auto result = Layout();
	const auto margin = int(st::videoControlsMargin);
	const auto height = int(st::videoControlsHeight);
	const auto width = std::max(this->width() - 2 * margin, 0);
	result.panel = QRect(
		margin,
		this->height() - margin - height,
		width,
		height);
	const auto &padding = st::videoControlsPadding;
	const auto button = st::videoButton.width;
	const auto top = result.panel.y() + (height - st::videoButton.height) / 2;
	auto left = result.panel.x() + padding.left();
	auto right = result.panel.x() + result.panel.width() - padding.right();
	result.play = QRect(left, top, button, st::videoButton.height);
	left += button + st::videoTimeSkip;
	if (_fullscreenAllowed) {
		right -= button;
		result.fullscreen = QRect(right, top, button, st::videoButton.height);
	}
	const auto compact = (width < st::videoControlsMinWidth + 160);
	if (!compact) {
		right -= st::videoVolumeWidth + st::videoTimeSkip;
		result.volume = QRect(
			right,
			result.panel.y(),
			st::videoVolumeWidth,
			height);
	}
	right -= button;
	result.mute = QRect(right, top, button, st::videoButton.height);
	right -= st::videoTimeSkip;
	const auto timeWidth = st::videoTimeFont->width(
		Lang::Num(QStringLiteral("00:00:00 / 00:00:00")));
	if (width >= st::videoControlsMinWidth + timeWidth) {
		result.time = QRect(right - timeWidth, result.panel.y(), timeWidth, height);
		right -= timeWidth + st::videoTimeSkip;
	}
	result.seek = QRect(left, result.panel.y(), std::max(right - left, 0), height);
	return result;
}

VideoPlayerWidget::Part VideoPlayerWidget::partAt(QPoint position) const {
	if (!_controlsShown || !_controlsVisible) {
		return Part::None;
	}
	const auto layout = controlsLayout();
	if (!layout.panel.contains(position)) {
		return Part::None;
	} else if (layout.play.contains(position)) {
		return Part::Play;
	} else if (layout.fullscreen.contains(position)) {
		return Part::Fullscreen;
	} else if (layout.mute.contains(position)) {
		return Part::Mute;
	} else if (layout.volume.contains(position)) {
		return Part::Volume;
	} else if (layout.seek.contains(position)) {
		return Part::Seek;
	}
	return Part::None;
}

void VideoPlayerWidget::showControls() {
	if (!_controlsVisible) {
		_controlsVisible = true;
		_controlsOpacity.start([this] { update(); }, 0., 1., kControlsFade);
	}
	update();
	if (_playing && _pressed == Part::None) {
		_controlsTimer.start();
	} else {
		_controlsTimer.stop();
	}
}

void VideoPlayerWidget::controlsTimeout() {
	if (!_playing || _pressed != Part::None) {
		return;
	}
	const auto position = mapFromGlobal(QCursor::pos());
	if (rect().contains(position) && controlsLayout().panel.contains(position)) {
		_controlsTimer.start(); // the mouse rests on the controls
		return;
	}
	_controlsVisible = false;
	_controlsOpacity.start([this] { update(); }, 1., 0., kControlsFade);
	if (_fullscreen) {
		setCursor(Qt::BlankCursor);
	}
	update();
}

void VideoPlayerWidget::applySeek(int x, const QRect &seek) {
	if (seek.width() <= 0) {
		return;
	}
	_seekingProgress = std::clamp(
		(x - seek.x()) / double(seek.width()),
		0.,
		1.);
	update();
}

void VideoPlayerWidget::applyVolume(int x, const QRect &volume) {
	if (volume.width() <= 0) {
		return;
	}
	const auto value = std::clamp(
		(x - volume.x()) / double(volume.width()),
		0.,
		1.);
	if (Core::Application::Exists()) {
		Core::App().settings().setPlaybackVolume(value);
	}
	if (_output) {
		_output->setVolume(float(value));
	}
	if (value > 0. && _muted) {
		setMuted(false);
	}
	update();
}

void VideoPlayerWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (_radius > 0) {
		auto clip = QPainterPath();
		clip.addRoundedRect(QRectF(rect()), _radius, _radius);
		p.setClipPath(clip);
	}
	p.fillRect(rect(), st::videoBg);
	const auto target = videoRect();
	if (!_frame.isNull()) {
		p.drawImage(target, _frame);
	} else if (!_poster.isNull()) {
		p.drawImage(target, _poster);
	}
	if (_failed) {
		p.setFont(st::videoErrorFont);
		p.setPen(st::mediaOverFgDim);
		p.drawText(
			rect(),
			Qt::AlignCenter,
			tr::lng_eli_video_couldnt_load_check_connection(tr::now));
		return;
	}
	if (!_playing && _pressed != Part::Seek) {
		// The big button in the middle while paused.
		const auto size = int(st::videoBigPlaySize);
		const auto circle = QRect(
			(width() - size) / 2,
			(height() - size) / 2,
			size,
			size);
		if (height() >= size * 2) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::videoBigPlayBg);
			p.drawEllipse(circle);
			st::videoBigPlayIcon.paintInCenter(p, circle.translated(2, 0));
		}
	}
	if (_controlsShown) {
		const auto opacity = _controlsOpacity.value(_controlsVisible ? 1. : 0.);
		if (opacity > 0.) {
			p.setOpacity(opacity);
			paintControls(p);
		}
	}
}

void VideoPlayerWidget::paintControls(QPainter &p) {
	const auto layout = controlsLayout();
	if (layout.panel.width() < st::videoButton.width * 3) {
		return;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(st::videoControlsBg);
	p.drawRoundedRect(
		layout.panel,
		st::videoControlsRadius,
		st::videoControlsRadius);

	const auto over = [&](Part part, const QRect &rect) {
		if (_over == part || _pressed == part) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::mediaOverBgOver);
			p.drawEllipse(rect.adjusted(2, 2, -2, -2));
		}
	};
	over(Part::Play, layout.play);
	(_playing ? st::videoPauseIcon : st::videoPlayIcon).paintInCenter(
		p,
		layout.play);

	const auto duration = durationMs();
	const auto progress = (_seekingProgress >= 0.)
		? _seekingProgress
		: (duration > 0)
		? (positionMs() / double(duration))
		: 0.;
	PaintBar(
		p,
		layout.seek,
		progress,
		(_over == Part::Seek) || (_pressed == Part::Seek));

	if (!layout.time.isEmpty()) {
		p.setFont(st::videoTimeFont);
		p.setPen(st::videoTimeFg);
		p.drawText(layout.time, Qt::AlignCenter, timeText());
	}

	over(Part::Mute, layout.mute);
	const auto silent = _muted || (volume() <= 0.);
	(silent ? st::videoMutedIcon : st::videoVolumeIcon).paintInCenter(
		p,
		layout.mute);
	if (!layout.volume.isEmpty()) {
		PaintBar(
			p,
			layout.volume,
			silent ? 0. : volume(),
			(_over == Part::Volume) || (_pressed == Part::Volume));
	}
	if (!layout.fullscreen.isEmpty()) {
		over(Part::Fullscreen, layout.fullscreen);
		PaintFullscreenIcon(p, layout.fullscreen, _fullscreen);
	}
}

void VideoPlayerWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	setFocus();
	const auto position = e->pos();
	_pressed = partAt(position);
	_pressedOnVideo = (_pressed == Part::None);
	const auto layout = controlsLayout();
	if (_pressed == Part::Seek) {
		applySeek(position.x(), layout.seek);
	} else if (_pressed == Part::Volume) {
		applyVolume(position.x(), layout.volume);
	}
	showControls();
}

void VideoPlayerWidget::mouseMoveEvent(QMouseEvent *e) {
	const auto position = e->pos();
	if (_fullscreen) {
		unsetCursor();
	}
	if (_pressed == Part::Seek) {
		applySeek(position.x(), controlsLayout().seek);
	} else if (_pressed == Part::Volume) {
		applyVolume(position.x(), controlsLayout().volume);
	} else if (_pressed == Part::None) {
		const auto over = partAt(position);
		if (_over != over) {
			_over = over;
			update();
		}
		setCursor((over != Part::None) ? style::cur_pointer : style::cur_default);
	}
	showControls();
}

void VideoPlayerWidget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, Part::None);
	const auto released = partAt(e->pos());
	if (pressed == Part::Seek) {
		const auto progress = std::exchange(_seekingProgress, -1.);
		if (progress >= 0.) {
			ensurePlayer();
			if (_player->source().isEmpty() && !_source.isEmpty()) {
				_player->setSource(QUrl::fromLocalFile(_source));
			}
			seek(progress);
		}
	} else if (pressed == Part::Play && released == Part::Play) {
		toggle();
	} else if (pressed == Part::Mute && released == Part::Mute) {
		if (volume() <= 0.) {
			applyVolume(
				controlsLayout().volume.x() + controlsLayout().volume.width(),
				controlsLayout().volume);
			setMuted(false);
		} else {
			setMuted(!_muted);
		}
	} else if (pressed == Part::Fullscreen && released == Part::Fullscreen) {
		setFullscreen(!_fullscreen);
	} else if (pressed == Part::None
		&& std::exchange(_pressedOnVideo, false)
		&& rect().contains(e->pos())) {
		toggle();
	}
	showControls();
}

void VideoPlayerWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && partAt(e->pos()) == Part::None) {
		// The first click of the pair toggled playback: undo that.
		toggle();
		setFullscreen(!_fullscreen);
	}
}

void VideoPlayerWidget::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over != Part::None) {
		_over = Part::None;
		update();
	}
}

void VideoPlayerWidget::keyPressEvent(QKeyEvent *e) {
	switch (e->key()) {
	case Qt::Key_Space:
	case Qt::Key_K:
		toggle();
		break;
	case Qt::Key_Left: seekBy(-kSeekStep); break;
	case Qt::Key_Right: seekBy(kSeekStep); break;
	case Qt::Key_M: setMuted(!_muted); showControls(); break;
	case Qt::Key_F:
		setFullscreen(!_fullscreen);
		break;
	case Qt::Key_Escape:
		if (_fullscreen) {
			setFullscreen(false);
			break;
		}
		[[fallthrough]];
	default:
		RpWidget::keyPressEvent(e);
		return;
	}
}

void VideoPlayerWidget::wheelEvent(QWheelEvent *e) {
	// The wheel over a video belongs to whatever scrolls around it.
	e->ignore();
}

void VideoPlayerWidget::hideEvent(QHideEvent *e) {
	RpWidget::hideEvent(e);
	if (_player && _playing && !_reparenting) {
		_player->pause();
	}
}

} // namespace Media
