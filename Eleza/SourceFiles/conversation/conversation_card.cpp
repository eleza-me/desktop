#include "conversation/conversation_card.h"

#include "conversation/conversation_time.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/widgets/misc_widgets.h"
#include "styles/style_conversation.h"

#include <algorithm>

namespace Conversation {

QPainterPath TailedRect(const QRectF &rect, double radius, double tail) {
	const auto r = std::min({ radius, rect.height() / 2., rect.width() / 2. });
	const auto t = std::min(tail, r);
	auto path = QPainterPath();
	path.moveTo(rect.left() + r, rect.top());
	path.lineTo(rect.right() - r, rect.top());
	path.arcTo(QRectF(rect.right() - 2 * r, rect.top(), 2 * r, 2 * r), 90, -90);
	path.lineTo(rect.right(), rect.bottom() - r);
	path.arcTo(
		QRectF(rect.right() - 2 * r, rect.bottom() - 2 * r, 2 * r, 2 * r),
		0,
		-90);
	path.lineTo(rect.left() + t, rect.bottom());
	path.arcTo(
		QRectF(rect.left(), rect.bottom() - 2 * t, 2 * t, 2 * t),
		270,
		-90);
	path.lineTo(rect.left(), rect.top() + r);
	path.arcTo(QRectF(rect.left(), rect.top(), 2 * r, 2 * r), 180, -90);
	path.closeSubpath();
	return path;
}

void PaintExpiryRing(
		QPainter &p,
		const QRectF &rect,
		double fraction,
		bool final,
		double stroke,
		const QColor &track) {
	auto hq = PainterHighQualityEnabler(p);
	const auto half = stroke / 2.;
	const auto inner = rect.marginsRemoved(QMarginsF(half, half, half, half));
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(track, stroke));
	p.drawEllipse(inner);
	fraction = std::clamp(fraction, 0., 1.);
	if (fraction <= 0.) {
		return;
	}
	auto pen = QPen(
		final ? st::conversationRingFinalFg->c : st::conversationRingFg->c,
		stroke);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	if (fraction >= 1.) {
		p.drawEllipse(inner);
		return;
	}
	// From the top; the remaining part shrinks towards the start, in the
	// reading direction of the interface.
	const auto span = int(std::lround(fraction * 360. * 16.));
	p.drawArc(inner, 90 * 16, style::RightToLeft() ? span : -span);
}

CardPainter::CardPainter(
	Main::Session *session,
	std::function<void(const QString &userId)> update,
	QObject *parent)
: QObject(parent)
, _session(session)
, _userpics(session, std::move(update)) {
}

bool CardPainter::isOwn(const Data::OpenConversation &item) const {
	if (!_session) {
		return false;
	}
	auto &data = _session->data();
	return (!item.speakerId.isEmpty() && item.speakerId == data.userId())
		|| !data.chats().chatOfConversation(item.id).isEmpty();
}

Card CardPainter::make(
		const Data::OpenConversation &item,
		bool forceAnonymous) const {
	auto result = Card();
	result.item = item;
	result.topic.setText(st::conversationTopicStyle, item.topic);
	result.own = isOwn(item);
	result.anonymous = forceAnonymous
		|| item.speakerMasked
		|| item.speakerId.isEmpty();
	if (_session) {
		result.category = _session->data().conversations().categoryTitle(
			item.category);
	}
	refreshName(result);
	return result;
}

bool CardPainter::refreshName(Card &card) const {
	auto name = QString();
	if (card.anonymous) {
		name = card.item.maskName.isEmpty()
			? tr::lng_anonymous(tr::now)
			: card.item.maskName;
	} else if (_session) {
		const auto user = _session->data().users().get(card.item.speakerId);
		name = user.deleted
			? tr::lng_deleted_account(tr::now)
			: user.displayName;
	}
	if (card.speakerName == name) {
		return false;
	}
	card.speakerName = name;
	return true;
}

