#include "media/media_player_widgets.h"

#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/media_image.h"
#include "ui/layers/box_content.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_media_player.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

#include <cmath>

namespace Media {
namespace {

using namespace Qt::StringLiterals;
using Controller = Window::SessionController;

constexpr auto kPlaylistPreload = 6;
constexpr auto kBarsInterval = 120;

[[nodiscard]] QString Title(Main::Session *session, const AudioTrack &track) {
	if (track.voice && !track.sender.isEmpty() && session) {
		const auto name = Support::MemberName(session, track.sender);
		if (!name.isEmpty()) {
			return name;
		}
	}
	return track.title.isEmpty()
		? (track.voice ? tr::lng_media_voice(tr::now) : tr::lng_media_music(tr::now))
		: track.title;
}

[[nodiscard]] QString Subtitle(const AudioTrack &track) {
	if (track.voice) {
		return track.sender.isEmpty() ? QString() : tr::lng_media_voice(tr::now);
	}
	return track.performer;
}

[[nodiscard]] QString Time(qint64 ms) {
	return Lang::Duration(int(std::max(ms, qint64(0)) / 1000));
}

// A seekable line: a timeline, left to right in both directions.
void PaintSeek(
		QPainter &p,
		const QRectF &line,
		double progress,
		bool knob,
		int knobSize) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::borderSubtle);
	p.drawRoundedRect(line, line.height() / 2., line.height() / 2.);
	const auto done = line.width() * std::clamp(progress, 0., 1.);
	p.setBrush(st::brandPrimary);
	p.drawRoundedRect(
		QRectF(line.x(), line.y(), done, line.height()),
		line.height() / 2.,
		line.height() / 2.);
	if (knob) {
		const auto x = line.x() + done;
		const auto y = line.center().y();
		p.drawEllipse(QPointF(x, y), knobSize / 2., knobSize / 2.);
	}
}

// The bars of the song playing, moving while it plays.
void PaintBars(QPainter &p, const QRect &box, bool playing) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::brandPrimary);
	const auto width = int(st::playerBarsWidth);
	const auto skip = int(st::playerBarsSkip);
	const auto total = 3 * width + 2 * skip;
	const auto left = box.x() + (box.width() - total) / 2;
	const auto bottom = box.y() + box.height() * 3 / 4;
	const auto max = box.height() / 2.;
	const auto now = playing ? (Ui::Now() / 1000.) : 0.;
	for (auto i = 0; i != 3; ++i) {
		const auto phase = std::abs(std::sin(now * 5.5 + i * 1.7));
		const auto height = playing ? (max * (0.3 + 0.7 * phase)) : (max * (0.35 + 0.2 * i));
		p.drawRoundedRect(
			QRectF(left + i * (width + skip), bottom - height, width, height),
			width / 2.,
			width / 2.);
	}
}

// ---- the bar

class Bar final : public Ui::RpWidget {
public:
	Bar(QWidget *parent, Controller *controller);

	void refresh();

protected:
	int resizeGetHeight(int newWidth) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	void layoutButtons();
	[[nodiscard]] QRect textRect() const;
	[[nodiscard]] QRect speedRect() const;
	[[nodiscard]] QRect seekArea() const;
	void seekTo(int x);

	Controller *_controller = nullptr;
	Ui::IconButton *_previous = nullptr;
	Ui::IconButton *_play = nullptr;
	Ui::IconButton *_next = nullptr;
	Ui::IconButton *_playlist = nullptr;
	Ui::IconButton *_close = nullptr;
	bool _overText = false;
	bool _overSeek = false;
	bool _seeking = false;
	int _textLeft = 0;
	int _textRight = 0;

};

