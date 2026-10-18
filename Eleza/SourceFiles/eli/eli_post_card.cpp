#include "eli/eli_post_card.h"

#include "data/data_blurhash.h"
#include "data/data_feed.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "eli/eli_logic.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_audio_player.h"
#include "stories/stories_ring.h"
#include "support/media_image.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "styles/style_eli.h"

#include <QtCore/QDateTime>
#include <QtGui/QPainterPath>

namespace Eli {
namespace {

constexpr auto kFitStep = 160;
constexpr auto kPreviewSide = 32;

[[nodiscard]] QSize RoundedFit(QSize size) {
	const auto round = [](int value) {
		return ((std::max(value, 1) + kFitStep - 1) / kFitStep) * kFitStep;
	};
	return QSize(round(size.width()), round(size.height()));
}

[[nodiscard]] QString Separator() {
	return tr::lng_eli_meta_separator(tr::now);
}

[[nodiscard]] QString CountText(qint64 count) {
	return (count > 0) ? Lang::CompactCount(count) : QString();
}

[[nodiscard]] bool IsVisual(const Data::PostMedia &media) {
	return media.kind == u"image" || media.kind == u"video";
}

void FillRounded(QPainter &p, const QRect &rect, int radius, const QBrush &brush) {
	p.setPen(Qt::NoPen);
	p.setBrush(brush);
	p.drawRoundedRect(rect, radius, radius);
}

void StrokeRounded(QPainter &p, const QRect &rect, int radius, const QColor &color, double width = 1.) {
	auto pen = QPen(color);
	pen.setWidthF(width);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto half = width / 2.;
	p.drawRoundedRect(
		QRectF(rect).adjusted(half, half, -half, -half),
		radius - half,
		radius - half);
}

// `text` cut to `width` only when it really does not fit.
[[nodiscard]] QString Elided(
		const style::font &font,
		const QString &text,
		int width,
		Qt::TextElideMode mode = Qt::ElideRight) {
	return (width <= 0)
		? QString()
		: (font->width(text) <= width)
		? text
		: font->elided(text, width, mode);
}

// Text of one line at the leading side of `rect` (physical), elided.
void DrawLine(
		QPainter &p,
		const QRect &rect,
		const style::font &font,
		const QString &text,
		bool trailing = false) {
	if (rect.width() <= 0 || text.isEmpty()) {
		return;
	}
	const auto width = font->width(text);
	const auto shown = (width > rect.width())
		? font->elided(text, rect.width())
		: text;
	const auto shownWidth = std::min(width, rect.width());
	const auto atRight = (style::RightToLeft() != trailing);
	p.setFont(font);
	p.drawText(
		atRight ? (rect.x() + rect.width() - shownWidth) : rect.x(),
		rect.y() + (rect.height() - font->height) / 2 + font->ascent,
		shown);
}

} // namespace

// A solid play triangle (the icon set only has an outlined one to tint).
void PaintPlayGlyph(QPainter &p, const QRect &circle, const QColor &color) {
	const auto side = circle.width() * 0.36;
	const auto center = QPointF(circle.center()) + QPointF(0.5 + side * 0.08, 0.5);
	auto path = QPainterPath();
	path.moveTo(center.x() - side * 0.42, center.y() - side * 0.56);
	path.lineTo(center.x() + side * 0.56, center.y());
	path.lineTo(center.x() - side * 0.42, center.y() + side * 0.56);
	path.closeSubpath();
	auto pen = QPen(color);
	pen.setWidthF(std::max(side * 0.18, 1.5));
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(color);
	p.drawPath(path);
}

Ui::Text::TextWithEntities PostText(const QString &text, bool links) {
	// Mentions, links and tags keep their own direction inside a paragraph
	// of the other one ("@sara" must not turn into "sara@" in Persian text).
	auto result = Ui::Text::IsolateLinks(
		Ui::Text::ParseEntities(text, Ui::Text::ParseAll));
	if (!links) {
		// Plain (a snippet, a quoted post), but still with the marks.
		result.entities.clear();
	}
	return result;
}

QString IsolatedHandle(const QString &handle) {
	return Lang::Handle(handle);
}

QString AuthorName(Main::Session &session, const QString &userId) {
	auto &users = session.data().users();
	const auto me = (userId == users.myId()) && users.me().valid();
	const auto user = me ? users.me().card() : users.get(userId);
	if (user.deleted) {
		return tr::lng_deleted_account(tr::now);
	}
	return user.displayName.isEmpty()
		? tr::lng_eli_name_loading(tr::now)
		: user.displayName;
}

QString PostTime(qint64 createdAtMs) {
	return Lang::RelativeTime(QDateTime::fromMSecsSinceEpoch(createdAtMs));
}

QString TimeLeft(qint64 endsAtSeconds) {
	const auto left = std::max(
		endsAtSeconds - QDateTime::currentSecsSinceEpoch(),
		qint64(0));
	if (left < 3600) {
		return tr::lng_conv_minutes_left(
			tr::now,
			lt_count,
			double(std::max(left / 60, qint64(1))));
	} else if (left < 86400) {
		return tr::lng_conv_hours_left(tr::now, lt_count, double(left / 3600));
	}
	return tr::lng_conv_days_left(tr::now, lt_count, double(left / 86400));
}

PostCard::PostCard(CardHost *host, const QString &postId, CardOptions options)
: _host(host)
, _options(std::move(options)) {
	_post.id = postId;
	refresh();
}

PostCard::~PostCard() {
	unloadMedia();
}

void PostCard::setOptions(CardOptions options) {
	_options = std::move(options);
	rebuildTexts();
	_layoutWidth = -1;
}

bool PostCard::mine() const {
	return _post.author == _host->cardSession().userId();
}

QRect PostCard::rtl(int x, int y, int w, int h) const {
	return style::rtlrect(x, y, w, h, _width);
}

QRect PostCard::rtl(const QRect &rect) const {
	return style::rtlrect(rect, _width);
}

bool PostCard::refresh() {
	const auto found = _host->cardSession().data().feed().post(_post.id);
	if (!found) {
		if (_post.author.isEmpty()) {
			rebuildTexts();
		}
		return false;
	}
	const auto textChanged = (_post.text != found->text)
		|| (_post.deleted != found->deleted)
		|| (_post.type != found->type)
		|| (_post.edited != found->edited)
		|| (_post.author != found->author)
		|| (_post.media.size() != found->media.size())
		|| (_post.threadParts.size() != found->threadParts.size())
		|| (_post.poll.has_value() != found->poll.has_value())
		|| (_post.quote.get() != found->quote.get())
		|| (_post.canReply != found->canReply);
	const auto pollWas = _pollResults;
	_post = *found;
	rebuildTexts();
	if (textChanged || pollWas != _pollResults) {
		_layoutWidth = -1;
		return true;
	}
	// Counts changed: the action rects stay, their texts are re-measured.
	if (_width > 0) {
		_layoutWidth = -1;
		const auto was = _height;
		resizeGetHeight(_width);
		return (_height != was);
	}
	return false;
}

void PostCard::refreshPeople() {
	rebuildTexts();
	_layoutWidth = -1;
	if (_width > 0) {
		resizeGetHeight(_width);
	}
}

void PostCard::refreshTime() {
	const auto time = PostTime(_post.createdAt);
	if (time != _time) {
		refreshPeople();
	}
}

void PostCard::setFollowState(const QString &state) {
	if (_followState == state) {
		return;
	}
	_followState = state;
	_layoutWidth = -1;
	if (_width > 0) {
		resizeGetHeight(_width);
	}
}

void PostCard::setExpanded(bool expanded) {
	if (_expanded == expanded) {
		return;
	}
	_expanded = expanded;
	_layoutWidth = -1;
}

QString PostCard::audioTrackId() const {
	if (!_post.clip) {
		return QString();
	}
	return Media::AudioTrack::IdForPost(_post.id, _post.clip->media);
}

void PostCard::rebuildTexts() {
	auto &session = _host->cardSession();
	auto &users = session.data().users();
	const auto author = (_post.author == users.myId() && users.me().valid())
		? users.me().card()
		: users.get(_post.author);
	_name = _post.author.isEmpty()
		? tr::lng_eli_name_loading(tr::now)
		: AuthorName(session, _post.author);
	_handle = author.deleted ? QString() : IsolatedHandle(author.handle);
	_verified = author.verified && !author.deleted;
	_time = PostTime(_post.createdAt);

	_contextText = QString();
	if (_options.pinned) {
		_contextText = tr::lng_eli_pinned(tr::now);
	} else if (_options.reason == u"repost" && !_options.actor.isEmpty()) {
		_contextText = tr::lng_eli_user_reposted(
			tr::now,
			lt_name,
			AuthorName(session, _options.actor));
	} else if (_options.reason == u"follows" || _options.reason == u"follow") {
		_contextText = tr::lng_eli_because_follow_them(tr::now);
	}
	_replyingText = QString();
	if (!_options.replyingTo.isEmpty()) {
		const auto parent = users.get(_options.replyingTo);
		if (!parent.handle.isEmpty() && !parent.deleted) {
			_replyingText = tr::lng_eli_replying_to(
				tr::now,
				lt_handle,
				IsolatedHandle(parent.handle));
		}
	}

	const auto &textStyle = detail() ? st::eliDetailTextStyle : st::eliTextStyle;
	_deletedText = _post.deleted;
	_block = Block::None;
	_parts.clear();
	_stats.clear();
	_pollPercents.clear();
	_pollResults = false;
	if (_post.deleted) {
		_text.setText(
			st::eliTextStyle,
			reply()
				? tr::lng_eli_comment_was_deleted(tr::now)
				: tr::lng_eli_post_was_deleted(tr::now));
		rebuildTiles();
		rebuildActions();
		return;
	}
	const auto isInvite = (_post.type == u"conversation_invite");
	auto body = isInvite ? QString() : _post.text;
	if (body.isEmpty() && _post.poll) {
		body = _post.poll->question;
	}
	_text.setMarkedText(textStyle, PostText(body));

	if (isInvite) {
		_block = Block::Invite;
		const auto &conversation = _post.conversation;
		const auto state = conversation ? conversation->state : QString();
		_inviteOpen = (state == u"open" || state == u"waiting");
		_inviteJoin = _inviteOpen && !mine() && conversation;
		_invitePill = _inviteOpen
			? tr::lng_chat_waiting_listener(tr::now)
			: tr::lng_ended(tr::now);
		_inviteLeft = (_inviteOpen && conversation && conversation->expiresAt > 0)
			? TimeLeft(conversation->expiresAt)
			: QString();
		const auto topic = (conversation && !conversation->topic.isEmpty())
			? conversation->topic
			: _post.text;
		_inviteTopic.setText(st::eliInviteTopicStyle, topic);
	} else if (_post.type == u"poll" && _post.poll) {
		_block = Block::Poll;
		const auto &poll = *_post.poll;
		const auto now = QDateTime::currentSecsSinceEpoch();
		_pollClosed = poll.closed || (poll.endsAt > 0 && poll.endsAt <= now);
		_pollResults = (poll.myVote >= 0) || _pollClosed;
		auto votes = poll.votes;
		votes.resize(poll.options.size());
		_pollPercents = Logic::PollPercents(votes);
		auto total = poll.total;
		if (total <= 0) {
			for (const auto value : votes) {
				total += value;
			}
		}
		_pollFooter = tr::lng_eli_poll_total(tr::now, lt_count, double(total))
			+ Separator()
			+ (_pollClosed ? tr::lng_eli_poll_ended(tr::now) : TimeLeft(poll.endsAt));
		_pollRetract = (poll.myVote >= 0 && !_pollClosed)
			? tr::lng_eli_retract_vote(tr::now)
			: QString();
	} else if (_post.type == u"audio" && _post.clip) {
		_block = Block::Audio;
		const auto &clip = *_post.clip;
		_audioTitle = !clip.title.isEmpty()
			? clip.title
			: (clip.source == u"record")
			? tr::lng_eli_voice_note(tr::now)
			: tr::lng_eli_sound(tr::now);
		auto duration = clip.endMs - clip.startMs;
		if (duration <= 0) {
			for (const auto &media : _post.media) {
				if (media.id == clip.media) {
					duration = media.durationMs;
				}
			}
			if (duration <= 0 && !_post.media.isEmpty()) {
				duration = _post.media.front().durationMs;
			}
		}
		const auto length = Lang::Duration(int(std::max(duration, qint64(0)) / 1000));
		_audioSub = clip.artist.isEmpty()
			? length
			: (clip.artist + Separator() + length);
	} else if (_post.type == u"video_note" && !_post.media.isEmpty()) {
		_block = Block::VideoNote;
	} else if (_post.type == u"quote") {
		_block = Block::Quote;
	} else if (_post.type == u"thread" || !_post.threadParts.isEmpty()) {
		_block = Block::Thread;
	} else if (std::any_of(_post.media.cbegin(), _post.media.cend(), IsVisual)) {
		_block = Block::Media;
	}

	if (_block == Block::Quote) {
		const auto &quote = _post.quote;
		_quoteUnavailable = !quote || _post.quoteUnavailable || quote->deleted;
		if (_quoteUnavailable) {
			_quoteText.setText(
				st::eliQuoteTextStyle,
				tr::lng_eli_post_isnt_available(tr::now));
		} else {
			const auto quoted = users.get(quote->author);
			_quoteName = AuthorName(session, quote->author);
			_quoteTime = PostTime(quote->createdAt);
			_quoteMeta = ((quoted.handle.isEmpty() || quoted.deleted)
				? QString()
				: (IsolatedHandle(quoted.handle) + Separator()))
				+ _quoteTime;
			_quoteText.setMarkedText(
				st::eliQuoteTextStyle,
				PostText(quote->text, false));
		}
	}
	if (_block == Block::Thread) {
		auto texts = QStringList{ _post.text };
		for (const auto &part : _post.threadParts) {
			texts.push_back(part.text);
		}
		_threadMeta = tr::lng_eli_thread_meta(
			tr::now,
			lt_parts,
			Lang::Num(texts.size()),
			lt_minutes,
			Lang::Num(Logic::ReadingMinutes(texts)));
		if (detail() && _options.expandThread) {
			for (const auto &part : _post.threadParts) {
				auto entry = Part();
				entry.text.setMarkedText(
					st::eliDetailTextStyle,
					PostText(part.text));
				_parts.push_back(std::move(entry));
			}
		}
	}
	if (detail()) {
		const auto when = QDateTime::fromMSecsSinceEpoch(_post.createdAt);
		_detailMeta = tr::lng_eli_detail_time(
			tr::now,
			lt_date,
			Lang::SolarDate(when.date(), true),
			lt_time,
			Lang::Clock(when));
		if (_post.edited) {
			_detailMeta += Separator() + tr::lng_chat_edited(tr::now);
		}
		if (_post.counts.views > 0) {
			_detailMeta += Separator() + tr::lng_eli_views_count(
				tr::now,
				lt_n,
				Lang::CompactCount(_post.counts.views));
		}
		const auto add = [&](HitType type, qint64 count, const QString &label) {
			if (count <= 0) {
				return;
			}
			auto stat = Stat();
			stat.type = type;
			stat.count = Lang::CompactCount(count);
			stat.label = label;
			_stats.push_back(stat);
		};
		add(HitType::LikesStat, _post.counts.likes, tr::lng_eli_stat_likes(tr::now));
		if (_post.counts.hasDislikes) {
			add(HitType::DislikesStat, _post.counts.dislikes, tr::lng_eli_stat_dislikes(tr::now));
		}
		add(HitType::None, _post.counts.reposts, tr::lng_eli_stat_reposts(tr::now));
		add(HitType::None, _post.counts.quotes, tr::lng_eli_stat_quotes(tr::now));
	}
	rebuildTiles();
	rebuildActions();
}

void PostCard::rebuildTiles() {
	struct Wanted {
		QString postId;
		Data::PostMedia media;
		Data::MediaVariant variant = Data::MediaVariant::Feed;
		bool round = false;
		bool quote = false;
	};
	auto wanted = std::vector<Wanted>();
	const auto addVisual = [&](const Data::Post &post, bool quote) {
		for (const auto &media : post.media) {
			if (!IsVisual(media)) {
				continue;
			}
			auto entry = Wanted();
			entry.postId = post.id;
			entry.media = media;
			entry.variant = (media.kind == u"video")
				? Data::MediaVariant::Poster
				: Data::MediaVariant::Feed;
			entry.quote = quote;
			wanted.push_back(entry);
		}
	};
	auto quoteFrom = 0;
	if (!_post.deleted) {
		switch (_block) {
		case Block::Media:
		case Block::Thread:
			addVisual(_post, false);
			break;
		case Block::Quote:
			addVisual(_post, false);
			quoteFrom = int(wanted.size());
			if (!_quoteUnavailable && _post.quote) {
				addVisual(*_post.quote, true);
			}
			break;
		case Block::VideoNote: {
			auto entry = Wanted();
			entry.postId = _post.id;
			entry.media = _post.media.front();
			for (const auto &media : _post.media) {
				if (media.kind == u"video_note") {
					entry.media = media;
					break;
				}
			}
			entry.variant = Data::MediaVariant::Poster;
			entry.round = true;
			wanted.push_back(entry);
		} break;
		case Block::Audio:
			if (_post.clip && !_post.clip->cover.isEmpty()) {
				auto entry = Wanted();
				entry.postId = _post.id;
				entry.media.id = _post.clip->cover;
				entry.media.kind = u"image"_s;
				entry.variant = Data::MediaVariant::Thumb;
				wanted.push_back(entry);
			}
			break;
		default:
			break;
		}
	}
	if (quoteFrom == 0 && _block != Block::Quote) {
		quoteFrom = int(wanted.size());
	}
	auto same = (wanted.size() == _tiles.size());
	for (auto i = 0; same && i != int(wanted.size()); ++i) {
		same = (wanted[i].postId == _tiles[i].postId)
			&& (wanted[i].media.id == _tiles[i].media.id)
			&& (wanted[i].variant == _tiles[i].variant);
	}
	_quoteTilesFrom = quoteFrom;
	if (same) {
		for (auto i = 0; i != int(wanted.size()); ++i) {
			_tiles[i].media = wanted[i].media;
		}
		return;
	}
	const auto loaded = _mediaLoaded;
	unloadMedia();
	_tiles.clear();
	for (const auto &entry : wanted) {
		auto tile = Tile();
		tile.postId = entry.postId;
		tile.media = entry.media;
		tile.variant = entry.variant;
		tile.round = entry.round;
		tile.quote = entry.quote;
		_tiles.push_back(std::move(tile));
	}
	_layoutWidth = -1;
	if (loaded) {
		_mediaLoaded = true; // handles are requested after the next layout
	}
}

void PostCard::rebuildActions() {
	_actions.clear();
	if (_post.deleted || _post.id.isEmpty()) {
		return;
	}
	const auto add = [&](HitType type, qint64 count, bool active, bool disabled = false) {
		auto action = Action();
		action.type = type;
		action.count = CountText(count);
		action.active = active;
		action.disabled = disabled;
		_actions.push_back(action);
	};
	const auto liked = (_post.my.reaction == u"like");
	const auto disliked = (_post.my.reaction == u"dislike");
	add(HitType::Reply, _post.counts.replies, false, !_post.canReply);
	if (!reply()) {
		add(
			HitType::Repost,
			_post.counts.reposts + _post.counts.quotes,
			_post.my.reposted,
			!_post.canRepost && !_post.my.reposted);
	}
	add(HitType::Like, _post.counts.likes, liked);
	add(
		HitType::Dislike,
		_post.counts.hasDislikes ? _post.counts.dislikes : 0,
		disliked);
	if (!reply()) {
		add(HitType::Bookmark, 0, _post.my.bookmarked);
		add(HitType::Share, 0, false);
	}
}

int PostCard::resizeGetHeight(int width) {
	if (width <= 0) {
		return _height;
	}
	if (_layoutWidth == width) {
		return _height;
	}
	_width = width;
	_layoutWidth = width;
	layout();
	if (_mediaLoaded) {
		for (auto &tile : _tiles) {
			requestTile(tile);
		}
	}
	return _height;
}

void PostCard::addArea(HitType type, const QRect &rect, int index) {
	if (!rect.isEmpty()) {
		_areas.push_back({ Hit{ type, index }, rect });
	}
}

void PostCard::layout() {
	_areas.clear();
	_pollRects.clear();
	_showMoreRect = QRect();
	_moreRect = QRect();
	_followRect = QRect();
	_replyingRect = QRect();
	_contextRect = QRect();
	_blockRect = QRect();
	_threadCtaRect = QRect();
	_inviteButtonRect = QRect();
	_audioPlayRect = QRect();
	_pollRetractRect = QRect();
	_statsRect = QRect();
	_detailMetaRect = QRect();
	_actionsRect = QRect();

	const auto &padding = detail() ? st::eliDetailPadding : st::eliCardPadding;
	const auto nested = reply() && (_options.depth > 0);
	const auto indent = nested ? st::eliReplyIndent : 0;
	const auto userpic = detail()
		? st::eliDetailUserpic
		: nested
		? st::eliReplyUserpicNested
		: st::eliCardUserpic;
	const auto left = padding.left() + indent;
	const auto contentRight = detail() ? padding.right() : st::eliCardPadding.left();
	auto y = padding.top();

	const auto columnLeft = left + userpic + st::eliCardUserpicSkip;
	if (!_contextText.isEmpty()) {
		_contextRect = QRect(
			columnLeft,
			y - 2,
			_width - columnLeft - contentRight,
			st::eliContextHeight);
		y += st::eliContextHeight;
		if (_options.reason == u"repost" && !_options.pinned) {
			addArea(HitType::ContextActor, rtl(_contextRect));
		}
	}
	const auto userpicTop = y;
	_userpicRect = rtl(left, y, userpic, userpic);

	auto contentLeft = 0;
	auto contentWidth = 0;
	const auto moreSize = st::eliMoreSize;
	const auto hasMore = !_post.deleted && !_post.id.isEmpty();
	if (detail()) {
		contentLeft = padding.left();
		contentWidth = _width - padding.left() - padding.right();
		const auto moreLeft = _width - padding.right() - moreSize + 4;
		if (hasMore) {
			_moreRect = rtl(moreLeft, y + (userpic - moreSize) / 2, moreSize, moreSize);
		}
		auto headerRight = moreLeft - st::eliHeaderSkip;
		if (!_followState.isEmpty() && !mine() && hasMore) {
			const auto text = (_followState == u"following")
				? tr::lng_following(tr::now)
				: (_followState == u"requested")
				? tr::lng_eli_follow_requested(tr::now)
				: tr::lng_follow(tr::now);
			const auto followWidth = st::eliDetailFollowFont->width(text)
				+ 2 * st::eliDetailFollowPadding;
			const auto followLeft = headerRight - followWidth - 4;
			_followRect = rtl(
				followLeft,
				y + (userpic - st::eliDetailFollowHeight) / 2,
				followWidth,
				st::eliDetailFollowHeight);
			headerRight = followLeft - 8;
		}
		_headerRect = QRect(columnLeft, y, std::max(headerRight - columnLeft, 0), userpic);
		const auto tick = _verified
			? (st::eliVerifiedIcon.width() + st::eliHeaderSkip)
			: 0;
		_nameWidth = std::min(
			st::eliDetailNameFont->width(_name),
			std::max(_headerRect.width() - tick, 0));
		_nameElided = Elided(st::eliDetailNameFont, _name, _nameWidth);
		_handleWidth = std::min(
			st::eliDetailHandleFont->width(_handle),
			_headerRect.width());
		_handleElided = Elided(st::eliDetailHandleFont, _handle, _handleWidth);
		_handleShown = !_handle.isEmpty();
		y += userpic + st::eliCardUserpicSkip;
	} else {
		contentLeft = columnLeft;
		contentWidth = _width - contentLeft - contentRight;
		const auto moreLeft = _width - padding.right() - moreSize + 4;
		if (hasMore) {
			_moreRect = rtl(moreLeft, y - 4, moreSize, moreSize);
		}
		const auto available = std::max(
			(hasMore ? (moreLeft - st::eliHeaderSkip) : (_width - contentRight))
				- contentLeft,
			0);
		_headerRect = QRect(contentLeft, y, available, st::eliHeaderHeight);
		_timeText = Separator() + _time;
		if (_post.edited && !_post.deleted) {
			_timeText += Separator() + tr::lng_chat_edited(tr::now);
		}
		_timeWidth = st::eliHandleFont->width(_timeText);
		const auto tick = _verified
			? (st::eliVerifiedIcon.width() + st::eliHeaderSkip)
			: 0;
		const auto nameNatural = st::eliNameFont->width(_name);
		const auto handleNatural = _handle.isEmpty()
			? 0
			: st::eliHandleFont->width(_handle);
		if (available - tick - _timeWidth < 48) {
			// Very narrow: the time gives way before the name does.
			_timeWidth = std::max(available - tick - 48, 0);
			_timeText = st::eliHandleFont->elided(_timeText, _timeWidth);
			_timeWidth = st::eliHandleFont->width(_timeText);
		}
		const auto forName = std::max(available - tick - _timeWidth, 0);
		_nameWidth = std::min(nameNatural, forName);
		_nameElided = (nameNatural > _nameWidth)
			? st::eliNameFont->elided(_name, _nameWidth)
			: _name;
		_nameWidth = std::min(st::eliNameFont->width(_nameElided), forName);
		const auto forHandle = forName - _nameWidth - st::eliHeaderSkip;
		_handleShown = (handleNatural > 0) && (forHandle >= handleNatural);
		_handleWidth = _handleShown ? handleNatural : 0;
		_handleElided = _handleShown ? _handle : QString();
		y += st::eliHeaderHeight;
	}
	// The name (and the handle next to it) opens the profile.
	{
		const auto font = detail() ? st::eliDetailNameFont : st::eliNameFont;
		const auto tick = _verified
			? (st::eliVerifiedIcon.width() + st::eliHeaderSkip)
			: 0;
		auto width = _nameWidth + tick;
		if (!detail() && _handleShown) {
			width += st::eliHeaderSkip + _handleWidth;
		}
		_nameRect = rtl(
			_headerRect.x(),
			_headerRect.y(),
			detail() ? std::max(width, _handleWidth) : width,
			detail() ? _headerRect.height() : font->height + 2);
		if (!_post.author.isEmpty()) {
			addArea(HitType::Userpic, _userpicRect);
			addArea(HitType::Author, _nameRect);
		}
	}
	if (hasMore) {
		addArea(HitType::More, _moreRect);
	}
	addArea(HitType::Follow, _followRect);

	if (!_replyingText.isEmpty() && !_post.deleted) {
		_replyingRect = rtl(contentLeft, y, contentWidth, st::eliReplyingHeight);
		const auto width = std::min(
			st::eliReplyingFont->width(_replyingText),
			contentWidth);
		addArea(
			HitType::ReplyingTo,
			rtl(contentLeft, y, width, st::eliReplyingHeight));
		y += st::eliReplyingHeight;
	}

	// The text.
	_textRect = QRect();
	_textLines = 0;
	_textClamped = false;
	if (!_text.isEmpty()) {
		const auto total = _text.countLines(contentWidth);
		const auto clamp = (detail() || _expanded || _post.deleted)
			? Logic::Clamp{ total, false }
			: Logic::ClampLines(
				total,
				st::eliTextClampLines,
				st::eliTextClampMinHidden);
		_textLines = clamp.clamped ? clamp.lines : 0;
		_textClamped = clamp.clamped;
		const auto height = _text.countHeight(contentWidth, _textLines);
		_textRect = rtl(contentLeft, y, contentWidth, height);
		y += height;
		if (_textClamped) {
			const auto label = tr::lng_eli_show_more(tr::now);
			_showMoreRect = rtl(
				contentLeft,
				y,
				st::eliShowMoreFont->width(label),
				st::eliShowMoreHeight);
			addArea(HitType::ShowMore, _showMoreRect);
			y += st::eliShowMoreHeight;
		}
	}

	if (!_post.deleted) {
		const auto blockHeight = layoutBlock(
			contentLeft,
			y + (_text.isEmpty() ? 2 : st::eliBlockSkip),
			contentWidth);
		if (blockHeight > 0) {
			y += (_text.isEmpty() ? 2 : st::eliBlockSkip) + blockHeight;
		}
		if (detail()) {
			y = layoutDetailFooter(contentLeft, y + st::eliBlockSkip, contentWidth);
		} else {
			y = layoutActions(contentLeft, y + st::eliActionsSkip, contentWidth);
		}
	} else {
		y += st::eliBlockSkip;
	}
	y += padding.bottom();
	const auto minimal = userpicTop + userpic + st::eliCardUserpicSkip;
	_height = std::max(y, detail() ? y : minimal);
	_lineTop = userpicTop + userpic + 4;
}

int PostCard::layoutMedia(
		int left,
		int top,
		int width,
		int first,
		int count,
		int maxHeight) {
	if (count <= 0) {
		return 0;
	}
	auto sizes = QVector<QSize>();
	for (auto i = first; i != first + count; ++i) {
		sizes.push_back(QSize(_tiles[i].media.width, _tiles[i].media.height));
	}
	const auto grid = Logic::LayoutMedia(sizes, width, st::eliMediaSkip, maxHeight);
	for (auto i = 0; i != count; ++i) {
		auto &tile = _tiles[first + i];
		if (i < int(grid.tiles.size())) {
			const auto &rect = grid.tiles[i];
			tile.rect = rtl(left + rect.x(), top + rect.y(), rect.width(), rect.height());
			tile.more = (i + 1 == int(grid.tiles.size())) ? grid.more : 0;
			addArea(
				tile.quote ? HitType::QuoteMedia : HitType::Media,
				tile.rect,
				first + i);
		} else {
			tile.rect = QRect();
			tile.more = 0;
		}
		if (tile.preparedFor != tile.rect.size()) {
			tile.prepared = QPixmap();
		}
	}
	return grid.height;
}

int PostCard::layoutPoll(int left, int top, int width) {
	const auto &poll = *_post.poll;
	auto y = top;
	for (auto i = 0; i != int(poll.options.size()); ++i) {
		const auto rect = rtl(left, y, width, st::eliPollOptionHeight);
		_pollRects.push_back(rect);
		if (!_pollResults) {
			addArea(HitType::PollOption, rect, i);
		}
		y += st::eliPollOptionHeight + st::eliPollOptionSkip;
	}
	_pollFooterRect = rtl(left, y - 2, width, st::eliPollFooterHeight);
	if (!_pollRetract.isEmpty()) {
		const auto footerWidth = st::eliPollFooterFont->width(_pollFooter + Separator());
		const auto retractWidth = st::eliPollFooterFont->width(_pollRetract);
		_pollRetractRect = rtl(
			left + footerWidth,
			y - 2,
			std::min(retractWidth, std::max(width - footerWidth, 0)),
			st::eliPollFooterHeight);
		addArea(HitType::PollRetract, _pollRetractRect);
	}
	y += st::eliPollFooterHeight - 2;
	return y - top;
}

int PostCard::layoutQuote(int left, int top, int width) {
	const auto &padding = st::eliQuotePadding;
	const auto innerLeft = left + padding.left();
	const auto innerWidth = width - padding.left() - padding.right();
	auto y = top + padding.top();
	if (_quoteUnavailable) {
		const auto height = _quoteText.countHeight(innerWidth);
		_quoteHeaderRect = QRect();
		_quoteTextRect = rtl(innerLeft, y, innerWidth, height);
		y += height + padding.bottom();
		return y - top;
	}
	_quoteHeaderRect = QRect(innerLeft, y, innerWidth, st::eliQuoteHeaderHeight);
	y += st::eliQuoteHeaderHeight + 2;
	_quoteTextRect = QRect();
	if (!_quoteText.isEmpty()) {
		const auto height = _quoteText.countHeight(innerWidth, st::eliQuoteTextLines);
		_quoteTextRect = rtl(innerLeft, y, innerWidth, height);
		y += height;
	}
	const auto tiles = int(_tiles.size()) - _quoteTilesFrom;
	if (tiles > 0) {
		y += st::eliQuoteMediaSkip;
		y += layoutMedia(
			innerLeft,
			y,
			innerWidth,
			_quoteTilesFrom,
			tiles,
			st::eliQuoteMediaMaxHeight);
	}
	y += padding.bottom();
	return y - top;
}

int PostCard::layoutInvite(int left, int top, int width) {
	const auto &padding = st::eliInvitePadding;
	const auto innerLeft = left + padding.left();
	const auto innerWidth = width - padding.left() - padding.right();
	auto y = top + padding.top() + st::eliInviteTopHeight + st::eliInviteTopicSkip;
	const auto height = _inviteTopic.countHeight(innerWidth, st::eliInviteTopicLines);
	_inviteTopicRect = rtl(innerLeft, y, innerWidth, height);
	y += height;
	if (_inviteJoin) {
		y += st::eliInviteButtonSkip;
		_inviteButtonRect = rtl(innerLeft, y, innerWidth, st::eliInviteButtonHeight);
		addArea(HitType::InviteJoin, _inviteButtonRect);
		y += st::eliInviteButtonHeight;
	}
	y += padding.bottom();
	return y - top;
}

int PostCard::layoutBlock(int left, int top, int width) {
	const auto maxHeight = detail()
		? st::eliMediaMaxHeightDetail
		: st::eliMediaMaxHeight;
	auto height = 0;
	switch (_block) {
	case Block::None:
		return 0;
	case Block::Media:
		height = layoutMedia(left, top, width, 0, int(_tiles.size()), maxHeight);
		break;
	case Block::VideoNote: {
		const auto size = std::min(width, st::eliVideoNoteSize);
		if (!_tiles.empty()) {
			_tiles[0].rect = rtl(left, top, size, size);
			if (_tiles[0].preparedFor != _tiles[0].rect.size()) {
				_tiles[0].prepared = QPixmap();
			}
			addArea(HitType::VideoNote, _tiles[0].rect, 0);
		}
		height = size;
	} break;
	case Block::Poll:
		height = layoutPoll(left, top, width);
		break;
	case Block::Quote: {
		// Own media first, then the quoted post in its frame.
		auto y = top;
		if (_quoteTilesFrom > 0) {
			y += layoutMedia(left, y, width, 0, _quoteTilesFrom, maxHeight);
			y += st::eliBlockSkip;
		}
		const auto quoteTop = y;
		const auto quoteHeight = layoutQuote(left, y, width);
		_blockRect = rtl(left, quoteTop, width, quoteHeight);
		if (!_quoteUnavailable) {
			addArea(HitType::Quote, _blockRect);
		}
		return (quoteTop + quoteHeight) - top;
	}
	case Block::Invite:
		height = layoutInvite(left, top, width);
		break;
	case Block::Audio: {
		height = st::eliAudioHeight;
		const auto play = st::eliAudioPlaySize;
		_audioPlayRect = rtl(
			left + width - st::eliAudioPadding - play,
			top + (height - play) / 2,
			play,
			play);
		if (!_tiles.empty()) {
			_tiles[0].rect = rtl(
				left + st::eliAudioPadding,
				top + (height - st::eliAudioCover) / 2,
				st::eliAudioCover,
				st::eliAudioCover);
			if (_tiles[0].preparedFor != _tiles[0].rect.size()) {
				_tiles[0].prepared = QPixmap();
			}
		}
		addArea(HitType::Audio, rtl(left, top, width, height));
	} break;
	case Block::Thread: {
		auto y = top;
		if (!_tiles.empty()) {
			y += layoutMedia(left, y, width, 0, int(_tiles.size()), maxHeight);
			y += st::eliBlockSkip;
		}
		if (!_parts.empty()) {
			for (auto &part : _parts) {
				y += st::eliThreadPartSkip;
				part.top = y + st::eliThreadNumberSize + 6;
				part.height = part.text.countHeight(width);
				y = part.top + part.height;
			}
			y += st::eliThreadPartSkip;
		}
		_threadCtaRect = rtl(left, y, width, st::eliThreadCtaHeight);
		addArea(HitType::Thread, _threadCtaRect);
		y += st::eliThreadCtaHeight;
		_blockRect = rtl(left, top, width, y - top);
		return y - top;
	}
	}
	_blockRect = rtl(left, top, width, height);
	return height;
}

int PostCard::layoutActions(int left, int top, int width) {
	if (_actions.empty()) {
		return top;
	}
	const auto circle = st::eliActionCircle;
	const auto height = reply() ? st::eliReplyActionsHeight : st::eliActionsHeight;
	const auto circleTop = top + (height - circle) / 2;
	const auto shift = (circle - st::eliIconReply.width()) / 2;
	auto trailing = 0;
	for (const auto &action : _actions) {
		if (action.type == HitType::Bookmark || action.type == HitType::Share) {
			trailing += circle;
		}
	}
	// Every counted action takes what its number needs; the room that is
	// left is shared between them as gaps.
	auto leading = 0;
	auto natural = 0;
	for (auto &action : _actions) {
		if (action.type == HitType::Bookmark || action.type == HitType::Share) {
			continue;
		}
		action.countWidth = action.count.isEmpty()
			? 0
			: st::eliActionCountFont->width(action.count);
		natural += circle + (action.countWidth ? (action.countWidth + 2) : 0);
		++leading;
	}
	const auto room = width + shift - trailing - natural;
	const auto gap = std::clamp(
		room / std::max(leading, 1),
		0,
		int(st::eliActionSlotMax) - circle);
	auto x = left - shift;
	auto right = left + width + shift;
	for (auto i = int(_actions.size()) - 1; i >= 0; --i) {
		auto &action = _actions[i];
		if (action.type != HitType::Bookmark && action.type != HitType::Share) {
			continue;
		}
		right -= circle;
		action.circle = rtl(right, circleTop, circle, circle);
		action.rect = action.circle;
		action.countWidth = 0;
	}
	for (auto &action : _actions) {
		if (action.type == HitType::Bookmark || action.type == HitType::Share) {
			continue;
		}
		action.circle = rtl(x, circleTop, circle, circle);
		const auto full = circle + (action.countWidth ? (action.countWidth + 2) : 0);
		action.rect = rtl(x, circleTop, full, circle);
		x += full + gap;
	}
	for (const auto &action : _actions) {
		addArea(action.type, action.rect);
	}
	_actionsRect = rtl(left, top, width, height);
	return top + height;
}

int PostCard::layoutDetailFooter(int left, int top, int width) {
	auto y = top;
	_detailMetaRect = rtl(left, y, width, st::eliDetailMetaHeight);
	y += st::eliDetailMetaHeight;
	if (!_stats.empty()) {
		_statsRect = rtl(left, y, width, st::eliDetailStatsHeight);
		auto x = left;
		for (auto &stat : _stats) {
			stat.countWidth = st::eliDetailStatCountFont->width(stat.count);
			const auto full = stat.countWidth
				+ st::eliHeaderSkip
				+ st::eliDetailStatLabelFont->width(stat.label);
			stat.rect = rtl(x, y, full, st::eliDetailStatsHeight);
			if (stat.type != HitType::None) {
				addArea(stat.type, stat.rect);
			}
			x += full + st::eliDetailStatsSkip;
		}
		y += st::eliDetailStatsHeight;
	}
	const auto count = int(_actions.size());
	if (count > 0) {
		const auto circle = st::eliActionCircle + 4;
		const auto circleTop = y + (st::eliDetailActionsHeight - circle) / 2;
		const auto slot = width / count;
		for (auto i = 0; i != count; ++i) {
			auto &action = _actions[i];
			action.countWidth = 0;
			action.circle = rtl(
				left + i * slot + (slot - circle) / 2,
				circleTop,
				circle,
				circle);
			action.rect = action.circle;
			addArea(action.type, action.rect);
		}
		_actionsRect = rtl(left, y, width, st::eliDetailActionsHeight);
		y += st::eliDetailActionsHeight;
	}
	return y;
}

// ---- media

void PostCard::loadMedia() {
	_mediaLoaded = true;
	for (auto &tile : _tiles) {
		requestTile(tile);
	}
}

void PostCard::unloadMedia() {
	_mediaLoaded = false;
	for (auto &tile : _tiles) {
		QObject::disconnect(tile.ready);
		QObject::disconnect(tile.failed);
		tile.handle = nullptr;
		tile.prepared = QPixmap();
		tile.preparedFor = QSize();
		tile.preparedFinal = false;
	}
}

void PostCard::requestTile(Tile &tile) {
	if (tile.handle || tile.rect.isEmpty() || tile.media.id.isEmpty()) {
		return;
	}
	auto options = Data::Media::Options();
	options.fit = RoundedFit(tile.rect.size() * _host->cardPixelRatio());
	tile.handle = _host->cardSession().data().media().request(
		Data::MediaRef::PostMedia(tile.postId, tile.media.id),
		tile.variant,
		options);
	if (tile.handle->isReady() || tile.handle->isFailed()) {
		return;
	}
	const auto host = _host;
	const auto id = _post.id;
	const auto raw = tile.handle.get();
	tile.ready = QObject::connect(
		raw,
		&Data::MediaHandle::ready,
		_host->cardGuard(),
		[=] { host->cardRepaint(id); });
	tile.failed = QObject::connect(
		raw,
		&Data::MediaHandle::failed,
		_host->cardGuard(),
		[=] { host->cardRepaint(id); });
}

void PostCard::prepareTile(const Tile &tile) const {
	const auto ready = tile.handle
		&& tile.handle->isReady()
		&& !tile.handle->image().isNull();
	if (!tile.prepared.isNull()
		&& tile.preparedFor == tile.rect.size()
		&& (tile.preparedFinal || !ready)) {
		return;
	}
	auto source = ready ? tile.handle->image() : QImage();
	if (source.isNull()) {
		if (!tile.previewTried) {
			tile.previewTried = true;
			if (!tile.media.blurhash.isEmpty()) {
				const auto aspect = Logic::ClampAspect(
					tile.media.width,
					tile.media.height);
				const auto width = (aspect >= 1.)
					? kPreviewSide
					: std::max(int(kPreviewSide * aspect), 4);
				const auto height = (aspect >= 1.)
					? std::max(int(kPreviewSide / aspect), 4)
					: kPreviewSide;
				tile.preview = Data::BlurhashImage(tile.media.blurhash, width, height);
			}
		}
		source = tile.preview;
	}
	tile.preparedFor = tile.rect.size();
	tile.preparedFinal = ready;
	if (source.isNull()) {
		tile.prepared = QPixmap();
		return;
	}
	const auto ratio = _host->cardPixelRatio();
	auto image = Ui::Images::PrepareCover(source, tile.rect.size(), ratio);
	if (tile.round) {
		image = Ui::Images::Circle(std::move(image));
	} else if (_block == Block::Audio) {
		image = Ui::Images::Round(std::move(image), st::eliAudioCoverRadius);
	} else {
		// Round the corners that lie on the outer edge of the tiles' group.
		auto group = QRect();
		for (const auto &other : _tiles) {
			if (other.quote == tile.quote && !other.rect.isEmpty()) {
				group = group.united(other.rect);
			}
		}
		const auto radius = st::eliMediaRadius;
		const auto leftEdge = (tile.rect.left() == group.left());
		const auto rightEdge = (tile.rect.right() == group.right());
		const auto topEdge = (tile.rect.top() == group.top());
		const auto bottomEdge = (tile.rect.bottom() == group.bottom());
		image = Ui::Images::Round(
			std::move(image),
			(leftEdge && topEdge) ? radius : 0,
			(rightEdge && topEdge) ? radius : 0,
			(rightEdge && bottomEdge) ? radius : 0,
			(leftEdge && bottomEdge) ? radius : 0);
	}
	tile.prepared = QPixmap::fromImage(std::move(image));
	tile.prepared.setDevicePixelRatio(ratio);
}

int PostCard::imageIndexOfTile(int tile) const {
	if (tile < 0 || tile >= int(_tiles.size()) || _tiles[tile].media.kind != u"image") {
		return -1;
	}
	auto result = 0;
	for (auto i = 0; i != tile; ++i) {
		if (_tiles[i].quote == _tiles[tile].quote && _tiles[i].media.kind == u"image") {
			++result;
		}
	}
	return result;
}

bool PostCard::tileIsVideo(int tile) const {
	return (tile >= 0)
		&& (tile < int(_tiles.size()))
		&& (_tiles[tile].media.kind != u"image");
}

QImage PostCard::tileImage(int tile) const {
	if (tile < 0 || tile >= int(_tiles.size())) {
		return QImage();
	}
	const auto &entry = _tiles[tile];
	return (entry.handle && entry.handle->isReady())
		? entry.handle->image()
		: QImage();
}

// ---- hit testing

bool PostCard::Clickable(HitType type) {
	return (type != HitType::None);
}

Hit PostCard::hitTest(QPoint point) const {
	if (point.y() < 0 || point.y() >= _height || point.x() < 0 || point.x() >= _width) {
		return {};
	}
	for (const auto &area : _areas) {
		if (area.rect.contains(point)) {
			// The quote frame is under its own media.
			if (area.hit.type == HitType::Quote) {
				for (const auto &other : _areas) {
					if (other.hit.type == HitType::QuoteMedia && other.rect.contains(point)) {
						return other.hit;
					}
				}
			}
			return area.hit;
		}
	}
	if (!_text.isEmpty() && !_post.deleted && _textRect.contains(point) && _text.hasLinks()) {
		auto context = Ui::Text::PaintContext();
		context.position = _textRect.topLeft();
		context.availableWidth = _textRect.width();
		context.elisionLines = _textLines;
		context.autoDirection = true;
		const auto state = _text.getState(point, context);
		if (state.link >= 0) {
			return { HitType::Link, state.link };
		}
	}
	for (auto i = 0; i != int(_parts.size()); ++i) {
		const auto &part = _parts[i];
		const auto rect = QRect(_textRect.x(), part.top, _textRect.width(), part.height);
		if (rect.contains(point) && part.text.hasLinks()) {
			auto context = Ui::Text::PaintContext();
			context.position = rect.topLeft();
			context.availableWidth = rect.width();
			context.autoDirection = true;
			const auto state = part.text.getState(point, context);
			if (state.link >= 0) {
				return { HitType::Link, (i + 1) * 1000 + state.link };
			}
		}
	}
	if (_post.deleted || _post.id.isEmpty()) {
		return {};
	}
	return { HitType::Body, -1 };
}

Ui::Text::Link PostCard::link(int index) const {
	const auto part = index / 1000;
	const auto local = index % 1000;
	const auto &text = (part > 0 && part <= int(_parts.size()))
		? _parts[part - 1].text
		: _text;
	return (local >= 0 && local < text.linksCount())
		? text.link(local)
		: Ui::Text::Link();
}

QRect PostCard::hitRect(Hit hit) const {
	for (const auto &area : _areas) {
		if (area.hit == hit) {
			return area.rect;
		}
	}
	if (hit.type == HitType::Link) {
		return _textRect.united(_blockRect);
	}
	return QRect(0, 0, _width, _height);
}

QString PostCard::tooltip(Hit hit) const {
	switch (hit.type) {
	case HitType::Reply:
		return _post.canReply
			? tr::lng_reply(tr::now)
			: tr::lng_eli_replies_are_closed(tr::now);
	case HitType::Repost: return tr::lng_eli_repost(tr::now);
	case HitType::Like: return tr::lng_eli_like(tr::now);
	case HitType::Dislike: return tr::lng_eli_dislike(tr::now);
	case HitType::Bookmark:
		return _post.my.bookmarked
			? tr::lng_eli_saved(tr::now)
			: tr::lng_eli_save(tr::now);
	case HitType::Share: return tr::lng_share(tr::now);
	case HitType::More: return tr::lng_eli_more_options(tr::now);
	case HitType::Media:
	case HitType::QuoteMedia:
		return (hit.index >= 0 && hit.index < int(_tiles.size()))
			? _tiles[hit.index].media.alt
			: QString();
	default:
		break;
	}
	return QString();
}

// ---- painting

void PostCard::paint(Painter &p, const CardPaintState &state) const {
	if (_width <= 0) {
		return;
	}
	if (state.selected) {
		p.fillRect(
			rtl(0, 0, st::eliCardSelectedWidth, _height),
			st::eliCardSelectedFg);
	}
	// The line that ties a reply to the ones hanging under it.
	if (reply() && _options.line && _lineTop < _height) {
		const auto center = _userpicRect.center().x();
		p.fillRect(
			QRect(
				center - st::eliThreadLineWidth / 2,
				_lineTop,
				st::eliThreadLineWidth,
				_height - _lineTop),
			st::eliThreadLineFg);
	}
	if (!_contextText.isEmpty()) {
		paintContext(p);
	}
	if (const auto ring = _host->cardSession().data().stories().ring(_post.author);
		ring != Data::StoryRing::None) {
		// The face steps in to leave room for the story ring.
		const auto outset = Stories::RingOutset();
		const auto inner = _userpicRect.marginsRemoved(
			QMargins(outset, outset, outset, outset));
		_host->cardUserpics().paint(p, inner, _post.author);
		Stories::PaintRing(p, inner, ring);
	} else {
		_host->cardUserpics().paint(p, _userpicRect, _post.author);
	}
	paintHeader(p, state);
	paintText(p, state);
	if (!_post.deleted) {
		switch (_block) {
		case Block::None:
			break;
		case Block::Media:
			for (const auto &tile : _tiles) {
				paintTile(p, tile, false);
			}
			break;
		case Block::VideoNote:
			if (!_tiles.empty()) {
				paintTile(p, _tiles[0], state.hover.type == HitType::VideoNote);
			}
			break;
		case Block::Poll:
			paintPoll(p, state);
			break;
		case Block::Quote:
			for (auto i = 0; i != _quoteTilesFrom && i != int(_tiles.size()); ++i) {
				paintTile(p, _tiles[i], false);
			}
			paintQuote(p, state);
			break;
		case Block::Invite:
			paintInvite(p, state);
			break;
		case Block::Audio:
			paintAudio(p, state);
			break;
		case Block::Thread:
			paintThread(p, state);
			break;
		}
		if (detail()) {
			paintDetailFooter(p, state);
		}
		paintActions(p, state);
	}
	if (state.burst >= 0. && state.burst < 1.) {
		const auto scale = (state.burst < 0.3)
			? (0.4 + 2. * state.burst)
			: 1.;
		const auto opacity = (state.burst < 0.6)
			? 1.
			: (1. - (state.burst - 0.6) / 0.4);
		p.save();
		p.setOpacity(opacity);
		p.translate(state.burstCenter);
		p.scale(scale, scale);
		const auto size = st::eliHeartBurstIcon.size();
		{
			PainterHighQualityEnabler hq(p);
			st::eliHeartBurstIcon.paintInCenter(
				p,
				QRect(-size.width() / 2, -size.height() / 2, size.width(), size.height()));
		}
		p.restore();
	}
}

void PostCard::paintContext(Painter &p) const {
	const auto &icon = _options.pinned
		? st::eliContextIconPin
		: (_options.reason == u"repost")
		? st::eliContextIconRepost
		: st::eliContextIconFollow;
	const auto rect = _contextRect; // logical
	icon.paint(
		p,
		rect.x(),
		rect.y() + (rect.height() - icon.height()) / 2,
		_width);
	p.setPen(st::eliContextFg);
	const auto textLeft = rect.x() + icon.width() + st::eliContextIconSkip;
	DrawLine(
		p,
		rtl(textLeft, rect.y(), rect.width() - icon.width() - st::eliContextIconSkip, rect.height()),
		st::eliContextFont,
		_contextText);
}

void PostCard::paintHeader(Painter &p, const CardPaintState &state) const {
	const auto over = (state.hover.type == HitType::Author);
	const auto &header = _headerRect; // logical
	if (detail()) {
		const auto nameTop = header.y() + (header.height() - st::eliDetailNameFont->height - st::eliDetailHandleFont->height - 2) / 2;
		p.setPen(st::eliNameFg);
		p.setFont(over ? st::eliDetailNameFont->underline() : st::eliDetailNameFont);
		p.drawTextLeft(header.x(), nameTop, _width, _nameElided, _nameWidth);
		if (_verified) {
			st::eliVerifiedIcon.paint(
				p,
				header.x() + _nameWidth + st::eliHeaderSkip,
				nameTop + (st::eliDetailNameFont->height - st::eliVerifiedIcon.height()) / 2,
				_width);
		}
		if (_handleShown) {
			p.setPen(st::eliMetaFg);
			p.setFont(st::eliDetailHandleFont);
			p.drawTextLeft(
				header.x(),
				nameTop + st::eliDetailNameFont->height + 2,
				_width,
				_handleElided,
				_handleWidth);
		}
		if (!_followRect.isEmpty()) {
			const auto following = (_followState != u"none");
			const auto text = (_followState == u"following")
				? tr::lng_following(tr::now)
				: (_followState == u"requested")
				? tr::lng_eli_follow_requested(tr::now)
				: tr::lng_follow(tr::now);
			const auto hovered = (state.hover.type == HitType::Follow);
			PainterHighQualityEnabler hq(p);
			const auto radius = _followRect.height() / 2;
			if (following) {
				if (hovered) {
					FillRounded(p, _followRect, radius, st::stateHover);
				}
				StrokeRounded(p, _followRect, radius, st::eliDetailFollowBorderFg->c);
				p.setPen(st::textPrimary);
			} else {
				p.setOpacity(hovered ? 0.86 : 1.);
				FillRounded(p, _followRect, radius, st::eliDetailFollowBg);
				p.setOpacity(1.);
				p.setPen(st::eliDetailFollowFg);
			}
			p.setFont(st::eliDetailFollowFont);
			p.drawTextCentered(_followRect, text);
		}
	} else {
		const auto top = header.y();
		auto x = header.x();
		p.setPen(_post.deleted ? st::eliMetaFg : st::eliNameFg);
		p.setFont(over ? st::eliNameFont->underline() : st::eliNameFont);
		p.drawTextLeft(x, top, _width, _nameElided, _nameWidth);
		x += _nameWidth;
		if (_verified) {
			x += st::eliHeaderSkip;
			st::eliVerifiedIcon.paint(
				p,
				x,
				top + (st::eliNameFont->height - st::eliVerifiedIcon.height()) / 2,
				_width);
			x += st::eliVerifiedIcon.width();
		}
		p.setPen(st::eliMetaFg);
		p.setFont(st::eliHandleFont);
		if (_handleShown) {
			x += st::eliHeaderSkip;
			p.drawTextLeft(x, top, _width, _handleElided, _handleWidth);
			x += _handleWidth;
		}
		if (_timeWidth > 0) {
			p.drawTextLeft(x, top, _width, _timeText, _timeWidth);
		}
	}
	if (!_moreRect.isEmpty()) {
		const auto hovered = (state.hover.type == HitType::More)
			|| (state.pressed.type == HitType::More);
		if (hovered) {
			PainterHighQualityEnabler hq(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliMoreBgOver);
			p.drawEllipse(_moreRect);
		}
		if (hovered) {
			st::eliMoreIcon.paintInCenter(p, _moreRect, st::textPrimary->c);
		} else {
			st::eliMoreIcon.paintInCenter(p, _moreRect);
		}
	}
	if (!_replyingRect.isEmpty()) {
		p.setPen((state.hover.type == HitType::ReplyingTo)
			? st::brandText
			: st::eliReplyingToFg);
		DrawLine(p, _replyingRect, st::eliReplyingFont, _replyingText);
	}
}

void PostCard::paintText(Painter &p, const CardPaintState &state) const {
	if (_text.isEmpty()) {
		return;
	}
	if (_textRect.intersects(state.clip)) {
		p.setPen(_deletedText ? st::eliDeletedFg : st::eliTextFg);
		auto context = Ui::Text::PaintContext();
		context.position = _textRect.topLeft();
		context.availableWidth = _textRect.width();
		context.elisionLines = _textLines;
		context.autoDirection = !_deletedText;
		context.clip = state.clip;
		context.hoveredLink = (state.hover.type == HitType::Link && state.hover.index < 1000)
			? state.hover.index
			: -1;
		_text.draw(p, context);
	}
	if (!_showMoreRect.isEmpty()) {
		const auto over = (state.hover.type == HitType::ShowMore);
		p.setPen(st::eliShowMoreFg);
		DrawLine(
			p,
			_showMoreRect,
			over ? st::eliShowMoreFont->underline() : st::eliShowMoreFont,
			tr::lng_eli_show_more(tr::now));
	}
}

void PostCard::paintTile(Painter &p, const Tile &tile, bool over) const {
	Q_UNUSED(over);
	if (tile.rect.isEmpty()) {
		return;
	}
	prepareTile(tile);
	PainterHighQualityEnabler hq(p);
	if (tile.prepared.isNull()) {
		if (tile.round) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliMediaBg);
			p.drawEllipse(tile.rect);
		} else if (_block == Block::Audio) {
			FillRounded(p, tile.rect, st::eliAudioCoverRadius, st::eliAudioCoverBg);
		} else {
			auto group = QRect();
			for (const auto &other : _tiles) {
				if (other.quote == tile.quote && !other.rect.isEmpty()) {
					group = group.united(other.rect);
				}
			}
			auto clip = QPainterPath();
			clip.addRoundedRect(group, st::eliMediaRadius, st::eliMediaRadius);
			auto own = QPainterPath();
			own.addRect(tile.rect);
			p.fillPath(clip.intersected(own), st::eliMediaBg);
		}
	} else {
		p.drawPixmap(tile.rect.topLeft(), tile.prepared);
	}
	if (_block == Block::Audio) {
		return;
	}
	const auto video = (tile.media.kind != u"image");
	if (video) {
		const auto small = (std::min(tile.rect.width(), tile.rect.height()) < 120);
		const auto size = small ? (st::eliMediaPlaySize * 2 / 3) : st::eliMediaPlaySize;
		const auto circle = QRect(
			tile.rect.center().x() - size / 2 + 1,
			tile.rect.center().y() - size / 2 + 1,
			size,
			size);
		p.setPen(Qt::NoPen);
		p.setBrush(st::eliMediaPlayBg);
		p.drawEllipse(circle);
		PaintPlayGlyph(p, circle, st::staticWhite->c);
		if (tile.media.durationMs > 0 && tile.more == 0) {
			const auto text = Lang::Duration(int(tile.media.durationMs / 1000));
			const auto &padding = st::eliMediaBadgePadding;
			const auto width = st::eliMediaBadgeFont->width(text)
				+ padding.left()
				+ padding.right();
			const auto height = st::eliMediaBadgeFont->height
				+ padding.top()
				+ padding.bottom();
			const auto badge = tile.round
				? QRect(
					tile.rect.center().x() - width / 2,
					tile.rect.bottom() - height - st::eliMediaBadgeSkip,
					width,
					height)
				: QRect(
					style::RightToLeft()
						? (tile.rect.right() + 1 - st::eliMediaBadgeSkip - width)
						: (tile.rect.left() + st::eliMediaBadgeSkip),
					tile.rect.bottom() + 1 - st::eliMediaBadgeSkip - height,
					width,
					height);
			FillRounded(p, badge, height / 2, st::eliMediaBadgeBg);
			p.setPen(st::eliMediaBadgeFg);
			p.setFont(st::eliMediaBadgeFont);
			p.drawTextCentered(badge, text);
		}
	}
	if (tile.more > 0) {
		auto group = QRect();
		for (const auto &other : _tiles) {
			if (other.quote == tile.quote && !other.rect.isEmpty()) {
				group = group.united(other.rect);
			}
		}
		auto clip = QPainterPath();
		clip.addRoundedRect(group, st::eliMediaRadius, st::eliMediaRadius);
		auto own = QPainterPath();
		own.addRect(tile.rect);
		p.fillPath(clip.intersected(own), st::eliMediaMoreBg);
		p.setPen(st::eliMediaBadgeFg);
		p.setFont(st::eliMediaMoreFont);
		p.drawTextCentered(
			tile.rect,
			tr::lng_eli_more_media(tr::now, lt_n, Lang::Num(tile.more)));
	}
}

