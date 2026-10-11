// Dialogs::InnerWidget — the rows of the chat list. Lives inside a scroll area,
// paints only the visible rows from prepared (cached) texts and follows the
// Data::Chats signals row by row.
//
// Rows: the «درخواست‌ها» and «بایگانی‌شده» entries (main folder, no search), then
// the chats — pinned first in the user's own order (drag a pinned row to change
// it), then by the newest of the last message and the draft.
#pragma once

#include "data/data_types.h"
#include "dialogs/dialogs_logic.h"
#include "support/media_image.h"
#include "ui/effects/ripple_animation.h"
#include "ui/rp_widget.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>

#include <memory>
#include <vector>

class Painter;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Dialogs {

class InnerWidget final : public Ui::RpWidget {
	Q_OBJECT

public:
	InnerWidget(QWidget *parent, Window::SessionController *controller);
	~InnerWidget();

	void setFolder(Folder folder);
	[[nodiscard]] Folder folder() const {
		return _folder;
	}
	void setQuery(const QString &query);
	[[nodiscard]] bool searching() const {
		return !_query.isEmpty();
	}
	// The chat open in the main column ("" for none).
	void setActiveChat(const QString &id);

	[[nodiscard]] int rowsCount() const {
		return int(_rows.size());
	}

	// Keyboard selection.
	void selectSkip(int direction);
	void selectSkipPage(int height, int direction);
	void clearSelection();
	bool chooseSelected();
	// The chat `direction` rows away from `id` (from the first / last row when
	// `id` is not listed); empty when there is none.
	[[nodiscard]] QString neighbourChat(const QString &id, int direction) const;

Q_SIGNALS:
	void chatChosen(const QString &id);
	void folderChosen(Dialogs::Folder folder);
	void scrollToRequested(int top, int bottom);
	// The number of rows changed (empty state, skeleton).
	void rowsChanged();

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	struct Row {
		enum class Type {
			Chat,
			Requests,
			Archive,
		};
		Type type = Type::Chat;
		Data::Chat chat;
		QString handle; // for the search
		int count = 0;  // folder entries
		bool accent = false; // folder entries: something unread inside

		// Prepared for painting.
		bool prepared = false;
		QString title;
		Preview preview;
		QString date;
		int dateWidth = 0;
		Tick tick = Tick::None;
		QString badge;
		bool mention = false; // a group: an unread mention of me / reply to me
		QString titleElided;
		int titleElidedFor = -1;
		int titleElidedWidth = 0;
		QString textElided;
		int textElidedFor = -1;
		int prefixWidth = 0;
	};
	struct Thumb {
		QString key;
		std::shared_ptr<Ui::ImageSource> source;
		QMetaObject::Connection connection;
		QImage prepared;
	};
	struct RowState {
		bool over = false;
		bool active = false;
		bool dragged = false;
	};

	void scheduleRebuild();
	void rebuild();
	void chatChanged(const QString &id);
	void refreshPresenceWatch();
	void prepare(Row &row);
	void paintRow(Painter &p, Row &row, int top, const RowState &state);
	void paintUserpic(Painter &p, const Row &row, const QRect &rect, const RowState &state);
	[[nodiscard]] const QImage &thumbFor(const Row &row);
	[[nodiscard]] style::color rowBg(const RowState &state) const;

	[[nodiscard]] int rowTop(int index) const;
	[[nodiscard]] int rowAt(QPoint position) const;
	[[nodiscard]] QRect rowRect(int index) const;
	void updateRow(int index);
	void setOver(int index);
	void setSelected(int index);
	void activate(int index);
	void showMenu(int index, QPoint globalPosition);

	[[nodiscard]] bool canDrag(int index) const;
	[[nodiscard]] int pinnedStart() const;
	[[nodiscard]] int pinnedCount() const;
	void startDrag();
	void updateDrag(QPoint position);
	void finishDrag(bool commit);
	[[nodiscard]] QStringList pinnedOrder() const;
	void savePinnedOrder(const QStringList &order);

	void addRipple(int index, QPoint position);
	void stopRipple();

	Window::SessionController *_controller = nullptr;
	Support::UserpicPainter _userpics;
	Folder _folder = Folder::Main;
	QString _query;
	QString _activeChat;

	std::vector<Row> _rows;
	QHash<QString, int> _indexById;
	QHash<QString, Thumb> _thumbs;
	QSet<QString> _watched;
	bool _rebuildScheduled = false;
	bool _rebuildAfterDrag = false;

	int _visibleTop = 0;
	int _visibleBottom = 0;

	int _over = -1;
	int _pressed = -1;
	int _selected = -1;
	QString _menuChat;
	QPointer<Ui::PopupMenu> _menu;
	QPoint _pressPosition;
	bool _mouseSelection = false;

	bool _dragging = false;
	int _dragIndex = -1;
	int _dragGrab = 0; // from the row's top to the cursor
	int _dragTop = 0;

	std::unique_ptr<Ui::RippleAnimation> _ripple;
	int _rippleRow = -1;
	QString _rippleChat;

	QTimer _clock;

};

} // namespace Dialogs
