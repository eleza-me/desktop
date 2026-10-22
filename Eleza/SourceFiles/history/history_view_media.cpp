#include "history/history_view_media.h"

#include "history/history_album.h"

#include "data/data_calls.h"

#include "base/platform_file.h"
#include "data/data_feed.h"
#include "data/data_image_decode.h"
#include "data/data_session.h"
#include "data/data_social_types.h"
#include "data/data_stickers.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/media_image.h"
#include "support/sticker_player.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtGui/QPainterPath>

#include <cmath>

namespace History::View {
namespace {

constexpr auto kLocalImageSide = 1280;

[[nodiscard]] QPainterPath RoundedPath(const QRectF &rect, const int radii[4]) {
	auto path = QPainterPath();
	const auto tl = double(radii[0]);
	const auto tr = double(radii[1]);
	const auto br = double(radii[2]);
	const auto bl = double(radii[3]);
	path.moveTo(rect.left() + tl, rect.top());
	path.lineTo(rect.right() - tr, rect.top());
	if (tr > 0.) {
		path.arcTo(rect.right() - 2 * tr, rect.top(), 2 * tr, 2 * tr, 90, -90);
	}
	path.lineTo(rect.right(), rect.bottom() - br);
	if (br > 0.) {
		path.arcTo(rect.right() - 2 * br, rect.bottom() - 2 * br, 2 * br, 2 * br, 0, -90);
	}
	path.lineTo(rect.left() + bl, rect.bottom());
	if (bl > 0.) {
		path.arcTo(rect.left(), rect.bottom() - 2 * bl, 2 * bl, 2 * bl, 270, -90);
	}
	path.lineTo(rect.left(), rect.top() + tl);
	if (tl > 0.) {
		path.arcTo(rect.left(), rect.top(), 2 * tl, 2 * tl, 180, -90);
	}
	path.closeSubpath();
	return path;
}

// One elided line inside [left, left + width), hugging the leading side of
// the interface (or the given one).
void DrawLine(
		QPainter &p,
		const style::font &font,
		const QString &text,
		int left,
		int top,
		int width,
		Qt::TextElideMode mode = Qt::ElideRight) {
	if (width <= 0 || text.isEmpty()) {
		return;
	}
	const auto full = font->width(text);
	const auto shown = (full > width) ? font->elided(text, width, mode) : text;
	const auto shownWidth = (full > width) ? font->width(shown) : full;
	p.setFont(font);
	p.drawText(
		style::RightToLeft() ? (left + width - shownWidth) : left,
		top + font->ascent,
		shown);
}

void PaintBadge(
		QPainter &p,
		const QString &text,
		QPoint topLeft) {
	const auto &font = st::historyMediaBadgeFont;
	const auto &padding = st::historyMediaBadgePadding;
	const auto width = font->width(text) + padding.left() + padding.right();
	const auto height = font->height + padding.top() + padding.bottom();
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::historyMetaOverBg);
	p.drawRoundedRect(
		QRectF(topLeft.x(), topLeft.y(), width, height),
		height / 2.,
		height / 2.);
	p.setPen(st::historyMetaOverFg);
	p.setFont(font);
	p.drawText(
		topLeft.x() + padding.left(),
		topLeft.y() + padding.top() + font->ascent,
		text);
}

void PaintGlassCircle(QPainter &p, const QRect &circle, const style::icon &icon) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::historyMediaPlayBg);
	p.drawEllipse(circle);
	icon.paintInCenter(p, circle);
}

class LocalImageSource final : public Ui::ImageSource {
public:
	LocalImageSource(const QString &path, int maxSide)
	: _path(path)
	, _maxSide(maxSide) {
	}

	QImage request(QSize size) override {
		Q_UNUSED(size);
		if (!_started) {
			_started = true;
			const auto weak = QPointer<LocalImageSource>(this);
			const auto path = _path;
			const auto side = _maxSide;
			QThreadPool::globalInstance()->start([=] {
				// A file of the user's, or one a peer sent: the safe decoder.
				const auto image = Data::DecodeImageFile(path, side);
				QMetaObject::invokeMethod(qApp, [=] {
					if (weak) {
						weak->done(image);
					}
				}, Qt::QueuedConnection);
			});
		}
		return _image;
	}

	bool failed() const override {
		return _failed;
	}

private:
	void done(const QImage &image) {
		_image = image;
		_failed = image.isNull();
		emit updated();
	}

	QString _path;
	int _maxSide = 0;
	QImage _image;
	bool _started = false;
	bool _failed = false;

};

[[nodiscard]] bool Uploading(const Data::Message &message) {
	return message.local()
		&& message.status != Data::MessageStatus::Failed
		&& (message.uploadProgress >= 0.
			|| (!message.localPath.isEmpty() && message.attachment.isEmpty()));
}

// ---- photo / video / gif

class PhotoMedia final : public Media {
public:
	PhotoMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		const auto &message = data();
		const auto limit = std::min(
			maxWidth - 2 * st::historyMediaPadding,
			int(st::historyMediaMaxWidth));
		auto size = MediaBoxSize(
			message.media ? message.media->width : 0,
			message.media ? message.media->height : 0,
			message.kind,
			limit);
		if (hasCaption) {
			// A narrow picture under a caption is cropped a little wider so
			// the caption has room.
			size.setWidth(std::max(
				size.width(),
				std::min(int(st::historyMediaCaptionMinWidth), limit)));
		}
		return size;
	}

	bool fullBleed() const override {
		return true;
	}

	std::shared_ptr<Ui::ImageSource> imageSource() const override {
		return _source;
	}

	QImage currentImage() const override {
		return _last;
	}

	void unload() override {
		_source = nullptr;
		_prepared = PreparedImage();
		_last = QImage();
		_sourcePath = QString();
		_sourceSeq = -1;
	}

	void dataChanged() override {
		// A sent message keeps the local picture it already shows.
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &message = data();
		ensureSource();
		const auto ratio = delegate()->elementRatio();
		auto image = QImage();
		if (_source) {
			image = _source->request(rect.size() * ratio);
			if (image.isNull()) {
				image = _source->preview();
			}
		}
		const auto hidden = message.mediaSpoiler
			&& !delegate()->elementRevealed(message.key());
		if (image.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			p.fillPath(RoundedPath(QRectF(rect), context.radii), st::historyMediaBg);
		} else {
			_last = image;
			PaintCover(p, rect, image, context.radii, hidden, _prepared);
		}
		if (context.selected) {
			auto hq = PainterHighQualityEnabler(p);
			p.fillPath(
				RoundedPath(QRectF(rect), context.radii),
				st::historySelectedBg);
		}
		const auto video = (message.kind == Data::MessageKind::Video);
		const auto gif = (message.kind == Data::MessageKind::Gif);
		const auto center = QRect(
			rect.x() + (rect.width() - st::historyMediaPlaySize) / 2,
			rect.y() + (rect.height() - st::historyMediaPlaySize) / 2,
			st::historyMediaPlaySize,
			st::historyMediaPlaySize);
		const auto download = delegate()->elementDownload(message);
		if (Uploading(message)) {
			PaintProgressRing(
				p,
				center,
				std::max(message.uploadProgress, 0.),
				st::historyMediaCancelIcon,
				st::historyMediaPlayBg->c,
				st::staticWhite->c);
		} else if (hidden) {
			PaintGlassCircle(p, center, st::historyMediaSpoilerIcon);
		} else if (download.loading) {
			PaintProgressRing(
				p,
				center,
				download.progress,
				st::historyMediaCancelIcon,
				st::historyMediaPlayBg->c,
				st::staticWhite->c);
		} else if (video) {
			PaintGlassCircle(p, center, st::historyMediaPlayIcon);
		}
		const auto badgeTop = rect.y() + st::historyMediaBadgeMargin;
		const auto badgeLeft = rect.x() + st::historyMediaBadgeMargin;
		if (hidden) {
			// Nothing gives the content away.
		} else if (video && message.media && message.media->durationMs > 0) {
			PaintBadge(
				p,
				MediaDurationText(message.media->durationMs),
				QPoint(badgeLeft, badgeTop));
		} else if (gif) {
			PaintBadge(p, tr::lng_history_gif(tr::now), QPoint(badgeLeft, badgeTop));
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (!rect.contains(point)) {
			return result;
		}
		const auto &message = data();
		const auto center = QRect(
			rect.x() + (rect.width() - st::historyMediaPlaySize) / 2,
			rect.y() + (rect.height() - st::historyMediaPlaySize) / 2,
			st::historyMediaPlaySize,
			st::historyMediaPlaySize);
		if (Uploading(message)
			|| delegate()->elementDownload(message).loading) {
			result.zone = center.contains(point) ? Zone::MediaCancel : Zone::Bubble;
		} else if (message.mediaSpoiler
			&& !delegate()->elementRevealed(message.key())) {
			result.zone = Zone::MediaSpoiler;
		} else {
			result.zone = Zone::Media;
		}
		return result;
	}

private:
	void ensureSource() {
		const auto &message = data();
		const auto local = !message.localPath.isEmpty()
			&& (message.kind == Data::MessageKind::Photo)
			&& QFileInfo::exists(message.localPath);
		if (_source
			&& ((local && _sourcePath == message.localPath)
				|| (!local && _sourceSeq == message.seq))) {
			return;
		}
		if (local) {
			_source = MakeLocalImageSource(message.localPath, kLocalImageSide);
			_sourcePath = message.localPath;
			_sourceSeq = -1;
		} else if (message.seq > 0) {
			_source = Support::MakeMessageImageSource(
				delegate()->elementSession(),
				message,
				Data::MediaVariant::Feed);
			_sourcePath = QString();
			_sourceSeq = message.seq;
		} else {
			_source = nullptr;
			return;
		}
		_prepared = PreparedImage();
		watch(_source);
	}

	std::shared_ptr<Ui::ImageSource> _source;
	QString _sourcePath;
	qint64 _sourceSeq = -1;
	PreparedImage _prepared;
	QImage _last;

};