CardPainter::Layout CardPainter::layout(const Card &card, int width) const {
	auto result = Layout();
	const auto &margin = st::conversationCardMargin;
	const auto &padding = st::conversationCardPadding;
	const auto skip = int(st::conversationCardSkip);
	const auto cardWidth = std::max(width - margin.left() - margin.right(), 1);
	const auto left = margin.left() + padding.left();
	const auto inner = std::max(cardWidth - padding.left() - padding.right(), 1);
	auto top = margin.top() + padding.top();

	// ---- ring, remaining, category
	const auto rowHeight = int(st::conversationTagHeight);
	const auto ring = int(st::conversationRingSize);
	result.ring = QRect(left, top + (rowHeight - ring) / 2, ring, ring);
	result.remainingLeft = left + ring + skip;
	auto tagWidth = 0;
	if (!card.category.isEmpty()) {
		const auto available = inner / 2;
		result.tagText = st::conversationTagFont->elided(
			card.category,
			std::max(available - 2 * int(st::conversationTagPadding), 0));
		tagWidth = st::conversationTagFont->width(result.tagText)
			+ 2 * int(st::conversationTagPadding);
		result.tag = QRect(
			left + inner - tagWidth,
			top,
			tagWidth,
			rowHeight);
	}
	result.remainingWidth = std::max(
		inner - ring - skip - (tagWidth ? (tagWidth + skip) : 0),
		0);
	top += rowHeight + skip;

	// ---- topic
	const auto topicHeight = card.topic.countHeight(
		inner,
		st::conversationTopicLines);
	result.topic = QRect(left, top, inner, topicHeight);
	top += topicHeight + skip;

	// ---- speaker, pill, join
	const auto userpic = int(st::conversationSpeakerSize);
	const auto line = int(st::conversationSpeakerHeight);
	result.pillText = card.own
		? tr::lng_conversation_yours(tr::now)
		: tr::lng_chat_waiting_listener(tr::now);
	const auto pillWidth = st::conversationPillFont->width(result.pillText)
		+ 2 * int(st::conversationPillPadding)
		+ int(st::conversationPillDot)
		+ int(st::conversationPillDotSkip);
	result.joinText = card.own
		? tr::lng_conversation_view(tr::now)
		: tr::lng_eli_join_conversation(tr::now);
	const auto joinIcon = card.own
		? 0
		: (st::conversationJoinIcon.width() + int(st::conversationJoinIconSkip));
	const auto joinWidth = st::conversationJoinFont->width(result.joinText)
		+ joinIcon
		+ 2 * int(st::conversationJoinPadding);
	const auto joinHeight = int(st::conversationJoinHeight);

	const auto nameFull = st::conversationSpeakerNameFont->width(
		card.speakerName);
	const auto nameNatural = std::min(
		nameFull,
		int(st::conversationSpeakerNameMax));
	const auto fixed = userpic + skip + skip + pillWidth;
	const auto oneLine = fixed + skip + joinWidth;
	// Everything on one line when it fits (the name may shrink a little);
	// otherwise the button gets its own line.
	const auto wrap = (oneLine + std::min(
		nameNatural,
		int(st::conversationSpeakerNameMin)) > inner);
	const auto nameRoom = wrap
		? (inner - fixed)
		: (inner - oneLine);
	const auto nameWidth = std::clamp(nameRoom, 0, nameNatural);
	result.nameText = (nameWidth >= nameFull)
		? card.speakerName
		: st::conversationSpeakerNameFont->elided(card.speakerName, nameWidth);
	const auto nameShown = std::min(
		st::conversationSpeakerNameFont->width(result.nameText),
		nameWidth);
	result.userpic = QRect(left, top + (line - userpic) / 2, userpic, userpic);
	result.name = QRect(
		left + userpic + skip,
		top + (line - st::conversationSpeakerNameFont->height) / 2,
		nameShown,
		st::conversationSpeakerNameFont->height);
	const auto pillFits = (fixed + nameShown <= inner);
	if (pillFits) {
		result.pill = QRect(
			result.name.x() + nameShown + (nameShown ? skip : 0),
			top + (line - int(st::conversationPillHeight)) / 2,
			pillWidth,
			st::conversationPillHeight);
	}
	if (wrap) {
		top += line + skip;
		result.join = QRect(left, top, inner, joinHeight);
		top += joinHeight;
	} else {
		result.join = QRect(
			left + inner - joinWidth,
			top + (line - joinHeight) / 2,
			joinWidth,
			joinHeight);
		top += line;
	}
	top += padding.bottom();
	result.card = QRect(
		margin.left(),
		margin.top(),
		cardWidth,
		top - margin.top());
	result.height = top + margin.bottom();
	return result;
}

int CardPainter::height(const Card &card, int width) const {
	return layout(card, width).height;
}