void PostCard::paintPoll(Painter &p, const CardPaintState &state) const {
	if (!_post.poll) {
		return;
	}
	const auto &poll = *_post.poll;
	PainterHighQualityEnabler hq(p);
	for (auto i = 0; i != int(_pollRects.size()) && i != int(poll.options.size()); ++i) {
		const auto &rect = _pollRects[i];
		if (!rect.intersects(state.clip)) {
			continue;
		}
		const auto mine = (poll.myVote == i);
		const auto over = (state.hover == Hit{ HitType::PollOption, i });
		const auto radius = st::eliPollOptionRadius;
		auto textLeft = st::eliPollOptionPadding;
		auto percentWidth = 0;
		if (_pollResults) {
			const auto percent = (i < int(_pollPercents.size())) ? _pollPercents[i] : 0;
			auto clip = QPainterPath();
			clip.addRoundedRect(rect, radius, radius);
			const auto barWidth = std::max(rect.width() * percent / 100, percent ? radius : 0);
			auto bar = QPainterPath();
			bar.addRect(style::RightToLeft()
				? QRect(rect.right() + 1 - barWidth, rect.y(), barWidth, rect.height())
				: QRect(rect.x(), rect.y(), barWidth, rect.height()));
			FillRounded(p, rect, radius, st::eliPollBarBg);
			p.fillPath(
				clip.intersected(bar),
				mine ? st::eliPollBarBgMine : st::eliPollBarBg);
			if (mine) {
				StrokeRounded(p, rect, radius, st::brandPrimary->c);
			}
			const auto percentText = tr::lng_eli_percent(
				tr::now,
				lt_n,
				Lang::Num(percent));
			percentWidth = st::eliPollPercentFont->width(percentText);
			p.setPen(st::eliPollFg);
			p.setFont(st::eliPollPercentFont);
			p.drawText(
				style::RightToLeft()
					? (rect.x() + st::eliPollOptionPadding)
					: (rect.right() + 1 - st::eliPollOptionPadding - percentWidth),
				rect.y()
					+ (rect.height() - st::eliPollPercentFont->height) / 2
					+ st::eliPollPercentFont->ascent,
				percentText);
			if (mine) {
				const auto logicalLeft = style::RightToLeft()
					? (_width - rect.right() - 1)
					: rect.x();
				st::eliPollCheckIcon.paint(
					p,
					logicalLeft + textLeft,
					rect.y() + (rect.height() - st::eliPollCheckIcon.height()) / 2,
					_width);
				textLeft += st::eliPollCheckIcon.width() + 6;
			}
		} else {
			if (over) {
				FillRounded(p, rect, radius, st::brandSoft);
			}
			StrokeRounded(
				p,
				rect,
				radius,
				over ? st::eliPollBorderFgOver->c : st::eliPollBorderFg->c,
				over ? 1.5 : 1.);
		}
		p.setPen(st::eliPollFg);
		const auto label = QRect(
			rect.x() + (style::RightToLeft()
				? (percentWidth ? percentWidth + st::eliPollOptionPadding + 8 : st::eliPollOptionPadding)
				: textLeft),
			rect.y(),
			rect.width() - textLeft - (percentWidth ? percentWidth + st::eliPollOptionPadding + 8 : st::eliPollOptionPadding),
			rect.height());
		DrawLine(p, label, mine ? st::eliPollFontMine : st::eliPollFont, poll.options[i]);
	}
	p.setPen(st::eliPollFooterFg);
	DrawLine(
		p,
		_pollFooterRect,
		st::eliPollFooterFont,
		_pollRetract.isEmpty() ? _pollFooter : (_pollFooter + Separator()));
	if (!_pollRetractRect.isEmpty()) {
		const auto over = (state.hover.type == HitType::PollRetract);
		p.setPen(st::eliPollRetractFg);
		DrawLine(
			p,
			_pollRetractRect,
			over ? st::eliPollFooterFont->underline() : st::eliPollFooterFont,
			_pollRetract);
	}
}

