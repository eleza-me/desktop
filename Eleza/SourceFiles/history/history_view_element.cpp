#include "history/history_view_element.h"

#include "history/history_view_media.h"
#include "history/history_album.h"
#include "data/data_group_rules.h"
#include "lang/lang_format.h"
#include "history/history_themes.h"
#include "lang_auto.h"
#include "support/chat_text.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/userpic.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtGui/QPainterPath>

#include <cmath>

namespace History::View {
namespace {

constexpr auto kReplyTextMaxWidth = 260;

[[nodiscard]] int PillHeight() {
	const auto &padding = st::historyServicePadding;
	return st::historyServiceStyle.lineHeight + padding.top() + padding.bottom();
}

void PaintPill(
		QPainter &p,
		const QRect &rect,
		const QString &text) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(ServiceBg());
	const auto radius = std::min(rect.height() / 2., 14.);
	p.drawRoundedRect(rect, radius, radius);
	p.setPen(st::historyServiceFg);
	p.setFont(st::historyServiceFont);
	p.drawText(rect, Qt::AlignCenter, text);
}

[[nodiscard]] QPainterPath BubblePath(
		const QRectF &rect,
		double tl,
		double tr,
		double br,
		double bl) {
	auto path = QPainterPath();
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

// The little sweep at the bottom corner of the last bubble of a group.
[[nodiscard]] QPainterPath TailPath(const QRectF &bubble, bool right) {
	const auto size = QSizeF(st::historyTailSize);
	const auto out = double(st::historyTailOut);
	auto path = QPainterPath();
	const auto top = bubble.bottom() - size.height();
	if (!right) {
		const auto left = bubble.left() - out;
		path.moveTo(left + 7., top);
		path.lineTo(left + 7., top + 17.);
		path.lineTo(left, top + 17.);
		path.cubicTo(
			QPointF(left + 3.5, top + 15.5),
			QPointF(left + 5.5, top + 12.),
			QPointF(left + 7., top));
	} else {
		const auto edge = bubble.right() + out;
		path.moveTo(edge - 7., top);
		path.lineTo(edge - 7., top + 17.);
		path.lineTo(edge, top + 17.);
		path.cubicTo(
			QPointF(edge - 3.5, top + 15.5),
			QPointF(edge - 5.5, top + 12.),
			QPointF(edge - 7., top));
	}
	path.closeSubpath();
	return path;
}

} // namespace

struct Element::ReplyView {
	QString name;
	Ui::Text::String text;
	bool quote = false;
	int lines = 1;
	int naturalWidth = 0;
};

struct Element::PreviewView {
	QString site;
	Ui::Text::String title;
	Ui::Text::String description;
	std::shared_ptr<Ui::ImageSource> image;
	PreparedImage prepared;
	QMetaObject::Connection watching;
	int titleHeight = 0;
	int descriptionHeight = 0;
	int textWidth = 0;
	int thumb = 0;