Bar::Bar(QWidget *parent, Controller *controller)
: RpWidget(parent)
, _controller(controller)
, _previous(new Ui::IconButton(this, st::playerBarPrevious))
, _play(new Ui::IconButton(this, st::playerBarPlay))
, _next(new Ui::IconButton(this, st::playerBarNext))
, _playlist(new Ui::IconButton(this, st::playerBarPlaylist))
, _close(new Ui::IconButton(this, st::playerBarClose)) {
	setMouseTracking(true);
	// The layout takes the height the bar has.
	resize(width(), st::playerBarHeight);
	auto &player = Player();
	_previous->setClickedCallback([] { Player().previous(); });
	_next->setClickedCallback([] { Player().next(); });
	_play->setClickedCallback([] {
		auto &player = Player();
		if (player.state() == AudioPlayer::State::Playing) {
			player.pause();
		} else {
			player.resume();
		}
	});
	_playlist->setClickedCallback([=] { ShowMusicPlayer(controller); });
	_close->setClickedCallback([] { Player().stop(); });
	connect(&player, &AudioPlayer::stateChanged, this, [this] { refresh(); });
	connect(&player, &AudioPlayer::trackChanged, this, [this] { refresh(); });
	connect(&player, &AudioPlayer::queueChanged, this, [this] { refresh(); });
	connect(&player, &AudioPlayer::speedChanged, this, [this] { update(); });
	connect(&player, &AudioPlayer::positionChanged, this, [this] {
		update(seekArea());
	});
	style::OnPaletteChanged(this, [this] { update(); });
	refresh();
}

int Bar::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::playerBarHeight;
}

void Bar::refresh() {
	const auto &player = Player();
	const auto active = (player.state() != AudioPlayer::State::Stopped);
	const auto &track = player.current();
	const auto playing = (player.state() == AudioPlayer::State::Playing)
		|| (player.state() == AudioPlayer::State::Loading);
	_play->setIconOverride(
		playing ? &st::playerBarPauseIcon : nullptr,
		playing ? &st::playerBarPauseIconOver : nullptr);
	// A song goes round its queue; a voice only on to the next one.
	const auto queue = player.queueSize() > 1;
	_previous->setVisible(queue || !track.voice);
	_next->setVisible(queue || !track.voice);
	_playlist->setVisible(!track.voice && !track.chat.isEmpty());
	// (Never shown before it is in the layout: it would be a window.)
	if (parentWidget() && isHidden() == active) {
		setVisible(active);
	}
	layoutButtons();
	update();
}

void Bar::layoutButtons() {
	const auto &padding = st::playerBarPadding;
	const auto top = (height() - st::playerBarProgressHeight - _play->height()) / 2;
	// The transport buttons keep their order in both directions (a media
	// player's), at the leading side.
	auto left = padding.left();
	const auto place = [&](Ui::IconButton *button) {
		if (!button->isHidden()) {
			button->moveToLeft(left, top, width());
			left += button->width();
		}
	};
	if (style::RightToLeft()) {
		place(_next);
		place(_play);
		place(_previous);
	} else {
		place(_previous);
		place(_play);
		place(_next);
	}
	_textLeft = left + st::playerBarTextSkip;
	auto right = padding.right();
	for (const auto button : { _close, _playlist }) {
		if (!button->isHidden()) {
			button->moveToRight(right, top, width());
			right += button->width();
		}
	}
	if (Player().current().voice) {
		right += st::playerBarSpeedSize.width() + st::playerBarTextSkip;
	}
	_textRight = right + st::playerBarTextSkip;
}

void Bar::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	layoutButtons();
}

QRect Bar::textRect() const {
	const auto top = 0;
	const auto bottom = height() - st::playerBarProgressHeight;
	return style::rtlrect(
		_textLeft,
		top,
		std::max(width() - _textLeft - _textRight, 0),
		bottom - top,
		width());
}

QRect Bar::speedRect() const {
	if (!Player().current().voice) {
		return QRect();
	}
	const auto size = st::playerBarSpeedSize;
	auto right = st::playerBarPadding.right();
	for (const auto button : { _close, _playlist }) {
		if (!button->isHidden()) {
			right += button->width();
		}
	}
	return style::rtlrect(
		width() - right - size.width(),
		(height() - st::playerBarProgressHeight - size.height()) / 2,
		size.width(),
		size.height(),
		width());
}

QRect Bar::seekArea() const {
	return QRect(
		0,
		height() - st::playerBarSeekArea,
		width(),
		st::playerBarSeekArea);
}