void PostCard::paintQuote(Painter &p, const CardPaintState &state) const {
	const auto &rect = _blockRect;
	if (!rect.intersects(state.clip)) {
		return;
	}
	{
		PainterHighQualityEnabler hq(p);
		const auto over = (state.hover.type == HitType::Quote);
		if (over) {
			FillRounded(p, rect, st::eliQuoteRadius, st::eliQuoteBgOver);
		}
		StrokeRounded(p, rect, st::eliQuoteRadius, st::eliQuoteBorderFg->c);
	}
	if (_quoteUnavailable) {
		p.setPen(st::eliDeletedFg);
		_quoteText.draw(p, {
			.position = _quoteTextRect.topLeft(),
			.availableWidth = _quoteTextRect.width(),
		});
		return;
	}
	const auto &header = _quoteHeaderRect; // logical
	const auto userpic = st::eliQuoteUserpic;
	_host->cardUserpics().paint(
		p,
		rtl(header.x(), header.y() + (header.height() - userpic) / 2, userpic, userpic),
		_post.quote ? _post.quote->author : QString());
	auto x = header.x() + userpic + 6;
	const auto available = header.width() - userpic - 6;
	// The name first; what is left goes to the handle and the time.
	const auto nameNatural = st::eliQuoteNameFont->width(_quoteName);
	const auto metaNatural = st::eliQuoteMetaFont->width(_quoteMeta);
	const auto nameWidth = (nameNatural + 6 + metaNatural <= available)
		? nameNatural
		: std::min(nameNatural, std::max(available * 3 / 5, available - metaNatural - 6));
	const auto name = Elided(st::eliQuoteNameFont, _quoteName, nameWidth);
	const auto textTop = header.y() + (header.height() - st::eliQuoteNameFont->height) / 2;
	p.setPen(st::eliNameFg);
	p.setFont(st::eliQuoteNameFont);
	p.drawTextLeft(x, textTop, _width, name, st::eliQuoteNameFont->width(name));
	x += st::eliQuoteNameFont->width(name) + 6;
	const auto metaAvailable = available - st::eliQuoteNameFont->width(name) - 6;
	// The time alone when the handle does not fit with it.
	const auto meta = (metaNatural <= metaAvailable)
		? _quoteMeta
		: Elided(st::eliQuoteMetaFont, _quoteTime, metaAvailable);
	p.setPen(st::eliMetaFg);
	p.setFont(st::eliQuoteMetaFont);
	p.drawTextLeft(x, textTop, _width, meta, st::eliQuoteMetaFont->width(meta));
	if (!_quoteTextRect.isEmpty()) {
		p.setPen(st::eliTextFg);
		auto context = Ui::Text::PaintContext();
		context.position = _quoteTextRect.topLeft();
		context.availableWidth = _quoteTextRect.width();
		context.elisionLines = st::eliQuoteTextLines;
		context.autoDirection = true;
		_quoteText.draw(p, context);
	}
	for (auto i = _quoteTilesFrom; i < int(_tiles.size()); ++i) {
		paintTile(p, _tiles[i], false);
	}
}