// ---- round video

class VideoNoteMedia final : public Media {
public:
	VideoNoteMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto side = std::min(int(st::historyVideoNoteSize), maxWidth);
		return QSize(side, side);
	}

	bool bubbleless() const override {
		return true;
	}

	std::shared_ptr<Ui::ImageSource> imageSource() const override {
		return _source;
	}

	QImage currentImage() const override {
		return _last;
	}

	void unload() override {
		_source = nullptr;
		_prepared = PreparedImage();
		_last = QImage();
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &message = data();
		if (!_source && message.seq > 0) {
			_source = Support::MakeMessageImageSource(
				delegate()->elementSession(),
				message,
				Data::MediaVariant::Feed);
			watch(_source);
		}
		const auto ratio = delegate()->elementRatio();
		auto image = QImage();
		if (_source) {
			image = _source->request(rect.size() * ratio);
			if (image.isNull()) {
				image = _source->preview();
			}
		}
		const auto radius = rect.width() / 2;
		const int radii[4] = { radius, radius, radius, radius };
		auto hq = PainterHighQualityEnabler(p);
		if (image.isNull()) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyMediaBg);
			p.drawEllipse(rect);
		} else {
			_last = image;
			PaintCover(p, rect, image, radii, false, _prepared);
		}
		if (context.selected) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::historySelectedBg);
			p.drawEllipse(rect);
		}
		const auto center = QRect(
			rect.x() + (rect.width() - st::historyMediaPlaySize) / 2,
			rect.y() + (rect.height() - st::historyMediaPlaySize) / 2,
			st::historyMediaPlaySize,
			st::historyMediaPlaySize);
		const auto download = delegate()->elementDownload(message);
		if (Uploading(message)) {
			PaintProgressRing(
				p,
				center,
				std::max(message.uploadProgress, 0.),
				st::historyMediaCancelIcon,
				st::historyMediaPlayBg->c,
				st::staticWhite->c);
		} else if (download.loading) {
			PaintProgressRing(
				p,
				center,
				download.progress,
				st::historyMediaCancelIcon,
				st::historyMediaPlayBg->c,
				st::staticWhite->c);
		} else {
			PaintGlassCircle(p, center, st::historyMediaPlayIcon);
		}
		if (message.media && message.media->durationMs > 0) {
			const auto text = MediaDurationText(message.media->durationMs);
			const auto &font = st::historyMediaBadgeFont;
			const auto &padding = st::historyMediaBadgePadding;
			const auto width = font->width(text) + padding.left() + padding.right();
			// Under the circle's left side, where the pill does not cover it.
			PaintBadge(
				p,
				text,
				QPoint(
					context.outgoing ? rect.x() : (rect.right() + 1 - width),
					rect.y()));
		}
		if (!message.mine && !message.played && message.seq > 0) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::brandPrimary);
			const auto dot = st::historyVoiceDot + 2;
			p.drawEllipse(
				context.outgoing ? rect.x() : (rect.right() - dot),
				rect.bottom() - dot - 22,
				dot,
				dot);
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		const auto center = QPointF(rect.center());
		const auto radius = rect.width() / 2.;
		if (std::hypot(point.x() - center.x(), point.y() - center.y()) > radius) {
			return result;
		}
		const auto &message = data();
		const auto ring = QRect(
			rect.x() + (rect.width() - st::historyMediaPlaySize) / 2,
			rect.y() + (rect.height() - st::historyMediaPlaySize) / 2,
			st::historyMediaPlaySize,
			st::historyMediaPlaySize);
		if (Uploading(message) || delegate()->elementDownload(message).loading) {
			result.zone = ring.contains(point) ? Zone::MediaCancel : Zone::Bubble;
		} else {
			result.zone = Zone::Media;
		}
		return result;
	}

private:
	std::shared_ptr<Ui::ImageSource> _source;
	PreparedImage _prepared;
	QImage _last;

};

// ---- voice and music

