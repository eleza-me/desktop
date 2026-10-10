#include "dialogs/dialogs_widget.h"

#include "data/data_chats.h"
#include "data/data_session.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/dialogs_menu.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"
#include "support/connection_label.h"
#include "support/error_text.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "ui/ui_utility.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "window/window_shortcuts.h"
#include "styles/style_dialogs.h"

#include <QtGui/QKeyEvent>
#include <QtWidgets/QTextEdit>

namespace Dialogs {
namespace {

const auto kChatsSubTab = u"chats"_s;
const auto kSubTabKey = u"dialogs.sub_tab"_s;

enum EmptyState {
	kStateRows,
	kStateSkeleton,
	kStateNothingFound,
	kStateError,
	kStateEmptyMain,
	kStateEmptyRequests,
	kStateEmptyArchived,
};

} // namespace

// «لیست گفتگوها لود نشد — تلاش دوباره» above a list that is shown from the cache.
class Widget::Banner final : public Ui::AbstractButton {
public:
	explicit Banner(QWidget *parent) : AbstractButton(parent) {
		resize(width(), st::dialogsBannerHeight);
	}

	void setText(const QString &text) {
		_text = text;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(rect(), st::dialogsBannerBg);
		const auto &padding = st::dialogsBannerPadding;
		const auto action = tr::lng_try_again(tr::now);
		const auto actionWidth = st::dialogsBannerActionFont->width(action);
		p.setFont(st::dialogsBannerActionFont);
		p.setPen(st::dialogsBannerActionFg);
		p.drawTextRight(
			padding.right(),
			(height() - st::dialogsBannerActionFont->height) / 2,
			width(),
			action,
			actionWidth);
		const auto available = width()
			- padding.left()
			- padding.right()
			- actionWidth
			- st::dialogsDateSkip;
		p.setFont(st::dialogsBannerFont);
		p.setPen(st::dialogsBannerFg);
		p.drawTextLeft(
			padding.left(),
			(height() - st::dialogsBannerFont->height) / 2,
			width(),
			st::dialogsBannerFont->elided(_text, std::max(available, 0)));
	}

private:
	QString _text;

};

Widget::Widget(QWidget *parent, Window::SessionController *controller)
: SectionWidget(parent, controller)
, _bar(new Ui::TopBar(this))
, _search(new Ui::SearchField(this, tr::lng_tab_search(tr::now)))
, _banner(new Banner(this))
, _scroll(new Ui::ScrollArea(this)) {
	setFocusPolicy(Qt::StrongFocus);

	_connection = Support::ConnectionState::For(&controller->session());
	if (_connection) {
		connect(
			_connection.data(),
			&Support::ConnectionState::changed,
			this,
			[this] { refreshTitle(); });
	}
	connect(_bar, &Ui::TopBar::backRequested, this, [this] {
		showFolder(Folder::Main);
	});

	_inner = _scroll->setOwnedWidget(
		Ui::object_ptr<InnerWidget>(_scroll, controller));
	connect(_inner, &InnerWidget::chatChosen, this, [this](const QString &id) {
		openChat(id);
	});
	connect(_inner, &InnerWidget::folderChosen, this, [this](Folder folder) {
		showFolder(folder);
	});
	connect(_inner, &InnerWidget::scrollToRequested, this, [this](
			int top,
			int bottom) {
		_scroll->scrollToY(top, bottom);
	});
	connect(_inner, &InnerWidget::rowsChanged, this, [this] {
		refreshState();
	});

	connect(_search, &Ui::SearchField::changed, this, [this](
			const QString &text) {
		queryChanged(text);
	});
	connect(_search, &Ui::SearchField::submitted, this, [this] {
		if (!_inner->chooseSelected()) {
			_inner->selectSkip(1);
			_inner->chooseSelected();
		}
	});
	// Esc in the field is not handled through SearchField::cancelled: the
	// key travels on to this widget (keyPressEvent) and would be handled a
	// second time by the window («clear the search» and «close the chat»).
	_search->field()->rawTextEdit()->installEventFilter(this);

	_banner->hide();
	_banner->setText(tr::lng_dialogs_load_failed(tr::now));
	_banner->setClickedCallback([this] {
		this->controller()->session().data().chats().loadInbox(this);
	});

	const auto chats = &controller->session().data().chats();
	connect(chats, &Data::Chats::inboxLoadingChanged, this, [this] {
		refreshState();
	});
	connect(chats, &Data::Chats::unreadChanged, this, [this] {
		refreshTabTitles();
	});
	connect(
		controller,
		&Window::SessionController::mainSectionChanged,
		this,
		[this] { refreshActiveChat(); });
	connect(
		controller,
		&Window::SessionController::tabChanged,
		this,
		[this] { refreshActiveChat(); });

	setupSubTabs();
	refreshTopBar();
	refreshState();
	refreshActiveChat();
}

Widget::~Widget() = default;

QString Widget::sectionKey() const {
	return Window::Routes::Chats();
}

Folder Widget::folder() const {
	return _inner->folder();
}

void Widget::setupSubTabs() {
	auto own = Window::SideSubTab();
	own.id = kChatsSubTab;
	own.order = 0;
	_subTabs.push_back({ std::move(own), nullptr });
	for (auto &tab : Window::SideSubTabs(Window::Tab::Chats)) {
		if (tab.id != kChatsSubTab && tab.factory && tab.title) {
			_subTabs.push_back({ std::move(tab), nullptr });
		}
	}
	std::stable_sort(_subTabs.begin(), _subTabs.end(), [](
			const SubTab &a,
			const SubTab &b) {
		return a.descriptor.order < b.descriptor.order;
	});
	if (_subTabs.size() < 2) {
		return;
	}
	_tabs = new Ui::TabBar(this, st::dialogsTabs);
	_tabs->setStretch(true);
	refreshTabTitles();
	connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		showSubTab(index, true);
	});
	const auto remembered = controller()->session().account().settings().string(
		kSubTabKey,
		kChatsSubTab);
	for (auto i = 0, count = int(_subTabs.size()); i != count; ++i) {
		if (_subTabs[i].descriptor.id == remembered) {
			_tabs->setActive(i, anim::type::instant);
			showSubTab(i, false);
			break;
		}
	}
	// Another module asks for one of the sub-tabs («موضوع‌ها» for the route
	// "conversation/topics"), possibly before this column existed.
	const auto request = [this](const QString &id) {
		for (auto i = 0, count = int(_subTabs.size()); i != count; ++i) {
			if (_subTabs[i].descriptor.id == id) {
				if (_inner->folder() != Folder::Main) {
					showFolder(Folder::Main);
				}
				_tabs->setActive(i, anim::type::instant);
				showSubTab(i, true);
				return;
			}
		}
	};
	connect(
		controller(),
		&Window::SessionController::sideSubTabRequested,
		this,
		[=, this](Window::Tab tab, const QString &) {
			if (tab == Window::Tab::Chats) {
				request(controller()->takeRequestedSideSubTab(tab));
			}
		});
	const auto pending = controller()->takeRequestedSideSubTab(
		Window::Tab::Chats);
	if (!pending.isEmpty()) {
		request(pending);
	}
}

