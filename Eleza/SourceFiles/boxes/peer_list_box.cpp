#include "boxes/peer_list_box.h"

#include "data/data_chats.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/ui_phrases.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "styles/style_boxes.h"

#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>

namespace Boxes {
namespace {

constexpr auto kRemoteSearchDelay = 350;
constexpr auto kNearBottom = 240;

} // namespace

PeerListRow PeerListRow::FromUser(const Data::User &user) {
	auto result = PeerListRow();
	result.id = user.id;
	result.userId = user.id;
	result.title = user.deleted
		? tr::lng_deleted_account(tr::now)
		: user.displayName;
	result.subtitle = Lang::Handle(user.handle);
	result.verified = user.verified;
	if (user.deleted) {
		result.special = Ui::UserpicKind::Deleted;
	}
	return result;
}

PeerListRow PeerListRow::FromChat(
		Main::Session *session,
		const Data::Chat &chat) {
	auto result = PeerListRow();
	result.id = chat.id;
	result.title = Support::ChatTitle(session, chat);
	result.verified = chat.verified;
	if (chat.isGroup) {
		result.groupId = chat.id;
		result.groupPhoto = chat.group ? chat.group->photo : QString();
	} else if (chat.saved) {
		result.special = Ui::UserpicKind::Saved;
	} else if (chat.peerDeleted) {
		result.special = Ui::UserpicKind::Deleted;
	} else if (chat.anonymous || chat.peerUserId.isEmpty()) {
		result.special = Ui::UserpicKind::Anonymous;
	} else {
		result.userId = chat.peerUserId;
	}
	return result;
}

QString PeerListController::searchPlaceholder() const {
	return tr::lng_tab_search(tr::now);
}

QString PeerListController::emptyText() const {
	return tr::lng_nothing_found(tr::now);
}

QString PeerListController::submitText() const {
	return tr::lng_continue(tr::now);
}

PeerListBox::PeerListBox(
	QWidget *parent,
	Main::Session *session,
	std::unique_ptr<PeerListController> controller)
: BoxContent(parent)
, _session(session)
, _controller(std::move(controller))
, _userpics(std::make_unique<Support::UserpicPainter>(
	session,
	[this](const QString &userId) { userpicUpdated(userId); })) {
	_controller->setDelegate(this);
}

PeerListBox::~PeerListBox() {
	// The controller may refer to the delegate in its destructor.
	_controller = nullptr;
}

void PeerListBox::prepare() {
	setTitle(_controller->title());
	_search = new Ui::SearchField(this, _controller->searchPlaceholder());
	auto list = Ui::object_ptr<Ui::ListWidget>(
		this,
		static_cast<Ui::ListDelegate*>(this));
	_list = setInnerWidget(std::move(list), 0, 0);

	_searchTimer = new QTimer(this);
	_searchTimer->setSingleShot(true);
	_searchTimer->setInterval(kRemoteSearchDelay);
	QObject::connect(_searchTimer, &QTimer::timeout, this, [this] {
		_controller->searchQueryChanged(searchQuery());
	});
	QObject::connect(_search, &Ui::SearchField::changed, this, [this] {
		if (_controller->searchRemotely()) {
			_searchTimer->start();
		} else {
			_controller->searchQueryChanged(searchQuery());
			applyFilter();
		}
	});
	QObject::connect(_search, &Ui::SearchField::submitted, this, [this] {
		if (_list->selectedRow() >= 0) {
			rowChosen(_list->selectedRow());
		} else if (_filtered.size() == 1) {
			rowChosen(0);
		} else if (_controller->multiSelect() && !_selected.isEmpty()) {
			_controller->submit(selectedIds());
		}
	});
	QObject::connect(_search, &Ui::SearchField::cancelled, this, [this] {
		closeBox();
	});
	QObject::connect(_list, &Ui::ListWidget::rowClicked, this, [this](int index) {
		rowChosen(index);
	});
	QObject::connect(
		_list,
		&Ui::ListWidget::scrollToRequested,
		this,
		[this](int top, int bottom) {
			scrollArea()->scrollToY(top, bottom);
		});
	scrollArea()->setNearEdgeThreshold(kNearBottom);
	QObject::connect(scrollArea(), &Ui::ScrollArea::nearBottom, this, [this] {
		if (!_loading && !_rows.isEmpty()) {
			_controller->loadMore();
		}
	});
	_search->field()->rawTextEdit()->installEventFilter(this);

	if (_session) {
		// Names and pictures that arrive later.
		QObject::connect(
			&_session->data().users(),
			&Data::Users::changed,
			this,
			[this] { _list->update(); });
	}

	if (_controller->multiSelect()) {
		_submit = addButton(_controller->submitText(), [this] {
			if (!_selected.isEmpty()) {
				_controller->submit(selectedIds());
			}
		}, st::defaultRoundButton);
		addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });
		refreshSubmit();
	} else {
		addButton(tr::lng_close(tr::now), [this] { closeBox(); });
	}
	setDimensions(st::boxWidth, st::boxListHeight);
	_controller->prepare();
}