	~PreviewView() {
		QObject::disconnect(watching);
	}
};

Element::Element(
	Delegate *delegate,
	const Data::Message &message,
	const QVector<Data::Message> &album)
: _delegate(delegate)
, _data(message)
, _album(album)
, _key(message.key()) {
}

Element::~Element() = default;

const Data::Message &Element::albumItem(int index) const {
	return (index >= 0 && index < int(_album.size())) ? _album[index] : _data;
}

const Data::Message &Element::messageAt(const HitResult &hit) const {
	return albumItem(hit.item);
}

QVector<Data::Message> Element::messages() const {
	return _album.isEmpty() ? QVector<Data::Message>{ _data } : _album;
}

const Data::Message &Element::captionMessage() const {
	return albumItem(AlbumCaptionIndex(_album));
}

qint64 Element::lastSeq() const {
	auto result = _data.seq;
	for (const auto &item : _album) {
		result = std::max(result, item.seq);
	}
	return result;
}

bool Element::setData(
		const Data::Message &message,
		const QVector<Data::Message> &album) {
	auto albumView = (album.size() == _album.size());
	auto albumLayout = albumView;
	for (auto i = 0; albumLayout && i != int(album.size()); ++i) {
		if (!SameLayout(album[i], _album[i])) {
			albumView = albumLayout = false;
		} else if (albumView && !SameForView(album[i], _album[i])) {
			albumView = false;
		}
	}
	const auto sameKind = (_data.kind == message.kind)
		&& (_data.kindName == message.kindName)
		&& albumLayout;
	const auto sameView = SameForView(_data, message) && albumView;
	const auto sameLayout = sameView
		|| (SameLayout(_data, message) && albumLayout);
	_data = message;
	_album = album;
	_key = message.key();
	if (sameView) {
		return false;
	} else if (sameLayout) {
		if (_media) {
			_media->dataChanged();
		}
		return false;
	}
	// Rebuild everything but the attachment's view: it keeps its picture.
	auto media = sameKind ? std::move(_media) : nullptr;
	unloadHeavy();
	_media = std::move(media);
	if (_media) {
		_media->dataChanged();
	}
	_dirty = true;
	return true;
}

bool Element::setFlags(const RowFlags &flags) {
	if (_flags == flags) {
		return false;
	}
	const auto dateChanged = (_flags.showDate != flags.showDate);
	_flags = flags;
	if (dateChanged) {
		_dateText = QString();
	}
	_dirty = true;
	return true;
}

bool Element::bubbleless() const {
	return _jumbo || (_media && _media->bubbleless());
}

QRect Element::contentRect() const {
	return QRect(0, _contentTop, _width, std::max(_height - _contentTop, 0));
}

QString Element::dateText() const {
	return Lang::DayLabel(MessageDate(_data));
}

bool Element::hasText() const {
	return _loaded
		? !_text.isEmpty()
		: (!_service && HasCopyableText(_data));
}

const QString &Element::text() const {
	return _loaded ? _text.toString() : _data.text;
}

int Element::textLength() const {
	return int(text().size());
}

QString Element::selectedText(Ui::Text::Selection selection) const {
	const auto &full = text();
	const auto from = std::clamp(selection.from, 0, int(full.size()));
	const auto till = std::clamp(selection.to, from, int(full.size()));
	// Without the direction marks that were added for showing.
	return Ui::Text::StripIsolates(full.mid(from, till - from));
}

Ui::Text::Selection Element::adjustSelection(
		Ui::Text::Selection selection,
		Ui::Text::SelectType type) const {
	return _loaded ? _text.adjustSelection(selection, type) : selection;
}

const Ui::Text::Link *Element::link(int index) const {
	return (_loaded && index >= 0 && index < _text.linksCount())
		? &_text.link(index)
		: nullptr;
}

void Element::setSpoilersRevealed(bool revealed) {
	if (_loaded) {
		_text.setSpoilersRevealed(revealed);
	}
}

Ui::Text::PaintContext Element::textGeometry() const {
	auto result = Ui::Text::PaintContext();
	result.position = _textRect.topLeft();
	result.availableWidth = _textRect.width();
	result.autoDirection = true;
	return result;
}

int Element::textCursorAt(QPoint point) const {
	if (!_loaded || _text.isEmpty()) {
		return 0;
	} else if (point.y() < _textRect.y()) {
		return 0;
	} else if (point.y() >= _textRect.bottom() + 1) {
		return _text.length();
	}
	return _text.getState(point, textGeometry()).cursor;
}

void Element::unloadHeavy() {
	_text.clear();
	_reply = nullptr;
	_preview = nullptr;
	_media = nullptr;
	_reactions.clear();
	_keyboard.clear();
	_loaded = false;
}

void Element::createMedia() {
	if (!_media) {
		_media = isAlbum()
			? CreateAlbumMedia(_delegate, this)
			: CreateMedia(_delegate, this);
	}
}

void Element::ensureLoaded() {
	if (_loaded) {
		return;
	}
	_loaded = true;
	_service = IsService(_data);
	_jumbo = 0;
	_serviceText = QString();
	_forwardedText = QString();
	_viaText = QString();
	_group = false;
	_sender = SenderInfo();
	if (_service) {
		_serviceText = _delegate->elementServiceText(_data);
		if (_serviceText.isEmpty()) {
			_serviceText = Support::MessagePreview(_data);
		}
		return;
	}
	if (_delegate->elementGroup() && !outgoing()) {
		_group = true;
		_sender = _delegate->elementSender(_data);
	}
	if (_data.underReview) {
		// Held back by a moderator: the place of the message stays, nothing
		// of it is shown.
		_text.setText(
			st::historyTextStyle,
			tr::lng_groups_under_review(tr::now));
		return;
	}
	createMedia();
	const auto kind = _data.kind;
	const auto textKind = (kind == Data::MessageKind::Text)
		|| (kind == Data::MessageKind::Unknown);
	const auto captionKind = (kind == Data::MessageKind::Photo)
		|| (kind == Data::MessageKind::Video)
		|| (kind == Data::MessageKind::Gif)
		|| (kind == Data::MessageKind::ForwardedPost)
		|| (kind == Data::MessageKind::StoryReply);
	if (kind == Data::MessageKind::Unknown && _data.text.trimmed().isEmpty()) {
		_text.setText(st::historyTextStyle, tr::lng_history_unsupported(tr::now));
	} else if ((textKind || captionKind) && !_data.text.isEmpty()) {
		if (textKind
			&& kind == Data::MessageKind::Text
			&& _data.replyToSeq <= 0
			&& !_data.forwarded
			&& !_data.linkPreview
			&& _data.entities.isEmpty()) {
			_jumbo = JumboEmojiCount(_data.text);
		}
		if (_jumbo) {
			_text.setText(
				(_jumbo == 1)
					? st::historyJumboStyle1
					: (_jumbo == 2)
					? st::historyJumboStyle2
					: st::historyJumboStyle3,
				_data.text.trimmed());
		} else {
			_text.setMarkedText(
				st::historyTextStyle,
				TextForMessage(_data, _delegate->elementBotCommands()));
			if (_text.hasSpoilers()) {
				_text.setSpoilersRevealed(_delegate->elementRevealed(_key));
			}
		}
	}
	if (_data.forwarded) {
		_forwardedText = tr::lng_history_forwarded_message(tr::now);
	}
	if (_data.viaBot) {
		const auto name = !_data.viaBot->handle.isEmpty()
			? Lang::Handle(_data.viaBot->handle)
			: _data.viaBot->name;
		if (!name.isEmpty()) {
			_viaText = tr::lng_history_via_bot(tr::now, lt_bot, name);
		}
	}
	if (_data.replyToSeq > 0) {
		_reply = std::make_unique<ReplyView>();
		const auto target = _delegate->elementReplyTarget(_data);
		if (target) {
			_reply->name = _delegate->elementSenderName(*target);
		}
		auto text = QString();
		if (_data.quote && !_data.quote->text.isEmpty()) {
			text = _data.quote->text;
			_reply->quote = true;
		} else if (target) {
			text = Support::MessagePreview(*target);
		} else {
			text = tr::lng_history_message(tr::now);
		}
		_reply->text.setText(st::historyReplyTextStyle, text);
		_reply->naturalWidth = st::historyReplyBar
			+ st::historyReplyPadding.left()
			+ st::historyReplyPadding.right()
			+ std::max(
				st::historyReplyNameFont->width(_reply->name),
				std::min(_reply->text.maxWidth(), kReplyTextMaxWidth))
			+ (_reply->quote ? (st::historyQuoteIcon.width() + 6) : 0);
	}
	if (_data.linkPreview
		&& (!_data.linkPreview->title.isEmpty()
			|| !_data.linkPreview->description.isEmpty()
			|| !_data.linkPreview->siteName.isEmpty())) {
		const auto &preview = *_data.linkPreview;
		_preview = std::make_unique<PreviewView>();
		_preview->site = preview.siteName;
		if (!preview.title.isEmpty()) {
			_preview->title.setText(st::historyPreviewTitleStyle, preview.title);
		}
		if (!preview.description.isEmpty()) {
			_preview->description.setText(
				st::historyPreviewTextStyle,
				preview.description);
		}
		if (!preview.imageUrl.isEmpty()) {
			auto options = Support::ImageOptions();
			options.fit = QSize(320, 320);
			_preview->image = Support::MakeImageSource(
				_delegate->elementSession(),
				Data::MediaRef::Url(preview.imageUrl),
				Data::MediaVariant::Thumb,
				options);
			if (_preview->image) {
				const auto delegate = _delegate;
				_preview->watching = QObject::connect(
					_preview->image.get(),
					&Ui::ImageSource::updated,
					delegate->elementGuard(),
					[=, this] { delegate->elementRepaint(this); });
			}
		}
	}
	_reactions.clear();
	// The server offers a dozen reactions at most; whatever a broken answer
	// lists beyond that must not turn one message into a wall of chips.
	constexpr auto kMaxReactionChips = 24;
	for (const auto &reaction : SortedReactions(_data)) {
		if (int(_reactions.size()) >= kMaxReactionChips) {
			break;
		}
		auto chip = ReactionChip();
		chip.emoji = reaction.emoji;
		chip.count = (reaction.count > 1) ? Lang::Num(reaction.count) : QString();
		chip.mine = reaction.mine;
		_reactions.push_back(std::move(chip));
	}
	_keyboard.clear();
	if (_data.replyMarkup) {
		auto row = 0;
		for (const auto &buttons : _data.replyMarkup->inlineKeyboard) {
			auto column = 0;
			for (const auto &button : buttons) {
				auto view = KeyboardButton();
				view.text = button.text;
				view.row = row;
				view.column = column++;
				view.url = (button.kind == u"url") || !button.url.isEmpty();
				_keyboard.push_back(std::move(view));
			}
			++row;
		}
	}
}

QString Element::metaText() const {
	if (failed()) {
		return tr::lng_chat_not_sent(tr::now);
	}
	const auto time = Lang::Clock(MessageDateTime(_data));
	return _data.edited
		? (tr::lng_chat_edited(tr::now) + u' ' + time)
		: time;
}

int Element::metaWidth() const {
	auto result = st::historyMetaFont->width(_metaText);
	if (outgoing() && !failed()) {
		result += st::historyMetaInnerSkip + st::historyTickSize;
	}
	return result;
}

int Element::estimateHeight(int width) const {
	auto result = _flags.attachedToPrevious
		? int(st::historyGroupSkip)
		: int(st::historyMessageSkip);
	if (_flags.showDate) {
		result += PillHeight()
			+ st::historyServiceMargin.top()
			+ st::historyServiceMargin.bottom();
	}
	if (_flags.showUnread) {
		result += st::historyUnreadHeight
			+ st::historyUnreadMargin.top()
			+ st::historyUnreadMargin.bottom();
	}
	if (IsService(_data)) {
		return result + PillHeight();
	}
	const auto &padding = st::historyBubblePadding;
	const auto column = std::min(width, int(st::historyComposeMaxWidth));
	const auto bubble = std::max(
		std::min(
			int(st::historyBubbleMaxWidth),
			column - 2 * st::historySideSkip - st::historyFarSkip),
		140);
	const auto inner = bubble - padding.left() - padding.right();
	switch (_data.kind) {
	case Data::MessageKind::Photo:
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
	case Data::MessageKind::VideoNote:
		result += MediaBoxSize(
			_data.media ? _data.media->width : 0,
			_data.media ? _data.media->height : 0,
			_data.kind,
			std::min(bubble, int(st::historyMediaMaxWidth))).height() + 4;
		break;
	case Data::MessageKind::Sticker:
		result += st::historyStickerSize;
		break;
	case Data::MessageKind::Location:
	case Data::MessageKind::LiveLocation:
		result += st::historyLocationSize.height() + 56;
		break;
	case Data::MessageKind::ForwardedPost:
		result += 110;
		break;
	case Data::MessageKind::StoryReply:
	case Data::MessageKind::StoryReaction:
		result += st::historyStoryThumb.height() + 24;
		break;
	case Data::MessageKind::Text:
	case Data::MessageKind::Unknown:
		break;
	default:
		result += st::historyFileIconSize + padding.top() + padding.bottom();
		break;
	}
	if (HasCopyableText(_data) || _data.kind == Data::MessageKind::Unknown) {
		// About seven pixels per character, line breaks counted.
		const auto perLine = std::max(inner / 7, 8);
		auto lines = 0;
		for (const auto &part : QStringView(_data.text).split(u'\n')) {
			lines += 1 + int(part.size()) / perLine;
		}
		result += std::max(lines, 1) * st::historyTextStyle.lineHeight
			+ padding.top()
			+ padding.bottom();
	}
	if (_data.replyToSeq > 0) {
		result += 44;
	}
	if (!_data.reactions.isEmpty()) {
		result += st::historyReactionHeight + st::historyReactionTop;
	}
	return result;
}

int Element::resizeGetHeight(int width) {
	if (_dirty || _width != width || !_loaded) {
		layout(width);
	}
	return _height;
}

void Element::layout(int width) {
	ensureLoaded();
	_width = width;
	_dirty = false;
	_metaText = metaText();
	_dateTop = -1;
	_dateBottom = 0;
	_unreadTop = -1;
	_serviceRect = QRect();
	_bubble = QRect();
	_senderRect = _userpicRect = QRect();
	_forwardedRect = _viaRect = _replyRect = QRect();
	_mediaRect = _textRect = _previewRect = _metaRect = _retryRect = QRect();
	_metaOverMedia = false;
	_metaOutside = false;
	_keyboardBottom = 0;
	auto top = 0;
	if (_flags.showDate) {
		top += st::historyServiceMargin.top();
		_dateTop = top;
		top += PillHeight() + st::historyServiceMargin.bottom();
		_dateBottom = top;
		if (_dateText.isEmpty()) {
			_dateText = dateText();
		}
	}
	if (_flags.showUnread) {
		top += st::historyUnreadMargin.top();
		_unreadTop = top;
		top += st::historyUnreadHeight + st::historyUnreadMargin.bottom();
	}
	_contentTop = top;
	top += _flags.attachedToPrevious
		? st::historyGroupSkip
		: st::historyMessageSkip;
	if (_service) {
		layoutService(width, top);
		return;
	}
	const auto column = std::min(width, int(st::historyComposeMaxWidth));
	const auto columnLeft = (width - column) / 2;
	// In a group the pictures of the senders take a column of their own
	// before the incoming bubbles.
	const auto userpics = userpicSpace();
	const auto maxWidth = std::max(
		std::min(
			int(st::historyBubbleMaxWidth),
			column - 2 * st::historySideSkip - st::historyFarSkip - userpics),
		140);
	layoutBubble(maxWidth);
	const auto left = outgoing()
		? (columnLeft + column - st::historySideSkip - _bubble.width())
		: (columnLeft + st::historySideSkip + userpics);
	const auto shift = QPoint(left, top);
	const auto move = [&](QRect &rect) {
		if (!rect.isNull()) {
			rect.translate(shift);
		}
	};
	move(_bubble);
	move(_senderRect);
	move(_forwardedRect);
	move(_viaRect);
	move(_replyRect);
	move(_mediaRect);
	move(_textRect);
	move(_previewRect);
	move(_metaRect);
	for (auto &chip : _reactions) {
		chip.rect.translate(shift);
	}
	_reactionsTop += top;
	top += _bubble.height();
	if (userpics > 0 && !_flags.attachedToNext) {
		// Next to the last bubble of the run, at its bottom.
		const auto size = int(st::historyGroupUserpic);
		_userpicRect = QRect(
			columnLeft + st::historySideSkip - st::historyGroupUserpicShift,
			_bubble.bottom() + 1 - size,
			size,
			size);
	}
	if (failed()) {
		const auto size = st::historyRetrySize;
		_retryRect = QRect(
			outgoing()
				? (_bubble.x() - st::historyRetrySkip - size)
				: (_bubble.right() + 1 + st::historyRetrySkip),
			_bubble.bottom() + 1 - size - 4,
			size,
			size);
	}
	if (!_keyboard.empty()) {
		layoutKeyboard(top + st::historyKeyboardSkip);
		top = _keyboardBottom;
	}
	_height = top;
}

void Element::layoutService(int width, int top) {
	const auto &padding = st::historyServicePadding;
	const auto &font = st::historyServiceFont;
	const auto limit = std::min(
		int(st::historyServiceMaxWidth),
		width - 2 * st::historySideSkip) - padding.left() - padding.right();
	const auto natural = font->width(_serviceText);
	// The whole text is kept: a wider layout later shows more of it.
	_serviceShown = (natural > limit && limit > 0)
		? font->elided(_serviceText, limit)
		: _serviceText;
	const auto textWidth = std::min(natural, std::max(limit, 1));
	const auto pillWidth = textWidth + padding.left() + padding.right();
	_serviceRect = QRect(
		(width - pillWidth) / 2,
		top,
		pillWidth,
		PillHeight());
	_height = top + PillHeight() + 1;
}

void Element::layoutBubble(int maxWidth) {
	const auto &padding = st::historyBubblePadding;
	const auto hasText = !_text.isEmpty();
	const auto metaW = metaWidth();
	const auto metaH = int(st::historyMetaHeight);
	const auto rtl = style::RightToLeft();
	const auto innerMax = maxWidth - padding.left() - padding.right();
	const auto &overPadding = st::historyMetaOverPadding;
	const auto pillW = metaW + overPadding.left() + overPadding.right();
	const auto pillH = metaH + overPadding.top() + overPadding.bottom();

	// Chips flow from the leading side, wrapping inside `width`. Returns the
	// bottom of the last row and its used width.
	const auto layoutReactions = [&](int left, int top, int width) {
		auto x = 0;
		auto y = top;
		auto lastWidth = 0;
		const auto &chipPadding = st::historyReactionPadding;
		for (auto &chip : _reactions) {
			const auto chipWidth = chipPadding.left()
				+ st::historyReactionEmojiFont->width(chip.emoji)
				+ (chip.count.isEmpty()
					? 0
					: (4 + st::historyReactionCountFont->width(chip.count)))
				+ chipPadding.right();
			if (x > 0 && x + chipWidth > width) {
				x = 0;
				y += st::historyReactionHeight + st::historyReactionSkip;
			}
			chip.rect = QRect(
				left + (rtl ? (width - x - chipWidth) : x),
				y,
				chipWidth,
				st::historyReactionHeight);
			x += chipWidth + st::historyReactionSkip;
			lastWidth = x - st::historyReactionSkip;
		}
		return std::pair(y + st::historyReactionHeight, lastWidth);
	};
	const auto reactionsNatural = [&] {
		auto result = 0;
		const auto &chipPadding = st::historyReactionPadding;
		for (const auto &chip : _reactions) {
			result += chipPadding.left()
				+ st::historyReactionEmojiFont->width(chip.emoji)
				+ (chip.count.isEmpty()
					? 0
					: (4 + st::historyReactionCountFont->width(chip.count)))
				+ chipPadding.right()
				+ st::historyReactionSkip;
		}
		return result ? (result - st::historyReactionSkip) : 0;
	};
	_reactionsTop = 0;

	if (bubbleless()) {
		// Emoji-only text, stickers, round videos: no bubble around them.
		auto top = 0;
		auto width = 0;
		auto replyHeight = 0;
		if (_reply) {
			const auto replyWidth = std::min(_reply->naturalWidth, maxWidth);
			_reply->lines = 1;
			replyHeight = st::historyReplyPadding.top()
				+ (_reply->name.isEmpty() ? 0 : int(st::historyReplyNameHeight))
				+ st::historyReplyTextStyle.lineHeight
				+ st::historyReplyPadding.bottom();
			_replyRect = QRect(0, top, replyWidth, replyHeight);
			top += replyHeight + st::historyReplySkip;
			width = replyWidth;
		}
		auto content = QRect();
		if (_jumbo) {
			const auto textWidth = std::min(_text.maxWidth(), maxWidth);
			_textRect = QRect(0, top, textWidth, _text.countHeight(textWidth));
			content = _textRect;
		} else {
			const auto size = _media->countSize(maxWidth, false);
			_mediaRect = QRect(0, top, size.width(), size.height());
			content = _mediaRect;
		}
		width = std::max(width, content.width());
		top = content.bottom() + 1;
		auto reactionsBottom = top;
		auto reactionsWidth = 0;
		if (!_reactions.empty()) {
			reactionsWidth = std::min(reactionsNatural(), maxWidth);
			width = std::max(width, reactionsWidth);
		}
		width = std::max(width, pillW);
		// Everything hugs the sender's side of the row.
		const auto align = [&](QRect &rect) {
			if (outgoing() && !rect.isNull()) {
				rect.moveLeft(width - rect.width());
			}
		};
		align(_replyRect);
		align(_textRect);
		align(_mediaRect);
		content = _jumbo ? _textRect : _mediaRect;
		// The time is a pill at the bottom corner.
		_metaOverMedia = true;
		_metaOutside = (_jumbo != 0);
		if (_jumbo) {
			_metaRect = QRect(
				outgoing() ? (width - pillW) : 0,
				top,
				pillW,
				pillH);
			top += pillH;
		} else {
			_metaRect = QRect(
				outgoing() ? (width - pillW) : (content.right() + 1 - pillW),
				content.bottom() + 1 - pillH,
				pillW,
				pillH);
			if (_metaRect.x() < 0) {
				_metaRect.moveLeft(0);
			}
		}
		if (!_reactions.empty()) {
			_reactionsTop = top + st::historyReactionTop;
			const auto result = layoutReactions(
				outgoing() ? (width - reactionsWidth) : 0,
				_reactionsTop,
				reactionsWidth);
			reactionsBottom = result.first;
			top = reactionsBottom;
		}
		_bubble = QRect(0, 0, width, top);
		return;
	}

	const auto fullBleed = _media && _media->fullBleed();
	const auto mediaPad = int(st::historyMediaPadding);
	auto mediaSize = QSize();
	auto inner = 0;
	auto bubbleWidth = 0;
	const auto named = showsSenderName();
	const auto hasHeader = named
		|| !_forwardedText.isEmpty()
		|| !_viaText.isEmpty()
		|| _reply;
	// «Caption above»: the text of a photo / video goes over the picture.
	const auto textAbove = hasText
		&& fullBleed
		&& _data.captionAbove
		&& !_data.underReview;
	const auto textIsLast = hasText
		&& !textAbove
		&& !_preview
		&& _reactions.empty();
	auto metaInline = false;
	auto lastRtl = rtl;
	if (fullBleed) {
		mediaSize = _media->countSize(maxWidth, hasText || hasHeader);
		bubbleWidth = mediaSize.width() + 2 * mediaPad;
		inner = bubbleWidth - padding.left() - padding.right();
	} else {
		if (_media) {
			mediaSize = _media->countSize(innerMax, hasText);
		}
		auto others = std::max(
			int(st::historyBubbleMinWidth) - padding.left() - padding.right(),
			mediaSize.width());
		if (_reply) {
			others = std::max(
				others,
				std::min(
					_reply->naturalWidth,
					std::min(innerMax, int(st::historyReplyMaxWidth))));
		}
		if (named) {
			others = std::max(
				others,
				std::min(
					st::historyGroupNameFont->width(_sender.name)
						+ (_sender.tag.isEmpty()
							? 0
							: (st::historyGroupTagSkip
								+ st::historyGroupTagFont->width(_sender.tag))),
					innerMax));
		}
		if (!_forwardedText.isEmpty()) {
			others = std::max(
				others,
				std::min(
					st::historyForwardedFont->width(_forwardedText)
						+ st::historyForwardedIcon.width()
						+ 5,
					innerMax));
		}
		if (!_viaText.isEmpty()) {
			others = std::max(
				others,
				std::min(st::historyForwardedFont->width(_viaText), innerMax));
		}
		if (_preview) {
			others = std::max(
				others,
				std::min(innerMax, int(st::historyPreviewWidth)));
		}
		if (!_reactions.empty()) {
			others = std::max(others, std::min(reactionsNatural(), innerMax));
		}
		if (!hasText) {
			// The time needs room next to nothing but itself.
			others = std::max(others, metaW);
		}
		inner = std::min(
			std::max(others, hasText ? _text.maxWidth() : 0),
			innerMax);
		if (hasText && _text.maxWidth() > inner) {
			inner = std::max(others, _text.countMaxLineWidth(inner));
		}
	}
	if (textIsLast) {
		const auto last = _text.countLastLine(inner);
		lastRtl = last.rtl;
		const auto need = last.width + st::historyMetaSkip + metaW;
		if (need <= inner) {
			metaInline = true;
		} else if (!fullBleed
			&& need <= innerMax
			&& _text.countLines(inner) == 1) {
			inner = need;
			metaInline = true;
		}
	} else if (hasText) {
		lastRtl = _text.countLastLine(inner).rtl;
	}
	if (!fullBleed) {
		bubbleWidth = inner + padding.left() + padding.right();
	}

	const auto left = padding.left();
	auto top = (fullBleed && !hasHeader && !textAbove)
		? mediaPad
		: padding.top();
	if (named) {
		_senderRect = QRect(left, top, inner, st::historyGroupNameHeight);
		top += st::historyGroupNameHeight;
		// The role gives way to the name, the name to the bubble's width.
		const auto tagWidth = _sender.tag.isEmpty()
			? 0
			: st::historyGroupTagFont->width(_sender.tag);
		const auto withTag = (tagWidth > 0)
			&& (tagWidth + st::historyGroupTagSkip <= inner / 2);
		_senderTag = withTag ? _sender.tag : QString();
		_senderName = st::historyGroupNameFont->elided(
			_sender.name,
			inner - (withTag ? (tagWidth + st::historyGroupTagSkip) : 0));
	}
	if (!_forwardedText.isEmpty()) {
		_forwardedRect = QRect(left, top, inner, st::historyForwardedHeight);
		top += st::historyForwardedHeight;
	}
	if (!_viaText.isEmpty()) {
		_viaRect = QRect(left, top, inner, st::historyForwardedHeight);
		top += st::historyForwardedHeight;
	}
	if (_reply) {
		const auto textWidth = inner
			- st::historyReplyBar
			- st::historyReplyPadding.left()
			- st::historyReplyPadding.right()
			- (_reply->quote ? (st::historyQuoteIcon.width() + 6) : 0);
		_reply->lines = _reply->quote
			? std::clamp(
				_reply->text.countLines(std::max(textWidth, 1)),
				1,
				int(st::historyQuoteLines))
			: 1;
		const auto height = st::historyReplyPadding.top()
			+ (_reply->name.isEmpty() ? 0 : int(st::historyReplyNameHeight))
			+ _reply->lines * st::historyReplyTextStyle.lineHeight
			+ st::historyReplyPadding.bottom();
		if (hasHeader && top > padding.top()) {
			top += 2;
		}
		_replyRect = QRect(left, top, inner, height);
		top += height + st::historyReplySkip;
	}
	auto mediaIsLast = false;
	if (textAbove) {
		const auto height = _text.countHeight(inner);
		_textRect = QRect(left, top, inner, height);
		top += height + int(st::historyMediaCaptionSkip);
	}
	if (_media) {
		if (fullBleed) {
			if (hasHeader && !textAbove) {
				top += 1;
			}
			_mediaRect = QRect(
				mediaPad,
				top,
				bubbleWidth - 2 * mediaPad,
				mediaSize.height());
			top += mediaSize.height();
			top += (hasText && !textAbove) ? int(st::historyMediaCaptionSkip) : 0;
		} else {
			_mediaRect = QRect(left, top + 1, inner, mediaSize.height());
			top += mediaSize.height() + 1;
			top += hasText ? 6 : 0;
		}
		mediaIsLast = (!hasText || textAbove) && !_preview && _reactions.empty();
	}
	if (hasText && !textAbove) {
		const auto height = _text.countHeight(inner);
		_textRect = QRect(left, top, inner, height);
		top += height;
	}
	if (_preview) {
		top += st::historyPreviewSkip;
		const auto &previewPadding = st::historyPreviewPadding;
		_preview->thumb = _preview->image ? int(st::historyPreviewThumb) : 0;
		_preview->textWidth = inner
			- st::historyReplyBar
			- previewPadding.left()
			- previewPadding.right()
			- (_preview->thumb
				? (_preview->thumb + st::historyPreviewThumbSkip)
				: 0);
		const auto textWidth = std::max(_preview->textWidth, 1);
		_preview->titleHeight = _preview->title.isEmpty()
			? 0
			: _preview->title.countHeight(textWidth, st::historyPreviewTitleLines);
		_preview->descriptionHeight = _preview->description.isEmpty()
			? 0
			: _preview->description.countHeight(
				textWidth,
				st::historyPreviewTextLines);
		const auto textHeight = (_preview->site.isEmpty()
			? 0
			: (st::historyPreviewSiteFont->height + 1))
			+ _preview->titleHeight
			+ _preview->descriptionHeight;
		const auto height = previewPadding.top()
			+ std::max(textHeight, _preview->thumb)
			+ previewPadding.bottom();
		_previewRect = QRect(left, top, inner, height);
		top += height;
	}
	auto metaTop = 0;
	auto metaAtLeft = rtl;
	if (!_reactions.empty()) {
		_reactionsTop = top + st::historyReactionTop;
		const auto result = layoutReactions(left, _reactionsTop, inner);
		top = result.first;
		if (result.second + st::historyMetaSkip + metaW <= inner) {
			// The time shares the last row of chips, on the other side.
			metaTop = top - metaH - 2;
			metaAtLeft = rtl;
		} else {
			top += 2;
			metaTop = top;
			top += metaH;
			metaAtLeft = rtl;
		}
	} else if (textIsLast) {
		metaAtLeft = lastRtl;
		if (metaInline) {
			metaTop = top - metaH - 2;
		} else {
			metaTop = top + 1;
			top += metaH + 1;
		}
	} else if (_preview) {
		top += 3;
		metaTop = top;
		top += metaH;
		metaAtLeft = lastRtl;
	} else if (mediaIsLast && fullBleed) {
		_metaOverMedia = true;
		_metaRect = QRect(
			_mediaRect.right() + 1 - pillW - st::historyMetaOverMargin,
			_mediaRect.bottom() + 1 - pillH - st::historyMetaOverMargin,
			pillW,
			pillH);
		top += mediaPad;
	} else if (mediaIsLast && _media->hasMetaCorner()) {
		// The row left its bottom corner on the trailing side free.
		metaTop = top - metaH + 1;
		metaAtLeft = rtl;
	} else {
		top += 2;
		metaTop = top;
		top += metaH;
		metaAtLeft = rtl;
	}
	if (!_metaOverMedia) {
		_metaRect = QRect(
			metaAtLeft ? left : (left + inner - metaW),
			metaTop,
			metaW,
			metaH);
		top += padding.bottom();
	}
	_bubble = QRect(0, 0, bubbleWidth, top);
}

void Element::layoutKeyboard(int top) {
	const auto width = std::max(
		_bubble.width(),
		std::min(int(st::historyKeyboardMinWidth), _width - 2 * st::historySideSkip));
	const auto left = outgoing()
		? (_bubble.right() + 1 - width)
		: _bubble.x();
	auto rows = 0;
	for (const auto &button : _keyboard) {
		rows = std::max(rows, button.row + 1);
	}
	auto y = top;
	for (auto row = 0; row != rows; ++row) {
		auto count = 0;
		for (const auto &button : _keyboard) {
			if (button.row == row) {
				++count;
			}
		}
		if (!count) {
			continue;
		}
		const auto skip = int(st::historyKeyboardSkip);
		const auto buttonWidth = (width - (count - 1) * skip) / count;
		for (auto &button : _keyboard) {
			if (button.row != row) {
				continue;
			}
			// Columns follow the reading direction.
			const auto index = style::RightToLeft()
				? (count - 1 - button.column)
				: button.column;
			button.rect = QRect(
				left + index * (buttonWidth + skip),
				y,
				(index == count - 1)
					? (width - index * (buttonWidth + skip))
					: buttonWidth,
				st::historyKeyboardHeight);
		}
		y += st::historyKeyboardHeight + skip;
	}
	_keyboardBottom = y - st::historyKeyboardSkip;
}

void Element::paint(Painter &p, const PaintContext &context) {
	if (!_loaded || _dirty) {
		const auto was = _height;
		layout(_width);
		if (_height != was) {
			_delegate->elementRelayout(this);
		}
	}
	const auto &colors = _delegate->elementColors();
	const auto clip = context.clip;
	if (_dateTop >= 0 && clip.y() < _dateBottom && context.dateOpacity > 0.) {
		p.setOpacity(context.dateOpacity);
		paintDate(p);
		p.setOpacity(1.);
	}
	if (_unreadTop >= 0) {
		paintUnread(p);
	}
	const auto content = contentRect();
	if (!clip.intersects(content)) {
		return;
	}
	if (context.selected) {
		// The whole row is tinted, lighter than selected text.
		p.setOpacity(0.55);
		p.fillRect(content, st::historySelectedBg);
		p.setOpacity(1.);
	} else if (context.highlight > 0. && context.highlightRange.empty()) {
		p.setOpacity(context.highlight * 0.55);
		p.fillRect(content, st::historyHighlightBg);
		p.setOpacity(1.);
	}
	if (_service) {
		paintService(p);
		return;
	}
	if (!bubbleless()) {
		paintBubble(p, colors);
	}
	if (!_senderRect.isNull() || !_userpicRect.isNull()) {
		paintSender(p, colors);
	}
	const auto accent = outgoing() ? colors.accentOut : colors.accentIn;
	const auto rtl = style::RightToLeft();
	if (!_forwardedRect.isNull()) {
		const auto &icon = st::historyForwardedIcon;
		const auto iconLeft = rtl
			? (_forwardedRect.right() + 1 - icon.width())
			: _forwardedRect.x();
		icon.paintInCenter(
			p,
			QRect(iconLeft, _forwardedRect.y(), icon.width(), _forwardedRect.height() - 2),
			accent);
		p.setPen(accent);
		p.setFont(st::historyForwardedFont);
		const auto available = _forwardedRect.width() - icon.width() - 5;
		const auto text = st::historyForwardedFont->elided(_forwardedText, available);
		const auto textWidth = st::historyForwardedFont->width(text);
		p.drawText(
			rtl ? (iconLeft - 5 - textWidth) : (iconLeft + icon.width() + 5),
			_forwardedRect.y() + st::historyForwardedFont->ascent,
			text);
	}
	if (!_viaRect.isNull()) {
		p.setPen(accent);
		p.setFont(st::historyForwardedFont);
		const auto text = st::historyForwardedFont->elided(_viaText, _viaRect.width());
		const auto textWidth = st::historyForwardedFont->width(text);
		p.drawText(
			rtl ? (_viaRect.right() + 1 - textWidth) : _viaRect.x(),
			_viaRect.y() + st::historyForwardedFont->ascent,
			text);
	}
	if (_reply) {
		paintReply(p, colors);
	}
	if (_media && !_mediaRect.isNull() && clip.intersects(_mediaRect)) {
		auto media = MediaPaint();
		media.colors = &colors;
		media.outgoing = outgoing();
		media.hasCaption = !_text.isEmpty();
		media.selected = context.selected;
		media.hovered = context.hovered;
		if (_media->fullBleed()) {
			const auto big = int(st::historyMediaRadius);
			const auto small = int(st::historyMediaRadiusSmall);
			const auto top = (_mediaRect.y() - _bubble.y() <= st::historyMediaPadding + 1);
			const auto bottom = (_bubble.bottom() - _mediaRect.bottom()
				<= st::historyMediaPadding + 1);
			const auto senderTop = (_flags.attachedToPrevious ? small : big);
			const auto senderBottom = small;
			media.radii[0] = top ? (outgoing() ? big : senderTop) : small;
			media.radii[1] = top ? (outgoing() ? senderTop : big) : small;
			media.radii[2] = bottom ? (outgoing() ? senderBottom : big) : small;
			media.radii[3] = bottom ? (outgoing() ? big : senderBottom) : small;
		}
		_media->paint(p, _mediaRect, media);
	}
	if (!_text.isEmpty() && clip.intersects(_textRect)) {
		auto geometry = textGeometry();
		geometry.selection = context.textSelection;
		geometry.hoveredLink = (context.hovered == Zone::Link)
			? context.hoveredLink
			: -1;
		geometry.clip = clip;
		if (context.highlight > 0. && !context.highlightRange.empty()) {
			// The quoted part flashes, not the whole row.
			if (geometry.selection.empty()) {
				// The range is in the message text; the shown one has
				// direction marks around its links.
				geometry.selection = Ui::Text::IsolatedSelection(
					_text.toString(),
					context.highlightRange);
			}
		}
		p.setPen(bubbleless()
			? st::textPrimary->c
			: outgoing()
			? colors.textOut
			: colors.textIn);
		_text.draw(p, geometry);
	}
	if (_preview) {
		paintPreview(p, colors);
	}
	if (!_reactions.empty()) {
		paintReactions(p, colors, context);
	}
	paintMeta(p, colors);
	if (!_keyboard.empty()) {
		paintKeyboard(p, context);
	}
	if (!_retryRect.isNull()) {
		paintRetry(p);
	}
}

void Element::paintDate(Painter &p) const {
	const auto &padding = st::historyServicePadding;
	const auto width = st::historyServiceFont->width(_dateText)
		+ padding.left()
		+ padding.right();
	PaintPill(
		p,
		QRect((_width - width) / 2, _dateTop, width, PillHeight()),
		_dateText);
}

void Element::paintUnread(Painter &p) const {
	const auto rect = QRect(0, _unreadTop, _width, st::historyUnreadHeight);
	p.fillRect(rect, ServiceBg());
	p.setPen(st::historyUnreadFg);
	p.setFont(st::historyServiceFont);
	p.drawText(rect, Qt::AlignCenter, tr::lng_chat_unread_messages(tr::now));
}

void Element::paintService(Painter &p) const {
	PaintPill(p, _serviceRect, _serviceShown);
}

void Element::paintBubble(Painter &p, const ChatColors &colors) const {
	auto hq = PainterHighQualityEnabler(p);
	const auto big = double(st::historyBubbleRadius);
	const auto small = double(st::historyBubbleRadiusSmall);
	const auto rect = QRectF(_bubble);
	const auto tail = !_flags.attachedToNext;
	const auto senderTop = _flags.attachedToPrevious ? small : big;
	const auto senderBottom = tail ? 0. : small;
	auto path = outgoing()
		? BubblePath(rect, big, senderTop, senderBottom, big)
		: BubblePath(rect, senderTop, big, big, senderBottom);
	if (tail) {
		path = path.united(TailPath(rect, outgoing()));
	}
	if (colors.shadow.alpha() > 0) {
		p.fillPath(path.translated(0., 1.), colors.shadow);
	}
	p.fillPath(path, outgoing() ? colors.bubbleOut : colors.bubbleIn);
}

bool Element::showsSenderName() const {
	return _group
		&& !_sender.name.isEmpty()
		&& !_flags.attachedToPrevious
		&& !bubbleless();
}

int Element::userpicSpace() const {
	return _group
		? int(st::historyGroupUserpic + st::historyGroupUserpicSkip)
		: 0;
}

void Element::paintSender(Painter &p, const ChatColors &colors) const {
	if (!_userpicRect.isNull()) {
		_delegate->elementPaintUserpic(p, _userpicRect, _sender.userId);
	}
	if (_senderRect.isNull()) {
		return;
	}
	const auto rtl = style::RightToLeft();
	// One of eight colours by the person's id, pulled towards the text
	// colour of the bubble so it reads on a light and on a dark one.
	auto color = Ui::UserpicColor(_sender.colorIndex)->c;
	color = (colors.bubbleIn.lightness() > 127)
		? color.darker(135)
		: color.lighter(120);
	const auto &font = st::historyGroupNameFont;
	const auto nameWidth = font->width(_senderName);
	p.setPen(color);
	p.setFont(font);
	p.drawText(
		rtl ? (_senderRect.right() + 1 - nameWidth) : _senderRect.x(),
		_senderRect.y() + font->ascent,
		_senderName);
	if (!_senderTag.isEmpty()) {
		const auto &tagFont = st::historyGroupTagFont;
		const auto tagWidth = tagFont->width(_senderTag);
		p.setPen(colors.metaIn);
		p.setFont(tagFont);
		p.drawText(
			rtl ? _senderRect.x() : (_senderRect.right() + 1 - tagWidth),
			_senderRect.y() + font->ascent,
			_senderTag);
	}
}

void Element::paintReply(Painter &p, const ChatColors &colors) const {
	const auto rtl = style::RightToLeft();
	const auto rect = _replyRect;
	const auto &padding = st::historyReplyPadding;
	const auto accent = outgoing() ? colors.accentOut : colors.accentIn;
	{
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		path.addRoundedRect(
			QRectF(rect),
			st::historyReplyRadius,
			st::historyReplyRadius);
		if (bubbleless()) {
			// Over the wallpaper: a small bubble of its own.
			p.fillPath(path, outgoing() ? colors.bubbleOut : colors.bubbleIn);
		}
		p.fillPath(path, outgoing() ? colors.tintOut : colors.tintIn);
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
	const auto quoteWidth = _reply->quote
		? (st::historyQuoteIcon.width() + 6)
		: 0;
	const auto textWidth = rect.width()
		- st::historyReplyBar
		- padding.left()
		- padding.right()
		- quoteWidth;
	const auto textLeft = rtl
		? (rect.x() + padding.right() + quoteWidth)
		: (rect.x() + st::historyReplyBar + padding.left());
	auto top = rect.y() + padding.top();
	if (_reply->quote) {
		const auto &icon = st::historyQuoteIcon;
		icon.paintInCenter(
			p,
			QRect(
				rtl ? (rect.x() + padding.right() - 2) : (rect.right() - padding.right() - icon.width() + 2),
				rect.y() + padding.top(),
				icon.width(),
				icon.height()),
			accent);
	}
	if (!_reply->name.isEmpty()) {
		p.setPen(accent);
		p.setFont(st::historyReplyNameFont);
		const auto name = st::historyReplyNameFont->elided(_reply->name, textWidth);
		const auto nameWidth = st::historyReplyNameFont->width(name);
		p.drawText(
			rtl ? (textLeft + textWidth - nameWidth) : textLeft,
			top + st::historyReplyNameFont->ascent,
			name);
		top += st::historyReplyNameHeight;
	}
	auto geometry = Ui::Text::PaintContext();
	geometry.position = QPoint(textLeft, top);
	geometry.availableWidth = std::max(textWidth, 1);
	geometry.elisionLines = _reply->lines;
	geometry.autoDirection = _reply->quote;
	p.setPen(outgoing()
		? colors.metaOut
		: colors.metaIn);
	if (bubbleless() || !outgoing()) {
		p.setPen(st::historyReplyTextFg);
	}
	_reply->text.draw(p, geometry);
}

void Element::paintPreview(Painter &p, const ChatColors &colors) const {
	const auto rtl = style::RightToLeft();
	const auto rect = _previewRect;
	const auto &padding = st::historyPreviewPadding;
	const auto accent = outgoing() ? colors.accentOut : colors.accentIn;
	{
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		path.addRoundedRect(
			QRectF(rect),
			st::historyReplyRadius,
			st::historyReplyRadius);
		p.fillPath(path, outgoing() ? colors.tintOut : colors.tintIn);
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
	const auto thumb = _preview->thumb;
	const auto textWidth = std::max(_preview->textWidth, 1);
	const auto textLeft = rtl
		? (rect.x() + padding.right() + (thumb ? (thumb + st::historyPreviewThumbSkip) : 0))
		: (rect.x() + st::historyReplyBar + padding.left());
	auto top = rect.y() + padding.top();
	if (!_preview->site.isEmpty()) {
		p.setPen(accent);
		p.setFont(st::historyPreviewSiteFont);
		const auto site = st::historyPreviewSiteFont->elided(_preview->site, textWidth);
		const auto siteWidth = st::historyPreviewSiteFont->width(site);
		p.drawText(
			rtl ? (textLeft + textWidth - siteWidth) : textLeft,
			top + st::historyPreviewSiteFont->ascent,
			site);
		top += st::historyPreviewSiteFont->height + 1;
	}
	auto geometry = Ui::Text::PaintContext();
	geometry.availableWidth = textWidth;
	geometry.autoDirection = true;
	if (_preview->titleHeight) {
		geometry.position = QPoint(textLeft, top);
		geometry.elisionLines = st::historyPreviewTitleLines;
		p.setPen(outgoing() ? colors.textOut : colors.textIn);
		_preview->title.draw(p, geometry);
		top += _preview->titleHeight;
	}
	if (_preview->descriptionHeight) {
		geometry.position = QPoint(textLeft, top);
		geometry.elisionLines = st::historyPreviewTextLines;
		p.setPen(outgoing() ? colors.metaOut : st::historyReplyTextFg->c);
		_preview->description.draw(p, geometry);
	}
	if (thumb && _preview->image) {
		const auto box = QRect(
			rtl
				? (rect.x() + padding.right())
				: (rect.right() + 1 - padding.right() - thumb),
			rect.y() + padding.top(),
			thumb,
			thumb);
		const auto ratio = _delegate->elementRatio();
		const auto image = _preview->image->request(box.size() * ratio);
		const auto radius = int(st::historyPreviewThumbRadius);
		const int radii[4] = { radius, radius, radius, radius };
		if (image.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyMediaBg);
			p.drawRoundedRect(box, radius, radius);
		} else {
			PaintCover(p, box, image, radii, false, _preview->prepared);
		}
	}
}

void Element::paintReactions(
		Painter &p,
		const ChatColors &colors,
		const PaintContext &context) const {
	const auto outside = bubbleless();
	const auto &padding = st::historyReactionPadding;
	const auto rtl = style::RightToLeft();
	for (const auto &chip : _reactions) {
		const auto hovered = (context.hovered == Zone::Reaction)
			&& (context.hoveredText == chip.emoji);
		auto bg = chip.mine
			? (outgoing() ? colors.accentOut : colors.accentIn)
			: outside
			? st::historyReactionOutsideBg->c
			: outgoing()
			? colors.tintOut
			: colors.tintIn;
		if (hovered && !chip.mine) {
			bg.setAlphaF(std::min(bg.alphaF() * 1.7f, 1.f));
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(bg);
			p.drawRoundedRect(
				chip.rect,
				chip.rect.height() / 2.,
				chip.rect.height() / 2.);
		}
		const auto emojiWidth = st::historyReactionEmojiFont->width(chip.emoji);
		const auto emojiLeft = rtl
			? (chip.rect.right() + 1 - padding.left() - emojiWidth)
			: (chip.rect.x() + padding.left());
		p.setFont(st::historyReactionEmojiFont);
		p.setPen(st::textPrimary);
		p.drawText(
			emojiLeft,
			chip.rect.y()
				+ (chip.rect.height() - st::historyReactionEmojiFont->height) / 2
				+ st::historyReactionEmojiFont->ascent,
			chip.emoji);
		if (!chip.count.isEmpty()) {
			const auto countWidth = st::historyReactionCountFont->width(chip.count);
			p.setFont(st::historyReactionCountFont);
			p.setPen(chip.mine
				? st::historyReactionMineFg->c
				: outside
				? st::historyReactionOutsideFg->c
				: outgoing()
				? colors.metaOut
				: colors.accentIn);
			p.drawText(
				rtl ? (emojiLeft - 4 - countWidth) : (emojiLeft + emojiWidth + 4),
				chip.rect.y()
					+ (chip.rect.height() - st::historyReactionCountFont->height) / 2
					+ st::historyReactionCountFont->ascent,
				chip.count);
		}
	}
}

void Element::paintMeta(Painter &p, const ChatColors &colors) const {
	if (_metaRect.isNull()) {
		return;
	}
	auto color = failed()
		? st::historyMetaFgFailed->c
		: outgoing()
		? colors.metaOut
		: colors.metaIn;
	auto inner = _metaRect;
	if (_metaOverMedia) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyMetaOverBg);
		p.drawRoundedRect(
			_metaRect,
			_metaRect.height() / 2.,
			_metaRect.height() / 2.);
		inner = _metaRect.marginsRemoved(st::historyMetaOverPadding);
		color = failed() ? st::accentHeart->c : st::historyMetaOverFg->c;
	}
	const auto &font = st::historyMetaFont;
	p.setFont(font);
	p.setPen(color);
	p.drawText(
		inner.x(),
		inner.y() + (inner.height() - font->height) / 2 + font->ascent,
		_metaText);
	if (!outgoing() || failed()) {
		return;
	}
	const auto icon = QRect(
		inner.right() + 1 - st::historyTickSize,
		inner.y() + (inner.height() - st::historyTickSize) / 2,
		st::historyTickSize,
		st::historyTickSize);
	switch (_data.status) {
	case Data::MessageStatus::Queued:
	case Data::MessageStatus::Sending:
		st::historySendingIcon.paintInCenter(p, icon, color);
		break;
	case Data::MessageStatus::Read:
		st::historyReadIcon.paintInCenter(
			p,
			icon,
			_metaOverMedia ? color : colors.accentOut);
		break;
	case Data::MessageStatus::Sent:
	case Data::MessageStatus::Failed:
		st::historySentIcon.paintInCenter(p, icon, color);
		break;
	}
}

