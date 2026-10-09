#include "conversation/conversation_topics.h"

#include "conversation/conversation_boxes.h"
#include "conversation/conversation_time.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/connection_label.h"
#include "support/error_text.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/chips.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_conversation.h"

#include <QtCore/QDateTime>
#include <QtGui/QKeyEvent>

namespace Conversation {
namespace {

constexpr auto kHintCoalesceMs = 300;
constexpr auto kOfflineRefreshMs = 30'000;
constexpr auto kMaxRefreshPages = 5;
constexpr auto kNearBottom = 400;
constexpr auto kSkeletonCards = 3;
// The chip of «همه» (an empty id means "no chip" to the chips row).
const auto kAllChip = u"*"_s;

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

// «امروز از چه می‌خواهی حرف بزنی؟» — looks like a field, opens the box.
class ComposeButton final : public Ui::RippleButton {
public:
	explicit ComposeButton(QWidget *parent)
	: RippleButton(parent, st::defaultRipple) {
		resize(width(), st::conversationComposeHeight);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto radius = double(st::conversationComposeRadius);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(isOver()
				? st::conversationComposeBgOver
				: st::conversationComposeBg);
			p.drawRoundedRect(rect(), radius, radius);
		}
		paintRipple(p, 0, 0);
		const auto &icon = st::conversationComposeIcon;
		const auto left = int(st::conversationComposeIconLeft);
		icon.paint(p, left, (height() - icon.height()) / 2, width());
		const auto textLeft = left
			+ icon.width()
			+ int(st::conversationComposeIconSkip);
		p.setFont(st::conversationComposeFont);
		p.setPen(st::conversationComposeFg);
		p.drawTextLeft(
			textLeft,
			(height() - st::conversationComposeFont->height) / 2,
			width(),
			st::conversationComposeFont->elided(
				tr::lng_conv_what_do_want_talk_about(tr::now),
				std::max(width() - textLeft - left, 0)));
	}

	void onStateChanged(State was, StateChangeSource source) override {
		RippleButton::onStateChanged(was, source);
		update();
	}

	QImage prepareRippleMask() const override {
		return Ui::RippleAnimation::RoundRectMask(
			size(),
			st::conversationComposeRadius);
	}

};

} // namespace

class TopicsList::Skeleton final : public Ui::RpWidget {
public:
	explicit Skeleton(QWidget *parent)
	: RpWidget(parent)
	, _animation([this] { update(); }) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		const auto each = CardPainter::SkeletonHeight();
		auto top = int(st::conversationListTopSkip);
		for (auto i = 0; i != kSkeletonCards; ++i) {
			if (top >= height()) {
				break;
			}
			CardPainter::PaintSkeleton(
				p,
				QRect(0, top, width(), each),
				_animation,
				rect());
			top += each;
		}
	}
	void showEvent(QShowEvent *e) override {
		RpWidget::showEvent(e);
		_animation.start();
	}
	void hideEvent(QHideEvent *e) override {
		RpWidget::hideEvent(e);
		_animation.stop();
	}

private:
	Ui::SkeletonAnimation _animation;

};