void Widget::refreshTabTitles() {
	if (!_tabs) {
		return;
	}
	auto titles = QStringList();
	for (const auto &tab : _subTabs) {
		if (tab.descriptor.id == kChatsSubTab) {
			const auto unread
				= controller()->session().data().chats().unreadChats();
			titles.push_back((unread > 0)
				? tr::lng_dialogs_chats_unread(
					tr::now,
					lt_n,
					Lang::Num(unread))
				: tr::lng_dialogs_chats(tr::now));
		} else {
			titles.push_back(tab.descriptor.title());
		}
	}
	const auto active = _tabs->count() ? _tabs->active() : _subTab;
	_tabs->setTabs(titles);
	_tabs->setActive(active, anim::type::instant);
	_tabs->resizeToWidth(width());
}

bool Widget::chatsTabShown() const {
	return _subTabs[_subTab].descriptor.id == kChatsSubTab;
}

void Widget::showSubTab(int index, bool remember) {
	if (index < 0 || index >= int(_subTabs.size())) {
		return;
	}
	_subTab = index;
	auto &tab = _subTabs[index];
	if (remember) {
		controller()->session().account().settings().set(
			kSubTabKey,
			tab.descriptor.id);
	}
	if (tab.descriptor.id != kChatsSubTab && !tab.widget) {
		auto widget = tab.descriptor.factory(this, controller());
		tab.widget = widget.release();
		if (tab.widget) {
			tab.widget->setParent(this);
		}
	}
	updateControlsGeometry();
	refreshState();
}