void PeerListBox::setInnerFocus() {
	_search->setFocusFast();
}

bool PeerListBox::eventFilter(QObject *object, QEvent *event) {
	if (event->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(event)->key();
		if (key == Qt::Key_Down) {
			_list->selectSkip(1);
			return true;
		} else if (key == Qt::Key_Up) {
			_list->selectSkip(-1);
			return true;
		} else if (key == Qt::Key_PageDown) {
			_list->selectSkipPage(scrollArea()->height(), 1);
			return true;
		} else if (key == Qt::Key_PageUp) {
			_list->selectSkipPage(scrollArea()->height(), -1);
			return true;
		}
	}
	return BoxContent::eventFilter(object, event);
}

void PeerListBox::resizeEvent(QResizeEvent *e) {
	const auto &padding = st::boxSearchPadding;
	_search->resizeToWidth(width() - padding.left() - padding.right());
	_search->moveToLeft(padding.left(), padding.top());
	const auto top = padding.top() + _search->height() + padding.bottom();
	BoxContent::resizeEvent(e);
	scrollArea()->setGeometry(0, top, width(), std::max(height() - top, 0));
}

void PeerListBox::paintEvent(QPaintEvent *e) {
	BoxContent::paintEvent(e);
	if (!_filtered.empty()) {
		return;
	}
	auto p = Painter(this);
	const auto top = scrollArea()->y() + st::boxListEmptyTop;
	p.setFont(st::normalFont);
	p.setPen(st::boxListEmptyFg);
	p.drawTextCentered(
		QRect(0, top, width(), st::normalFont->height),
		_loading ? tr::lng_loading(tr::now) : _controller->emptyText());
}

const PeerListRow &PeerListBox::shown(int index) const {
	return _rows[_filtered[index]];
}

int PeerListBox::rowsCount() const {
	return int(_filtered.size());
}

int PeerListBox::rowHeight(int index, int width) const {
	Q_UNUSED(index);
	Q_UNUSED(width);
	return st::peerListRowHeight;
}

bool PeerListBox::rowClickable(int index) const {
	return !shown(index).disabled;
}