void PostCard::paintInvite(Painter &p, const CardPaintState &state) const {
	const auto &rect = _blockRect;
	if (!rect.intersects(state.clip)) {
		return;
	}
	PainterHighQualityEnabler hq(p);
	FillRounded(p, rect, st::eliInviteRadius, st::eliInviteBg);
	const auto &padding = st::eliInvitePadding;
	const auto logicalLeft = (style::RightToLeft() ? (_width - rect.right() - 1) : rect.x())
		+ padding.left();
	const auto top = rect.y() + padding.top();
	st::eliInviteIcon.paint(
		p,
		logicalLeft,
		top + (st::eliInviteTopHeight - st::eliInviteIcon.height()) / 2,
		_width);
	auto x = logicalLeft + st::eliInviteIcon.width() + 8;
	const auto pillWidth = st::eliInvitePillFont->width(_invitePill)
		+ 2 * st::eliInvitePillPadding;
	const auto pill = rtl(
		x,
		top + (st::eliInviteTopHeight - st::eliInvitePillHeight) / 2,
		pillWidth,
		st::eliInvitePillHeight);
	FillRounded(p, pill, st::eliInvitePillHeight / 2, st::eliInvitePillBg);
	p.setPen(st::eliInviteFg);
	p.setFont(st::eliInvitePillFont);
	p.drawTextCentered(pill, _invitePill);
	x += pillWidth + 8;
	if (!_inviteLeft.isEmpty()) {
		p.setOpacity(0.72);
		p.setFont(st::eliInviteLeftFont);
		p.drawTextLeft(
			x,
			top + (st::eliInviteTopHeight - st::eliInviteLeftFont->height) / 2,
			_width,
			_inviteLeft);
		p.setOpacity(1.);
	}
	p.setPen(st::eliInviteFg);
	auto context = Ui::Text::PaintContext();
	context.position = _inviteTopicRect.topLeft();
	context.availableWidth = _inviteTopicRect.width();
	context.elisionLines = st::eliInviteTopicLines;
	context.autoDirection = true;
	_inviteTopic.draw(p, context);
	if (!_inviteButtonRect.isEmpty()) {
		const auto over = (state.hover.type == HitType::InviteJoin);
		FillRounded(
			p,
			_inviteButtonRect,
			_inviteButtonRect.height() / 2,
			over ? st::eliInviteButtonBgOver : st::eliInviteButtonBg);
		p.setPen(st::eliInviteButtonFg);
		p.setFont(st::eliInviteButtonFont);
		p.drawTextCentered(_inviteButtonRect, tr::lng_eli_join_conversation(tr::now));
	}
}

