// The message list: a tall widget inside the section's scroll area that owns
// one History::View::Element per visible message, lays them out for its
// width, paints only the rows in the viewport and turns mouse input into
// actions (open, reply, select, menu…).
//
//   _scroll = Ui::CreateChild<Ui::ScrollArea>(this);
//   _list = _scroll->setOwnedWidget(Ui::object_ptr<ListWidget>(_scroll, controller, chatId, _scroll));
//   _list->start(route.seq());
//
// The store is the source of truth: on Data::Messages::historyChanged the list
// diffs the new timeline against its rows (by Message::key()), keeps the rows
// that survived with their layouts and keeps the viewport where it was:
// glued to the bottom when it was there, otherwise anchored to the row at
// the bottom edge.
#pragma once

#include "data/data_media.h"
#include "data/data_messages.h"
#include "history/history_view_element.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/rp_widget.h"

#include <QtCore/QBasicTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>

#include <memory>
#include <vector>

namespace Media {
class VideoPlayerWidget;
} // namespace Media

namespace Ui {
class PopupMenu;
class ScrollArea;
} // namespace Ui

namespace Support {
class UserpicPainter;
} // namespace Support

namespace Window {
class SessionController;
} // namespace Window

namespace History {

class Downloads;

class ListWidget final : public Ui::RpWidget, private View::Delegate {
	Q_OBJECT

public:
	ListWidget(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &chatId,
		Ui::ScrollArea *scroll);
	~ListWidget();

	// Shows what the database has, then reads the newest page (or the page
	// around `aroundSeq`).
	void start(qint64 aroundSeq = 0);
	// The section is on screen in an active window: messages that become
	// visible are marked as read.
	void setActive(bool active);
	void setTheme(const QString &theme);
	void setAnonymousPeer(bool anonymous);
	// Whether I can write to the chat (reply / edit / reactions make sense).
	void setCanWrite(bool canWrite);
	void setCanReact(bool canReact);
	// A chat with a bot: «/command» in the text is a link, buttons that
	// switch to inline mode use `botHandle` when the message names no bot.
	void setBotChat(bool bot, const QString &botHandle);

	// ---- navigation
	void jumpTo(qint64 seq, const std::optional<Data::Quote> &quote = std::nullopt);
	// To the newest message (loading the newest page when the window was
	// moved away by a jump).
	void jumpToBottom();
	[[nodiscard]] bool atBottom() const {
		return _atBottom;
	}
	// The viewport does not show the end of the chat: the reader scrolled
	// up (a message that comes in is no longer followed), or the window of
	// loaded messages is further up.
	[[nodiscard]] bool awayFromBottom() const;
	// The newest message at the bottom edge of the viewport (0: none).
	[[nodiscard]] qint64 bottomVisibleSeq() const;
	void scrollByPage(bool up);

	// ---- state
	[[nodiscard]] bool empty() const {
		return _rows.empty();
	}
	[[nodiscard]] bool loading() const {
		return _initialLoading;
	}
	[[nodiscard]] bool loadFailed() const {
		return _loadFailed;
	}
	void retryLoad();
	[[nodiscard]] std::optional<Data::Message> lastEditable() const;
	[[nodiscard]] const Data::Message *message(qint64 seq) const;

	// ---- selection
	[[nodiscard]] int selectedCount() const {
		return int(_selected.size());
	}
	[[nodiscard]] QVector<Data::Message> selectedMessages() const;
	[[nodiscard]] bool hasTextSelection() const;
	void clearSelection();
	// Copies the selected text / messages. False when nothing is selected.
	bool copySelection();
	// The sender's name as copied text / reply bars show it.
	[[nodiscard]] QString senderName(const Data::Message &message);