void Element::paintKeyboard(Painter &p, const PaintContext &context) const {
	const auto &font = st::historyKeyboardFont;
	for (const auto &button : _keyboard) {
		const auto hovered = (context.hovered == Zone::Button)
			&& (context.hoveredRow == button.row)
			&& (context.hoveredColumn == button.column);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(hovered ? st::historyKeyboardBgOver : st::historyKeyboardBg);
			p.drawRoundedRect(
				button.rect,
				st::historyKeyboardRadius,
				st::historyKeyboardRadius);
		}
		if (_delegate->elementButtonLoading(_data, button.row, button.column)) {
			auto hq = PainterHighQualityEnabler(p);
			const auto size = 16;
			Ui::InfiniteRadialAnimation::DrawFrame(
				p,
				QRect(
					button.rect.x() + (button.rect.width() - size) / 2,
					button.rect.y() + (button.rect.height() - size) / 2,
					size,
					size),
				2.,
				st::historyKeyboardFg->c,
				Ui::Now());
			continue;
		}
		p.setPen(st::historyKeyboardFg);
		p.setFont(font);
		const auto text = font->elided(button.text, button.rect.width() - 20);
		p.drawText(button.rect, Qt::AlignCenter, text);
		if (button.url) {
			const auto &icon = st::historyKeyboardUrlIcon;
			icon.paintInCenter(
				p,
				QRect(
					button.rect.right() - icon.width() - 4,
					button.rect.y() + 4,
					icon.width(),
					icon.height()));
		}
	}
}