class VoiceMedia final : public Media {
public:
	VoiceMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto &message = data();
		const auto music = (message.kind == Data::MessageKind::Audio);
		auto width = int(st::historyVoiceWidth);
		if (music) {
			const auto title = titleText();
			const auto performer = performerText();
			width = st::historyVoicePlaySize
				+ st::historyVoiceSkip
				+ std::max({
					st::historyAudioTitleFont->width(title),
					st::historyAudioPerformerFont->width(performer),
					150 });
		}
		width = std::clamp(width, 160, std::max(maxWidth, 160));
		return QSize(width, music ? 54 : int(st::historyVoicePlaySize));
	}

	bool hasMetaCorner() const override {
		return true;
	}

	void unload() override {
		_bars.clear();
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &message = data();
		const auto &colors = *context.colors;
		const auto music = (message.kind == Data::MessageKind::Audio);
		const auto rtl = style::RightToLeft();
		const auto playback = delegate()->elementPlayback(message);
		const auto accent = context.outgoing ? colors.accentOut : colors.accentIn;
		const auto dim = context.outgoing ? colors.metaOut : colors.metaIn;
		const auto circle = playRect(rect);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(accent);
			p.drawEllipse(circle);
		}
		if (Uploading(message)) {
			PaintProgressRing(
				p,
				circle,
				std::max(message.uploadProgress, 0.),
				st::historyFileCancelIcon,
				accent,
				st::textOnBrand->c);
		} else if (playback.loading && playback.loadProgress > 0.) {
			// How much of the file is here; a click cancels.
			PaintProgressRing(
				p,
				circle,
				playback.loadProgress,
				st::historyFileCancelIcon,
				accent,
				st::textOnBrand->c);
		} else if (playback.loading) {
			auto hq = PainterHighQualityEnabler(p);
			Ui::InfiniteRadialAnimation::DrawFrame(
				p,
				circle.marginsRemoved(QMargins(4, 4, 4, 4)),
				2.,
				st::textOnBrand->c,
				Ui::Now());
			st::historyVoicePauseIcon.paintInCenter(p, circle);
		} else {
			(playback.playing
				? st::historyVoicePauseIcon
				: st::historyVoicePlayIcon).paintInCenter(p, circle);
		}
		const auto bodyLeft = rtl
			? rect.x()
			: (circle.right() + 1 + st::historyVoiceSkip);
		const auto bodyWidth = rect.width()
			- st::historyVoicePlaySize
			- st::historyVoiceSkip;
		const auto duration = message.media ? message.media->durationMs : 0;
		auto timeText = playback.current
			? (MediaDurationText(playback.positionMs)
				+ u" / "_s
				+ MediaDurationText(duration))
			: MediaDurationText(duration);
		if (music && !playback.current && message.media && message.media->size > 0) {
			// The length and the size before it plays.
			timeText = Lang::Isolated(timeText)
				+ u" · "_s
				+ Lang::Isolated(Lang::Bytes(message.media->size));
		}
		if (music) {
			p.setPen(context.outgoing ? colors.textOut : colors.textIn);
			DrawLine(
				p,
				st::historyAudioTitleFont,
				titleText(),
				bodyLeft,
				rect.y(),
				bodyWidth);
			p.setPen(dim);
			DrawLine(
				p,
				st::historyAudioPerformerFont,
				performerText(),
				bodyLeft,
				rect.y() + st::historyAudioTitleFont->height,
				bodyWidth);
			// The seek line appears while the track is the current one.
			const auto lineTop = rect.y() + 40;
			if (playback.current) {
				auto hq = PainterHighQualityEnabler(p);
				const auto line = QRectF(
					bodyLeft,
					lineTop,
					bodyWidth,
					st::historyAudioSeekHeight);
				p.setPen(Qt::NoPen);
				auto faded = dim;
				faded.setAlphaF(faded.alphaF() * 0.35f);
				p.setBrush(faded);
				p.drawRoundedRect(line, line.height() / 2., line.height() / 2.);
				p.setBrush(accent);
				const auto done = line.width() * std::clamp(playback.progress, 0., 1.);
				p.drawRoundedRect(
					QRectF(line.x(), line.y(), done, line.height()),
					line.height() / 2.,
					line.height() / 2.);
				// The knob to drag.
				const auto knob = st::historyAudioSeekKnob / 2.;
				p.drawEllipse(
					QPointF(line.x() + done, line.y() + line.height() / 2.),
					knob,
					knob);
			}
			p.setPen(dim);
			p.setFont(st::historyVoiceTimeFont);
			const auto timeWidth = st::historyVoiceTimeFont->width(timeText);
			p.drawText(
				rtl ? (bodyLeft + bodyWidth - timeWidth) : bodyLeft,
				rect.y() + 40 + st::historyVoiceTimeFont->ascent
					+ (playback.current ? 5 : -2),
				timeText);
			return;
		}
		if (_bars.empty()) {
			_bars = WaveformBars(
				message.media ? message.media->waveform : QByteArray(),
				st::historyVoiceBars);
		}
		// The waveform is a timeline: always left to right.
		const auto wave = waveRect(rect);
		const auto step = st::historyVoiceBarWidth + st::historyVoiceBarSkip;
		const auto count = std::min(
			int(_bars.size()),
			int(std::floor(wave.width() / step)));
		const auto played = playback.current
			? int(std::lround(playback.progress * count))
			: 0;
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			auto inactive = dim;
			inactive.setAlphaF(inactive.alphaF() * 0.55f);
			for (auto i = 0; i != count; ++i) {
				const auto height = std::max(
					3.,
					double(_bars[i]) * st::historyVoiceWaveHeight);
				p.setBrush((i < played) ? accent : inactive);
				p.drawRoundedRect(
					QRectF(
						wave.x() + i * step,
						wave.y() + (wave.height() - height) / 2.,
						st::historyVoiceBarWidth,
						height),
					st::historyVoiceBarWidth / 2.,
					st::historyVoiceBarWidth / 2.);
			}
		}
		const auto timeTop = wave.bottom() + 1 + st::historyVoiceTimeTop - 2;
		p.setPen(dim);
		p.setFont(st::historyVoiceTimeFont);
		const auto timeWidth = st::historyVoiceTimeFont->width(timeText);
		const auto timeLeft = rtl
			? (bodyLeft + bodyWidth - timeWidth)
			: bodyLeft;
		p.drawText(timeLeft, timeTop + st::historyVoiceTimeFont->ascent, timeText);
		if (!message.mine && !message.played && message.seq > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(accent);
			const auto dot = st::historyVoiceDot;
			p.drawEllipse(
				rtl ? (timeLeft - dot - 5) : (timeLeft + timeWidth + 5),
				timeTop + (st::historyVoiceTimeFont->height - dot) / 2,
				dot,
				dot);
		}
		if (playback.current) {
			const auto chip = speedRect(rect);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(context.outgoing ? colors.tintOut : colors.tintIn);
			p.drawRoundedRect(chip, chip.height() / 2., chip.height() / 2.);
			p.setPen(accent);
			p.setFont(st::historyVoiceSpeedFont);
			const auto text = tr::lng_history_speed(
				tr::now,
				lt_value,
				Lang::Num(QString::number(playback.speed)));
			p.drawText(chip, Qt::AlignCenter, text);
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (!rect.contains(point)) {
			return result;
		}
		result.zone = Zone::Bubble;
		const auto &message = data();
		if (playRect(rect).contains(point)) {
			result.zone = Uploading(message) ? Zone::MediaCancel : Zone::VoicePlay;
			return result;
		}
		const auto playback = delegate()->elementPlayback(message);
		if (!playback.current) {
			return result;
		}
		if (message.kind == Data::MessageKind::Voice) {
			if (speedRect(rect).contains(point)) {
				result.zone = Zone::VoiceSpeed;
				return result;
			}
			const auto wave = waveRect(rect).marginsAdded(QMargins(2, 4, 2, 4));
			if (wave.contains(point)) {
				result.zone = Zone::VoiceSeek;
				result.ratio = std::clamp(
					double(point.x() - wave.x()) / std::max(wave.width(), 1),
					0.,
					1.);
			}
		} else {
			const auto bodyLeft = style::RightToLeft()
				? rect.x()
				: (rect.x() + st::historyVoicePlaySize + st::historyVoiceSkip);
			const auto bodyWidth = rect.width()
				- st::historyVoicePlaySize
				- st::historyVoiceSkip;
			const auto line = QRect(bodyLeft, rect.y() + 34, bodyWidth, 16);
			if (line.contains(point)) {
				result.zone = Zone::VoiceSeek;
				result.ratio = std::clamp(
					double(point.x() - line.x()) / std::max(line.width(), 1),
					0.,
					1.);
			}
		}
		return result;
	}

private:
	[[nodiscard]] QRect playRect(const QRect &rect) const {
		const auto size = st::historyVoicePlaySize;
		return QRect(
			style::RightToLeft() ? (rect.right() + 1 - size) : rect.x(),
			rect.y(),
			size,
			size);
	}
	[[nodiscard]] QRect waveRect(const QRect &rect) const {
		const auto bodyLeft = style::RightToLeft()
			? rect.x()
			: (rect.x() + st::historyVoicePlaySize + st::historyVoiceSkip);
		const auto bodyWidth = rect.width()
			- st::historyVoicePlaySize
			- st::historyVoiceSkip;
		const auto full = int(std::ceil(st::historyVoiceBars
			* (st::historyVoiceBarWidth + st::historyVoiceBarSkip)));
		const auto width = std::min(full, bodyWidth);
		return QRect(
			style::RightToLeft() ? (bodyLeft + bodyWidth - width) : bodyLeft,
			rect.y() + 1,
			width,
			st::historyVoiceWaveHeight);
	}
	[[nodiscard]] QRect speedRect(const QRect &rect) const {
		const auto wave = waveRect(rect);
		const auto size = st::historyVoiceSpeedSize;
		// Next to the time, under the middle of the waveform.
		return QRect(
			wave.x() + (wave.width() - size.width()) / 2,
			wave.bottom() + 2,
			size.width(),
			size.height());
	}
	[[nodiscard]] QString titleText() const {
		const auto &message = data();
		if (message.audio && !message.audio->title.isEmpty()) {
			return message.audio->title;
		}
		return message.text.isEmpty()
			? tr::lng_media_music(tr::now)
			: message.text;
	}
	[[nodiscard]] QString performerText() const {
		const auto &message = data();
		if (message.audio && !message.audio->performer.isEmpty()) {
			return message.audio->performer;
		}
		// The file's name when the title came from the tags.
		const auto name = message.text.trimmed();
		return (message.audio && !message.audio->title.isEmpty())
			? name
			: QString();
	}

	std::vector<float> _bars;

};

// ---- file

