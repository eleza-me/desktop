// One row of the chat: a message laid out and painted by hand (no widget per
// message). The list owns the elements, gives each its width, paints the ones
// inside the viewport and asks them what is under the mouse.
//
// An element caches its text layouts for the width it was laid out with. Far
// from the viewport the list calls unloadHeavy(): the layouts and images go,
// the height stays, and everything is rebuilt when the row is needed again.
//
// Sides are physical on purpose: my messages hug the right edge and the
// peer's the left one in both interface directions.
#pragma once

#include "data/data_types.h"
#include "history/history_logic.h"
#include "history/history_themes.h"
#include "ui/text/text.h"

#include <QtCore/QRect>

#include <memory>

class Painter;

namespace Main {
class Session;
} // namespace Main

namespace Data {
struct Post;
} // namespace Data

namespace History::View {

class Element;
class Media;
class NoteReplyView;

enum class Zone {
	None,
	Side,          // the row outside of the bubble
	Bubble,        // the bubble where nothing special is
	Text,          // message text (selectable)
	Link,          // a link inside the text: `link`
	Spoiler,       // a hidden spoiler inside the text
	Reply,         // the reply header
	Media,         // photo / video / sticker / round video: opens it
	MediaSpoiler,  // a hidden photo / video
	MediaCancel,   // the ring of an upload / a download
	VoicePlay,
	VoiceSeek,     // the waveform / seek line: `ratio`
	VoiceSpeed,
	File,          // the file row: download / open
	Location,
	LiveStop,
	Post,          // a shared feed post
	Story,         // the story a reply / a reaction points at
	Preview,       // the link preview card
	Reaction,      // a reaction chip: `text`
	Button,        // an inline keyboard button: `row`, `column`
	Retry,         // the «!» of a failed message
	ViaBot,
	Sender,        // a group: the name above a bubble or the picture next to it
	Date,          // the day pill / a service message
};

struct HitResult {
	Zone zone = Zone::None;
	int link = -1;
	int symbol = 0;      // text hit-testing (Zone::Text / Link / Spoiler)
	int cursor = 0;
	bool uponSymbol = false;
	bool captionText = false;
	int row = 0;
	int column = 0;
	double ratio = 0.;
	QString text;
	int item = -1;       // an album: the tile under the point (Zone::Media…)

	[[nodiscard]] bool inside() const {
		return zone != Zone::None && zone != Zone::Side && zone != Zone::Date;
	}
};

// State of a file download as the row shows it.
struct DownloadState {
	bool loading = false;
	double progress = 0.;
	QString path; // where the saved copy is, when there is one
	bool failed = false; // the last attempt failed (until the next one)
	bool oversized = false; // … because the file was larger than declared
};

// What an audio row shows of the player.
struct PlaybackState {
	bool current = false;
	bool playing = false;
	bool loading = false;
	double progress = 0.;
	qint64 positionMs = 0;
	double speed = 1.;
};

// Who wrote an incoming message of a group.
struct SenderInfo {
	QString userId;
	QString name;       // empty: no name line (a group that hides nothing still may have no card yet)
	QString tag;        // «مالک», «مدیر» or an admin's own title; empty for a member
	int colorIndex = 0; // Data::GroupRules::NameColorIndex: the same person, the same colour
};

class Delegate {
public:
	[[nodiscard]] virtual Main::Session *elementSession() = 0;
	// Connections of images are bound to this object.
	[[nodiscard]] virtual QObject *elementGuard() = 0;
	[[nodiscard]] virtual double elementRatio() = 0;
	[[nodiscard]] virtual const ChatColors &elementColors() = 0;
	// The row has to be painted again / may have another height now.
	virtual void elementRepaint(Element *element) = 0;
	virtual void elementRelayout(Element *element) = 0;
	// The message a reply points to: null while it is not known (the
	// delegate fetches it once and calls elementRelayout).
	[[nodiscard]] virtual const Data::Message *elementReplyTarget(
		const Data::Message &message) = 0;
	[[nodiscard]] virtual QString elementSenderName(
		const Data::Message &message) = 0;
	// Null while the post is being fetched (elementRelayout follows).
	[[nodiscard]] virtual const Data::Post *elementPost(
		const QString &postId) = 0;
	[[nodiscard]] virtual DownloadState elementDownload(
		const Data::Message &message) = 0;
	[[nodiscard]] virtual PlaybackState elementPlayback(
		const Data::Message &message) = 0;
	[[nodiscard]] virtual bool elementRevealed(const QString &key) = 0;
	// A pressed inline button is waiting for the bot's answer.
	[[nodiscard]] virtual bool elementButtonLoading(
		const Data::Message &message,
		int row,
		int column) = 0;