bool Widget::handleCommand(Window::Command command) {
	// (The window-wide ones - Ctrl+K, next / previous chat, new message -
	// are registered in dialogs_module.cpp: they work from every tab.)
	switch (command) {
	case Window::Command::Find:
		focusSearch();
		return true;
	case Window::Command::Refresh:
		if (chatsTabShown()) {
			controller()->session().data().chats().loadInbox(this);
			return true;
		}
		return false;
	default: return false;
	}
}

void Widget::focusSearch() {
	if (_tabs && !chatsTabShown()) {
		for (auto i = 0, count = int(_subTabs.size()); i != count; ++i) {
			if (_subTabs[i].descriptor.id == kChatsSubTab) {
				_tabs->setActive(i);
				showSubTab(i, true);
				break;
			}
		}
	}
	_search->setFocusFast();
	_search->field()->selectAll();
}

void Widget::setInnerFocus() {
	setFocus();
}

bool Widget::onBackRequest() {
	if (!_search->text().isEmpty()) {
		_search->clear();
		setFocus();
		return true;
	} else if (_inner->folder() != Folder::Main) {
		showFolder(Folder::Main);
		return true;
	}
	return false;
}

void Widget::searchCancelled() {
	if (!_search->text().isEmpty()) {
		_search->clear();
	} else if (_inner->folder() != Folder::Main) {
		showFolder(Folder::Main);
	} else if (controller()->mainStackDepth() > 0) {
		controller()->showBack();
	} else {
		setFocus();
	}
}

void Widget::showFolder(Folder folder) {
	if (_inner->folder() == folder) {
		return;
	}
	if (folder != Folder::Main && _tabs && !chatsTabShown()) {
		for (auto i = 0, count = int(_subTabs.size()); i != count; ++i) {
			if (_subTabs[i].descriptor.id == kChatsSubTab) {
				_tabs->setActive(i, anim::type::instant);
				showSubTab(i, false);
				break;
			}
		}
	}
	if (!_search->text().isEmpty()) {
		_search->clear();
	}
	_inner->setFolder(folder);
	_scroll->scrollToY(0);
	refreshTopBar();
	updateControlsGeometry();
	refreshState();
	refreshActiveChat();
}

void Widget::refreshTitle() {
	// The main list swaps its title for «در حال اتصال…» like Telegram; a
	// folder keeps its name and tells the state in the subtitle.
	const auto folder = _inner->folder();
	const auto state = (_connection && !_connection->online())
		? _connection->text()
		: QString();
	if (folder == Folder::Main) {
		_bar->setTitle(state.isEmpty() ? tr::lng_tab_talk(tr::now) : state);
		_bar->setSubtitle(QString());
	} else {
		_bar->setTitle((folder == Folder::Requests)
			? tr::lng_dialogs_requests(tr::now)
			: tr::lng_dialogs_archived(tr::now));
		_bar->setSubtitle(state);
	}
}

void Widget::refreshTopBar() {
	const auto folder = _inner->folder();
	refreshTitle();
	_bar->setBackShown(folder != Folder::Main);
	_bar->clearActions();
	if (folder == Folder::Main) {
		const auto create = _bar->addAction(st::dialogsNewMessageButton);
		Ui::InstallTooltip(create, [] {
			return tr::lng_dialogs_new_message(tr::now);
		});
		create->setClickedCallback([this] {
			ShowNewMessageBox(controller());
		});
	}
}

