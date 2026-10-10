// Eli::PostList — a virtualised, custom painted list of post cards. The feed
// tabs, topic pages, bookmarks, the replies under a post and the embedded
// lists of profiles / search results are all this widget with a different
// loader.
//
//   auto descriptor = Eli::ListDescriptor();
//   descriptor.loader = [=](const QString &cursor, base::Guard guard, auto done, auto fail) {
//       feed.bookmarks(cursor, guard, [=](const Data::PostsPage &page) { done(Eli::ListPage::From(page)); }, fail);
//   };
//   const auto list = scroll->setOwnedWidget(Ui::object_ptr<Eli::PostList>(scroll, controller, std::move(descriptor)));
//
// The list has no scroll of its own: it reports its height, paints only what
// the scroll area (or the embedding host) says is visible, keeps media
// handles only for cards near that range, pages by the loader's cursor when
// the end comes close, and lives on the store's signals afterwards
// (postChanged / postRemoved / authorBlocked, people cards, the audio player).
// Cards seen for a second are reported as views; a card that re-appears with
// counts older than a minute asks for fresh ones.
//
// Mouse: every part of a card acts as on the web (profile, links, media, poll,
// quote, actions); right click opens the post menu (the people list on the
// like / dislike buttons); double click likes. Keyboard (when focused):
// J / K next / previous post, L like, R reply, B save, Enter opens.
#pragma once

#include "base/timer.h"
#include "eli/eli_logic.h"
#include "eli/eli_post_card.h"
#include "ui/effects/animations.h"
#include "ui/rp_widget.h"
#include "ui/widgets/misc_widgets.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

namespace Ui {
class LinkButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Eli {

struct ListPage {
	QVector<Data::FeedItem> items;
	QString next; // empty: the last page

	[[nodiscard]] static ListPage From(const Data::FeedPage &page);
	[[nodiscard]] static ListPage From(const Data::PostsPage &page);
};

using ListLoader = Fn<void(
	const QString &cursor,
	base::Guard guard,
	Fn<void(const ListPage &)> done,
	Data::FailCallback fail)>;

struct ListDescriptor {
	// Null: the owner fills the list with setRows().
	ListLoader loader;
	CardMode mode = CardMode::Feed;
	// The state shown when the first page is empty.
	Fn<Ui::EmptyPlaceholderDescriptor()> empty;
	// Keeps at most this many posts (the first N).
	int cap = 0;
	QString pinnedId;
	// After each page (the home feed records the newest post time).
	Fn<void(const ListPage &page, bool first)> onPage;
	// A removed post stays (the owner decides what the tree shows).
	bool keepRemoved = false;
	// The reply button of a row asks the owner instead of opening compose.
	bool replyToOwner = false;
};

struct ListRow {
	QString id;
	CardOptions options;
	// A «نمایش پاسخ‌های بیشتر» row instead of a post: `id` is its key.
	QString moreText;
};

class PostList final : public Ui::RpWidget, public CardHost {
	Q_OBJECT

public:
	PostList(
		QWidget *parent,
		Window::SessionController *controller,
		ListDescriptor descriptor);
	~PostList();

	// Loads the first page again; the shown posts stay until it arrives (and
	// when it fails).
	void reload();
	void loadMore();
	void prepend(const QString &postId);
	void remove(const QString &postId);
	// Static content (replies): cards of ids that stay are kept.
	void setRows(const QVector<ListRow> &rows);

	[[nodiscard]] QStringList ids() const;
	[[nodiscard]] int count() const {
		return int(_rows.size());
	}
	[[nodiscard]] bool loading() const {
		return _loading;
	}
	[[nodiscard]] bool loadedOnce() const {
		return _loadedOnce;
	}
	[[nodiscard]] bool hasMore() const {
		return !_done;
	}

