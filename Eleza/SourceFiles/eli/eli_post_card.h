// Eli::PostCard — one post laid out and painted by hand (not a widget): the
// header, the text with its links and «نمایش بیشتر», the block of its type
// (media grid, video, round video note, audio clip, poll, quote, conversation
// invite, thread), the action row. Lists (eli_post_list.h) own the cards, give
// them a width, paint the visible ones and route the mouse through hitTest().
//
// Three looks: Feed (lists), Detail (the opened post: larger, with the date,
// the people counts and the follow button) and Reply (compact rows under a
// post, one level of nesting).
//
// A card keeps its text layouts for the last width and holds media handles
// only between loadMedia() and unloadMedia() (the list calls them for cards
// near the viewport); tiles reserve their boxes from the media's size and show
// the blurhash until the picture arrives, so nothing jumps.
#pragma once

#include "data/data_media.h"
#include "data/data_social_types.h"
#include "ui/text/text.h"

#include <QtCore/QPointer>
#include <QtGui/QPixmap>

#include <memory>
#include <vector>

class Painter;
class QPainter;

namespace Main {
class Session;
} // namespace Main

namespace Support {
class UserpicPainter;
} // namespace Support

namespace Eli {

enum class CardMode {
	Feed,
	Detail,
	Reply,
};

struct CardOptions {
	CardMode mode = CardMode::Feed;
	bool pinned = false;
	// Why the feed shows the post: "repost" (by `actor`), "follows".
	QString reason;
	QString actor;
	// Reply rows.
	int depth = 0;
	bool line = false; // the next row hangs under this one
	QString replyingTo; // the author a nested reply answers (user id)
	// Detail of a thread: the parts are shown in full.
	bool expandThread = false;
};

enum class HitType {
	None,
	Body, // the card itself: opens the post
	Userpic,
	Author,
	More,
	Link, // index = the link of the text
	ShowMore,
	Media, // index = the tile
	VideoNote,
	PollOption, // index = the option
	PollRetract,
	Quote,
	QuoteMedia,
	InviteJoin,
	Audio,
	Thread,
	Reply,
	Repost,
	Like,
	Dislike,
	Bookmark,
	Share,
	LikesStat,
	DislikesStat,
	Follow,
	ContextActor,
	ReplyingTo,
};

struct Hit {
	HitType type = HitType::None;
	int index = -1;

	friend bool operator==(const Hit &, const Hit &) = default;
};

// What the cards of one list share.
class CardHost {
public:
	[[nodiscard]] virtual Main::Session &cardSession() const = 0;
	[[nodiscard]] virtual Support::UserpicPainter &cardUserpics() = 0;
	[[nodiscard]] virtual double cardPixelRatio() const = 0;
	// Something of the card changed without changing its height.
	virtual void cardRepaint(const QString &postId) = 0;
	// Guard for the connections of media handles.
	[[nodiscard]] virtual QObject *cardGuard() = 0;

protected:
	~CardHost() = default;

};

struct CardPaintState {
	Hit hover;
	Hit pressed;
	bool selected = false; // keyboard selection
	QRect clip; // card coordinates
	// 0..1 of the heart burst (double click), < 0 when there is none.
	double burst = -1.;
	QPoint burstCenter;
};

class PostCard final {
public:
	PostCard(CardHost *host, const QString &postId, CardOptions options);
	~PostCard();

	[[nodiscard]] const QString &id() const {
		return _post.id;
	}
	[[nodiscard]] const Data::Post &post() const {
		return _post;
	}
	[[nodiscard]] const CardOptions &options() const {
		return _options;
	}
	void setOptions(CardOptions options);

	// Re-reads the post from the store and rebuilds what is shown. Returns
	// true when the height may have changed.
	bool refresh();
	// Names / handles came or changed.
	void refreshPeople();
	// The relative time label (once a minute for visible cards).
	void refreshTime();
	// My relationship with the author (Detail): "none" | "requested" |
	// "following", empty hides the button.
	void setFollowState(const QString &state);
	[[nodiscard]] const QString &followState() const {
		return _followState;
	}