TopicsList::TopicsList(
	QWidget *parent,
	Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _painter(&controller->session(), [this](const QString &) {
	if (_list) {
		_list->update();
	}
})
, _compose(new ComposeButton(this))
, _chips(new Ui::ChipsRow(this, st::conversationChips))
, _scroll(new Ui::ScrollArea(this))
, _skeleton(new Skeleton(this))
, _tickTimer([this] { tick(); })
, _hintTimer([this] { refresh(); })
, _offlineTimer([this] {
	if (_loadedOnce) {
		refresh();
	}
}) {
	setFocusPolicy(Qt::StrongFocus);
	_list = _scroll->setOwnedWidget(Ui::object_ptr<Ui::ActionList>(
		_scroll,
		static_cast<Ui::ActionListDelegate*>(this),
		st::conversationList));
	_list->setSkips(st::conversationListTopSkip, st::conversationListBottomSkip);
	_scroll->setNearEdgeThreshold(kNearBottom);
	_skeleton->hide();

	_compose->setClickedCallback([this] {
		ShowNewConversationBox(_controller);
	});
	setupChips();

	QObject::connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
		loadMore();
	});
	QObject::connect(
		_list,
		&Ui::ListWidget::rowClicked,
		this,
		[this](int index) { activate(index); });
	QObject::connect(
		_list,
		&Ui::ActionList::actionClicked,
		this,
		[this](int index, int action) {
			if (index < 0 || index >= int(_cards.size())) {
				return;
			} else if (action == kCardSpeaker) {
				_controller->openRoute(
					Window::Routes::User(_cards[index].item.speakerId));
			} else {
				activate(index);
			}
		});
	QObject::connect(
		_list,
		&Ui::ListWidget::rowContextMenu,
		this,
		[this](int index, QPoint globalPosition) {
			if (index >= 0 && index < int(_cards.size())) {
				ShowTopicMenu(this, _controller, _cards[index], globalPosition);
			}
		});
	QObject::connect(
		_list,
		&Ui::ListWidget::scrollToRequested,
		this,
		[this](int top, int bottom) { _scroll->scrollToY(top, bottom); });

	auto &conversations = data().conversations();
	QObject::connect(
		&conversations,
		&Data::Conversations::openListChanged,
		this,
		[this](const QString &category) { hintReceived(category); });
	QObject::connect(
		&data().transport(),
		&Data::Transport::connectedChanged,
		this,
		[this](bool connected) {
			updateOfflinePolling();
			if (connected && !isHidden()) {
				data().conversations().watchOpen(_category);
				if (_loadedOnce) {
					refresh();
				}
			}
		});
	QObject::connect(
		&data().users(),
		&Data::Users::changed,
		this,
		[this] {
			auto changed = false;
			for (auto &card : _cards) {
				changed = _painter.refreshName(card) || changed;
			}
			if (changed) {
				_list->update();
			}
		});
	// A topic of mine got (or lost) its chat row: the «yours» look follows.
	QObject::connect(
		&data().chats(),
		&Data::Chats::listChanged,
		this,
		[this] {
			auto changed = false;
			for (auto &card : _cards) {
				const auto own = _painter.make(card.item).own;
				if (card.own != own) {
					card.own = own;
					changed = true;
				}
			}
			if (changed) {
				_list->refresh();
			}
		});
	QObject::connect(Events(), &Notifier::topicHidden, this, [this](
			const QString &id) {
		removeCard(id);
	});
	QObject::connect(Events(), &Notifier::topicCreated, this, [this] {
		if (_loadedOnce) {
			if (isHidden()) {
				_dirtyWhileHidden = true;
			} else {
				refresh();
			}
		}
	});
	style::OnPaletteChanged(this, [this] {
		_painter.clearUserpics();
		update();
	});

	updateStateWidgets();
	conversations.loadDismissed(this, [this] {
		_dismissedLoaded = true;
		if (!isHidden()) {
			load(true);
		}
	});
}

TopicsList::~TopicsList() {
	if (_watching) {
		data().conversations().unwatchOpen();
	}
}

Data::Session &TopicsList::data() const {
	return _controller->session().data();
}

void TopicsList::setupChips() {
	_chips->addChip(kAllChip, tr::lng_all(tr::now));
	for (const auto &category : data().conversations().config().categories) {
		_chips->addChip(category.slug, category.title);
	}
	_chips->setActive(kAllChip, anim::type::instant);
	QObject::connect(_chips, &Ui::ChipsRow::activated, this, [this](
			const QString &id) {
		selectCategory((id == kAllChip) ? QString() : id);
	});
}

void TopicsList::selectCategory(const QString &slug) {
	if (_category == slug) {
		return;
	}
	_category = slug;
	_chips->setActive(slug.isEmpty() ? kAllChip : slug);
	if (_watching) {
		data().conversations().watchOpen(_category);
	}
	_cards.clear();
	_loadedOnce = false;
	_scroll->scrollToY(0);
	load(true);
}

bool TopicsList::visibleItem(const Data::OpenConversation &item) const {
	return !data().conversations().isDismissed(item.id)
		&& !Expired(item.expiresAt, NowMs());
}