class FileMedia final : public Media {
public:
	FileMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto text = std::max(
			st::historyFileNameFont->width(name()),
			st::historyFileSizeFont->width(status()) + 70);
		const auto width = st::historyFileIconSize
			+ st::historyFileSkip
			+ std::clamp(text, 120, st::historyFileWidth
				- st::historyFileIconSize
				- st::historyFileSkip);
		return QSize(
			std::min(width, std::max(maxWidth, 160)),
			st::historyFileIconSize);
	}

	bool hasMetaCorner() const override {
		return true;
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &message = data();
		const auto &colors = *context.colors;
		const auto rtl = style::RightToLeft();
		const auto accent = context.outgoing ? colors.accentOut : colors.accentIn;
		const auto icon = iconRect(rect);
		const auto download = delegate()->elementDownload(message);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(accent);
			p.drawRoundedRect(
				icon,
				st::historyFileIconRadius,
				st::historyFileIconRadius);
		}
		const auto name = this->name();
		if (Uploading(message)) {
			PaintProgressRing(
				p,
				icon.marginsRemoved(QMargins(3, 3, 3, 3)),
				std::max(message.uploadProgress, 0.),
				st::historyFileCancelIcon,
				Qt::transparent,
				st::textOnBrand->c);
		} else if (download.loading) {
			PaintProgressRing(
				p,
				icon.marginsRemoved(QMargins(3, 3, 3, 3)),
				download.progress,
				st::historyFileCancelIcon,
				Qt::transparent,
				st::textOnBrand->c);
		} else if (download.path.isEmpty() && message.seq > 0) {
			st::historyFileDownloadIcon.paintInCenter(p, icon);
		} else {
			const auto label = FileExtensionLabel(name);
			if (!label.isEmpty()) {
				p.setPen(st::textOnBrand);
				p.setFont(st::historyFileExtFont);
				p.drawText(icon, Qt::AlignCenter, label.toUpper());
			} else {
				switch (FileTypeFor(name, message.media ? message.media->mime : QString())) {
				case FileType::Image:
					st::historyFileImageIcon.paintInCenter(p, icon);
					break;
				case FileType::Video:
					st::historyFileVideoIcon.paintInCenter(p, icon);
					break;
				case FileType::Audio:
					st::historyFileMusicIcon.paintInCenter(p, icon);
					break;
				default:
					st::historyFileIcon.paintInCenter(p, icon);
					break;
				}
			}
		}
		const auto textLeft = rtl
			? rect.x()
			: (icon.right() + 1 + st::historyFileSkip);
		const auto textWidth = rect.width()
			- st::historyFileIconSize
			- st::historyFileSkip;
		p.setPen(context.outgoing ? colors.textOut : colors.textIn);
		DrawLine(
			p,
			st::historyFileNameFont,
			name,
			textLeft,
			rect.y() + 1,
			textWidth,
			Qt::ElideMiddle);
		p.setPen(context.outgoing ? colors.metaOut : colors.metaIn);
		DrawLine(
			p,
			st::historyFileSizeFont,
			status(),
			textLeft,
			rect.y() + 1 + st::historyFileNameFont->height + 2,
			textWidth);
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (!rect.contains(point)) {
			return result;
		}
		const auto &message = data();
		const auto busy = Uploading(message)
			|| delegate()->elementDownload(message).loading;
		if (busy) {
			result.zone = iconRect(rect).contains(point)
				? Zone::MediaCancel
				: Zone::Bubble;
		} else {
			result.zone = Zone::File;
		}
		return result;
	}

private:
	[[nodiscard]] QRect iconRect(const QRect &rect) const {
		const auto size = st::historyFileIconSize;
		return QRect(
			style::RightToLeft() ? (rect.right() + 1 - size) : rect.x(),
			rect.y(),
			size,
			size);
	}
	[[nodiscard]] QString name() const {
		// The name is the sender's. It is shown exactly as the file will be
		// saved: without direction overrides ("invoice<RLO>fdp.exe" must not
		// read "invoiceexe.pdf"), control characters or path separators.
		const auto &message = data();
		return message.text.isEmpty()
			? tr::lng_media_file(tr::now)
			: base::platform::SafeFileName(message.text, tr::lng_media_file(tr::now));
	}
	[[nodiscard]] QString status() const {
		const auto &message = data();
		const auto size = message.media ? message.media->size : 0;
		const auto download = delegate()->elementDownload(message);
		if (Uploading(message) && size > 0 && message.uploadProgress >= 0.) {
			return tr::lng_history_progress(
				tr::now,
				lt_done,
				Lang::Bytes(qint64(size * message.uploadProgress)),
				lt_total,
				Lang::Bytes(size));
		} else if (download.loading && size > 0) {
			return tr::lng_history_progress(
				tr::now,
				lt_done,
				Lang::Bytes(qint64(size * download.progress)),
				lt_total,
				Lang::Bytes(size));
		} else if (download.failed && download.path.isEmpty()) {
			// Stays until the next attempt (a click on the icon).
			return download.oversized
				? tr::lng_chat_file_larger_than_declared(tr::now)
				: tr::lng_chat_file_isnt_available(tr::now);
		}
		return (size > 0) ? Lang::Bytes(size) : QString();
	}

};

// ---- sticker

class StickerMedia final : public Media {
public:
	StickerMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto sticker = resolved();
		const auto box = std::min(int(st::historyStickerSize), maxWidth);
		auto width = sticker ? sticker->width : 0;
		auto height = sticker ? sticker->height : 0;
		if (width <= 0 || height <= 0) {
			return QSize(box, box);
		}
		const auto scale = double(box) / std::max(width, height);
		return QSize(
			std::max(int(std::lround(width * scale)), 1),
			std::max(int(std::lround(height * scale)), 1));
	}

	bool bubbleless() const override {
		return true;
	}

	QImage currentImage() const override {
		return _last;
	}

	void unload() override {
		QObject::disconnect(_watching);
		_player = nullptr;
		_last = QImage();
	}

	~StickerMedia() override {
		QObject::disconnect(_watching);
	}

	// A sticker that is still being sent is known by its id alone: the
	// picture comes from the installed packs.
	[[nodiscard]] const Data::Sticker *resolved() const {
		const auto &message = data();
		if (!message.sticker) {
			return nullptr;
		} else if (!message.sticker->url.isEmpty() || message.sticker->id.isEmpty()) {
			return &*message.sticker;
		}
		const auto known = delegate()->elementSession()->data().stickers().sticker(
			message.sticker->id);
		return known ? known : &*message.sticker;
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto sticker = resolved();
		const auto ratio = delegate()->elementRatio();
		const auto size = rect.size() * ratio;
		if (sticker
			&& !sticker->url.isEmpty()
			&& (!_player || _player->size() != size)) {
			// Still, animated WebP or Lottie: rendered at the shown size,
			// moving while the row is painted.
			QObject::disconnect(_watching);
			_player = Support::StickerPlayer::Get(
				delegate()->elementSession(),
				sticker->url,
				size);
			if (_player) {
				const auto delegate = this->delegate();
				const auto parent = this->parent();
				_watching = QObject::connect(
					_player.get(),
					&Support::StickerPlayer::updated,
					delegate->elementGuard(),
					[=] { delegate->elementRepaint(parent); });
			}
		}
		const auto image = _player ? _player->frame() : QImage();
		if (image.isNull()) {
			// The emoji the sticker stands for, until the picture arrives.
			if (sticker && !sticker->emoji.isEmpty()) {
				auto font = st::historyJumboStyle1.font->f;
				font.setPixelSize(rect.height() / 2);
				p.setFont(font);
				p.setPen(st::textPrimary);
				p.setOpacity(0.5);
				p.drawText(rect, Qt::AlignCenter, sticker->emoji);
				p.setOpacity(1.);
			}
			return;
		}
		_last = image;
		const auto shown = QSizeF(image.size()) / ratio;
		p.drawImage(
			QRectF(
				rect.x() + (rect.width() - shown.width()) / 2.,
				rect.y() + (rect.height() - shown.height()) / 2.,
				shown.width(),
				shown.height()),
			image);
		if (context.selected) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historySelectedBg);
			p.drawRoundedRect(rect, st::radiusMd, st::radiusMd);
		}
	}

private:
	std::shared_ptr<Support::StickerPlayer> _player;
	QMetaObject::Connection _watching;
	QImage _last;

};

// ---- location