void PostCard::paintAudio(Painter &p, const CardPaintState &state) const {
	const auto &rect = _blockRect;
	if (!rect.intersects(state.clip)) {
		return;
	}
	PainterHighQualityEnabler hq(p);
	FillRounded(p, rect, st::eliAudioRadius, st::eliAudioBg);
	const auto logicalLeft = (style::RightToLeft() ? (_width - rect.right() - 1) : rect.x());
	const auto coverLeft = logicalLeft + st::eliAudioPadding;
	const auto cover = rtl(
		coverLeft,
		rect.y() + (rect.height() - st::eliAudioCover) / 2,
		st::eliAudioCover,
		st::eliAudioCover);
	if (!_tiles.empty()) {
		paintTile(p, _tiles[0], false);
	}
	if (_tiles.empty() || _tiles[0].prepared.isNull()) {
		FillRounded(p, cover, st::eliAudioCoverRadius, st::eliAudioCoverBg);
		st::eliAudioCoverIcon.paintInCenter(p, cover);
	}
	const auto trackId = audioTrackId();
	auto &player = Media::Player();
	const auto current = player.isCurrent(trackId);
	const auto playing = player.isPlaying(trackId) || player.isLoading(trackId);
	const auto textLeft = coverLeft + st::eliAudioCover + st::eliAudioPadding + 2;
	const auto textWidth = rect.width()
		- (textLeft - logicalLeft)
		- st::eliAudioPlaySize
		- 2 * st::eliAudioPadding;
	const auto textTop = rect.y() + (rect.height() - st::eliAudioTitleFont->height - st::eliAudioSubFont->height - 2) / 2;
	p.setPen(st::eliNameFg);
	DrawLine(
		p,
		rtl(textLeft, textTop, textWidth, st::eliAudioTitleFont->height),
		st::eliAudioTitleFont,
		_audioTitle);
	p.setPen(st::eliMetaFg);
	auto sub = _audioSub;
	if (current && player.durationMs() > 0) {
		sub = Lang::Duration(int(player.positionMs() / 1000))
			+ Separator()
			+ _audioSub;
	}
	DrawLine(
		p,
		rtl(textLeft, textTop + st::eliAudioTitleFont->height + 2, textWidth, st::eliAudioSubFont->height),
		st::eliAudioSubFont,
		sub);
	const auto over = (state.hover.type == HitType::Audio);
	p.setPen(Qt::NoPen);
	p.setBrush(over ? st::eliAudioPlayBgOver : st::eliAudioPlayBg);
	p.drawEllipse(_audioPlayRect);
	if (playing) {
		st::eliAudioPauseIcon.paintInCenter(p, _audioPlayRect);
	} else {
		PaintPlayGlyph(p, _audioPlayRect, st::textOnBrand->c);
	}
	if (current) {
		const auto inset = st::eliAudioRadius;
		const auto full = rect.width() - 2 * inset;
		const auto done = int(full * std::clamp(player.progress(), 0., 1.));
		const auto top = rect.bottom() + 1 - st::eliAudioProgressHeight - 4;
		p.setBrush(st::borderStrong);
		p.drawRoundedRect(
			QRect(rect.x() + inset, top, full, st::eliAudioProgressHeight),
			1.5,
			1.5);
		// The timeline of a player always runs left to right.
		p.setBrush(st::eliAudioProgressFg);
		p.drawRoundedRect(
			QRect(rect.x() + inset, top, done, st::eliAudioProgressHeight),
			1.5,
			1.5);
	}
}

