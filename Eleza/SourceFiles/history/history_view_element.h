// One row of the chat: a message laid out and painted by hand (no widget per
// message). The list owns the elements, gives each its width, paints the ones
// inside the viewport and asks them what is under the mouse.
//
// An element caches its text layouts for the width it was laid out with. Far
// from the viewport the list calls unloadHeavy(): the layouts and images go,
// the height stays, and everything is rebuilt when the row is needed again.
//
// Sides are physical on purpose: my messages hug the right edge and the
// peer's the left one in both interface directions, as in Telegram.
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
	Preview,       // the link preview card
	Reaction,      // a reaction chip: `text`
	Button,        // an inline keyboard button: `row`, `column`
	Retry,         // the «!» of a failed message
	ViaBot,
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
	Element(Delegate *delegate, const Data::Message &message);
	~Element();

	[[nodiscard]] const Data::Message &data() const {
		return _data;
	}
	[[nodiscard]] const QString &key() const {
		return _key;
	}
	// Returns true when the height may have changed.
	bool setData(const Data::Message &message);
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

	Delegate *_delegate = nullptr;
	Data::Message _data;
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
	std::vector<ReactionChip> _reactions;
	std::vector<KeyboardButton> _keyboard;
	QString _metaText;
	QString _forwardedText;
	QString _viaText;
	QString _serviceText;
	QString _dateText;

	// ---- layout (physical coordinates inside the row)
	int _dateTop = 0;
	int _dateBottom = 0;
	int _unreadTop = -1;
	int _contentTop = 0;
	QRect _bubble;
	QRect _serviceRect;
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