	// A chat with a bot: «/command» in a message is a link that sends it.
	[[nodiscard]] virtual bool elementBotCommands() {
		return false;
	}

	// ---- groups (a chat of two answers none of these)
	// Incoming messages name their sender above the first bubble of a run
	// and show the sender's picture next to the last one.
	[[nodiscard]] virtual bool elementGroup() {
		return false;
	}
	[[nodiscard]] virtual SenderInfo elementSender(
			const Data::Message &message) {
		Q_UNUSED(message);
		return SenderInfo();
	}
	virtual void elementPaintUserpic(
			Painter &p,
			const QRect &rect,
			const QString &userId) {
		Q_UNUSED(p);
		Q_UNUSED(rect);
		Q_UNUSED(userId);
	}
	// The words of a service message when the delegate knows more than the
	// message says (the names of the people in a note of a group); empty:
	// the common wording.
	[[nodiscard]] virtual QString elementServiceText(
			const Data::Message &message) {
		Q_UNUSED(message);
		return QString();
	}

protected:
	~Delegate() = default;

};

struct PaintContext {
	QRect clip;                       // in the element's coordinates
	bool selected = false;            // the whole message is selected
	Ui::Text::Selection textSelection; // inside the text / caption
	double highlight = 0.;            // the flash after a jump, 0..1
	double dateOpacity = 1.;          // the day pill fades under the floating one
	Ui::Text::Selection highlightRange; // the quoted part to mark, if any
	Zone hovered = Zone::None;
	int hoveredLink = -1;
	int hoveredRow = -1;
	int hoveredColumn = -1;
	QString hoveredText;
	bool searchMatch = false;
};

class Element final {
public:
	// `album`: the items of an album bubble (two or more photos / videos);
	// `message` is then the bubble's head (History::AlbumHeadMessage).
	Element(
		Delegate *delegate,
		const Data::Message &message,
		const QVector<Data::Message> &album = {});
	~Element();

	[[nodiscard]] const Data::Message &data() const {
		return _data;
	}
	[[nodiscard]] const QString &key() const {
		return _key;
	}
	// Returns true when the height may have changed.
	bool setData(
		const Data::Message &message,
		const QVector<Data::Message> &album = {});

	// ---- an album bubble
	[[nodiscard]] bool isAlbum() const {
		return !_album.isEmpty();
	}
	[[nodiscard]] const QVector<Data::Message> &album() const {
		return _album;
	}
	// The item `index` of the album (the head itself out of range).
	[[nodiscard]] const Data::Message &albumItem(int index) const;
	// The message a part of the row stands for: an album's tile or the head.
	[[nodiscard]] const Data::Message &messageAt(const HitResult &hit) const;
	// The messages of the row: the album's items, or the message.
	[[nodiscard]] QVector<Data::Message> messages() const;
	// The item that carries the caption (the head when there is none).
	[[nodiscard]] const Data::Message &captionMessage() const;
	// The newest seq of the row (an album's last item).
	[[nodiscard]] qint64 lastSeq() const;
	bool setFlags(const RowFlags &flags);
	[[nodiscard]] const RowFlags &flags() const {
		return _flags;
	}

	// Lays the row out for the width (cached) and returns its height.
	int resizeGetHeight(int width);
	[[nodiscard]] int height() const {
		return _height;
	}
	[[nodiscard]] int layoutWidth() const {
		return _width;
	}
	[[nodiscard]] bool laidOutFor(int width) const {
		return !_dirty && _width == width;
	}
	// A guess for a row that was never laid out.
	[[nodiscard]] int estimateHeight(int width) const;
	void invalidate() {
		_dirty = true;
	}

	void paint(Painter &p, const PaintContext &context);
	[[nodiscard]] HitResult hitTest(QPoint point) const;

	// Drops what can be rebuilt (text layouts, images).
	void unloadHeavy();
	[[nodiscard]] bool heavyLoaded() const {
		return _loaded;
	}

	// ---- geometry for the list
	[[nodiscard]] QRect bubbleRect() const {
		return _bubble;
	}
	// The part that flashes / is marked as selected: the row without the
	// day pill above it.
	[[nodiscard]] QRect contentRect() const;
	[[nodiscard]] int dateBottom() const {
		return _dateBottom;
	}
	[[nodiscard]] int dateTop() const {
		return _dateTop;
	}
	[[nodiscard]] QString dateText() const;
	[[nodiscard]] QRect mediaRect() const {
		return _mediaRect;
	}
	[[nodiscard]] bool isService() const {
		return _service;
	}