void PostCard::paintThread(Painter &p, const CardPaintState &state) const {
	for (const auto &tile : _tiles) {
		paintTile(p, tile, false);
	}
	PainterHighQualityEnabler hq(p);
	const auto logicalLeft = style::RightToLeft()
		? (_width - _textRect.right() - 1)
		: _textRect.x();
	for (auto i = 0; i != int(_parts.size()); ++i) {
		const auto &part = _parts[i];
		const auto rect = QRect(_textRect.x(), part.top, _textRect.width(), part.height);
		const auto badgeTop = part.top - st::eliThreadNumberSize - 6;
		if (!QRect(0, badgeTop, _width, part.height + st::eliThreadNumberSize + 6).intersects(state.clip)) {
			continue;
		}
		const auto badge = rtl(
			logicalLeft,
			badgeTop,
			st::eliThreadNumberSize,
			st::eliThreadNumberSize);
		p.setPen(Qt::NoPen);
		p.setBrush(st::eliThreadNumberBg);
		p.drawEllipse(badge);
		p.setPen(st::eliThreadNumberFg);
		p.setFont(st::eliThreadNumberFont);
		p.drawTextCentered(badge, Lang::Num(i + 2));
		p.fillRect(
			rtl(
				logicalLeft + st::eliThreadNumberSize + 8,
				badgeTop + st::eliThreadNumberSize / 2,
				_textRect.width() - st::eliThreadNumberSize - 8,
				1),
			st::eliCardLineFg);
		p.setPen(st::eliTextFg);
		auto context = Ui::Text::PaintContext();
		context.position = rect.topLeft();
		context.availableWidth = rect.width();
		context.autoDirection = true;
		context.clip = state.clip;
		context.hoveredLink = (state.hover.type == HitType::Link && (state.hover.index / 1000) == i + 1)
			? (state.hover.index % 1000)
			: -1;
		part.text.draw(p, context);
	}
	if (_threadCtaRect.isEmpty() || !_threadCtaRect.intersects(state.clip)) {
		return;
	}
	const auto over = (state.hover.type == HitType::Thread);
	const auto &rect = _threadCtaRect;
	p.setOpacity(over ? 0.8 : 1.);
	FillRounded(p, rect, st::eliThreadCtaRadius, st::eliThreadCtaBg);
	p.setOpacity(1.);
	const auto left = (style::RightToLeft() ? (_width - rect.right() - 1) : rect.x())
		+ st::eliThreadCtaPadding;
	st::eliThreadCtaIcon.paint(
		p,
		left,
		rect.y() + (rect.height() - st::eliThreadCtaIcon.height()) / 2,
		_width);
	auto x = left + st::eliThreadCtaIcon.width() + 8;
	const auto label = (!_post.threadRoot.isEmpty() && _post.threadRoot != _post.id)
		? tr::lng_eli_thread_continue(tr::now)
		: tr::lng_eli_read_thread(tr::now);
	const auto labelWidth = st::eliThreadCtaFont->width(label);
	p.setPen(st::eliThreadCtaFg);
	p.setFont(st::eliThreadCtaFont);
	p.drawTextLeft(
		x,
		rect.y() + (rect.height() - st::eliThreadCtaFont->height) / 2,
		_width,
		label,
		labelWidth);
	x += labelWidth;
	const auto metaAvailable = rect.width() - (x - left) - 2 * st::eliThreadCtaPadding;
	const auto meta = Separator() + _threadMeta;
	if (metaAvailable >= st::eliThreadCtaMetaFont->width(meta)) {
		p.setPen(st::eliMetaFg);
		p.setFont(st::eliThreadCtaMetaFont);
		p.drawTextLeft(
			x,
			rect.y() + (rect.height() - st::eliThreadCtaMetaFont->height) / 2,
			_width,
			meta);
	}
}