void Bar::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::playerBarBg);
	const auto &player = Player();
	const auto &track = player.current();
	const auto session = &_controller->session();

	const auto text = textRect();
	if (text.width() > 0) {
		const auto title = Title(session, track);
		const auto subtitle = Subtitle(track);
		const auto &titleFont = st::playerBarTitleFont;
		const auto &subtitleFont = st::playerBarSubtitleFont;
		const auto separator = u" — "_s;
		const auto titleWidth = std::min(titleFont->width(title), text.width());
		const auto top = text.y() + (text.height() - titleFont->height) / 2;
		p.setFont(titleFont);
		p.setPen(_overText ? st::brandText : st::textPrimary);
		const auto leading = style::RightToLeft()
			? (width() - text.x() - text.width())
			: text.x();
		p.drawTextLeft(leading, top, width(), titleFont->elided(title, text.width()));
		const auto rest = text.width() - titleWidth;
		if (!subtitle.isEmpty() && rest > subtitleFont->width(separator) * 2) {
			p.setFont(subtitleFont);
			p.setPen(st::textSecondary);
			p.drawTextLeft(
				leading + titleWidth,
				top + (titleFont->ascent - subtitleFont->ascent),
				width(),
				subtitleFont->elided(separator + subtitle, rest));
		}
	}

	if (const auto speed = speedRect(); !speed.isEmpty()) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::brandSoft);
		p.drawRoundedRect(speed, speed.height() / 2., speed.height() / 2.);
		p.setFont(st::playerBarSpeedFont);
		p.setPen(st::brandText);
		p.drawText(speed, Qt::AlignCenter, tr::lng_history_speed(
			tr::now,
			lt_value,
			Lang::Num(QString::number(player.speed()))));
	}

	const auto thick = (_overSeek || _seeking)
		? int(st::playerBarProgressOver)
		: int(st::playerBarProgressHeight);
	const auto line = QRectF(0, height() - thick, width(), thick);
	p.fillRect(line, st::borderSubtle);
	const auto progress = (player.state() == AudioPlayer::State::Loading)
		? player.loadProgress()
		: player.progress();
	p.fillRect(
		QRectF(0, line.y(), line.width() * std::clamp(progress, 0., 1.), thick),
		st::brandPrimary);
	p.fillRect(0, height() - thick - st::lineWidth, width(), st::lineWidth, st::chromeHairline);
}

void Bar::seekTo(int x) {
	Player().seek(std::clamp(double(x) / std::max(width(), 1), 0., 1.));
}

void Bar::mouseMoveEvent(QMouseEvent *e) {
	if (_seeking) {
		seekTo(e->pos().x());
		return;
	}
	const auto overSeek = seekArea().contains(e->pos());
	const auto overText = !overSeek && textRect().contains(e->pos());
	const auto overSpeed = speedRect().contains(e->pos());
	setCursor((overSeek || overText || overSpeed)
		? style::cur_pointer
		: style::cur_default);
	if (_overSeek != overSeek || _overText != overText) {
		_overSeek = overSeek;
		_overText = overText;
		update();
	}
}

void Bar::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && seekArea().contains(e->pos())) {
		_seeking = true;
		seekTo(e->pos().x());
		update();
	}
}

void Bar::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	} else if (_seeking) {
		_seeking = false;
		seekTo(e->pos().x());
		update();
		return;
	}
	const auto &track = Player().current();
	if (speedRect().contains(e->pos())) {
		Player().cycleSpeed();
	} else if (textRect().contains(e->pos())) {
		// A song opens the music player, a voice goes to its message.
		if (!track.voice && !track.chat.isEmpty()) {
			ShowMusicPlayer(_controller);
		} else {
			OpenTrackPlace(_controller, track);
		}
	}
}

void Bar::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_overSeek || _overText) {
		_overSeek = _overText = false;
		update();
	}
}

// ---- the music player

class Header final : public Ui::RpWidget {
public:
	Header(QWidget *parent, Controller *controller, Fn<void()> close);

	[[nodiscard]] int naturalHeight() const {
		return st::playerHeaderHeight;
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	enum class Control {
		None,
		Reverse,
		Repeat,
		Previous,
		Play,
		Next,
		Shuffle,
		ShowInChat,
		Seek,
	};
	[[nodiscard]] QRect controlRect(Control control) const;
	[[nodiscard]] Control controlAt(QPoint position) const;
	[[nodiscard]] QRect seekLine() const;
	void press(Control control);
	void refreshCover();

	Controller *_controller = nullptr;
	Fn<void()> _close;
	std::shared_ptr<Ui::ImageSource> _cover;
	QString _coverFor;
	Control _over = Control::None;
	bool _seeking = false;

};

Header::Header(QWidget *parent, Controller *controller, Fn<void()> close)
: RpWidget(parent)
, _controller(controller)
, _close(std::move(close)) {
	setMouseTracking(true);
	auto &player = Player();
	connect(&player, &AudioPlayer::stateChanged, this, [this] { update(); });
	connect(&player, &AudioPlayer::trackChanged, this, [this] {
		refreshCover();
		update();
	});
	connect(&player, &AudioPlayer::queueChanged, this, [this] { update(); });
	connect(&player, &AudioPlayer::positionChanged, this, [this] { update(); });
	style::OnPaletteChanged(this, [this] { update(); });
	refreshCover();
}

void Header::refreshCover() {
	const auto &track = Player().current();
	if (_coverFor == track.id) {
		return;
	}
	_coverFor = track.id;
	_cover = nullptr;
	if (track.ref.type == Data::MediaRef::Type::Attachment) {
		_cover = Support::MakeImageSource(
			&_controller->session(),
			track.ref,
			Data::MediaVariant::Cover);
		if (_cover) {
			connect(_cover.get(), &Ui::ImageSource::updated, this, [this] {
				update();
			});
		}
	}
}

int Header::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return st::playerHeaderHeight;
}