class LocationMedia final : public Media {
public:
	LocationMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto width = std::min(st::historyLocationSize.width(), maxWidth);
		auto height = st::historyLocationSize.height()
			+ st::historyLocationInfoPadding.top()
			+ st::historyLocationTitleFont->height
			+ st::historyLocationTextFont->height
			+ 2;
		if (canStop()) {
			height += st::historyLocationStopHeight + 4;
		}
		return QSize(width, height);
	}

	bool hasMetaCorner() const override {
		return !canStop();
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &message = data();
		const auto &colors = *context.colors;
		const auto live = (message.kind == Data::MessageKind::LiveLocation);
		const auto map = mapRect(rect);
		const auto accent = context.outgoing ? colors.accentOut : colors.accentIn;
		{
			// A drawn stand-in for a map: the app ships no tiles.
			auto hq = PainterHighQualityEnabler(p);
			auto path = QPainterPath();
			path.addRoundedRect(
				QRectF(map),
				st::historyLocationRadius,
				st::historyLocationRadius);
			p.save();
			p.setClipPath(path);
			p.fillRect(map, context.outgoing ? colors.tintOut : colors.tintIn);
			auto road = accent;
			road.setAlphaF(0.22f);
			const auto lat = message.location ? message.location->lat : 0.;
			const auto lng = message.location ? message.location->lng : 0.;
			// The same place always gets the same streets.
			auto seed = quint32(std::abs(lat) * 7919.) ^ quint32(std::abs(lng) * 104729.);
			const auto next = [&] {
				seed = seed * 1664525U + 1013904223U;
				return int((seed >> 16) & 0x7FFF);
			};
			p.setPen(QPen(road, 7., Qt::SolidLine, Qt::RoundCap));
			for (auto i = 0; i != 3; ++i) {
				const auto y = map.y() + 18 + next() % std::max(map.height() - 36, 1);
				p.drawLine(
					QPointF(map.x() - 10, y),
					QPointF(map.right() + 10, y + (next() % 50) - 25));
			}
			for (auto i = 0; i != 3; ++i) {
				const auto x = map.x() + 20 + next() % std::max(map.width() - 40, 1);
				p.drawLine(
					QPointF(x, map.y() - 10),
					QPointF(x + (next() % 60) - 30, map.bottom() + 10));
			}
			p.setPen(QPen(road, 2.5, Qt::SolidLine, Qt::RoundCap));
			for (auto i = 0; i != 4; ++i) {
				const auto x = map.x() + next() % std::max(map.width(), 1);
				const auto y = map.y() + next() % std::max(map.height(), 1);
				p.drawLine(
					QPointF(x, y),
					QPointF(x + (next() % 90) - 45, y + (next() % 70) - 35));
			}
			p.restore();
			const auto &pin = live
				? st::historyLocationLiveIcon
				: st::historyLocationPinIcon;
			const auto pinRect = QRect(
				map.x() + (map.width() - pin.width()) / 2,
				map.y() + (map.height() - pin.height()) / 2 - pin.height() / 4,
				pin.width(),
				pin.height());
			if (live && !liveEnded()) {
				auto halo = accent;
				halo.setAlphaF(0.18f);
				p.setPen(Qt::NoPen);
				p.setBrush(halo);
				p.drawEllipse(QPointF(pinRect.center()) + QPointF(0.5, 6.), 30., 30.);
			}
			pin.paintInCenter(p, pinRect, live ? accent : st::statusError->c);
			if (context.selected) {
				p.fillPath(path, st::historySelectedBg);
			}
		}
		const auto infoTop = map.bottom() + 1 + st::historyLocationInfoPadding.top();
		const auto infoLeft = rect.x() + st::historyLocationInfoPadding.left();
		const auto infoWidth = rect.width()
			- st::historyLocationInfoPadding.left()
			- st::historyLocationInfoPadding.right();
		p.setPen(context.outgoing ? colors.textOut : colors.textIn);
		DrawLine(p, st::historyLocationTitleFont, title(), infoLeft, infoTop, infoWidth);
		p.setPen(context.outgoing ? colors.metaOut : colors.metaIn);
		DrawLine(
			p,
			st::historyLocationTextFont,
			subtitle(),
			infoLeft,
			infoTop + st::historyLocationTitleFont->height + 1,
			// The time shares this line.
			infoWidth - (hasMetaCorner() ? 76 : 0));
		if (canStop()) {
			const auto stop = stopRect(rect);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			auto bg = context.outgoing ? colors.tintOut : colors.tintIn;
			if (context.hovered == Zone::LiveStop) {
				bg.setAlphaF(std::min(bg.alphaF() * 1.8f, 1.f));
			}
			p.setBrush(bg);
			p.drawRoundedRect(stop, 8., 8.);
			p.setPen(st::statusError);
			p.setFont(st::historyLocationStopFont);
			p.drawText(stop, Qt::AlignCenter, tr::lng_history_live_stop(tr::now));
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (!rect.contains(point)) {
			return result;
		}
		result.zone = (canStop() && stopRect(rect).contains(point))
			? Zone::LiveStop
			: Zone::Location;
		return result;
	}

private:
	[[nodiscard]] QRect mapRect(const QRect &rect) const {
		return QRect(
			rect.x(),
			rect.y(),
			rect.width(),
			st::historyLocationSize.height());
	}
	[[nodiscard]] QRect stopRect(const QRect &rect) const {
		return QRect(
			rect.x(),
			rect.bottom() + 1 - st::historyLocationStopHeight,
			rect.width(),
			st::historyLocationStopHeight);
	}
	[[nodiscard]] bool liveEnded() const {
		const auto &message = data();
		return !message.live
			|| message.live->stopped
			|| (message.live->until > 0
				&& message.live->until <= QDateTime::currentSecsSinceEpoch());
	}
	[[nodiscard]] bool canStop() const {
		const auto &message = data();
		return message.mine
			&& message.seq > 0
			&& (message.kind == Data::MessageKind::LiveLocation)
			&& !liveEnded();
	}
	[[nodiscard]] QString title() const {
		const auto &message = data();
		if (message.location && !message.location->label.isEmpty()) {
			return message.location->label;
		}
		return (message.kind == Data::MessageKind::LiveLocation)
			? tr::lng_media_live_location(tr::now)
			: tr::lng_media_location(tr::now);
	}
	[[nodiscard]] QString subtitle() const {
		const auto &message = data();
		if (message.kind == Data::MessageKind::LiveLocation) {
			if (liveEnded()) {
				return tr::lng_history_live_ended(tr::now);
			} else if (message.live && message.live->until > 0) {
				return tr::lng_history_live_until(
					tr::now,
					lt_time,
					Lang::Clock(QDateTime::fromSecsSinceEpoch(message.live->until)));
			}
		}
		if (message.location && !message.location->address.isEmpty()) {
			return message.location->address;
		} else if (message.location) {
			// Coordinates read left to right in both languages (isolated,
			// or the pair would swap inside right-to-left text).
			return QString(QChar(0x2066))
				+ QString::number(message.location->lat, 'f', 5)
				+ u", "_s
				+ QString::number(message.location->lng, 'f', 5)
				+ QChar(0x2069);
		}
		return QString();
	}

};

// ---- a shared feed post