int CardPainter::actionAt(const Card &card, QPoint point, int width) const {
	const auto geometry = layout(card, width);
	if (style::rtlrect(geometry.join, width).contains(point)) {
		return kCardJoin;
	} else if (!card.anonymous) {
		const auto speaker = geometry.userpic.united(geometry.name);
		if (style::rtlrect(speaker, width).contains(point)) {
			return kCardSpeaker;
		}
	}
	return kCardNone;
}

void CardPainter::paint(
		Painter &p,
		const Card &card,
		const QRect &rect,
		const CardState &state,
		qint64 nowMs) {
	const auto width = rect.width();
	const auto geometry = layout(card, width);
	p.translate(rect.topLeft());

	// ---- the surface
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto border = card.own ? 1.5 : 1.;
		const auto half = border / 2.;
		const auto body = QRectF(style::rtlrect(geometry.card, width))
			.marginsRemoved(QMarginsF(half, half, half, half));
		const auto over = state.over && !state.overAction;
		p.setPen(QPen(
			card.own
				? st::conversationCardOwnBorder->c
				: over
				? st::conversationCardBorderOver->c
				: st::conversationCardBorder->c,
			border));
		p.setBrush(card.own
			? st::conversationCardOwnBg
			: card.anonymous
			? st::conversationCardAnonBg
			: over
			? st::conversationCardBgOver
			: st::conversationCardBg);
		p.drawPath(TailedRect(
			body,
			st::conversationCardRadius,
			st::conversationCardTail));
	}

	// ---- ring, remaining, category
	const auto created = card.item.expiresAt - kDefaultWindowSeconds;
	PaintExpiryRing(
		p,
		QRectF(style::rtlrect(geometry.ring, width)),
		ExpiryFraction(card.item.expiresAt, created, nowMs),
		ExpiryFinal(card.item.expiresAt, nowMs),
		st::conversationRingStroke,
		st::conversationRingTrack->c);
	p.setFont(st::conversationRemainingFont);
	p.setPen(st::conversationRemainingFg);
	p.drawTextLeft(
		geometry.remainingLeft,
		geometry.ring.y()
			+ (geometry.ring.height() - st::conversationRemainingFont->height) / 2,
		width,
		st::conversationRemainingFont->elided(
			RemainingText(card.item.expiresAt, nowMs),
			geometry.remainingWidth));
	if (!geometry.tag.isEmpty()) {
		const auto tag = style::rtlrect(geometry.tag, width);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(QPen(st::conversationTagBorder->c, 1.));
			p.setBrush(Qt::NoBrush);
			p.drawPath(TailedRect(
				QRectF(tag).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
				st::conversationTagRadius,
				st::conversationCardTail));
		}
		p.setFont(st::conversationTagFont);
		p.setPen(card.anonymous
			? st::conversationTopicAnonFg
			: st::conversationTagFg);
		p.drawTextCentered(tag, geometry.tagText);
	}

	// ---- topic
	p.setPen((card.anonymous && !card.own)
		? st::conversationTopicAnonFg
		: st::conversationTopicFg);
	auto context = Ui::Text::PaintContext();
	context.position = style::rtlrect(geometry.topic, width).topLeft();
	context.availableWidth = geometry.topic.width();
	context.elisionLines = st::conversationTopicLines;
	context.autoDirection = true;
	card.topic.draw(p, context);

	// ---- speaker
	const auto userpic = style::rtlrect(geometry.userpic, width);
	if (card.anonymous) {
		_userpics.paintSpecial(
			p,
			userpic,
			Ui::UserpicKind::Anonymous,
			card.item.id,
			card.speakerName);
	} else {
		_userpics.paint(p, userpic, card.item.speakerId);
	}
	if (geometry.name.width() > 0) {
		const auto over = (state.overAction == kCardSpeaker);
		p.setFont(over
			? st::conversationSpeakerNameFont->underline()
			: st::conversationSpeakerNameFont);
		p.setPen(card.anonymous
			? st::conversationTopicAnonFg
			: st::conversationSpeakerNameFg);
		p.drawTextLeft(
			geometry.name.x(),
			geometry.name.y(),
			width,
			geometry.nameText);
	}
	if (!geometry.pill.isEmpty()) {
		const auto pill = style::rtlrect(geometry.pill, width);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(card.own
				? st::conversationPillOwnBg
				: st::conversationPillBg);
			p.drawPath(TailedRect(
				QRectF(pill),
				st::conversationPillRadius,
				st::conversationCardTail));
			const auto dot = int(st::conversationPillDot);
			const auto dotRect = style::rtlrect(
				geometry.pill.x() + int(st::conversationPillPadding),
				geometry.pill.y() + (geometry.pill.height() - dot) / 2,
				dot,
				dot,
				width);
			p.setBrush(card.own
				? st::conversationPillOwnFg
				: st::conversationPillDotFg);
			p.drawEllipse(dotRect);
		}
		p.setFont(st::conversationPillFont);
		p.setPen(card.own ? st::conversationPillOwnFg : st::conversationPillFg);
		p.drawTextLeft(
			geometry.pill.x()
				+ int(st::conversationPillPadding)
				+ int(st::conversationPillDot)
				+ int(st::conversationPillDotSkip),
			geometry.pill.y()
				+ (geometry.pill.height() - st::conversationPillFont->height) / 2,
			width,
			geometry.pillText);
	}

	// ---- the button
	const auto join = style::rtlrect(geometry.join, width);
	const auto joinOver = (state.overAction == kCardJoin)
		|| (state.pressedAction == kCardJoin);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(card.own
			? QPen(st::conversationCardOwnBorder->c, 1.)
			: QPen(Qt::NoPen));
		p.setBrush(card.own
			? (joinOver
				? st::conversationJoinOwnBgOver
				: st::conversationJoinOwnBg)
			: (joinOver ? st::conversationJoinBgOver : st::conversationJoinBg));
		p.drawPath(TailedRect(
			QRectF(join).marginsRemoved(card.own
				? QMarginsF(0.5, 0.5, 0.5, 0.5)
				: QMarginsF()),
			st::conversationJoinRadius,
			st::conversationCardTail));
	}
	const auto textWidth = st::conversationJoinFont->width(geometry.joinText);
	const auto iconWidth = card.own
		? 0
		: (st::conversationJoinIcon.width() + int(st::conversationJoinIconSkip));
	auto contentLeft = geometry.join.x()
		+ (geometry.join.width() - textWidth - iconWidth) / 2;
	if (!card.own) {
		st::conversationJoinIcon.paint(
			p,
			contentLeft,
			geometry.join.y()
				+ (geometry.join.height() - st::conversationJoinIcon.height()) / 2,
			width);
		contentLeft += iconWidth;
	}
	p.setFont(st::conversationJoinFont);
	p.setPen(card.own ? st::conversationJoinOwnFg : st::conversationJoinFg);
	p.drawTextLeft(
		contentLeft,
		geometry.join.y()
			+ (geometry.join.height() - st::conversationJoinFont->height) / 2,
		width,
		geometry.joinText,
		textWidth);
}