QRect Header::seekLine() const {
	const auto &padding = st::playerHeaderPadding;
	return QRect(
		padding.left(),
		st::playerSeekTop,
		width() - padding.left() - padding.right(),
		st::playerSeekHeight);
}

QRect Header::controlRect(Control control) const {
	if (control == Control::Seek) {
		const auto line = seekLine();
		return line.marginsAdded(QMargins(
			0,
			(st::playerSeekArea - line.height()) / 2,
			0,
			(st::playerSeekArea - line.height()) / 2));
	}
	// One row, the same order in both directions (a media player's).
	static const auto kRow = {
		Control::Reverse,
		Control::Repeat,
		Control::Previous,
		Control::Play,
		Control::Next,
		Control::Shuffle,
		Control::ShowInChat,
	};
	const auto size = int(st::playerControlSize);
	const auto play = int(st::playerPlaySize);
	const auto skip = int(st::playerControlSkip);
	const auto total = 6 * size + play + 6 * skip;
	auto left = (width() - total) / 2;
	const auto middle = st::playerControlsTop + st::playerControlsHeight / 2;
	for (const auto entry : kRow) {
		const auto side = (entry == Control::Play) ? play : size;
		if (entry == control) {
			return QRect(left, middle - side / 2, side, side);
		}
		left += side + skip;
	}
	return QRect();
}

Header::Control Header::controlAt(QPoint position) const {
	for (const auto control : {
			Control::Seek,
			Control::Reverse,
			Control::Repeat,
			Control::Previous,
			Control::Play,
			Control::Next,
			Control::Shuffle,
			Control::ShowInChat }) {
		if (controlRect(control).contains(position)) {
			if (control == Control::ShowInChat
				&& Player().current().chat.isEmpty()) {
				continue;
			}
			return control;
		}
	}
	return Control::None;
}