void TopicsList::setItems(const QVector<Data::OpenConversation> &items) {
	_cards.clear();
	appendItems(items);
}

void TopicsList::appendItems(const QVector<Data::OpenConversation> &items) {
	for (const auto &item : items) {
		if (!visibleItem(item)) {
			continue;
		}
		const auto known = std::any_of(
			_cards.cbegin(),
			_cards.cend(),
			[&](const Card &card) { return card.item.id == item.id; });
		if (!known) {
			_cards.push_back(_painter.make(item));
		}
	}
}

void TopicsList::load(bool reset) {
	if (!_dismissedLoaded) {
		return;
	}
	const auto generation = ++_generation;
	if (reset) {
		_next = QString();
		_pagesShown = 1;
		// The last list of «همه» shows at once while the fresh one loads.
		if (_category.isEmpty() && _cards.empty()) {
			if (const auto &cached = data().conversations().cachedOpen()) {
				setItems(cached->conversations);
			}
		}
	}
	_loading = true;
	contentChanged();
	data().conversations().loadOpen(_category, reset ? QString() : _next, this, [=, this](
			const Data::OpenConversationsPage &page) {
		if (generation != _generation) {
			return;
		}
		_next = page.nextCursor;
		if (reset) {
			setItems(page.conversations);
		} else {
			appendItems(page.conversations);
		}
		_error = std::nullopt;
		_loadedOnce = true;
		_loading = false;
		contentChanged();
	}, [=, this](const Api::Error &error) {
		if (generation != _generation) {
			return;
		}
		_error = error;
		_loadedOnce = true;
		_loading = false;
		contentChanged();
	});
}

// Re-reads as many pages as were on screen (at most five), then swaps the
// list: the cards do not jump while the pages arrive.
void TopicsList::refresh() {
	if (!_dismissedLoaded) {
		return;
	} else if (isHidden()) {
		_dirtyWhileHidden = true;
		return;
	}
	const auto generation = ++_generation;
	_loading = false;
	refreshPage(
		generation,
		std::clamp(_pagesShown, 1, kMaxRefreshPages),
		QString(),
		std::make_shared<QVector<Data::OpenConversation>>());
}

void TopicsList::refreshPage(
		int generation,
		int pagesLeft,
		const QString &cursor,
		std::shared_ptr<QVector<Data::OpenConversation>> collected) {
	data().conversations().loadOpen(_category, cursor, this, [=, this](
			const Data::OpenConversationsPage &page) {
		if (generation != _generation) {
			return;
		}
		*collected += page.conversations;
		if (pagesLeft > 1 && !page.nextCursor.isEmpty()) {
			refreshPage(generation, pagesLeft - 1, page.nextCursor, collected);
			return;
		}
		_next = page.nextCursor;
		setItems(*collected);
		_error = std::nullopt;
		_loadedOnce = true;
		contentChanged();
	}, [=, this](const Api::Error &error) {
		if (generation != _generation) {
			return;
		}
		// What is on screen stays; an empty screen tells why.
		if (_cards.empty()) {
			_error = error;
		}
		contentChanged();
	});
}

void TopicsList::loadMore() {
	if (_loading || _next.isEmpty() || !_loadedOnce) {
		return;
	}
	++_pagesShown;
	load(false);
}

void TopicsList::removeCard(const QString &id) {
	const auto i = std::find_if(_cards.begin(), _cards.end(), [&](const Card &card) {
		return card.item.id == id;
	});
	if (i != _cards.end()) {
		_cards.erase(i);
		contentChanged();
	}
}

void TopicsList::sweepExpired() {
	const auto now = NowMs();
	const auto was = _cards.size();
	_cards.erase(
		std::remove_if(_cards.begin(), _cards.end(), [&](const Card &card) {
			return Expired(card.item.expiresAt, now);
		}),
		_cards.end());
	if (_cards.size() != was) {
		contentChanged();
	}
}

void TopicsList::scheduleTick() {
	if (isHidden() || _cards.empty()) {
		_tickTimer.cancel();
		return;
	}
	auto expires = QList<qint64>();
	expires.reserve(int(_cards.size()));
	for (const auto &card : _cards) {
		expires.push_back(card.item.expiresAt);
	}
	_tickTimer.callOnce(NextTickDelay(expires, NowMs()));
}