	// ---- debug: a timeline that is not in the store (the stress test)
#ifdef ELEZA_DEBUG_TOOLS
	void setDebugItems(const QVector<Data::Message> &items);
#endif // ELEZA_DEBUG_TOOLS
	struct PaintStats {
		int paints = 0;
		qint64 totalUs = 0;
		qint64 maxUs = 0;
		qint64 layoutUs = 0;
		int rows = 0;
	};
	[[nodiscard]] PaintStats paintStats() const {
		return _stats;
	}
	void resetPaintStats() {
		_stats = PaintStats();
	}

Q_SIGNALS:
	void replyRequested(
		const Data::Message &message,
		const std::optional<Data::Quote> &quote);
	void editRequested(const Data::Message &message);
	void forwardRequested(const QVector<Data::Message> &messages);
	void deleteRequested(const QVector<Data::Message> &messages);
	void selectionChanged();
	// atBottom() / awayFromBottom() / the load state changed.
	void stateChanged();
	// Something the user did should put the caret back into the composer.
	void focusRequested();
	// A «/command» in a bot's message was clicked: it is sent.
	void botCommandRequested(const QString &command);
	// «@bot query» for a composer: this chat's, or (pickChat) one the user
	// picks (a «switch inline» button, a «via @bot» label).
	void inlineQueryRequested(const QString &text, bool pickChat);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void timerEvent(QTimerEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	struct Row {
		std::unique_ptr<View::Element> element;
		int top = 0;
		int height = 0;
		bool exact = false; // laid out for the current width
	};
	struct Anchor {
		QString key;        // the row at the viewport's bottom edge
		int offset = 0;     // viewport bottom minus the row's bottom
		bool bottom = true; // glued to the end of the list
	};
	struct MouseTarget {
		int index = -1;
		QString key;
		View::HitResult hit;
		QPoint local; // inside the row
	};
	struct InlineVideo {
		QString key;
		QPointer<Media::VideoPlayerWidget> widget;
		Data::MediaHandlePtr handle;
		bool gif = false;
	};

	// ---- View::Delegate
	Main::Session *elementSession() override;
	QObject *elementGuard() override;
	double elementRatio() override;
	const ChatColors &elementColors() override;
	void elementRepaint(View::Element *element) override;
	void elementRelayout(View::Element *element) override;
	const Data::Message *elementReplyTarget(const Data::Message &message) override;
	QString elementSenderName(const Data::Message &message) override;
	const Data::Post *elementPost(const QString &postId) override;
	View::DownloadState elementDownload(const Data::Message &message) override;
	View::PlaybackState elementPlayback(const Data::Message &message) override;
	bool elementRevealed(const QString &key) override;
	bool elementButtonLoading(const Data::Message &message, int row, int column) override;
	bool elementGroup() override;
	bool elementBotCommands() override;
	View::SenderInfo elementSender(const Data::Message &message) override;
	void elementPaintUserpic(Painter &p, const QRect &rect, const QString &userId) override;
	Support::NoteLine elementServiceText(const Data::Message &message) override;

	// ---- rows
	[[nodiscard]] const Data::History &history() const;
	[[nodiscard]] const QVector<Data::Message> &items() const;
	void refreshRows();
	void applyRows(const QVector<Data::Message> &items);
	void rebuildIndex();
	[[nodiscard]] int indexOfKey(const QString &key) const;
	[[nodiscard]] int indexOfSeq(qint64 seq) const;
	[[nodiscard]] int rowAt(int y) const; // -1 when above / below every row
	[[nodiscard]] int rowNear(int y) const; // clamped
	[[nodiscard]] QRect rowRect(int index) const;
	void updateRow(int index);

	// ---- layout
	[[nodiscard]] int viewportHeight() const;
	int layoutRows(int width);
	void layoutExactly(int from, int till);
	void recountTops();
	void relayoutKeepingAnchor();
	void rememberAnchor();
	void applyAnchor();
	void processPendingLayout();
	void trimLoaded();
	void checkLoadMore();
	void loadOlder();
	void loadNewer();
	void loadInitial();

	// ---- reading
	void checkRead();
	void markReadNow();

	// ---- painting helpers
	void paintFloatingDate(Painter &p, const QRect &clip);
	void paintTopLoader(Painter &p);
	void startHighlight(const QString &key, const std::optional<Data::Quote> &quote);
	[[nodiscard]] double highlightOpacity() const;
	[[nodiscard]] int rowTop(int index) const {
		return _rowsTop + _rows[index].top;
	}
	void scrolled();
	void ensureExactNearViewport();
	void positionInitially();
	void showAtSeq(qint64 seq, const std::optional<Data::Quote> &quote, bool animated);
	void invalidateAll();

	// ---- input (history_list_input.cpp)
	[[nodiscard]] MouseTarget targetAt(QPoint position) const;
	void updateHover(QPoint position);
	void clearHover();
	void refreshCursor();
	void activate(const MouseTarget &target, Qt::MouseButton button);
	void toggleSelected(const QString &key);
	void setSelectedRange(int from, int till);
	void setTextSelection(const QString &key, Ui::Text::Selection selection);
	void dragSelect(QPoint position);
	void checkDragScroll(QPoint position);
	void revealSpoiler(const QString &key);
	void openMedia(const Data::Message &message);
	void openPhoto(const Data::Message &message);
	// The bubble of a message on screen, in global coordinates (empty:
	// not on screen): the viewer grows out of it and closes into it.
	[[nodiscard]] QRect viewerOrigin(const QString &key) const;
	// The media that shows `message` (an album's tile for its item).
	[[nodiscard]] View::Media *mediaFor(const Data::Message &message) const;
	void playVideo(const Data::Message &message);
	void toggleInlineVideo(const Data::Message &message);
	void toggleVoice(const Data::Message &message);
	void openFile(const Data::Message &message);
	void showInFolder(const Data::Message &message);
	void saveAs(const Data::Message &message);
	void copyImage(const Data::Message &message);
	void openLocation(const Data::Message &message);
	void pressButton(const Data::Message &message, int row, int column);
	void toggleReaction(const Data::Message &message, const QString &emoji);
	void cancelTransfer(const Data::Message &message);
	void requestPin(const Data::Message &message, bool pin);
	void showRetryMenu(const Data::Message &message, QPoint globalPosition);
	void updateInlineVideos();
	void removeInlineVideo(const QString &key);
	void placeInlineVideos();
	[[nodiscard]] QString copyText(const QVector<Data::Message> &messages);

	// ---- menu (history_list_menu.cpp)
	void showContextMenu(
		const MouseTarget &target,
		QPoint globalPosition,
		bool allReactions = false);
	[[nodiscard]] bool isPinned(qint64 seq) const;
	[[nodiscard]] bool selectable(int index) const;
	void startInlineVideo(const QString &key, const QString &path, bool gif);
	void repaintSpinners();

	Window::SessionController *_controller = nullptr;
	Main::Session *_session = nullptr;
	QString _chatId;
	QPointer<Ui::ScrollArea> _scroll;
	Downloads *_downloads = nullptr;

	// ---- a group
	void loadRoles();
	bool _group = false;
	std::unique_ptr<Support::UserpicPainter> _userpics; // the senders' pictures
	QHash<QString, QString> _roles;                     // user id -> «مالک» / «مدیر» / a title
	bool _rolesLoading = false;
	bool _rolesAgain = false;

	std::vector<Row> _rows;
	QHash<QString, int> _indexByKey;
	QHash<qint64, int> _indexBySeq;
	QVector<Data::Message> _debugItems;
	bool _debugMode = false;
	int _rowsTop = 0;
	int _rowsHeight = 0;
	int _layoutWidth = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	std::vector<int> _pendingLayout; // row indexes with estimated heights
	QBasicTimer _layoutTimer;
	QBasicTimer _trimTimer;
	QBasicTimer _readTimer;
	QBasicTimer _dateTimer;
	QBasicTimer _dragScrollTimer;
	QBasicTimer _liveTimer;
	QBasicTimer _spinTimer; // rows that show a waiting spinner

	Anchor _anchor;
	bool _atBottom = true;
	bool _inLayout = false;
	bool _started = false;
	bool _initialLoading = false;
	bool _loadFailed = false;
	bool _loadingOlder = false;
	bool _loadingNewer = false;
	bool _active = false;
	bool _canWrite = true;
	bool _canReact = true;
	bool _anonymousPeer = false;
	bool _botChat = false;
	QString _botHandle;
	bool _positioned = false; // the first scroll position was chosen
	qint64 _firstUnreadSeq = 0;
	bool _unreadResolved = false; // the chat row was there to tell
	qint64 _pendingJump = 0;
	std::optional<Data::Quote> _pendingJumpQuote;
	qint64 _aroundSeq = 0;

	QString _theme;
	ChatColors _colors;
	int _colorsVersion = -1;

	// ---- transient view state
	QSet<QString> _revealed;                    // spoilers shown
	QSet<qint64> _requestedReplies;             // reply targets asked once
	QHash<qint64, Data::Message> _replyTargets; // those outside the window
	QSet<QString> _requestedPosts;
	QSet<QString> _loadingButtons;              // "seq:row:column"
	QString _highlightKey;
	Ui::Text::Selection _highlightRange;
	qint64 _highlightStarted = 0;
	QBasicTimer _highlightTimer;
	bool _dateFloating = false;
	Ui::Animations::Simple _dateOpacity;
	Ui::InfiniteRadialAnimation _topLoader;
	QSet<QString> _failedPosts;
	qint64 _olderRetryAt = 0;
	qint64 _newerRetryAt = 0;
	std::vector<InlineVideo> _videos;
	QSet<QString> _videoFailed;

	// ---- selection and mouse
	QSet<QString> _selected;
	QString _textSelectionKey;
	Ui::Text::Selection _textSelection;
	MouseTarget _hover;
	MouseTarget _press;
	bool _pressed = false;
	bool _dragging = false;
	bool _dragSelectingRows = false;
	bool _dragStartedInText = false;
	bool _dragAdds = true;
	QSet<QString> _dragBase;
	int _dragStartCursor = 0;
	Ui::Text::SelectType _selectType = Ui::Text::SelectType::Letters;
	Ui::Text::Selection _selectBase;
	QPoint _pressPosition;
	QPoint _lastMouse;
	QPoint _lastMouseGlobal; // where the pointer was seen last (screen)
	bool _skipClick = false; // the release that ends a double click
	Qt::KeyboardModifiers _releaseModifiers;
	QPointer<Ui::PopupMenu> _menu;

	PaintStats _stats;

};

} // namespace History