void Header::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto &player = Player();
	const auto &track = player.current();
	const auto &padding = st::playerHeaderPadding;
	const auto session = &_controller->session();

	// The cover and the names.
	const auto size = int(st::playerCoverSize);
	const auto cover = style::rtlrect(padding.left(), padding.top(), size, size, width());
	{
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		path.addRoundedRect(cover, st::playerCoverRadius, st::playerCoverRadius);
		p.save();
		p.setClipPath(path);
		p.fillRect(cover, st::playerCoverBg);
		const auto image = _cover
			? _cover->request(cover.size() * devicePixelRatioF())
			: QImage();
		if (!image.isNull()) {
			p.drawImage(QRectF(cover), image);
		} else {
			st::playerCoverIcon.paintInCenter(p, cover);
		}
		p.restore();
	}
	const auto textLeft = padding.left() + size + st::playerCoverSkip;
	const auto textWidth = std::max(width() - textLeft - padding.right(), 1);
	p.setFont(st::playerTitleFont);
	p.setPen(st::textPrimary);
	p.drawTextLeft(
		textLeft,
		padding.top() + 14,
		width(),
		st::playerTitleFont->elided(Title(session, track), textWidth));
	p.setFont(st::playerPerformerFont);
	p.setPen(st::textSecondary);
	p.drawTextLeft(
		textLeft,
		padding.top() + 14 + st::playerTitleFont->height + 4,
		width(),
		st::playerPerformerFont->elided(Subtitle(track), textWidth));

	// The timeline: elapsed on the left, what is left on the right.
	const auto line = seekLine();
	const auto loading = (player.state() == AudioPlayer::State::Loading);
	PaintSeek(
		p,
		QRectF(line),
		loading ? player.loadProgress() : player.progress(),
		!loading,
		st::playerSeekKnob);
	const auto duration = player.durationMs();
	const auto position = player.positionMs();
	p.setFont(st::playerTimeFont);
	p.setPen(st::textTertiary);
	const auto elapsed = Time(position);
	// A timeline: left to right in both directions.
	const auto left = Lang::Ltr(
		u"-"_s + Time(std::max(duration - position, qint64(0))));
	p.drawText(line.x(), st::playerTimeTop + st::playerTimeFont->ascent, elapsed);
	p.drawText(
		line.x() + line.width() - st::playerTimeFont->width(left),
		st::playerTimeTop + st::playerTimeFont->ascent,
		left);

	// The controls.
	auto hq = PainterHighQualityEnabler(p);
	const auto paintControl = [&](Control control, const style::icon &icon) {
		const auto rect = controlRect(control);
		if (_over == control) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::playerControlOver);
			p.drawEllipse(rect);
		}
		icon.paintInCenter(p, rect);
	};
	paintControl(
		Control::Reverse,
		player.reversed() ? st::playerReverseOnIcon : st::playerReverseIcon);
	paintControl(
		Control::Repeat,
		(player.repeat() == RepeatMode::One)
			? st::playerRepeatOneIcon
			: (player.repeat() == RepeatMode::All)
			? st::playerRepeatOnIcon
			: st::playerRepeatIcon);
	paintControl(Control::Previous, st::playerPreviousIcon);
	{
		const auto rect = controlRect(Control::Play);
		p.setPen(Qt::NoPen);
		p.setBrush((_over == Control::Play) ? st::brandPressed : st::brandPrimary);
		p.drawEllipse(rect);
		const auto playing = (player.state() == AudioPlayer::State::Playing)
			|| loading;
		(playing ? st::playerPauseIcon : st::playerPlayIcon).paintInCenter(p, rect);
	}
	paintControl(Control::Next, st::playerNextIcon);
	paintControl(
		Control::Shuffle,
		player.shuffled() ? st::playerShuffleOnIcon : st::playerShuffleIcon);
	if (!track.chat.isEmpty()) {
		paintControl(Control::ShowInChat, st::playerShowInChatIcon);
	}
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::borderSubtle);
}

void Header::press(Control control) {
	auto &player = Player();
	switch (control) {
	case Control::Reverse: player.setReversed(!player.reversed()); break;
	case Control::Repeat: player.setRepeat(NextRepeatMode(player.repeat())); break;
	case Control::Previous: player.previous(); break;
	case Control::Play:
		if (player.state() == AudioPlayer::State::Playing) {
			player.pause();
		} else {
			player.resume();
		}
		break;
	case Control::Next: player.next(); break;
	case Control::Shuffle: player.setShuffled(!player.shuffled()); break;
	case Control::ShowInChat: {
		const auto track = player.current();
		const auto controller = _controller;
		const auto close = _close;
		// The box goes first: it would cover the chat.
		if (close) {
			close();
		}
		OpenTrackPlace(controller, track);
	} break;
	case Control::Seek:
	case Control::None: break;
	}
	update();
}

void Header::mouseMoveEvent(QMouseEvent *e) {
	if (_seeking) {
		const auto line = seekLine();
		Player().seek(double(e->pos().x() - line.x()) / std::max(line.width(), 1));
		return;
	}
	const auto over = controlAt(e->pos());
	setCursor((over != Control::None) ? style::cur_pointer : style::cur_default);
	if (_over != over) {
		_over = over;
		update();
	}
}

void Header::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && controlAt(e->pos()) == Control::Seek) {
		_seeking = true;
		const auto line = seekLine();
		Player().seek(double(e->pos().x() - line.x()) / std::max(line.width(), 1));
	}
}

void Header::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	} else if (_seeking) {
		_seeking = false;
		return;
	}
	const auto control = controlAt(e->pos());
	if (control != Control::None && control == _over) {
		press(control);
	}
}

void Header::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over != Control::None) {
		_over = Control::None;
		update();
	}
}

class Playlist final : public Ui::RpWidget {
public:
	Playlist(QWidget *parent, Controller *controller);

	void scrollToCurrent(Fn<void(int top, int bottom)> scroll);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	[[nodiscard]] int rowAt(QPoint position) const;
	void changed();