class PostMedia final : public Media {
public:
	PostMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto width = std::min(int(st::historyPostWidth), maxWidth);
		const auto &padding = st::historyPostPadding;
		const auto post = delegate()->elementPost(data().forwardedPost);
		_known = (post != nullptr);
		_available = post && post->valid() && !post->deleted;
		auto height = padding.top() + st::historyPostLabelFont->height + 3;
		const auto thumb = (_available && !post->media.isEmpty())
			? int(st::historyPostThumb)
			: 0;
		const auto textWidth = width
			- padding.left()
			- padding.right()
			- st::historyReplyBar
			- (thumb ? (thumb + st::historyPreviewThumbSkip) : 0);
		if (_available) {
			const auto author = delegate()->elementSession()
				? delegate()->elementSession()->data().users().get(post->author)
				: Data::User();
			_author = !author.displayName.isEmpty()
				? author.displayName
				: !author.handle.isEmpty()
				? Lang::Handle(author.handle)
				: QString();
			_verified = author.verified;
			const auto text = post->text.trimmed();
			if (_textSource != text) {
				_textSource = text;
				// Plain (the card opens the post), but "@sara" and "#tag"
				// keep their direction inside the Persian text.
				_text.setText(
					st::historyPostTextStyle,
					Ui::Text::IsolateLinks(Ui::Text::ParseEntities(
						text,
						Ui::Text::ParseAll)).text);
			}
			height += st::historyPostAuthorFont->height;
			_textHeight = _text.isEmpty()
				? 0
				: _text.countHeight(textWidth, st::historyPostTextLines);
			height += _textHeight ? (_textHeight + 2) : 0;
			height = std::max(
				height,
				padding.top() + st::historyPostLabelFont->height + 3 + thumb);
			if (thumb && !_source) {
				const auto &media = post->media.front();
				auto options = Support::ImageOptions();
				options.blurhash = media.blurhash;
				options.aspect = QSize(media.width, media.height);
				_source = Support::MakeImageSource(
					delegate()->elementSession(),
					Data::MediaRef::PostMedia(post->id, media.id),
					Data::MediaVariant::Thumb,
					options);
				watch(_source);
			}
		} else {
			height += st::historyPostAuthorFont->height;
		}
		_textWidth = textWidth;
		_thumb = thumb;
		return QSize(width, height + padding.bottom());
	}

	void unload() override {
		_source = nullptr;
		_prepared = PreparedImage();
		_text.clear();
		_textSource = QString();
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &colors = *context.colors;
		const auto rtl = style::RightToLeft();
		const auto accent = context.outgoing ? colors.accentOut : colors.accentIn;
		const auto &padding = st::historyPostPadding;
		{
			auto hq = PainterHighQualityEnabler(p);
			auto path = QPainterPath();
			path.addRoundedRect(QRectF(rect), st::historyPostRadius, st::historyPostRadius);
			auto bg = context.outgoing ? colors.tintOut : colors.tintIn;
			if (context.hovered == Zone::Post) {
				bg.setAlphaF(std::min(bg.alphaF() * 1.6f, 1.f));
			}
			p.fillPath(path, bg);
			p.save();
			p.setClipPath(path);
			p.fillRect(
				QRect(
					rtl ? (rect.right() + 1 - st::historyReplyBar) : rect.x(),
					rect.y(),
					st::historyReplyBar,
					rect.height()),
				accent);
			p.restore();
		}
		const auto innerLeft = rect.x()
			+ (rtl ? padding.right() : (padding.left() + st::historyReplyBar));
		const auto innerWidth = rect.width()
			- padding.left()
			- padding.right()
			- st::historyReplyBar;
		auto top = rect.y() + padding.top();
		// «پست الی» with the feed's mark.
		const auto &icon = st::historyPostIcon;
		const auto label = tr::lng_media_eli_post(tr::now);
		const auto labelWidth = st::historyPostLabelFont->width(label);
		const auto iconLeft = rtl
			? (innerLeft + innerWidth - icon.width())
			: innerLeft;
		icon.paintInCenter(
			p,
			QRect(iconLeft, top, icon.width(), st::historyPostLabelFont->height),
			accent);
		p.setPen(accent);
		p.setFont(st::historyPostLabelFont);
		p.drawText(
			rtl ? (iconLeft - 5 - labelWidth) : (iconLeft + icon.width() + 5),
			top + st::historyPostLabelFont->ascent,
			label);
		top += st::historyPostLabelFont->height + 3;
		const auto textLeft = rtl
			? (innerLeft + (_thumb ? (_thumb + st::historyPreviewThumbSkip) : 0))
			: innerLeft;
		if (!_available) {
			p.setPen(context.outgoing ? colors.metaOut : colors.metaIn);
			DrawLine(
				p,
				st::historyPostTextStyle.font,
				_known
					? tr::lng_eli_post_isnt_available(tr::now)
					: tr::lng_loading(tr::now),
				textLeft,
				top,
				_textWidth);
			return;
		}
		p.setPen(context.outgoing ? colors.textOut : colors.textIn);
		DrawLine(p, st::historyPostAuthorFont, _author, textLeft, top, _textWidth);
		if (_textHeight) {
			auto geometry = Ui::Text::PaintContext();
			geometry.position = QPoint(
				textLeft,
				top + st::historyPostAuthorFont->height + 2);
			geometry.availableWidth = _textWidth;
			geometry.elisionLines = st::historyPostTextLines;
			geometry.autoDirection = true;
			p.setPen(context.outgoing ? colors.textOut : colors.textIn);
			_text.draw(p, geometry);
		}
		if (_thumb && _source) {
			const auto thumb = QRect(
				rtl ? innerLeft : (innerLeft + innerWidth - _thumb),
				top,
				_thumb,
				_thumb);
			const auto ratio = delegate()->elementRatio();
			auto image = _source->request(thumb.size() * ratio);
			if (image.isNull()) {
				image = _source->preview();
			}
			const auto radius = st::historyPostThumbRadius;
			const int radii[4] = { radius, radius, radius, radius };
			if (image.isNull()) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::historyMediaBg);
				p.drawRoundedRect(thumb, radius, radius);
			} else {
				PaintCover(p, thumb, image, radii, false, _prepared);
			}
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (rect.contains(point)) {
			result.zone = Zone::Post;
		}
		return result;
	}

private:
	Ui::Text::String _text;
	QString _textSource;
	QString _author;
	std::shared_ptr<Ui::ImageSource> _source;
	PreparedImage _prepared;
	int _textWidth = 0;
	int _textHeight = 0;
	int _thumb = 0;
	bool _known = false;
	bool _available = false;
	bool _verified = false;

};

// ---- a call record
//
// Written by the server when a call ends; the sender is the caller, so each
// side reads the direction from it:
//
//   تماس خروجی / تماس ورودی      ۵ دقیقه · ۰:۴۲        (it was answered)
//   تماس خروجی                   بی‌پاسخ / رد شده
//   تماس از دست رفته             برای تماس دوباره کلیک کن   (red)
//   تماس ورودی                   تماس رد شد                 (red)
//
// A click calls back (the list does it: a record is the row's media).

class CallMedia final : public Media {
public:
	CallMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto width = st::historyFileIconSize
			+ st::historyFileSkip
			+ std::max(
				st::historyFileNameFont->width(title()),
				st::historyFileSizeFont->width(subtitle()) + 70);
		return QSize(
			std::clamp(width, 180, std::max(maxWidth, 180)),
			st::historyFileIconSize);
	}

	bool hasMetaCorner() const override {
		return true;
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &colors = *context.colors;
		const auto rtl = style::RightToLeft();
		const auto size = st::historyFileIconSize;
		const auto icon = QRect(
			rtl ? (rect.right() + 1 - size) : rect.x(),
			rect.y(),
			size,
			size);
		const auto red = missed();
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			if (red) {
				p.setBrush(st::historyCallMissedBg);
			} else {
				p.setBrush(context.outgoing ? colors.accentOut : colors.accentIn);
			}
			p.drawEllipse(icon);
		}
		(red
			? st::historyCallMissedIcon
			: st::historyCallIcon).paintInCenter(p, icon);
		const auto textLeft = rtl ? rect.x() : (icon.right() + 1 + st::historyFileSkip);
		const auto textWidth = rect.width() - size - st::historyFileSkip;
		p.setPen(context.outgoing ? colors.textOut : colors.textIn);
		DrawLine(p, st::historyFileNameFont, title(), textLeft, rect.y() + 1, textWidth);
		if (red) {
			p.setPen(st::historyCallMissedFg);
		} else {
			p.setPen(context.outgoing ? colors.metaOut : colors.metaIn);
		}
		DrawLine(
			p,
			st::historyFileSizeFont,
			subtitle(),
			textLeft,
			rect.y() + 1 + st::historyFileNameFont->height + 2,
			textWidth);
	}