	// The post opened in the main column (painted as the active row).
	void setActivePost(const QString &postId);
	// A short flash of a row (the reply a notification pointed to).
	void highlight(const QString &postId);
	// [top, bottom) of a row, empty when it is not in the list.
	[[nodiscard]] QRect rowRect(const QString &postId) const;
	// The space the placeholder / skeleton fill when there are no rows.
	void setMinHeight(int height);
	[[nodiscard]] PostCard *card(const QString &postId) const;
	[[nodiscard]] QString selectedId() const;

	// J / K / L / R / B / Enter. Returns false for other keys.
	bool handleKey(QKeyEvent *e);

Q_SIGNALS:
	// The keyboard selection moved: make [top, bottom) visible.
	void scrollToRequested(int top, int bottom);
	void loadingChanged();
	void emptyChanged(bool empty);
	void replyRequested(const QString &postId);
	void followRequested(const QString &postId);
	void moreRequested(const QString &key);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	struct Row {
		QString id;
		std::unique_ptr<PostCard> card; // null for a «more» row
		QString moreText;
		int moreIndent = 0;
		int top = 0;
		int height = 0;
	};

	// CardHost
	Main::Session &cardSession() const override;
	Support::UserpicPainter &cardUserpics() override;
	double cardPixelRatio() const override;
	void cardRepaint(const QString &postId) override;
	QObject *cardGuard() override;

	void load(bool reset);
	void applyPage(const ListPage &page, bool reset);
	void appendRow(const Data::FeedItem &item);
	[[nodiscard]] CardOptions optionsFor(const Data::FeedItem &item) const;
	void rebuildIndex();
	void relayout();
	void refreshPlaceholder();
	void updateRow(int index);
	[[nodiscard]] int rowAt(int y) const;
	[[nodiscard]] int indexOf(const QString &postId) const;
	[[nodiscard]] QRect cardRect(int index) const; // widget coordinates
	void updateHover(QPoint position);
	void setHover(int row, Hit hit);
	void activate(int row, Hit hit, QPoint globalPosition);
	void select(int index);
	void rangeChanged();
	void checkViews();
	void manageMedia();
	void postChanged(const QString &id);
	void postRemoved(const QString &id);
	void peopleChanged(const QStringList &ids);
	void paintSkeleton(QPainter &p, int top, const QRect &clip) const;
	void showTooltip();
	[[nodiscard]] int tailHeight() const;
	[[nodiscard]] int cardWidth() const;
	[[nodiscard]] int cardLeft() const;

	Window::SessionController *_controller = nullptr;
	ListDescriptor _descriptor;
	std::unique_ptr<Support::UserpicPainter> _userpics;

	std::vector<Row> _rows;
	QHash<QString, int> _index;
	QString _next;
	bool _loading = false;
	bool _reloading = false;
	bool _done = false;
	bool _loadedOnce = false;
	bool _failedFirst = false;
	bool _failedMore = false;
	QString _errorText;
	int _generation = 0;

	Ui::EmptyPlaceholder *_placeholder = nullptr;
	bool _placeholderIsError = false;
	Ui::LinkButton *_retry = nullptr;
	mutable Ui::SkeletonAnimation _skeleton;

	int _layoutWidth = -1;
	int _rowsHeight = 0;
	int _minHeight = 0;
	int _visibleTop = 0;
	int _visibleBottom = 0;

	int _overRow = -1;
	Hit _overHit;
	int _pressedRow = -1;
	Hit _pressedHit;
	QString _selectedId;
	QString _activeId;
	QString _highlightId;
	Ui::Animations::Simple _highlight;
	QString _burstId;
	QPoint _burstPoint;
	Ui::Animations::Simple _burst;
	QPoint _lastMouse;
	base::Timer _tooltipTimer;

	Logic::ViewTracker _views;
	Logic::CountsFreshness _freshness;
	QSet<QString> _halfVisible;
	base::Timer _viewTimer;
	base::Timer _minuteTimer;

};

} // namespace Eli