void PostCard::paintActions(Painter &p, const CardPaintState &state) const {
	if (_actions.empty() || !_actionsRect.intersects(state.clip)) {
		return;
	}
	const auto large = detail();
	for (const auto &action : _actions) {
		const auto over = (state.hover.type == action.type) && !action.disabled;
		const auto pressed = (state.pressed.type == action.type) && !action.disabled;
		const style::icon *icon = nullptr;
		auto color = st::eliActionFg->c;
		auto bg = st::eliActionBgOver->c;
		switch (action.type) {
		case HitType::Reply:
			icon = large ? &st::eliIconReplyL : &st::eliIconReply;
			if (over) {
				color = st::eliActionFgOver->c;
			}
			break;
		case HitType::Repost:
			icon = large ? &st::eliIconRepostL : &st::eliIconRepost;
			if (over || action.active) {
				color = st::eliActionRepostFg->c;
			}
			break;
		case HitType::Like:
			icon = action.active
				? (large ? &st::eliIconLikeActiveL : &st::eliIconLikeActive)
				: (large ? &st::eliIconLikeL : &st::eliIconLike);
			if (over || action.active) {
				color = st::eliActionLikeFg->c;
			}
			bg = st::eliActionLikeBgOver->c;
			break;
		case HitType::Dislike:
			icon = action.active
				? (large ? &st::eliIconDislikeActiveL : &st::eliIconDislikeActive)
				: (large ? &st::eliIconDislikeL : &st::eliIconDislike);
			if (over || action.active) {
				color = st::eliActionDislikeFg->c;
			}
			bg = st::eliActionDislikeBgOver->c;
			break;
		case HitType::Bookmark:
			icon = action.active
				? (large ? &st::eliIconBookmarkActiveL : &st::eliIconBookmarkActive)
				: (large ? &st::eliIconBookmarkL : &st::eliIconBookmark);
			if (over || action.active) {
				color = st::eliActionBookmarkFg->c;
			}
			break;
		case HitType::Share:
			icon = large ? &st::eliIconShareL : &st::eliIconShare;
			if (over) {
				color = st::eliActionFgOver->c;
			}
			break;
		default:
			continue;
		}
		if (over || pressed) {
			PainterHighQualityEnabler hq(p);
			p.setPen(Qt::NoPen);
			p.setBrush(bg);
			p.setOpacity(pressed ? 1. : 0.8);
			p.drawEllipse(action.circle);
			p.setOpacity(1.);
		}
		if (action.disabled) {
			p.setOpacity(0.4);
		}
		icon->paintInCenter(p, action.circle, color);
		if (action.countWidth > 0) {
			p.setPen(color);
			p.setFont(st::eliActionCountFont);
			const auto top = action.circle.y()
				+ (action.circle.height() - st::eliActionCountFont->height) / 2;
			if (style::RightToLeft()) {
				p.drawText(
					action.circle.x() + 4 - st::eliActionCountSkip - action.countWidth,
					top + st::eliActionCountFont->ascent,
					action.count);
			} else {
				p.drawText(
					action.circle.right() + 1 - 4 + st::eliActionCountSkip,
					top + st::eliActionCountFont->ascent,
					action.count);
			}
		}
		if (action.disabled) {
			p.setOpacity(1.);
		}
	}
	// Views sit between the two groups when there is room for them.
	if (feed() && _post.counts.views > 0 && _actionsRect.width() >= st::eliActionViewsMinWidth) {
		auto lastLeading = QRect();
		auto firstTrailing = QRect();
		for (const auto &action : _actions) {
			if (action.type == HitType::Bookmark) {
				firstTrailing = action.circle;
			} else if (action.type == HitType::Dislike) {
				lastLeading = action.rect;
			}
		}
		const auto text = Lang::CompactCount(_post.counts.views);
		const auto textWidth = st::eliActionCountFont->width(text);
		const auto full = st::eliIconViews.width() + 4 + textWidth;
		const auto gap = style::RightToLeft()
			? (lastLeading.x() - firstTrailing.right() - 1)
			: (firstTrailing.x() - lastLeading.right() - 1);
		if (!firstTrailing.isEmpty() && gap >= full + 12) {
			// Hugs the trailing group.
			const auto top = firstTrailing.y();
			p.setPen(st::eliActionFg);
			p.setFont(st::eliActionCountFont);
			const auto textTop = top
				+ (firstTrailing.height() - st::eliActionCountFont->height) / 2
				+ st::eliActionCountFont->ascent;
			const auto iconTop = top + (firstTrailing.height() - st::eliIconViews.height()) / 2;
			if (style::RightToLeft()) {
				const auto right = firstTrailing.right() + 1 + 6 + full;
				st::eliIconViews.paintInCenter(
					p,
					QRect(right - st::eliIconViews.width(), iconTop, st::eliIconViews.width(), st::eliIconViews.height()));
				p.drawText(right - st::eliIconViews.width() - 4 - textWidth, textTop, text);
			} else {
				const auto left = firstTrailing.x() - 6 - full;
				st::eliIconViews.paintInCenter(
					p,
					QRect(left, iconTop, st::eliIconViews.width(), st::eliIconViews.height()));
				p.drawText(left + st::eliIconViews.width() + 4, textTop, text);
			}
		}
	}
}