void PeerListBox::paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) {
	Q_UNUSED(state);
	const auto &row = shown(index);
	const auto width = rect.width();
	const auto height = rect.height();
	const auto &padding = st::peerListPadding;
	p.translate(rect.topLeft());
	if (row.disabled) {
		p.setOpacity(0.5);
	}

	const auto size = int(st::peerListUserpicSize);
	const auto userpic = style::rtlrect(
		padding.left(),
		(height - size) / 2,
		size,
		size,
		width);
	if (!row.groupId.isEmpty()) {
		_userpics->paintGroup(p, userpic, row.groupId, row.groupPhoto, row.title);
	} else if (row.userId.isEmpty()) {
		_userpics->paintSpecial(p, userpic, row.special, row.id, row.title);
	} else {
		_userpics->paint(p, userpic, row.userId);
	}

	auto trailing = padding.right();
	if (_controller->multiSelect()) {
		const auto check = int(st::peerListCheckSize);
		const auto circle = style::rtlrect(
			width - padding.right() - check,
			(height - check) / 2,
			check,
			check,
			width);
		auto hq = PainterHighQualityEnabler(p);
		if (_selected.contains(row.id)) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::peerListCheckBg);
			p.drawEllipse(circle);
			st::peerListCheckIcon.paintInCenter(p, circle);
		} else {
			p.setPen(QPen(st::peerListCheckBorder->c, 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(QRectF(circle).adjusted(0.75, 0.75, -0.75, -0.75));
		}
		trailing += check + st::peerListUserpicSkip;
	}

	const auto left = padding.left() + size + st::peerListUserpicSkip;
	const auto available = width - left - trailing;
	const auto single = row.subtitle.isEmpty();
	const auto nameTop = single
		? ((height - st::peerListNameFont->height) / 2)
		: int(st::peerListNameTop);
	const auto badge = row.verified
		? (st::peerListVerifiedIcon.width() + st::peerListVerifiedSkip)
		: 0;
	const auto name = st::peerListNameFont->elided(
		row.title,
		std::max(available - badge, 0));
	const auto nameWidth = st::peerListNameFont->width(name);
	p.setFont(st::peerListNameFont);
	p.setPen(st::peerListNameFg);
	p.drawTextLeft(left, nameTop, width, name, nameWidth);
	if (row.verified) {
		st::peerListVerifiedIcon.paint(
			p,
			left + nameWidth + st::peerListVerifiedSkip,
			nameTop
				+ (st::peerListNameFont->height
					- st::peerListVerifiedIcon.height()) / 2,
			width);
	}
	if (!single) {
		const auto status = st::peerListStatusFont->elided(
			row.subtitle,
			available);
		p.setFont(st::peerListStatusFont);
		p.setPen(st::peerListStatusFg);
		p.drawTextLeft(left, st::peerListStatusTop, width, status);
	}
}

void PeerListBox::applyFilter() {
	_filtered.clear();
	const auto query = _controller->searchRemotely()
		? QString()
		: searchQuery().toLower();
	const auto needle = query.startsWith(QLatin1Char('@')) ? query.mid(1) : query;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto &row = _rows[i];
		if (needle.isEmpty()
			|| row.title.toLower().contains(needle)
			|| row.subtitle.toLower().contains(needle)) {
			_filtered.push_back(i);
		}
	}
	_list->clearSelection();
	_list->refresh();
	update();
}

void PeerListBox::rowChosen(int index) {
	if (index < 0 || index >= int(_filtered.size())) {
		return;
	}
	const auto row = shown(index);
	if (row.disabled) {
		return;
	} else if (!_controller->multiSelect()) {
		_controller->rowClicked(row);
		return;
	}
	const auto limit = _controller->selectionLimit();
	const auto selected = _selected.contains(row.id);
	if (!selected && limit > 0 && _selected.size() >= limit) {
		return;
	}
	setSelected(row.id, !selected);
}

void PeerListBox::refreshSubmit() {
	if (_submit) {
		_submit->setDisabled(_selected.isEmpty()
			&& !_controller->submitWithoutSelection());
		_submit->setText(_selected.isEmpty()
			? _controller->submitText()
			: (_controller->submitText()
				+ QStringLiteral(" (")
				+ Ui::FormatNumber(_selected.size())
				+ QLatin1Char(')')));
	}
}

void PeerListBox::userpicUpdated(const QString &userId) {
	for (auto i = 0; i != int(_filtered.size()); ++i) {
		if (shown(i).userId == userId || shown(i).groupId == userId) {
			_list->updateRow(i);
		}
	}
}

void PeerListBox::setRows(const QVector<PeerListRow> &rows) {
	_rows = rows;
	_loading = false;
	applyFilter();
	scrollArea()->scrollToY(0);
}

void PeerListBox::appendRows(const QVector<PeerListRow> &rows) {
	auto known = QSet<QString>();
	for (const auto &row : _rows) {
		known.insert(row.id);
	}
	for (const auto &row : rows) {
		if (!known.contains(row.id)) {
			known.insert(row.id);
			_rows.push_back(row);
		}
	}
	_loading = false;
	const auto top = scrollArea()->scrollTop();
	applyFilter();
	scrollArea()->scrollToY(top);
}

void PeerListBox::updateRow(const PeerListRow &row) {
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].id == row.id) {
			_rows[i] = row;
			_list->update();
			return;
		}
	}
}

