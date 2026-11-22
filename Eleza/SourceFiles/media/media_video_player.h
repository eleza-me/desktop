// Media::VideoPlayerWidget — a video painted by us (QMediaPlayer + QVideoSink)
// with overlay controls: play / pause, a seek bar with the time, volume and
// full screen. Usable inline (a bubble, a post) and inside the image viewer.
//
//   const auto video = Ui::CreateChild<Media::VideoPlayerWidget>(this);
//   video->setPoster(posterImage);            // shown until the first frame
//   video->setSource(localFilePath);          // a file from the media cache (Data::MediaVariant::Mp4)
//   video->setGeometry(...);
//   video->play();
//
//   video->setControlsShown(false);  video->setLooped(true);  video->setMuted(true);   // a gif
//
//   video->setStream(handle);  // a download that is still coming (Data::MediaStream):
//                              // plays what has arrived, seeks within it
//
// Click toggles playback, double click / F toggles full screen, Space, arrows
// (seek 5 s), M (mute) work while the widget has the focus. The controls hide
// after a moment of playing without mouse movement. Starting a video with sound
// pauses Media::Player().
#pragma once

#include "data/data_media.h"
#include "ui/effects/animations.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <memory>

class QAudioOutput;
class QMediaPlayer;
class QVideoSink;

namespace Data {
class MediaStream;
} // namespace Data

namespace Media {

// Whether a video or a voice message may start while it downloads
// (Data::MediaStream): with the FFmpeg backend of Qt 6.5+ (the default
// there), which reads the stream on threads of its own and does not hold
// the main thread while it does. Elsewhere (GStreamer, the default of
// Qt 6.4, reads on the main thread: the file seems to end early) they play
// once downloaded, as before.
[[nodiscard]] bool ProgressivePlayback();

class VideoPlayerWidget final : public Ui::RpWidget {
	Q_OBJECT

public:
	explicit VideoPlayerWidget(QWidget *parent);
	~VideoPlayerWidget();

	void setSource(const QString &filePath);
	// A download in progress (a Loading handle with its first bytes on
	// disk, Data::MediaStream::Streamable): played as it arrives, seeking
	// only into what is there. Once the file is complete it is the source;
	// if the player ran out of bytes first, it goes on from where it stopped.
	void setStream(Data::MediaHandlePtr handle);
	void setPoster(const QImage &poster);
	void play();
	void pause();
	void toggle();
	// Back to the start, paused.
	void stop();
	void seek(double progress);
	void seekBy(qint64 deltaMs);

	void setMuted(bool muted);
	[[nodiscard]] bool muted() const {
		return _muted;
	}
	void setLooped(bool looped);
	void setControlsShown(bool shown);
	void setFullscreenAllowed(bool allowed);
	// Rounded corners for inline use (0 in the viewer).
	void setRadius(int radius);

	[[nodiscard]] bool playing() const {
		return _playing;
	}
	[[nodiscard]] bool fullscreen() const {
		return _fullscreen;
	}
	// The size of a frame; empty until the first one arrived.
	[[nodiscard]] QSize videoSize() const {
		return _frame.size();
	}
	[[nodiscard]] qint64 positionMs() const;
	[[nodiscard]] qint64 durationMs() const;

	void setFullscreen(bool fullscreen);

Q_SIGNALS:
	void playingChanged(bool playing);
	// The sound was switched on or off (the button, M, setMuted).
	void mutedChanged(bool muted);
	void videoSizeChanged(QSize size);
	void finished();
	void failed();
	void fullscreenChanged(bool fullscreen);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	enum class Part {
		None,
		Play,
		Seek,
		Mute,
		Volume,
		Fullscreen,
	};
	struct Layout {
		QRect panel;
		QRect play;
		QRect time;
		QRect seek;
		QRect mute;
		QRect volume;
		QRect fullscreen;
	};

	void ensurePlayer();
	[[nodiscard]] bool hasSource() const;
	void setPlayerSource();
	void openStream(Data::MediaHandlePtr handle);
	void reopenStream();
	void dropStream();
	void streamStalled();
	void streamCompleted(const QString &path);
	// The position a seek may go to while the file is still arriving.
	[[nodiscard]] qint64 playablePosition(qint64 position) const;
	[[nodiscard]] Layout controlsLayout() const;
	[[nodiscard]] Part partAt(QPoint position) const;
	[[nodiscard]] QRect videoRect() const;
	[[nodiscard]] QString timeText() const;
	void applySeek(int x, const QRect &seek);
	void applyVolume(int x, const QRect &volume);
	void showControls();
	void controlsTimeout();
	void setPlaying(bool playing);
	void paintControls(QPainter &p);
	[[nodiscard]] double volume() const;

	// Declared before the player: destroyed after it.
	Data::MediaHandlePtr _streamHandle;
	std::unique_ptr<Data::MediaStream> _stream;
	qint64 _resumeAt = -1;    // ran out of bytes there: on from it once more came
	qint64 _rebufferAt = 0;   // ...this many bytes from the start
	qint64 _pendingSeek = -1; // applied once the new source is loaded
	std::unique_ptr<QMediaPlayer> _player;
	std::unique_ptr<QAudioOutput> _output;
	std::unique_ptr<QVideoSink> _sink;
	QString _source;
	QImage _poster;
	QImage _frame;
	bool _playing = false;
	bool _muted = false;
	bool _looped = false;
	bool _controlsShown = true;
	bool _fullscreenAllowed = true;
	bool _failed = false;
	bool _ended = false;
	int _radius = 0;

	bool _controlsVisible = true;
	Ui::Animations::Simple _controlsOpacity;
	QTimer _controlsTimer;
	Part _pressed = Part::None;
	Part _over = Part::None;
	bool _pressedOnVideo = false;
	double _seekingProgress = -1.;

	bool _fullscreen = false;
	bool _reparenting = false;
	QPointer<QWidget> _normalParent;
	QRect _normalGeometry;
	Qt::WindowFlags _normalFlags;

};

} // namespace Media