// The rings and the captions move on this local timer.
void TopicsList::tick() {
	sweepExpired();
	_list->update();
	scheduleTick();
}

void TopicsList::updateOfflinePolling() {
	// Without the connection there are no hints: the visible list is re-read
	// every half a minute until it is back.
	const auto needed = !isHidden() && !data().transport().connected();
	if (!needed) {
		_offlineTimer.cancel();
	} else if (!_offlineTimer.isActive()) {
		_offlineTimer.callEach(kOfflineRefreshMs);
	}
}

void TopicsList::hintReceived(const QString &category) {
	if (!_category.isEmpty() && !category.isEmpty() && category != _category) {
		return;
	} else if (!_loadedOnce) {
		return;
	} else if (isHidden()) {
		_dirtyWhileHidden = true;
		return;
	}
	_hintTimer.callOnce(kHintCoalesceMs);
}

void TopicsList::contentChanged() {
	_list->refresh();
	updateStateWidgets();
	scheduleTick();
	if (_loading && !_cards.empty()) {
		if (!_tailAnimation) {
			_tailAnimation = std::make_unique<Ui::InfiniteRadialAnimation>([this] {
				_list->updateRow(int(_cards.size()));
			});
		}
		_tailAnimation->start();
	} else if (_tailAnimation) {
		_tailAnimation->stop();
	}
}

void TopicsList::updateStateWidgets() {
	const auto empty = _cards.empty();
	const auto skeleton = empty && (!_loadedOnce || _loading);
	_skeleton->setVisible(skeleton);
	auto key = QString();
	if (empty && !skeleton) {
		key = _error
			? (u"error:"_s + Support::ErrorText(*_error, tr::lng_error_load(tr::now)))
			: u"empty"_s;
	}
	if (_placeholderKey == key) {
		return;
	}
	_placeholderKey = key;
	_placeholder.destroy();
	if (key.isEmpty()) {
		return;
	}
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	if (_error) {
		descriptor.icon = &st::conversationErrorIcon;
		descriptor.title = tr::lng_error_load(tr::now);
		descriptor.text = Support::ErrorText(*_error);
		descriptor.buttonText = tr::lng_retry(tr::now);
		descriptor.buttonCallback = [this] {
			_error = std::nullopt;
			_loadedOnce = false;
			load(true);
		};
	} else {
		descriptor.icon = &st::conversationEmptyIcon;
		descriptor.title = tr::lng_conv_no_open_conversations_yet(tr::now);
		descriptor.text = tr::lng_conv_be_first_open_one(tr::now);
		descriptor.buttonText = tr::lng_conv_start_conversation(tr::now);
		descriptor.buttonCallback = [this] {
			ShowNewConversationBox(_controller);
		};
	}
	_placeholder.create(this, std::move(descriptor));
	_placeholder->setGeometry(_scroll->geometry());
	_placeholder->show();
}

void TopicsList::activate(int index) {
	if (index < 0 || index >= int(_cards.size())) {
		return;
	}
	const auto &card = _cards[index];
	if (card.own) {
		OpenConversation(_controller, card.item.id);
	} else {
		ShowJoinBox(_controller, card.item.id);
	}
}

bool TopicsList::isLoaderRow(int index) const {
	return index >= int(_cards.size());
}

int TopicsList::rowsCount() const {
	return int(_cards.size()) + ((_loading && !_cards.empty()) ? 1 : 0);
}

int TopicsList::rowHeight(int index, int width) const {
	return isLoaderRow(index)
		? int(st::conversationTailHeight)
		: _painter.height(_cards[index], width);
}

bool TopicsList::rowClickable(int index) const {
	return !isLoaderRow(index);
}

int TopicsList::rowActionAt(int index, QPoint point, int width) const {
	return isLoaderRow(index)
		? 0
		: _painter.actionAt(_cards[index], point, width);
}

