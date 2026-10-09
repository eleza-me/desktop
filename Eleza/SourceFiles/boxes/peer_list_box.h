// Boxes::PeerListBox — a searchable list of people / chats with userpics, for
// «forward to…», «new message», block lists, members.
//
// The box draws and filters; a controller feeds it:
//
//   class BlockedController final : public Boxes::PeerListController {
//   public:
//       QString title() const override { return tr::lng_settings_blocked(tr::now); }
//       void prepare() override {
//           delegate()->setLoading(true);
//           session()->data().graph().blocked(..., [=](const QVector<Data::Profile> &list) {
//               auto rows = QVector<Boxes::PeerListRow>();
//               for (const auto &user : list) rows.push_back(Boxes::PeerListRow::FromUser(user));
//               delegate()->setRows(rows);          // also ends the loading state
//           });
//       }
//       void rowClicked(const Boxes::PeerListRow &row) override {
//           delegate()->closeBox();
//           ...
//       }
//   };
//   controller->show(Ui::Box<Boxes::PeerListBox>(&session, std::make_unique<BlockedController>()));
//
// Multi-select: return true from multiSelect(); rows get a check mark, the box
// gets a submit button and submit(ids) is called with the chosen row ids.
// Remote search: return true from searchRemotely() and answer
// searchQueryChanged(query) with setRows(); otherwise the box filters the rows
// by title and subtitle itself.
//
// Ready-made: Boxes::ChooseChatsBox(session, args, done) lists the user's chats
// (forward, share).
#pragma once

#include "data/data_types.h"
#include "ui/image/userpic.h"
#include "ui/layers/box_content.h"
#include "ui/widgets/list_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QSet>

#include <memory>

namespace Main {
class Session;
} // namespace Main

namespace Support {
class UserpicPainter;
} // namespace Support

namespace Ui {
class RoundButton;
class SearchField;
} // namespace Ui

namespace Boxes {

struct PeerListRow {
	QString id; // unique inside the list: a user id, a chat id
	QString title;
	QString subtitle;
	// The userpic: this user's picture, or a special one when empty.
	QString userId;
	Ui::UserpicKind special = Ui::UserpicKind::User;
	bool verified = false;
	bool disabled = false; // shown but cannot be chosen

	[[nodiscard]] static PeerListRow FromUser(const Data::User &user);
	// The subtitle is left empty (the caller knows what matters: a draft, the
	// last message, «online»).
	[[nodiscard]] static PeerListRow FromChat(
		Main::Session *session,
		const Data::Chat &chat);
};

class PeerListDelegate {
public:
	// Replaces the list (and ends the loading state).
	virtual void setRows(const QVector<PeerListRow> &rows) = 0;
	virtual void appendRows(const QVector<PeerListRow> &rows) = 0;
	virtual void updateRow(const PeerListRow &row) = 0; // by id
	virtual void removeRow(const QString &id) = 0;
	virtual void setLoading(bool loading) = 0;
	virtual void setSelected(const QString &id, bool selected) = 0;
	[[nodiscard]] virtual QStringList selectedIds() const = 0;
	[[nodiscard]] virtual QString searchQuery() const = 0;
	[[nodiscard]] virtual Main::Session *peerListSession() const = 0;
	// The box itself: a guard for requests, a parent for other boxes.
	[[nodiscard]] virtual Ui::BoxContent *peerListBox() = 0;
	virtual void closeBox() = 0;

protected:
	~PeerListDelegate() = default;

};

class PeerListController {
public:
	virtual ~PeerListController() = default;

	[[nodiscard]] virtual QString title() const = 0;
	// Fill the rows: delegate()->setRows(…), now or when the data arrives.
	virtual void prepare() = 0;
	// A row was chosen (single-select mode).
	virtual void rowClicked(const PeerListRow &row) = 0;

	[[nodiscard]] virtual QString searchPlaceholder() const;
	[[nodiscard]] virtual QString emptyText() const;
	[[nodiscard]] virtual bool searchRemotely() const {
		return false;
	}
	// The query changed (debounced by the box when searchRemotely()).
	virtual void searchQueryChanged(const QString &query) {
		Q_UNUSED(query);
	}
	// The list was scrolled near its end.
	virtual void loadMore() {
	}

	[[nodiscard]] virtual bool multiSelect() const {
		return false;
	}
	// 0 = unlimited.
	[[nodiscard]] virtual int selectionLimit() const {
		return 0;
	}
	[[nodiscard]] virtual QString submitText() const;
	virtual void submit(const QStringList &ids) {
		Q_UNUSED(ids);
	}

	void setDelegate(PeerListDelegate *delegate) {
		_delegate = delegate;
	}
	[[nodiscard]] PeerListDelegate *delegate() const {
		return _delegate;
	}
	[[nodiscard]] Main::Session *session() const {
		return _delegate ? _delegate->peerListSession() : nullptr;
	}

private:
	PeerListDelegate *_delegate = nullptr;

};

class PeerListBox final
	: public Ui::BoxContent
	, private Ui::ListDelegate
	, private PeerListDelegate {
public:
	PeerListBox(
		QWidget *parent,
		Main::Session *session,
		std::unique_ptr<PeerListController> controller);
	~PeerListBox();

	void setInnerFocus() override;

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	// Ui::ListDelegate
	int rowsCount() const override;
	int rowHeight(int index, int width) const override;
	void paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) override;
	bool rowClickable(int index) const override;

	// PeerListDelegate
	void setRows(const QVector<PeerListRow> &rows) override;
	void appendRows(const QVector<PeerListRow> &rows) override;
	void updateRow(const PeerListRow &row) override;
	void removeRow(const QString &id) override;
	void setLoading(bool loading) override;
	void setSelected(const QString &id, bool selected) override;
	QStringList selectedIds() const override;
	QString searchQuery() const override;
	Main::Session *peerListSession() const override;
	Ui::BoxContent *peerListBox() override;
	void closeBox() override;

	void applyFilter();
	void rowChosen(int index);
	void refreshSubmit();
	void userpicUpdated(const QString &userId);
	[[nodiscard]] const PeerListRow &shown(int index) const;

	QPointer<Main::Session> _session;
	std::unique_ptr<PeerListController> _controller;
	std::unique_ptr<Support::UserpicPainter> _userpics;
	Ui::SearchField *_search = nullptr;
	Ui::ListWidget *_list = nullptr;
	Ui::RoundButton *_submit = nullptr;
	QVector<PeerListRow> _rows;
	std::vector<int> _filtered; // indexes into _rows
	QSet<QString> _selected;
	QStringList _selectedOrder;
	QTimer *_searchTimer = nullptr;
	bool _loading = false;

};

struct ChooseChatsArgs {
	QString title;
	QString submitText; // multi-select only
	bool multiSelect = false;
	int limit = 0;
	bool includeSaved = true;
	bool includeConversations = false;
	// Only chats the user can write to.
	bool onlyWritable = true;
};

// The user's chats, most recent first; `done` gets the chosen chat ids (one in
// single-select mode) after the box closed.
[[nodiscard]] Ui::object_ptr<Ui::BoxContent> ChooseChatsBox(
	Main::Session *session,
	ChooseChatsArgs args,
	std::function<void(const QStringList &chatIds)> done);

} // namespace Boxes