void PostCard::paintDetailFooter(Painter &p, const CardPaintState &state) const {
	if (!_detailMetaRect.isEmpty()) {
		p.setPen(st::eliMetaFg);
		DrawLine(p, _detailMetaRect, st::eliDetailMetaFont, _detailMeta);
	}
	const auto line = [&](int y) {
		p.fillRect(
			QRect(_detailMetaRect.x(), y, _detailMetaRect.width(), 1),
			st::eliCardLineFg);
	};
	if (!_statsRect.isEmpty()) {
		line(_statsRect.y());
		for (const auto &stat : _stats) {
			const auto over = (stat.type != HitType::None)
				&& (state.hover.type == stat.type);
			const auto logicalLeft = style::RightToLeft()
				? (_width - stat.rect.right() - 1)
				: stat.rect.x();
			const auto top = stat.rect.y()
				+ (stat.rect.height() - st::eliDetailStatCountFont->height) / 2;
			p.setPen(st::eliNameFg);
			p.setFont(st::eliDetailStatCountFont);
			p.drawTextLeft(logicalLeft, top, _width, stat.count, stat.countWidth);
			p.setPen(st::eliDetailStatLabelFg);
			p.setFont(over
				? st::eliDetailStatLabelFont->underline()
				: st::eliDetailStatLabelFont);
			p.drawTextLeft(
				logicalLeft + stat.countWidth + st::eliHeaderSkip,
				top,
				_width,
				stat.label);
		}
	}
	if (!_actionsRect.isEmpty()) {
		line(_actionsRect.y());
	}
}

} // namespace Eli
