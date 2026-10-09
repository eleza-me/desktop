// A topic card (an open anonymous conversation) painted inside a virtualised
// list: the expiry ring with «N ساعت مانده», the category, the topic, the
// speaker with the «منتظر شنونده» pill and the join button. Shared by the
// topics list and by the search results.
//
//   Conversation::CardPainter _cards{ &session, [=](const QString &) { _list->update(); } };
//   auto card = _cards.make(item);
//   _cards.height(card, width);  _cards.actionAt(card, point, width);
//   _cards.paint(p, card, rect, { .over = ..., .overAction = ... }, nowMs);
#pragma once

#include "data/data_social_types.h"
#include "support/media_image.h"
#include "ui/text/text.h"

#include <QtCore/QObject>
#include <QtGui/QPainterPath>

class Painter;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class SkeletonAnimation;
} // namespace Ui

namespace Conversation {

enum CardAction {
	kCardNone = 0,
	kCardJoin = 1,
	kCardSpeaker = 2,
};

struct Card {
	Data::OpenConversation item;
	Ui::Text::String topic;
	QString category;
	QString speakerName;
	bool anonymous = false;
	// My own topic: highlighted, «مشاهده» instead of joining.
	bool own = false;
};

struct CardState {
	bool over = false;
	int overAction = 0;
	int pressedAction = 0;
};

// Three round corners and the design's small "tail" corner.
[[nodiscard]] QPainterPath TailedRect(const QRectF &rect, double radius, double tail);

// The ring: `fraction` of it is filled, amber when `final`.
void PaintExpiryRing(
	QPainter &p,
	const QRectF &rect,
	double fraction,
	bool final,
	double stroke,
	const QColor &track);

class CardPainter final : public QObject {
public:
	CardPainter(
		Main::Session *session,
		std::function<void(const QString &userId)> update,
		QObject *parent = nullptr);

	// `forceAnonymous`: search results never name the speaker.
	[[nodiscard]] Card make(
		const Data::OpenConversation &item,
		bool forceAnonymous = false) const;
	// Re-reads the speaker's name (the card arrived). True if it changed.
	bool refreshName(Card &card) const;

	[[nodiscard]] int height(const Card &card, int width) const;
	// `point` in the row's physical coordinates.
	[[nodiscard]] int actionAt(const Card &card, QPoint point, int width) const;
	void paint(
		Painter &p,
		const Card &card,
		const QRect &rect,
		const CardState &state,
		qint64 nowMs);

	[[nodiscard]] static int SkeletonHeight();
	static void PaintSkeleton(
		QPainter &p,
		const QRect &rect,
		const Ui::SkeletonAnimation &animation,
		const QRect &outer);

	void clearUserpics() {
		_userpics.clear();
	}

private:
	struct Layout {
		QRect card; // everything in logical (leading-side) coordinates
		QRect ring;
		QRect tag;
		int remainingLeft = 0;
		int remainingWidth = 0;
		QRect topic;
		QRect userpic;
		QRect name;
		QRect pill;
		QRect join;
		int height = 0;
		QString nameText;
		QString pillText;
		QString joinText;
		QString tagText;
	};

	[[nodiscard]] Layout layout(const Card &card, int width) const;
	[[nodiscard]] bool isOwn(const Data::OpenConversation &item) const;

	QPointer<Main::Session> _session;
	Support::UserpicPainter _userpics;

};

} // namespace Conversation