void Widget::queryChanged(const QString &text) {
	if (!text.trimmed().isEmpty() && _tabs && !chatsTabShown()) {
		for (auto i = 0, count = int(_subTabs.size()); i != count; ++i) {
			if (_subTabs[i].descriptor.id == kChatsSubTab) {
				_tabs->setActive(i);
				showSubTab(i, false);
				break;
			}
		}
	}
	_inner->setQuery(text);
	_scroll->scrollToY(0);
	refreshState();
}

void Widget::openChat(const QString &id) {
	if (id.isEmpty()) {
		return;
	}
	const auto route = Window::Routes::Chat(id);
	if (!_search->text().isEmpty()) {
		// Like Telegram: the search ends once a result was opened.
		_search->clear();
	}
	controller()->openRoute(route);
}

void Widget::jumpToChat(int direction) {
	auto active = QString();
	if (const auto section = controller()->mainSection()) {
		const auto route = Window::ParseRoute(section->sectionKey());
		if (route.type == Window::RouteType::Chat) {
			active = route.id;
		}
	}
	const auto next = _inner->neighbourChat(active, direction);
	if (!next.isEmpty()) {
		controller()->openRoute(Window::Routes::Chat(next));
	}
}

void Widget::refreshActiveChat() {
	auto active = QString();
	if (controller()->currentTab() == Window::Tab::Chats) {
		if (const auto section = controller()->mainSection()) {
			const auto route = Window::ParseRoute(section->sectionKey());
			if (route.type == Window::RouteType::Chat) {
				active = route.id;
			}
		}
	}
	_inner->setActiveChat(active);
}

void Widget::refreshState() {
	auto &chats = controller()->session().data().chats();
	const auto chatsShown = chatsTabShown();
	const auto rows = _inner->rowsCount();
	const auto loading = chats.inboxLoading();
	const auto &error = chats.inboxError();
	const auto state = (rows > 0)
		? kStateRows
		: _inner->searching()
		? kStateNothingFound
		: (error && !loading)
		? kStateError
		: (loading || !chats.inboxLoadedOnce())
		? kStateSkeleton
		: (_inner->folder() == Folder::Requests)
		? kStateEmptyRequests
		: (_inner->folder() == Folder::Archived)
		? kStateEmptyArchived
		: kStateEmptyMain;

	const auto bannerShown = chatsShown
		&& (state == kStateRows)
		&& error
		&& !loading;
	if (_banner->isHidden() == bannerShown) {
		_banner->setVisible(bannerShown);
		updateControlsGeometry();
	}

	if (state != _emptyState || (state == kStateError)) {
		_emptyState = state;
		delete _skeleton.data();
		delete _empty.data();
		if (state == kStateSkeleton) {
			_skeleton = new Ui::SkeletonList(
				this,
				Ui::SkeletonList::Kind::Chats);
		} else if (state != kStateRows) {
			auto descriptor = Ui::EmptyPlaceholderDescriptor();
			switch (state) {
			case kStateNothingFound:
				descriptor.icon = &st::emptyIconSearch;
				descriptor.title = tr::lng_nothing_found(tr::now);
				break;
			case kStateError:
				descriptor.icon = &st::emptyIconError;
				descriptor.title = tr::lng_dialogs_load_failed(tr::now);
				descriptor.text = Support::ErrorText(*error);
				descriptor.buttonText = tr::lng_try_again(tr::now);
				descriptor.buttonCallback = [this] {
					controller()->session().data().chats().loadInbox(this);
				};
				break;
			case kStateEmptyRequests:
				descriptor.icon = &st::emptyIconChats;
				descriptor.title = tr::lng_dialogs_requests_empty(tr::now);
				descriptor.text = tr::lng_dialogs_requests_about(tr::now);
				break;
			case kStateEmptyArchived:
				descriptor.icon = &st::emptyIconChats;
				descriptor.title = tr::lng_dialogs_archived_empty(tr::now);
				descriptor.text = tr::lng_dialogs_archived_about(tr::now);
				break;
			default:
				descriptor.icon = &st::emptyIconChats;
				descriptor.title = tr::lng_dialogs_empty_title(tr::now);
				descriptor.text = tr::lng_dialogs_empty_text(tr::now);
				descriptor.buttonText = tr::lng_dialogs_new_message(tr::now);
				descriptor.buttonCallback = [this] {
					ShowNewMessageBox(controller());
				};
			}
			_empty = new Ui::EmptyPlaceholder(this, std::move(descriptor));
		}
		updateControlsGeometry();
	}
	_scroll->setVisible(chatsShown && state == kStateRows);
	if (_skeleton) {
		_skeleton->setVisible(chatsShown);
	}
	if (_empty) {
		_empty->setVisible(chatsShown);
	}
	for (auto i = 0, count = int(_subTabs.size()); i != count; ++i) {
		if (const auto widget = _subTabs[i].widget.data()) {
			widget->setVisible(i == _subTab);
		}
	}
}

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateControlsGeometry();
}