private:
	[[nodiscard]] Data::CallRecordKind kind() const {
		const auto &message = data();
		return Data::ClassifyCallRecord(
			message.call ? *message.call : Data::CallRecord(),
			message.mine);
	}
	[[nodiscard]] bool missed() const {
		return Data::CallRecordIsMissed(kind());
	}
	[[nodiscard]] QString title() const {
		// How the call started: a voice call in which somebody turned a
		// camera on still reads as a voice call.
		const auto video = data().call && data().call->video;
		switch (kind()) {
		case Data::CallRecordKind::Missed:
			return video
				? tr::lng_calls_record_video_missed(tr::now)
				: tr::lng_calls_record_missed(tr::now);
		case Data::CallRecordKind::Incoming:
		case Data::CallRecordKind::IncomingDeclined:
			return video
				? tr::lng_calls_record_video_incoming(tr::now)
				: tr::lng_calls_record_incoming(tr::now);
		case Data::CallRecordKind::Outgoing:
		case Data::CallRecordKind::OutgoingNoAnswer:
		case Data::CallRecordKind::OutgoingDeclined:
			break;
		}
		return video
			? tr::lng_calls_record_video_outgoing(tr::now)
			: tr::lng_calls_record_outgoing(tr::now);
	}
	[[nodiscard]] QString subtitle() const {
		switch (kind()) {
		case Data::CallRecordKind::Missed:
			return tr::lng_calls_record_call_back(tr::now);
		case Data::CallRecordKind::OutgoingNoAnswer:
			return tr::lng_calls_record_no_answer(tr::now);
		case Data::CallRecordKind::OutgoingDeclined:
			return tr::lng_calls_record_declined_out(tr::now);
		case Data::CallRecordKind::IncomingDeclined:
			return tr::lng_calls_record_declined_in(tr::now);
		case Data::CallRecordKind::Outgoing:
		case Data::CallRecordKind::Incoming:
			break;
		}
		const auto seconds = data().call ? data().call->durationS : 0;
		return Lang::Duration(int(std::max(seconds, qint64(0))));
	}

};

// ---- album: photos / videos sent together, one tile each

class AlbumMedia final : public Media {
public:
	AlbumMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		ensureTiles();
		const auto &items = parent()->album();
		auto ratios = std::vector<double>();
		ratios.reserve(items.size());
		for (const auto &item : items) {
			const auto w = item.media ? item.media->width : 0;
			const auto h = item.media ? item.media->height : 0;
			ratios.push_back((w > 0 && h > 0) ? (double(w) / h) : 0.);
		}
		_frames = AlbumLayout(ratios);
		const auto width = std::max(
			std::min(
				maxWidth - 2 * st::historyMediaPadding,
				int(st::historyMediaMaxWidth)),
			1);
		const auto height = std::max(
			int(std::lround(AlbumHeight(_frames) * width / kAlbumMaxWidth)),
			1);
		return QSize(width, height);
	}

	bool fullBleed() const override {
		return true;
	}

	std::shared_ptr<Ui::ImageSource> imageSource() const override {
		return _tiles.empty() ? nullptr : _tiles.front()->imageSource();
	}

	QImage currentImage() const override {
		return _tiles.empty() ? QImage() : _tiles.front()->currentImage();
	}

	Media *albumTile(int index) override {
		return (index >= 0 && index < int(_tiles.size()))
			? _tiles[index].get()
			: nullptr;
	}

	void unload() override {
		for (const auto &tile : _tiles) {
			tile->unload();
		}
	}

	void dataChanged() override {
		if (int(_tiles.size()) != int(parent()->album().size())) {
			_tiles.clear();
			_frames.clear();
			return;
		}
		for (const auto &tile : _tiles) {
			tile->dataChanged();
		}
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		ensureTiles();
		if (_frames.size() != _tiles.size()) {
			return;
		}
		for (auto i = 0; i != int(_tiles.size()); ++i) {
			const auto box = tileRect(i, rect);
			if (!p.clipBoundingRect().isEmpty()
				&& !p.clipBoundingRect().intersects(box)) {
				continue;
			}
			auto tile = context;
			const auto sides = visualSides(_frames[i].sides);
			const auto inner = int(st::historyAlbumInnerRadius);
			const auto left = (sides & kAlbumSideLeft) != 0;
			const auto right = (sides & kAlbumSideRight) != 0;
			const auto top = (sides & kAlbumSideTop) != 0;
			const auto bottom = (sides & kAlbumSideBottom) != 0;
			tile.radii[0] = (left && top) ? context.radii[0] : inner;
			tile.radii[1] = (right && top) ? context.radii[1] : inner;
			tile.radii[2] = (right && bottom) ? context.radii[2] : inner;
			tile.radii[3] = (left && bottom) ? context.radii[3] : inner;
			_tiles[i]->paint(p, box, tile);
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (!rect.contains(point) || _frames.size() != _tiles.size()) {
			return result;
		}
		for (auto i = 0; i != int(_tiles.size()); ++i) {
			const auto box = tileRect(i, rect);
			if (box.contains(point)) {
				result = _tiles[i]->hitTest(point, box);
				result.item = i;
				return result;
			}
		}
		// The gaps between the tiles.
		result.zone = Zone::Bubble;
		return result;
	}

private:
	void ensureTiles() {
		const auto count = int(parent()->album().size());
		if (int(_tiles.size()) == count) {
			return;
		}
		_tiles.clear();
		for (auto i = 0; i != count; ++i) {
			auto tile = std::make_unique<PhotoMedia>(delegate(), parent());
			tile->setAlbumIndex(i);
			_tiles.push_back(std::move(tile));
		}
	}

	// The mosaic is mirrored in a right-to-left interface: the first item
	// is on the right.
	[[nodiscard]] static int visualSides(int sides) {
		if (!style::RightToLeft()) {
			return sides;
		}
		auto result = sides & (kAlbumSideTop | kAlbumSideBottom);
		if (sides & kAlbumSideLeft) {
			result |= kAlbumSideRight;
		}
		if (sides & kAlbumSideRight) {
			result |= kAlbumSideLeft;
		}
		return result;
	}

	[[nodiscard]] QRect tileRect(int index, const QRect &rect) const {
		const auto &f = _frames[index];
		const auto k = rect.width() / kAlbumMaxWidth;
		const auto x = style::RightToLeft() ? (kAlbumMaxWidth - f.x - f.w) : f.x;
		auto left = int(std::lround(x * k));
		auto right = int(std::lround((x + f.w) * k));
		auto top = int(std::lround(f.y * k));
		auto bottom = int(std::lround((f.y + f.h) * k));
		const auto gap = int(st::historyAlbumGap);
		const auto half = gap / 2;
		const auto other = gap - half;
		const auto sides = visualSides(f.sides);
		if (!(sides & kAlbumSideLeft)) {
			left += other;
		}
		if (!(sides & kAlbumSideRight)) {
			right -= half;
		}
		if (!(sides & kAlbumSideTop)) {
			top += other;
		}
		if (!(sides & kAlbumSideBottom)) {
			bottom -= half;
		}
		return QRect(
			rect.x() + left,
			rect.y() + top,
			std::max(right - left, 1),
			std::max(bottom - top, 1));
	}

	std::vector<std::unique_ptr<Media>> _tiles;
	std::vector<AlbumFrame> _frames;

};

} // namespace

HitResult Media::hitTest(QPoint point, const QRect &rect) const {
	auto result = HitResult();
	if (rect.contains(point)) {
		result.zone = Zone::Media;
	}
	return result;
}

void Media::watch(const std::shared_ptr<Ui::ImageSource> &source) {
	QObject::disconnect(_watching);
	if (!source) {
		return;
	}
	const auto delegate = _delegate;
	const auto parent = _parent;
	_watching = QObject::connect(
		source.get(),
		&Ui::ImageSource::updated,
		delegate->elementGuard(),
		[=] { delegate->elementRepaint(parent); });
}

// ---- a reply / a reaction to a story

class StoryMedia final : public Media {
public:
	StoryMedia(Delegate *delegate, Element *parent)
	: Media(delegate, parent) {
	}

	QSize countSize(int maxWidth, bool hasCaption) override {
		Q_UNUSED(hasCaption);
		const auto width = std::min(int(st::historyStoryWidth), maxWidth);
		const auto &padding = st::historyPostPadding;
		const auto &story = data().story;
		_reaction = (data().kind == Data::MessageKind::StoryReaction);
		_emoji = (story && _reaction) ? story->emoji : QString();
		_gone = !story
			|| story->expired(QDateTime::currentSecsSinceEpoch())
			|| (_source && _source->failed());
		if (!_gone && !_source) {
			_source = Support::MakeImageSource(
				delegate()->elementSession(),
				Data::MediaRef::Story(story->storyId),
				Data::MediaVariant::Thumb);
			watch(_source);
		}
		const auto thumb = _gone ? QSize() : st::historyStoryThumb;
		const auto line = st::historyPostLabelFont->height + 3;
		const auto emoji = _emoji.isEmpty() ? 0 : st::historyStoryEmojiFont->height;
		const auto text = _gone ? (st::historyPostTextStyle.font->height + 2) : 0;
		const auto height = std::max(
			line + emoji + text,
			thumb.height());
		return QSize(width, padding.top() + height + padding.bottom());
	}