	// ---- text
	[[nodiscard]] bool hasText() const;
	[[nodiscard]] const QString &text() const;
	[[nodiscard]] int textLength() const;
	[[nodiscard]] QString selectedText(Ui::Text::Selection selection) const;
	[[nodiscard]] Ui::Text::Selection adjustSelection(
		Ui::Text::Selection selection,
		Ui::Text::SelectType type) const;
	[[nodiscard]] const Ui::Text::Link *link(int index) const;
	void setSpoilersRevealed(bool revealed);
	// A text position for a point anywhere in the row (for drag selection).
	[[nodiscard]] int textCursorAt(QPoint point) const;

	[[nodiscard]] Media *media() const {
		return _media.get();
	}

private:
	struct ReplyView;
	struct PreviewView;
	struct ReactionChip {
		QString emoji;
		QString count;
		QRect rect;
		bool mine = false;
	};
	struct KeyboardButton {
		QString text;
		QRect rect;
		int row = 0;
		int column = 0;
		bool url = false;
	};

	void ensureLoaded();
	void createMedia();
	void layout(int width);
	void layoutService(int width, int top);
	void layoutBubble(int maxWidth);
	void layoutKeyboard(int top);
	[[nodiscard]] int metaWidth() const;
	[[nodiscard]] QString metaText() const;
	[[nodiscard]] bool outgoing() const {
		return _data.mine;
	}
	[[nodiscard]] bool bubbleless() const;
	[[nodiscard]] bool failed() const {
		return _data.status == Data::MessageStatus::Failed;
	}
	[[nodiscard]] Ui::Text::PaintContext textGeometry() const;

	void paintDate(Painter &p) const;
	void paintUnread(Painter &p) const;
	void paintService(Painter &p) const;
	void paintBubble(Painter &p, const ChatColors &colors) const;
	void paintReply(Painter &p, const ChatColors &colors) const;
	void paintPreview(Painter &p, const ChatColors &colors) const;
	void paintReactions(
		Painter &p,
		const ChatColors &colors,
		const PaintContext &context) const;
	void paintMeta(Painter &p, const ChatColors &colors) const;
	void paintKeyboard(Painter &p, const PaintContext &context) const;
	void paintRetry(Painter &p) const;
	void paintSender(Painter &p, const ChatColors &colors) const;
	[[nodiscard]] bool showsSenderName() const;
	[[nodiscard]] int userpicSpace() const;

	Delegate *_delegate = nullptr;
	Data::Message _data;
	QVector<Data::Message> _album;
	QString _key;
	RowFlags _flags;

	int _width = 0;
	int _height = 0;
	bool _dirty = true;
	bool _loaded = false;
	bool _service = false;
	int _jumbo = 0;

	// ---- loaded parts
	Ui::Text::String _text;
	std::unique_ptr<ReplyView> _reply;
	std::unique_ptr<PreviewView> _preview;
	std::unique_ptr<Media> _media;
	std::unique_ptr<NoteReplyView> _noteReply; // the note this text answers
	std::vector<ReactionChip> _reactions;
	std::vector<KeyboardButton> _keyboard;
	QString _metaText;
	QString _forwardedText;
	QString _viaText;
	QString _serviceText;
	QString _serviceShown; // elided for the current width
	QString _dateText;
	bool _group = false;       // an incoming message of a group
	SenderInfo _sender;
	QString _senderName;       // elided for the laid out width
	QString _senderTag;

	// ---- layout (physical coordinates inside the row)
	int _dateTop = 0;
	int _dateBottom = 0;
	int _unreadTop = -1;
	int _contentTop = 0;
	QRect _bubble;
	QRect _serviceRect;
	QRect _senderRect;         // the name line (inside the bubble)
	QRect _userpicRect;        // the sender's picture (beside the bubble)
	QRect _forwardedRect;
	QRect _viaRect;
	QRect _replyRect;
	QRect _mediaRect;
	QRect _textRect;
	QRect _previewRect;
	QRect _metaRect;
	QRect _retryRect;
	int _reactionsTop = 0;
	bool _metaOverMedia = false;
	bool _metaOutside = false;
	int _keyboardBottom = 0;

};

} // namespace History::View