	Controller *_controller = nullptr;
	QTimer _bars;
	int _over = -1;
	int _visibleBottom = 0;

};

Playlist::Playlist(QWidget *parent, Controller *controller)
: RpWidget(parent)
, _controller(controller) {
	setMouseTracking(true);
	auto &player = Player();
	connect(&player, &AudioPlayer::queueChanged, this, [this] { changed(); });
	connect(&player, &AudioPlayer::trackChanged, this, [this] { update(); });
	connect(&player, &AudioPlayer::stateChanged, this, [this] {
		if (Player().state() == AudioPlayer::State::Playing
			&& !Ui::AnimationsDisabled()) {
			_bars.start();
		} else {
			_bars.stop();
		}
		update();
	});
	_bars.setInterval(kBarsInterval);
	connect(&_bars, &QTimer::timeout, this, [this] {
		const auto position = Player().queuePosition();
		update(0, position * st::playerRowHeight, width(), st::playerRowHeight);
	});
	if (player.state() == AudioPlayer::State::Playing && !Ui::AnimationsDisabled()) {
		_bars.start();
	}
	style::OnPaletteChanged(this, [this] { update(); });
}

void Playlist::changed() {
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

int Playlist::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	const auto &player = Player();
	return player.queueSize() * st::playerRowHeight
		+ ((player.queueLoading() || player.queueHasMore())
			? int(st::playerListStateHeight)
			: 0);
}

void Playlist::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	Q_UNUSED(visibleTop);
	_visibleBottom = visibleBottom;
	auto &player = Player();
	// Older songs page in as the end of the list comes into view.
	if (player.queueHasMore()
		&& !player.queueLoading()
		&& _visibleBottom + kPlaylistPreload * st::playerRowHeight >= height()) {
		player.loadMoreQueue();
	}
}

void Playlist::scrollToCurrent(Fn<void(int top, int bottom)> scroll) {
	const auto position = Player().queuePosition();
	if (position >= 0 && scroll) {
		scroll(position * st::playerRowHeight, (position + 1) * st::playerRowHeight);
	}
}

int Playlist::rowAt(QPoint position) const {
	const auto row = position.y() / st::playerRowHeight;
	return (position.y() >= 0 && row < Player().queueSize()) ? row : -1;
}

void Playlist::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto &player = Player();
	const auto count = player.queueSize();
	const auto current = player.queuePosition();
	const auto &padding = st::playerRowPadding;
	const auto from = std::max(clip.top() / st::playerRowHeight, 0);
	const auto till = std::min(clip.bottom() / st::playerRowHeight + 1, count);
	const auto marker = int(st::playerRowMarker);
	const auto textLeft = padding.left() + marker + st::playerRowMarkerSkip;
	const auto textWidth = std::max(width() - textLeft - padding.right(), 1);
	for (auto i = from; i < till; ++i) {
		const auto &track = player.queueTrack(i);
		const auto top = i * st::playerRowHeight;
		const auto row = QRect(0, top, width(), st::playerRowHeight);
		if (i == current) {
			p.fillRect(row, st::stateSelected);
		} else if (i == _over) {
			p.fillRect(row, st::stateHover);
		}
		const auto box = style::rtlrect(
			padding.left(),
			top + (st::playerRowHeight - marker) / 2,
			marker,
			marker,
			width());
		if (i == current) {
			PaintBars(p, box, player.state() == AudioPlayer::State::Playing);
		} else {
			p.setFont(st::playerRowIndexFont);
			p.setPen(st::textTertiary);
			p.drawText(box, Qt::AlignCenter, Lang::Num(qint64(i + 1)));
		}
		const auto title = track.title.isEmpty()
			? tr::lng_media_music(tr::now)
			: track.title;
		auto status = track.performer;
		if (track.durationMs > 0) {
			status = status.isEmpty()
				? Time(track.durationMs)
				: (Lang::Isolated(status) + u" · "_s + Time(track.durationMs));
		}
		p.setFont(st::playerRowTitleFont);
		p.setPen((i == current) ? st::brandText : st::textPrimary);
		p.drawTextLeft(
			textLeft,
			top + st::playerRowTitleTop,
			width(),
			st::playerRowTitleFont->elided(title, textWidth));
		p.setFont(st::playerRowStatusFont);
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			textLeft,
			top + st::playerRowStatusTop,
			width(),
			st::playerRowStatusFont->elided(status, textWidth));
	}
	if (player.queueLoading() || player.queueHasMore()) {
		const auto state = QRect(
			0,
			count * st::playerRowHeight,
			width(),
			st::playerListStateHeight);
		if (state.intersects(clip)) {
			p.setFont(st::playerRowStatusFont);
			p.setPen(st::textTertiary);
			p.drawText(state, Qt::AlignCenter, tr::lng_loading(tr::now));
		}
	}
}