	int resizeGetHeight(int width);
	[[nodiscard]] int height() const {
		return _height;
	}
	[[nodiscard]] int width() const {
		return _width;
	}

	// The painter is translated to the card's top left corner.
	void paint(Painter &p, const CardPaintState &state) const;
	[[nodiscard]] Hit hitTest(QPoint point) const;
	// Where a hit is painted (anchors menus, limits repaints); the whole card
	// for Body.
	[[nodiscard]] QRect hitRect(Hit hit) const;
	[[nodiscard]] Ui::Text::Link link(int index) const;
	[[nodiscard]] QString tooltip(Hit hit) const;
	[[nodiscard]] static bool Clickable(HitType type);

	void setExpanded(bool expanded);
	[[nodiscard]] bool expanded() const {
		return _expanded;
	}

	void loadMedia();
	void unloadMedia();
	[[nodiscard]] bool mediaLoaded() const {
		return _mediaLoaded;
	}
	// Palette changed: prepared pixmaps depend on nothing from it, texts do not
	// cache colors either — only the previews are kept.
	[[nodiscard]] bool hasAudio() const {
		return _block == Block::Audio;
	}
	[[nodiscard]] QString audioTrackId() const;

	// The images of the post in order (the viewer), and the index of a tile
	// among them (-1 for a video tile).
	[[nodiscard]] int imageIndexOfTile(int tile) const;
	[[nodiscard]] bool tileIsVideo(int tile) const;
	[[nodiscard]] QImage tileImage(int tile) const;

private:
	enum class Block {
		None,
		Media,
		VideoNote,
		Poll,
		Quote,
		Invite,
		Audio,
		Thread,
	};
	struct Tile {
		QString postId;
		Data::PostMedia media;
		Data::MediaVariant variant = Data::MediaVariant::Feed;
		QRect rect; // physical, card coordinates
		bool round = false;
		bool quote = false;
		int more = 0;
		Data::MediaHandlePtr handle;
		QMetaObject::Connection ready;
		QMetaObject::Connection failed;
		mutable QPixmap prepared;
		mutable QSize preparedFor;
		mutable bool preparedFinal = false;
		mutable QImage preview;
		mutable bool previewTried = false;
	};
	struct Area {
		Hit hit;
		QRect rect; // physical, card coordinates
	};
	struct Action {
		HitType type = HitType::None;
		QRect rect; // physical: the icon circle + the count
		QRect circle;
		QString count;
		int countWidth = 0;
		bool active = false;
		bool disabled = false;
	};
	struct Part {
		Ui::Text::String text;
		int top = 0;
		int height = 0;
	};
	struct Stat {
		HitType type = HitType::None;
		QString count;
		QString label;
		QRect rect;
		int countWidth = 0;
	};

	[[nodiscard]] bool feed() const {
		return _options.mode == CardMode::Feed;
	}
	[[nodiscard]] bool detail() const {
		return _options.mode == CardMode::Detail;
	}
	[[nodiscard]] bool reply() const {
		return _options.mode == CardMode::Reply;
	}
	[[nodiscard]] bool mine() const;
	[[nodiscard]] QRect rtl(int x, int y, int w, int h) const;
	[[nodiscard]] QRect rtl(const QRect &rect) const;

	void rebuildTexts();
	void rebuildTiles();
	void rebuildActions();
	void layout();
	int layoutBlock(int left, int top, int width);
	int layoutMedia(int left, int top, int width, int first, int count, int maxHeight);
	int layoutPoll(int left, int top, int width);
	int layoutQuote(int left, int top, int width);
	int layoutInvite(int left, int top, int width);
	int layoutActions(int left, int top, int width);
	int layoutDetailFooter(int left, int top, int width);
	void addArea(HitType type, const QRect &rect, int index = -1);