int CardPainter::SkeletonHeight() {
	return st::conversationSkeletonHeight
		+ st::conversationCardMargin.top()
		+ st::conversationCardMargin.bottom();
}

void CardPainter::PaintSkeleton(
		QPainter &p,
		const QRect &rect,
		const Ui::SkeletonAnimation &animation,
		const QRect &outer) {
	const auto &margin = st::conversationCardMargin;
	const auto &padding = st::conversationCardPadding;
	const auto card = rect.marginsRemoved(margin);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::conversationCardBorder->c, 1.));
		p.setBrush(st::conversationCardBg);
		p.drawPath(TailedRect(
			QRectF(card).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
			st::conversationCardRadius,
			st::conversationCardTail));
	}
	const auto inner = card.marginsRemoved(padding);
	const auto radius = double(st::conversationSkeletonRadius);
	const auto line = int(st::conversationSkeletonLine);
	const auto title = int(st::conversationSkeletonTitle);
	const auto bar = [&](int top, int height, double part) {
		const auto width = int(inner.width() * part);
		animation.paint(
			p,
			QRectF(style::rtlrect(
				inner.x() - rect.x(),
				top,
				width,
				height,
				rect.width()).translated(rect.x(), 0)),
			radius,
			outer);
	};
	auto top = inner.y() + 4;
	bar(top, line, 0.4);
	top += line + 16;
	bar(top, title, 0.85);
	top += title + 18;
	bar(top, 30, 0.55);
}

} // namespace Conversation