void Widget::updateControlsGeometry() {
	const auto outer = width();
	_bar->resizeToWidth(outer);
	_bar->move(0, 0);
	auto top = _bar->height();

	const auto &padding = st::dialogsSearchPadding;
	_search->resizeToWidth(outer - padding.left() - padding.right());
	_search->moveToLeft(padding.left(), top + padding.top(), outer);
	top += padding.top() + _search->height() + padding.bottom();

	if (_tabs) {
		const auto shown = (_inner->folder() == Folder::Main);
		_tabs->setVisible(shown);
		if (shown) {
			_tabs->resizeToWidth(outer);
			_tabs->move(0, top);
			top += _tabs->height();
		}
	}
	if (!_banner->isHidden()) {
		_banner->setGeometry(0, top, outer, st::dialogsBannerHeight);
		top += st::dialogsBannerHeight;
	}
	_paneRect = QRect(0, top, outer, std::max(height() - top, 0));
	_scroll->setGeometry(_paneRect);
	if (_skeleton) {
		_skeleton->setGeometry(_paneRect);
	}
	if (_empty) {
		_empty->setGeometry(_paneRect);
	}
	for (const auto &tab : _subTabs) {
		if (const auto widget = tab.widget.data()) {
			// A foreign sub-tab starts where the banner would be.
			const auto y = _banner->isHidden()
				? _paneRect.y()
				: (_paneRect.y() - st::dialogsBannerHeight);
			widget->setGeometry(0, y, outer, std::max(height() - y, 0));
		}
	}
}

bool Widget::handleKey(QKeyEvent *e) {
	if (!chatsTabShown()) {
		return false;
	}
	const auto modifiers = e->modifiers()
		& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	if (modifiers) {
		return false;
	}
	switch (e->key()) {
	case Qt::Key_Down: _inner->selectSkip(1); return true;
	case Qt::Key_Up: _inner->selectSkip(-1); return true;
	case Qt::Key_PageDown:
		_inner->selectSkipPage(_scroll->height(), 1);
		return true;
	case Qt::Key_PageUp:
		_inner->selectSkipPage(_scroll->height(), -1);
		return true;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		if (!_inner->chooseSelected() && _inner->searching()) {
			_inner->selectSkip(1);
			_inner->chooseSelected();
		}
		return true;
	}
	return false;
}

void Widget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) {
		// One step back per press: the search, the folder, then the chat.
		searchCancelled();
		return;
	}
	if (handleKey(e)) {
		return;
	}
	const auto text = e->text();
	const auto modifiers = e->modifiers()
		& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	if (!modifiers
		&& !text.isEmpty()
		&& text.at(0).isPrint()
		&& !text.at(0).isSpace()) {
		// Typing anywhere in the column starts a search.
		focusSearch();
		_search->field()->insertText(text);
		return;
	}
	SectionWidget::keyPressEvent(e);
}

bool Widget::eventFilter(QObject *object, QEvent *event) {
	if (event->type() == QEvent::KeyPress
		&& object == _search->field()->rawTextEdit()) {
		const auto key = static_cast<QKeyEvent*>(event);
		switch (key->key()) {
		case Qt::Key_Down:
		case Qt::Key_Up:
		case Qt::Key_PageDown:
		case Qt::Key_PageUp:
			if (handleKey(key)) {
				return true;
			}
			break;
		}
	}
	return SectionWidget::eventFilter(object, event);
}

} // namespace Dialogs