	void paintContext(Painter &p) const;
	void paintHeader(Painter &p, const CardPaintState &state) const;
	void paintText(Painter &p, const CardPaintState &state) const;
	void paintTile(Painter &p, const Tile &tile, bool over) const;
	void paintPoll(Painter &p, const CardPaintState &state) const;
	void paintQuote(Painter &p, const CardPaintState &state) const;
	void paintInvite(Painter &p, const CardPaintState &state) const;
	void paintAudio(Painter &p, const CardPaintState &state) const;
	void paintThread(Painter &p, const CardPaintState &state) const;
	void paintActions(Painter &p, const CardPaintState &state) const;
	void paintDetailFooter(Painter &p, const CardPaintState &state) const;
	void prepareTile(const Tile &tile) const;
	void requestTile(Tile &tile);

	CardHost *_host = nullptr;
	CardOptions _options;
	Data::Post _post;
	bool _expanded = false;
	bool _mediaLoaded = false;
	QString _followState;

	// Content.
	Block _block = Block::None;
	QString _name;
	QString _handle; // "@sara", isolated as left-to-right
	bool _verified = false;
	QString _time;
	QString _contextText;
	QString _replyingText;
	Ui::Text::String _text;
	bool _deletedText = false;
	std::vector<Part> _parts; // thread parts after the first (Detail)
	std::vector<Tile> _tiles;
	int _quoteTilesFrom = 0;
	// Poll.
	QVector<int> _pollPercents;
	bool _pollResults = false;
	bool _pollClosed = false;
	QString _pollFooter;
	QString _pollRetract;
	// Quote.
	QString _quoteName;
	QString _quoteMeta;
	QString _quoteTime;
	Ui::Text::String _quoteText;
	bool _quoteUnavailable = false;
	// Invite.
	Ui::Text::String _inviteTopic;
	QString _invitePill;
	QString _inviteLeft;
	bool _inviteOpen = false;
	bool _inviteJoin = false;
	// Audio.
	QString _audioTitle;
	QString _audioSub;
	// Thread.
	QString _threadMeta;
	// Detail footer.
	QString _detailMeta;
	std::vector<Stat> _stats;
	std::vector<Action> _actions;

	// Layout (physical card coordinates unless noted).
	int _width = 0;
	int _height = 0;
	int _layoutWidth = -1;
	QRect _userpicRect;
	QRect _contextRect;
	QRect _headerRect; // logical: name .. time
	QRect _moreRect;
	QRect _followRect;
	QRect _replyingRect;
	QRect _nameRect;
	int _nameWidth = 0;
	int _handleWidth = 0;
	int _timeWidth = 0;
	bool _handleShown = true;
	QString _nameElided;
	QString _timeText;
	QString _handleElided;
	QRect _textRect;
	int _textLines = 0;
	bool _textClamped = false;
	QRect _showMoreRect;
	QRect _blockRect;
	std::vector<QRect> _pollRects;
	QRect _pollFooterRect;
	QRect _pollRetractRect;
	QRect _quoteHeaderRect;
	QRect _quoteTextRect;
	QRect _inviteTopicRect;
	QRect _inviteButtonRect;
	QRect _audioPlayRect;
	QRect _threadCtaRect;
	QRect _detailMetaRect;
	QRect _statsRect;
	QRect _actionsRect;
	int _lineTop = 0; // the reply thread line
	std::vector<Area> _areas;

};

// A solid play triangle in the middle of `circle` (the icon set only has an
// outlined one that takes a tint).
void PaintPlayGlyph(QPainter &p, const QRect &circle, const QColor &color);
// A post's text with its mentions / hashtags / links marked (and isolated so
// that each keeps its direction inside text of the other one).
[[nodiscard]] Ui::Text::TextWithEntities PostText(
	const QString &text,
	bool links = true);
// "@handle" wrapped so that it reads left to right inside Persian text.
[[nodiscard]] QString IsolatedHandle(const QString &handle);
// The author's name as cards show it («حساب حذف‌شده» for a deleted account,
// «…» until the card arrives).
[[nodiscard]] QString AuthorName(Main::Session &session, const QString &userId);
// «۵ دقیقه», «دیروز», … for a post time in milliseconds.
[[nodiscard]] QString PostTime(qint64 createdAtMs);
// «۳ ساعت مانده».
[[nodiscard]] QString TimeLeft(qint64 endsAtSeconds);

} // namespace Eli