void TopicsList::paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) {
	if (isLoaderRow(index)) {
		if (_tailAnimation) {
			const auto size = int(st::conversationTailSpinner);
			_tailAnimation->draw(
				p,
				QRect(
					rect.x() + (rect.width() - size) / 2,
					rect.y() + (rect.height() - size) / 2,
					size,
					size),
				2.5,
				st::brandPrimary->c);
		}
		return;
	}
	auto cardState = CardState();
	cardState.over = state.over;
	cardState.overAction = _list->overAction(index);
	cardState.pressedAction = _list->pressedAction(index);
	_painter.paint(p, _cards[index], rect, cardState, NowMs());
}

void TopicsList::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto &padding = st::conversationComposePadding;
	_compose->setGeometryToLeft(
		padding.left(),
		padding.top(),
		std::max(width() - padding.left() - padding.right(), 1),
		st::conversationComposeHeight,
		width());
	auto top = padding.top() + int(st::conversationComposeHeight) + padding.bottom();
	_chips->resizeToWidth(width());
	_chips->moveToLeft(0, top, width());
	top += _chips->height();
	const auto area = QRect(0, top, width(), std::max(height() - top, 0));
	_scroll->setGeometry(area);
	_skeleton->setGeometry(area);
	if (_placeholder) {
		_placeholder->setGeometry(area);
	}
}

void TopicsList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::conversationListBg);
}

void TopicsList::keyPressEvent(QKeyEvent *e) {
	switch (e->key()) {
	case Qt::Key_Down: _list->selectSkip(1); return;
	case Qt::Key_Up: _list->selectSkip(-1); return;
	case Qt::Key_PageDown: _list->selectSkipPage(_scroll->height(), 1); return;
	case Qt::Key_PageUp: _list->selectSkipPage(_scroll->height(), -1); return;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		if (_list->chooseSelected()) {
			return;
		}
		break;
	}
	RpWidget::keyPressEvent(e);
}

void TopicsList::showEvent(QShowEvent *e) {
	RpWidget::showEvent(e);
	_watching = true;
	data().conversations().watchOpen(_category);
	updateOfflinePolling();
	if (!_dismissedLoaded) {
		return;
	} else if (_loadedOnce) {
		// Coming back to the list reads it once (hints were not followed
		// while it was hidden).
		_dirtyWhileHidden = false;
		sweepExpired();
		refresh();
	} else if (!_loading) {
		load(true);
	}
	scheduleTick();
}

void TopicsList::hideEvent(QHideEvent *e) {
	RpWidget::hideEvent(e);
	if (_watching) {
		_watching = false;
		data().conversations().unwatchOpen();
	}
	_tickTimer.cancel();
	_hintTimer.cancel();
	updateOfflinePolling();
}

// ---- the list as a section

namespace {

class TopicsSection final : public Window::SectionWidget {
public:
	TopicsSection(QWidget *parent, Window::SessionController *controller)
	: SectionWidget(parent, controller)
	, _bar(new Ui::TopBar(this))
	, _list(new TopicsList(this, controller)) {
		_connection = new Support::ConnectionTitle(
			_bar,
			&controller->session(),
			tr::lng_conv_topics(tr::now));
		const auto create = _bar->addAction(st::topBarCreateButton);
		create->setClickedCallback([controller] {
			ShowNewConversationBox(controller);
		});
		Ui::InstallTooltip(create, [] {
			return tr::lng_conv_new_conversation(tr::now);
		});
		setTopBar(_bar);
	}

	QString sectionKey() const override {
		return Window::Routes::ConversationTopics();
	}
	void setInnerFocus() override {
		_list->setInnerFocus();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		const auto top = _bar->height();
		const auto listWidth = std::min(
			width(),
			int(st::conversationListMaxWidth));
		_list->setGeometry(
			(width() - listWidth) / 2,
			top,
			listWidth,
			std::max(height() - top, 0));
	}

private:
	Ui::TopBar *_bar = nullptr;
	TopicsList *_list = nullptr;
	Support::ConnectionTitle *_connection = nullptr;

};

} // namespace

Window::SectionFactory TopicsSectionFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new TopicsSection(parent, controller));
	};
}

} // namespace Conversation