void PeerListBox::removeRow(const QString &id) {
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].id == id) {
			_rows.removeAt(i);
			_selected.remove(id);
			_selectedOrder.removeAll(id);
			const auto top = scrollArea()->scrollTop();
			applyFilter();
			scrollArea()->scrollToY(top);
			refreshSubmit();
			return;
		}
	}
}

void PeerListBox::setLoading(bool loading) {
	if (_loading != loading) {
		_loading = loading;
		update();
	}
}

void PeerListBox::setSelected(const QString &id, bool selected) {
	if (selected == _selected.contains(id)) {
		return;
	}
	if (selected) {
		_selected.insert(id);
		_selectedOrder.push_back(id);
	} else {
		_selected.remove(id);
		_selectedOrder.removeAll(id);
	}
	_list->update();
	refreshSubmit();
}

QStringList PeerListBox::selectedIds() const {
	return _selectedOrder;
}

QString PeerListBox::searchQuery() const {
	return _search ? _search->text().trimmed() : QString();
}

Main::Session *PeerListBox::peerListSession() const {
	return _session.data();
}

Ui::BoxContent *PeerListBox::peerListBox() {
	return this;
}

void PeerListBox::closeBox() {
	BoxContent::closeBox();
}

namespace {

class ChatsController final : public PeerListController {
public:
	ChatsController(
		ChooseChatsArgs args,
		std::function<void(const QStringList &chatIds)> done)
	: _args(std::move(args))
	, _done(std::move(done)) {
	}

	QString title() const override {
		return _args.title.isEmpty() ? tr::lng_forward(tr::now) : _args.title;
	}

	void prepare() override {
		const auto session = this->session();
		if (!session) {
			delegate()->setRows({});
			return;
		}
		auto &chats = session->data().chats();
		auto rows = QVector<PeerListRow>();
		auto list = chats.list(Data::Chats::Section::Messages);
		if (_args.includeConversations) {
			list += chats.list(Data::Chats::Section::Conversations);
			std::sort(list.begin(), list.end(), Data::ChatLess);
		}
		auto hasSaved = false;
		for (const auto &chat : list) {
			if (chat.isGroup && !_args.includeGroups) {
				continue;
			}
			if (chat.saved) {
				hasSaved = true;
				if (!_args.includeSaved) {
					continue;
				}
			} else if (_args.onlyWritable
				&& (!chat.canSend || chat.peerDeleted)) {
				continue;
			}
			auto row = PeerListRow::FromChat(session, chat);
			if (chat.saved) {
				rows.push_front(row);
			} else {
				if (!chat.peerUserId.isEmpty() && !chat.anonymous) {
					const auto user = session->data().users().get(chat.peerUserId);
					if (!user.handle.isEmpty()) {
						row.subtitle = Lang::Handle(user.handle);
					}
				}
				rows.push_back(row);
			}
		}
		Q_UNUSED(hasSaved);
		delegate()->setRows(rows);
		if (!chats.inboxLoadedOnce() && rows.isEmpty()) {
			// The list is still on its way from the server.
			delegate()->setLoading(true);
			const auto box = delegate()->peerListBox();
			QObject::connect(&chats, &Data::Chats::listChanged, box, [this] {
				prepare();
			}, Qt::SingleShotConnection);
		}
	}

	void rowClicked(const PeerListRow &row) override {
		finish({ row.id });
	}

	bool multiSelect() const override {
		return _args.multiSelect;
	}
	int selectionLimit() const override {
		return _args.limit;
	}
	QString submitText() const override {
		return _args.submitText.isEmpty()
			? tr::lng_send(tr::now)
			: _args.submitText;
	}
	void submit(const QStringList &ids) override {
		finish(ids);
	}

private:
	void finish(const QStringList &ids) {
		// Closing destroys the box and this controller with it.
		const auto done = _done;
		delegate()->closeBox();
		if (done) {
			done(ids);
		}
	}

	ChooseChatsArgs _args;
	std::function<void(const QStringList &chatIds)> _done;

};

} // namespace

Ui::object_ptr<Ui::BoxContent> ChooseChatsBox(
		Main::Session *session,
		ChooseChatsArgs args,
		std::function<void(const QStringList &chatIds)> done) {
	return Ui::Box<PeerListBox>(
		session,
		std::make_unique<ChatsController>(std::move(args), std::move(done)));
}

} // namespace Boxes