void Playlist::mouseMoveEvent(QMouseEvent *e) {
	const auto row = rowAt(e->pos());
	setCursor((row >= 0) ? style::cur_pointer : style::cur_default);
	if (_over != row) {
		_over = row;
		update();
	}
}

void Playlist::mouseReleaseEvent(QMouseEvent *e) {
	const auto row = rowAt(e->pos());
	if (e->button() == Qt::LeftButton && row >= 0) {
		auto &player = Player();
		if (row == player.queuePosition()) {
			if (player.state() == AudioPlayer::State::Playing) {
				player.pause();
			} else {
				player.resume();
			}
		} else {
			player.playAt(row);
		}
	}
}

void Playlist::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

class PlayerBox final : public Ui::BoxContent {
public:
	PlayerBox(QWidget*, Controller *controller)
	: _controller(controller) {
	}

protected:
	void prepare() override {
		setTitle(tr::lng_media_player_title(tr::now));
		const auto weak = QPointer<PlayerBox>(this);
		_header = new Header(this, _controller, [=] {
			if (weak) {
				weak->closeBox();
			}
		});
		const auto list = setInnerWidget(
			Ui::object_ptr<Playlist>(this, _controller),
			_header->naturalHeight());
		addButton(tr::lng_close(tr::now), [=] { closeBox(); });
		setDimensions(st::playerBoxWidth, st::playerBoxHeight);
		connect(&Player(), &AudioPlayer::stateChanged, this, [=] {
			// Nothing plays any more: nothing to show.
			if (Player().state() == AudioPlayer::State::Stopped && weak) {
				Ui::PostponeCall(weak.data(), [=] {
					if (weak && Player().state() == AudioPlayer::State::Stopped) {
						weak->closeBox();
					}
				});
			}
		});
		Ui::PostponeCall(this, [=] {
			if (weak && weak->scrollArea()) {
				list->scrollToCurrent([=](int top, int bottom) {
					if (weak && weak->scrollArea()) {
						weak->scrollArea()->scrollToY(top, bottom);
					}
				});
			}
		});
	}

	void resizeEvent(QResizeEvent *e) override {
		BoxContent::resizeEvent(e);
		if (_header) {
			_header->setGeometry(0, 0, width(), _header->naturalHeight());
			_header->raise();
		}
	}

private:
	Controller *_controller = nullptr;
	Header *_header = nullptr;

};

} // namespace

void ShowMusicPlayer(Window::SessionController *controller) {
	if (Player().state() == AudioPlayer::State::Stopped) {
		return;
	}
	controller->show(Ui::Box<PlayerBox>(controller));
}

void OpenTrackPlace(
		Window::SessionController *controller,
		const AudioTrack &track) {
	if (!track.chat.isEmpty() && track.seq > 0) {
		// A chat open further back comes to the front (with everything it
		// had over it kept behind) instead of being opened once more.
		controller->showExistingSection(Window::Routes::Chat(track.chat));
		controller->openRoute(Window::Routes::Chat(track.chat, track.seq));
	} else if (const auto post = track.postId(); !post.isEmpty()) {
		controller->openRoute(Window::Routes::Post(post));
	}
}

void SetupNowPlaying(Window::SessionController *controller) {
	const auto bar = new Bar(nullptr, controller);
	bar->hide();
	controller->setMainStrip(bar);
	bar->refresh();
	// The toasts of failures go to the window that shows the bar.
	QObject::connect(
		&Player(),
		&AudioPlayer::failed,
		bar,
		[=](const QString &id, AudioFailure reason) {
			Q_UNUSED(id);
			controller->showToast((reason == AudioFailure::Gone)
				? tr::lng_chat_file_isnt_available(tr::now)
				: (reason == AudioFailure::Download)
				? tr::lng_media_player_download_failed(tr::now)
				: tr::lng_media_player_play_failed(tr::now));
		});
}

} // namespace Media