void Element::paintRetry(Painter &p) const {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::historyRetryBg);
	p.drawEllipse(_retryRect);
	// An exclamation mark.
	const auto center = QPointF(_retryRect.center()) + QPointF(0.5, 0.5);
	p.setPen(QPen(st::staticWhite->c, 2.2, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(center + QPointF(0., -5.), center + QPointF(0., 1.5));
	p.drawPoint(center + QPointF(0., 5.5));
}

HitResult Element::hitTest(QPoint point) const {
	auto result = HitResult();
	if (point.y() < 0 || point.y() >= _height) {
		return result;
	} else if (point.y() < _contentTop) {
		result.zone = Zone::Date;
		return result;
	} else if (_service) {
		result.zone = _serviceRect.contains(point) ? Zone::Date : Zone::Side;
		return result;
	} else if (!_loaded) {
		result.zone = _bubble.contains(point) ? Zone::Bubble : Zone::Side;
		return result;
	}
	for (const auto &button : _keyboard) {
		if (button.rect.contains(point)) {
			result.zone = Zone::Button;
			result.row = button.row;
			result.column = button.column;
			return result;
		}
	}
	if (!_retryRect.isNull()
		&& _retryRect.marginsAdded(QMargins(4, 4, 4, 4)).contains(point)) {
		result.zone = Zone::Retry;
		return result;
	}
	for (const auto &chip : _reactions) {
		if (chip.rect.contains(point)) {
			result.zone = Zone::Reaction;
			result.text = chip.emoji;
			return result;
		}
	}
	if (!_sender.userId.isEmpty()) {
		const auto nameWidth = _senderRect.isNull()
			? 0
			: st::historyGroupNameFont->width(_senderName);
		const auto name = _senderRect.isNull()
			? QRect()
			: QRect(
				style::RightToLeft()
					? (_senderRect.right() + 1 - nameWidth)
					: _senderRect.x(),
				_senderRect.y(),
				nameWidth,
				_senderRect.height());
		if (name.contains(point) || _userpicRect.contains(point)) {
			result.zone = Zone::Sender;
			result.text = _sender.userId;
			return result;
		}
	}
	// Text positions are useful for a drag that left the bubble too.
	if (!_text.isEmpty()) {
		const auto state = _text.getState(point, textGeometry());
		result.symbol = state.symbol;
		result.cursor = textCursorAt(point);
		if (_textRect.contains(point)) {
			result.uponSymbol = state.uponSymbol;
			result.captionText = (_media != nullptr);
			if (state.spoiler) {
				result.zone = Zone::Spoiler;
				return result;
			} else if (state.link >= 0) {
				result.zone = Zone::Link;
				result.link = state.link;
				return result;
			}
			result.zone = Zone::Text;
			return result;
		}
	}
	if (!_replyRect.isNull() && _replyRect.contains(point)) {
		result.zone = Zone::Reply;
		return result;
	}
	if (_media && _mediaRect.contains(point)) {
		const auto inside = _media->hitTest(point, _mediaRect);
		if (inside.zone != Zone::None) {
			const auto symbol = result.symbol;
			const auto cursor = result.cursor;
			result = inside;
			result.symbol = symbol;
			result.cursor = cursor;
			return result;
		}
	}
	if (!_previewRect.isNull() && _previewRect.contains(point)) {
		result.zone = Zone::Preview;
		return result;
	}
	if (!_viaRect.isNull() && _viaRect.contains(point)) {
		result.zone = Zone::ViaBot;
		return result;
	}
	if (bubbleless()) {
		result.zone = (_metaRect.contains(point) || _mediaRect.contains(point))
			? Zone::Bubble
			: Zone::Side;
		return result;
	}
	result.zone = _bubble.contains(point) ? Zone::Bubble : Zone::Side;
	return result;
}

} // namespace History::View