	void unload() override {
		_source = nullptr;
		_prepared = PreparedImage();
	}

	void dataChanged() override {
		if (_source && _source->failed()) {
			_gone = true;
		}
	}

	void paint(Painter &p, const QRect &rect, const MediaPaint &context) override {
		const auto &colors = *context.colors;
		const auto rtl = style::RightToLeft();
		const auto accent = context.outgoing ? colors.accentOut : colors.accentIn;
		const auto &padding = st::historyPostPadding;
		if (!_gone && _source && _source->failed()) {
			// 404: deleted or no longer mine to see.
			_gone = true;
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			auto path = QPainterPath();
			path.addRoundedRect(QRectF(rect), st::historyPostRadius, st::historyPostRadius);
			auto bg = context.outgoing ? colors.tintOut : colors.tintIn;
			if (context.hovered == Zone::Story && !_gone) {
				bg.setAlphaF(std::min(bg.alphaF() * 1.6f, 1.f));
			}
			p.fillPath(path, bg);
			p.save();
			p.setClipPath(path);
			p.fillRect(
				QRect(
					rtl ? (rect.right() + 1 - st::historyReplyBar) : rect.x(),
					rect.y(),
					st::historyReplyBar,
					rect.height()),
				accent);
			p.restore();
		}
		const auto innerLeft = rect.x()
			+ (rtl ? padding.right() : (padding.left() + st::historyReplyBar));
		const auto innerWidth = rect.width()
			- padding.left()
			- padding.right()
			- st::historyReplyBar;
		const auto thumb = _gone ? QSize() : st::historyStoryThumb;
		const auto thumbSkip = thumb.isEmpty() ? 0 : (thumb.width() + st::historyPreviewThumbSkip);
		const auto textLeft = rtl ? (innerLeft + thumbSkip) : innerLeft;
		const auto textWidth = innerWidth - thumbSkip;
		auto top = rect.y() + padding.top();
		p.setPen(accent);
		DrawLine(
			p,
			st::historyPostLabelFont,
			_reaction
				? tr::lng_stories_chat_reaction(tr::now)
				: tr::lng_stories_chat_reply(tr::now),
			textLeft,
			top,
			textWidth);
		top += st::historyPostLabelFont->height + 3;
		if (!_emoji.isEmpty()) {
			p.setPen(context.outgoing ? colors.textOut : colors.textIn);
			DrawLine(p, st::historyStoryEmojiFont, _emoji, textLeft, top, textWidth);
			top += st::historyStoryEmojiFont->height;
		}
		if (_gone) {
			p.setPen(context.outgoing ? colors.metaOut : colors.metaIn);
			DrawLine(
				p,
				st::historyPostTextStyle.font,
				tr::lng_stories_chat_gone(tr::now),
				textLeft,
				top,
				textWidth);
			return;
		}
		const auto box = QRect(
			rtl ? innerLeft : (innerLeft + innerWidth - thumb.width()),
			rect.y() + padding.top(),
			thumb.width(),
			thumb.height());
		const auto ratio = delegate()->elementRatio();
		auto image = _source ? _source->request(box.size() * ratio) : QImage();
		const auto radius = st::historyPostThumbRadius;
		const int radii[4] = { radius, radius, radius, radius };
		if (image.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyMediaBg);
			p.drawRoundedRect(box, radius, radius);
		} else {
			PaintCover(p, box, image, radii, false, _prepared);
		}
	}

	HitResult hitTest(QPoint point, const QRect &rect) const override {
		auto result = HitResult();
		if (rect.contains(point)) {
			result.zone = _gone ? Zone::Bubble : Zone::Story;
		}
		return result;
	}

private:
	std::shared_ptr<Ui::ImageSource> _source;
	PreparedImage _prepared;
	QString _emoji;
	bool _reaction = false;
	bool _gone = false;

};

std::unique_ptr<Media> CreateMedia(Delegate *delegate, Element *parent) {
	const auto &message = parent->data();
	switch (message.kind) {
	case Data::MessageKind::Photo:
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
		return std::make_unique<PhotoMedia>(delegate, parent);
	case Data::MessageKind::VideoNote:
		return std::make_unique<VideoNoteMedia>(delegate, parent);
	case Data::MessageKind::Voice:
	case Data::MessageKind::Audio:
		return std::make_unique<VoiceMedia>(delegate, parent);
	case Data::MessageKind::File:
		return std::make_unique<FileMedia>(delegate, parent);
	case Data::MessageKind::Sticker:
		return std::make_unique<StickerMedia>(delegate, parent);
	case Data::MessageKind::Location:
	case Data::MessageKind::LiveLocation:
		return std::make_unique<LocationMedia>(delegate, parent);
	case Data::MessageKind::ForwardedPost:
		return std::make_unique<PostMedia>(delegate, parent);
	case Data::MessageKind::Call:
		return std::make_unique<CallMedia>(delegate, parent);
	case Data::MessageKind::StoryReply:
	case Data::MessageKind::StoryReaction:
		return std::make_unique<StoryMedia>(delegate, parent);
	case Data::MessageKind::System:
		if (message.call) {
			return std::make_unique<CallMedia>(delegate, parent);
		}
		break;
	case Data::MessageKind::Text:
	case Data::MessageKind::Unknown:
		break;
	}
	return nullptr;
}

std::unique_ptr<Media> CreateAlbumMedia(Delegate *delegate, Element *parent) {
	return std::make_unique<AlbumMedia>(delegate, parent);
}

std::shared_ptr<Ui::ImageSource> MakeLocalImageSource(
		const QString &path,
		int maxSide) {
	return std::make_shared<LocalImageSource>(path, maxSide);
}

void PaintCover(
		QPainter &p,
		const QRect &rect,
		const QImage &source,
		const int radii[4],
		bool blurred,
		PreparedImage &cache) {
	if (source.isNull() || rect.isEmpty()) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	const auto corners = radii[0]
		| (radii[1] << 8)
		| (radii[2] << 16)
		| (radii[3] << 24);
	if (cache.image.isNull()
		|| cache.sourceKey != source.cacheKey()
		|| cache.size != rect.size()
		|| cache.corners != corners
		|| cache.blurred != blurred
		|| cache.ratio != ratio) {
		auto prepared = blurred
			? Ui::Images::BlurredPreview(source, rect.size(), ratio)
			: Ui::Images::PrepareCover(source, rect.size(), ratio);
		if (blurred) {
			// Twice: a spoiler must not be guessable.
			prepared = Ui::Images::PrepareCover(
				Ui::Images::Blur(
					prepared.scaled(
						std::max(prepared.width() / 8, 1),
						std::max(prepared.height() / 8, 1),
						Qt::IgnoreAspectRatio,
						Qt::SmoothTransformation),
					4),
				rect.size(),
				ratio);
		}
		cache.image = Ui::Images::Round(
			std::move(prepared),
			radii[0],
			radii[1],
			radii[2],
			radii[3]);
		cache.sourceKey = source.cacheKey();
		cache.size = rect.size();
		cache.corners = corners;
		cache.blurred = blurred;
		cache.ratio = ratio;
	}
	p.drawImage(QPointF(rect.topLeft()), cache.image);
}

void PaintProgressRing(
		QPainter &p,
		const QRect &circle,
		double progress,
		const style::icon &icon,
		const QColor &bg,
		const QColor &fg) {
	auto hq = PainterHighQualityEnabler(p);
	if (bg.alpha() > 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawEllipse(circle);
	}
	const auto line = st::historyMediaProgressLine;
	const auto inner = QRectF(circle).marginsRemoved(
		QMarginsF(3.5, 3.5, 3.5, 3.5));
	p.setPen(QPen(fg, line, Qt::SolidLine, Qt::RoundCap));
	p.setBrush(Qt::NoBrush);
	const auto shown = std::clamp(progress, 0.04, 1.);
	p.drawArc(inner, 90 * 16, -int(std::lround(shown * 360 * 16)));
	icon.paintInCenter(p, circle);
}

QString MediaDurationText(qint64 durationMs) {
	return Lang::Duration(int(std::max(durationMs, qint64(0)) / 1000));
}

} // namespace History::View
