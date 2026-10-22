// The list pages of «الی» in the main column: a topic (#tag) with «برتر /
// تازه», its open conversations and the follow / hide actions; «در حال
// شکل‌گیری» (trending); the page of a sound with the videos that use it;
// «ذخیره‌ها» (bookmarks).
#include "eli/eli_actions.h"

#include "data/data_feed.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "eli/eli_post_card.h"
#include "eli/eli_post_list.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtCore/QUrl>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>

namespace Eli {
namespace {

constexpr auto kMaxTopicConversations = 10;

[[nodiscard]] int Column(int width) {
	return std::min(width, int(st::eliCardMaxWidth));
}

[[nodiscard]] QString Hashtag(const QString &title) {
	// Isolated: the sign stays before the word in the word's own direction.
	return QChar(0x2068)
		+ tr::lng_eli_hashtag(tr::now, lt_title, title)
		+ QChar(0x2069);
}

// ---- topic

struct TopicConversation {
	QString id;
	QString topic;
};

class TopicHeader final : public Ui::RpWidget {
public:
	TopicHeader(QWidget *parent, const QString &slug)
	: RpWidget(parent)
	, _title(Hashtag(slug))
	, _follow(new Ui::RoundButton(this, tr::lng_follow(tr::now), st::defaultRoundButton))
	, _talk(new Ui::RoundButton(
		this,
		tr::lng_eli_start_conversation_about(tr::now),
		st::outlineRoundButton)) {
		_talk->setIcon(&st::eliTopicConvIcon);
		_follow->hide();
		_talk->hide();
	}

	[[nodiscard]] Ui::RoundButton *followButton() const {
		return _follow;
	}
	[[nodiscard]] Ui::RoundButton *talkButton() const {
		return _talk;
	}

	void setInfo(const Data::TopicInfo &info) {
		_title = Hashtag(info.title.isEmpty() ? info.slug : info.title);
		_counts = tr::lng_eli_topic_counts(
			tr::now,
			lt_posts,
			Lang::CompactCount(info.postsToday),
			lt_people,
			Lang::CompactCount(info.peopleToday),
			lt_conversations,
			Lang::Num(info.openConversations));
		_error = QString();
		_following = info.following;
		_loaded = true;
		_follow->show();
		_talk->show();
		refreshFollow();
	}

	void setFollowing(bool following) {
		_following = following;
		refreshFollow();
	}

	void setError(const QString &text) {
		_error = text;
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::eliTopicHeaderPadding;
		_column = Column(newWidth);
		_left = (newWidth - _column) / 2;
		const auto inner = _column - padding.left() - padding.right();
		auto y = padding.top() + st::eliTopicTitleFont->height + 6;
		y += st::eliTopicCountsFont->height;
		if (_loaded) {
			y += st::eliTopicButtonsTop;
			_follow->resizeToNaturalWidth(inner);
			_talk->resizeToNaturalWidth(inner);
			_follow->moveToLeft(_left + padding.left(), y, newWidth);
			if (_follow->width() + 8 + _talk->width() <= inner) {
				_talk->moveToLeft(
					_left + padding.left() + _follow->width() + 8,
					y,
					newWidth);
				y += _follow->height();
			} else {
				y += _follow->height() + 8;
				_talk->moveToLeft(_left + padding.left(), y, newWidth);
				y += _talk->height();
			}
		}
		return y + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::eliTopicHeaderPadding;
		const auto inner = _column - padding.left() - padding.right();
		p.setPen(st::textPrimary);
		p.setFont(st::eliTopicTitleFont);
		p.drawTextLeft(
			_left + padding.left(),
			padding.top(),
			width(),
			st::eliTopicTitleFont->elided(_title, inner));
		const auto top = padding.top() + st::eliTopicTitleFont->height + 6;
		p.setFont(st::eliTopicCountsFont);
		if (!_error.isEmpty()) {
			p.setPen(st::statusError);
			p.drawTextLeft(
				_left + padding.left(),
				top,
				width(),
				st::eliTopicCountsFont->elided(_error, inner));
		} else if (_loaded) {
			p.setPen(st::eliTopicCountsFg);
			p.drawTextLeft(
				_left + padding.left(),
				top,
				width(),
				st::eliTopicCountsFont->elided(_counts, inner));
		}
	}

private:
	void refreshFollow() {
		_follow->setText(_following
			? tr::lng_following(tr::now)
			: tr::lng_follow(tr::now));
		resizeToWidth(width());
		update();
	}

	QString _title;
	QString _counts;
	QString _error;
	Ui::RoundButton *_follow = nullptr;
	Ui::RoundButton *_talk = nullptr;
	bool _following = false;
	bool _loaded = false;
	int _column = 0;
	int _left = 0;

};

// «گفتگوهای باز» of a topic: night cards with a join button.
class TopicConversations final : public Ui::RpWidget {
public:
	TopicConversations(QWidget *parent, Window::SessionController *controller)
	: RpWidget(parent)
	, _controller(controller) {
	}

	void setItems(const QVector<TopicConversation> &items) {
		for (auto &card : _cards) {
			delete card.join;
		}
		_cards.clear();
		for (const auto &item : items) {
			auto card = Card();
			card.id = item.id;
			card.topic.setText(st::eliTopicConvTopicStyle, item.topic);
			card.join = new Ui::RoundButton(this, tr::lng_join(tr::now), st::eliTopicConvJoin);
			const auto id = item.id;
			const auto controller = _controller;
			card.join->setClickedCallback([=] {
				controller->openRoute(Window::Routes::Conversation(id));
			});
			card.join->show();
			_cards.push_back(std::move(card));
		}
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_cards.empty()) {
			return 0;
		}
		const auto &padding = st::eliTopicConvsPadding;
		const auto column = Column(newWidth);
		_left = (newWidth - column) / 2;
		const auto inner = column - padding.left() - padding.right();
		const auto skip = st::eliTopicConvCardSkip;
		const auto perRow = std::clamp(
			(inner + skip) / (st::eliTopicConvCardWidth + skip),
			1,
			3);
		const auto cardWidth = (inner - (perRow - 1) * skip) / perRow;
		const auto pad = st::eliTopicConvCardPadding;
		auto y = padding.top() + st::eliTopicConvsTitleHeight;
		for (auto i = 0; i < int(_cards.size()); i += perRow) {
			auto rowHeight = 0;
			for (auto j = i; j != std::min(i + perRow, int(_cards.size())); ++j) {
				auto &card = _cards[j];
				card.textHeight = card.topic.countHeight(cardWidth - 2 * pad, 3);
				rowHeight = std::max(
					rowHeight,
					pad + card.textHeight + 10 + card.join->height() + pad);
			}
			for (auto j = i; j != std::min(i + perRow, int(_cards.size())); ++j) {
				auto &card = _cards[j];
				card.rect = QRect(
					_left + padding.left() + (j - i) * (cardWidth + skip),
					y,
					cardWidth,
					rowHeight);
				card.join->resizeToNaturalWidth(cardWidth - 2 * pad);
				card.join->moveToLeft(
					card.rect.x() + pad,
					y + rowHeight - pad - card.join->height(),
					newWidth);
			}
			y += rowHeight + skip;
		}
		return y - skip + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		if (_cards.empty()) {
			return;
		}
		auto p = Painter(this);
		const auto &padding = st::eliTopicConvsPadding;
		p.setPen(st::textPrimary);
		p.setFont(st::eliTopicConvsTitleFont);
		p.drawTextLeft(
			_left + padding.left(),
			padding.top() + (st::eliTopicConvsTitleHeight - st::eliTopicConvsTitleFont->height) / 2 - 2,
			width(),
			tr::lng_eli_open_conversations(tr::now));
		const auto pad = st::eliTopicConvCardPadding;
		for (const auto &card : _cards) {
			const auto rect = style::rtlrect(card.rect, width());
			{
				PainterHighQualityEnabler hq(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::eliInviteBg);
				p.drawRoundedRect(rect, st::eliTopicConvCardRadius, st::eliTopicConvCardRadius);
			}
			p.setPen(st::eliInviteFg);
			auto context = Ui::Text::PaintContext();
			context.position = rect.topLeft() + QPoint(pad, pad);
			context.availableWidth = rect.width() - 2 * pad;
			context.elisionLines = 3;
			context.autoDirection = true;
			card.topic.draw(p, context);
		}
	}

private:
	struct Card {
		QString id;
		Ui::Text::String topic;
		Ui::RoundButton *join = nullptr;
		QRect rect; // logical
		int textHeight = 0;
	};

	Window::SessionController *_controller = nullptr;
	std::vector<Card> _cards;
	int _left = 0;

};

class TopicSection final : public Window::SectionWidget {
public:
	TopicSection(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &slug)
	: SectionWidget(parent, controller)
	, _slug(slug)
	, _bar(new Ui::TopBar(this))
	, _scroll(new Ui::ScrollArea(this)) {
		_bar->setTitle(Hashtag(slug));
		setTopBar(_bar);
		_more = _bar->addAction(st::topBarMoreButton);
		_more->setClickedCallback([this] { showMenu(); });

		_content = _scroll->setOwnedWidget(Ui::object_ptr<Ui::VerticalLayout>(_scroll));
		_header = _content->add(Ui::object_ptr<TopicHeader>(_content, slug));
		_conversations = _content->add(
			Ui::object_ptr<TopicConversations>(_content, controller));
		_tabs = _content->add(Ui::object_ptr<Ui::TabBar>(_content, st::eliTopicTabs));
		_tabs->setStretch(true);
		_tabs->setTabs({ tr::lng_eli_topic_top(tr::now), tr::lng_eli_topic_new(tr::now) });
		connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
			showTab(index);
		});
		_lists[0] = createList(u"top"_s);
		_scroll->setNearEdgeThreshold(600);
		connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
			if (const auto list = _lists[_tab]) {
				list->loadMore();
			}
		});

		_header->followButton()->setClickedCallback([this] {
			if (!_info.slug.isEmpty()) {
				const auto on = !_info.following;
				_info.following = on;
				_header->setFollowing(on);
				// Back as it was when the server did not take it.
				this->controller()->session().data().feed().followTopic(
					_info.slug,
					on,
					base::Guard(this),
					nullptr,
					[=, this] {
						if (_info.following == on) {
							_info.following = !on;
							_header->setFollowing(!on);
						}
					});
			}
		});
		_header->talkButton()->setClickedCallback([this] {
			// The conversation module owns the «new conversation» sheet; the
			// topic rides along as a hint.
			const auto title = _info.title.isEmpty() ? _slug : _info.title;
			this->controller()->openRoute(
				Window::Routes::ConversationTopics()
					+ u"?new="_s
					+ QString::fromUtf8(QUrl::toPercentEncoding(Hashtag(title))));
		});

		auto &feed = controller->session().data().feed();
		feed.topic(slug, base::Guard(this), [this](const Data::TopicInfo &info) {
			_info = info;
			_bar->setTitle(Hashtag(info.title.isEmpty() ? info.slug : info.title));
			_header->setInfo(info);
		}, [this](const Api::Error &error) {
			_header->setError(Support::ErrorText(
				error,
				tr::lng_eli_topic_info_failed(tr::now)));
		});
		feed.topicConversations(slug, base::Guard(this), [this](const QStringList &ids) {
			loadConversations(ids.mid(0, kMaxTopicConversations));
		});
	}

	QString sectionKey() const override {
		return Window::Routes::Topic(_slug);
	}

	void setInnerFocus() override {
		if (const auto list = _lists[_tab]) {
			list->setFocus();
		}
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		_scroll->setGeometry(0, _bar->height(), width(), std::max(height() - _bar->height(), 0));
	}

	void keyPressEvent(QKeyEvent *e) override {
		const auto list = _lists[_tab];
		if (!list || !list->handleKey(e)) {
			SectionWidget::keyPressEvent(e);
		}
	}

private:
	PostList *createList(const QString &tab) {
		const auto controller = this->controller();
		const auto slug = _slug;
		auto descriptor = ListDescriptor();
		descriptor.loader = [=](
				const QString &cursor,
				base::Guard guard,
				Fn<void(const ListPage &)> done,
				Data::FailCallback fail) {
			controller->session().data().feed().topicPosts(
				slug,
				tab,
				cursor,
				guard,
				[=](const Data::PostsPage &page) { done(ListPage::From(page)); },
				fail);
		};
		descriptor.empty = [] {
			auto result = Ui::EmptyPlaceholderDescriptor();
			result.icon = &st::emptyIconEli;
			result.title = tr::lng_eli_no_posts_topic_yet(tr::now);
			return result;
		};
		const auto list = _content->add(
			Ui::object_ptr<PostList>(_content, controller, std::move(descriptor)));
		list->setMinHeight(280);
		connect(list, &PostList::scrollToRequested, this, [=, this](int top, int bottom) {
			_scroll->scrollToY(top + list->y(), bottom + list->y());
		});
		return list;
	}

	void showTab(int index) {
		_tab = (index == 1) ? 1 : 0;
		if (!_lists[_tab]) {
			_lists[_tab] = createList((_tab == 1) ? u"new"_s : u"top"_s);
		}
		for (auto i = 0; i != 2; ++i) {
			if (_lists[i]) {
				_lists[i]->setVisible(i == _tab);
			}
		}
		_content->resizeToWidth(_content->width());
	}

	void loadConversations(const QStringList &ids) {
		if (ids.isEmpty()) {
			return;
		}
		// Each conversation is read once to know whether it is still open.
		const auto left = std::make_shared<int>(int(ids.size()));
		const auto found = std::make_shared<QVector<TopicConversation>>();
		const auto finish = [=, this] {
			if (--*left > 0) {
				return;
			}
			auto ordered = QVector<TopicConversation>();
			for (const auto &id : ids) {
				for (const auto &item : *found) {
					if (item.id == id) {
						ordered.push_back(item);
					}
				}
			}
			_conversations->setItems(ordered);
		};
		for (const auto &id : ids) {
			controller()->session().api().request(
				Api::Get,
				u"/v1/conversations/"_s + Data::PathSegment(id))
				.guard(this)
				.done([=](const QJsonObject &result) {
					const auto state = result.value("state"_L1).toString();
					if (state == u"open" || state == u"waiting") {
						found->push_back({ id, result.value("topic"_L1).toString() });
					}
					finish();
				})
				.fail([=](const Api::Error &) { finish(); })
				.send();
		}
	}

	void showMenu() {
		if (_info.id.isEmpty()) {
			return;
		}
		const auto menu = new Ui::PopupMenu(this);
		const auto hidden = _info.hidden;
		menu->addAction(
			hidden
				? tr::lng_eli_topic_show_again(tr::now)
				: tr::lng_eli_topic_not_interested(tr::now),
			[=, this] {
				_info.hidden = !hidden;
				const auto window = controller();
				controller()->session().data().feed().hideTopic(
					_info.id,
					!hidden,
					base::Guard(this),
					[=] {
						window->showToast(hidden
							? tr::lng_eli_topic_shown_again(tr::now)
							: tr::lng_eli_topic_hidden_toast(tr::now));
					},
					[=, this] {
						if (_info.hidden == !hidden) {
							_info.hidden = hidden;
						}
					});
			},
			hidden ? &st::eliMenuIconEye : &st::eliMenuIconEyeOff);
		menu->popupAnchored(_more);
	}

	QString _slug;
	Data::TopicInfo _info;
	Ui::TopBar *_bar = nullptr;
	Ui::IconButton *_more = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	TopicHeader *_header = nullptr;
	TopicConversations *_conversations = nullptr;
	Ui::TabBar *_tabs = nullptr;
	PostList *_lists[2] = { nullptr, nullptr };
	int _tab = 0;

};

// ---- trends

class TrendsList final : public Ui::RpWidget {
public:
	TrendsList(QWidget *parent, Window::SessionController *controller)
	: RpWidget(parent)
	, _controller(controller) {
		setMouseTracking(true);
	}

	void setTrends(const QVector<Data::Trend> &trends) {
		_trends = trends;
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_column = Column(newWidth);
		_left = (newWidth - _column) / 2;
		return int(_trends.size()) * st::eliTrendRowHeight + st::eliListBottomSkip;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto &padding = st::eliTrendPadding;
		const auto rowHeight = st::eliTrendRowHeight;
		const auto from = std::max(e->rect().top() / rowHeight, 0);
		const auto till = std::min(
			(e->rect().top() + e->rect().height()) / rowHeight + 1,
			int(_trends.size()));
		for (auto i = from; i < till; ++i) {
			const auto &trend = _trends[i];
			const auto top = i * rowHeight;
			if (i == _over) {
				p.fillRect(QRect(_left, top, _column, rowHeight), st::stateHover);
			}
			p.setPen(st::eliTrendRankFg);
			p.setFont(st::eliTrendRankFont);
			p.drawTextLeft(
				_left + padding.left(),
				top + (rowHeight - st::eliTrendRankFont->height) / 2,
				width(),
				Lang::Num(i + 1));
			const auto textLeft = _left + padding.left() + st::eliTrendRankWidth;
			const auto chevron = st::eliTrendChevron.width() + 8;
			auto available = _column
				- padding.left()
				- st::eliTrendRankWidth
				- padding.right()
				- chevron;
			const auto textTop = top
				+ (rowHeight - st::eliTrendTitleFont->height - st::eliTrendCountsFont->height - 2) / 2;
			const auto rising = trend.rising
				? tr::lng_eli_rising(tr::now)
				: QString();
			const auto risingWidth = rising.isEmpty()
				? 0
				: (st::eliTrendRisingFont->width(rising) + 14);
			const auto title = st::eliTrendTitleFont->elided(
				Hashtag(trend.title.isEmpty() ? trend.slug : trend.title),
				std::max(available - risingWidth - 8, 40));
			const auto titleWidth = st::eliTrendTitleFont->width(title);
			p.setPen(st::textPrimary);
			p.setFont(st::eliTrendTitleFont);
			p.drawTextLeft(textLeft, textTop, width(), title, titleWidth);
			if (risingWidth) {
				const auto badge = style::rtlrect(
					textLeft + titleWidth + 8,
					textTop + (st::eliTrendTitleFont->height - 20) / 2,
					risingWidth,
					20,
					width());
				PainterHighQualityEnabler hq(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::eliTrendRisingBg);
				p.drawRoundedRect(badge, 10, 10);
				p.setPen(st::eliTrendRisingFg);
				p.setFont(st::eliTrendRisingFont);
				p.drawTextCentered(badge, rising);
			}
			p.setPen(st::textTertiary);
			p.setFont(st::eliTrendCountsFont);
			p.drawTextLeft(
				textLeft,
				textTop + st::eliTrendTitleFont->height + 2,
				width(),
				st::eliTrendCountsFont->elided(
					tr::lng_eli_trend_counts(
						tr::now,
						lt_posts,
						Lang::CompactCount(trend.posts),
						lt_people,
						Lang::CompactCount(trend.authors),
						lt_conversations,
						Lang::Num(trend.openConversations)),
					available));
			st::eliTrendChevron.paint(
				p,
				_left + _column - padding.right() - st::eliTrendChevron.width(),
				top + (rowHeight - st::eliTrendChevron.height()) / 2,
				width());
			p.fillRect(QRect(_left, top + rowHeight - 1, _column, 1), st::eliCardLineFg);
		}
	}

	[[nodiscard]] int rowAt(QPoint point) const {
		if (point.x() < _left || point.x() >= _left + _column || point.y() < 0) {
			return -1;
		}
		const auto index = point.y() / st::eliTrendRowHeight;
		return (index < int(_trends.size())) ? index : -1;
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = rowAt(e->pos());
		if (over != _over) {
			_over = over;
			setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEvent(QEvent *e) override {
		RpWidget::leaveEvent(e);
		_over = -1;
		update();
	}

	void mousePressEvent(QMouseEvent *e) override {
		_pressed = (e->button() == Qt::LeftButton) ? rowAt(e->pos()) : -1;
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto pressed = std::exchange(_pressed, -1);
		if (e->button() == Qt::LeftButton && pressed >= 0 && pressed == rowAt(e->pos())) {
			_controller->openRoute(Window::Routes::Topic(_trends[pressed].slug));
		}
	}

	void contextMenuEvent(QContextMenuEvent *e) override {
		const auto index = rowAt(e->pos());
		if (index < 0 || _trends[index].id.isEmpty()) {
			return;
		}
		const auto id = _trends[index].id;
		const auto menu = new Ui::PopupMenu(this);
		menu->addAction(tr::lng_eli_topic_not_interested(tr::now), [=, this] {
			// The row leaves at once and comes back where it was when the
			// server did not take it.
			const auto i = std::find_if(
				_trends.cbegin(),
				_trends.cend(),
				[&](const Data::Trend &trend) { return trend.id == id; });
			if (i == _trends.cend()) {
				return;
			}
			const auto at = int(i - _trends.cbegin());
			const auto trend = *i;
			_controller->session().data().feed().hideTopic(
				id,
				true,
				base::Guard(this),
				[controller = _controller] {
					controller->showToast(tr::lng_eli_topic_hidden_toast(tr::now));
				},
				[=, this] {
					const auto known = std::any_of(
						_trends.cbegin(),
						_trends.cend(),
						[&](const Data::Trend &other) { return other.id == id; });
					if (!known) {
						_trends.insert(
							std::min(at, int(_trends.size())),
							trend);
						_over = -1;
						resizeToWidth(width());
						update();
					}
				});
			_trends.erase(
				std::remove_if(_trends.begin(), _trends.end(), [&](const Data::Trend &trend) {
					return trend.id == id;
				}),
				_trends.end());
			_over = -1;
			resizeToWidth(width());
			update();
		}, &st::eliMenuIconEyeOff);
		menu->popup(e->globalPos());
	}

private:
	Window::SessionController *_controller = nullptr;
	QVector<Data::Trend> _trends;
	int _column = 0;
	int _left = 0;
	int _over = -1;
	int _pressed = -1;

};

// A section with a top bar, a scroll area and one state at a time.
class SimpleSection : public Window::SectionWidget {
public:
	SimpleSection(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &title)
	: SectionWidget(parent, controller)
	, _bar(new Ui::TopBar(this))
	, _scroll(new Ui::ScrollArea(this)) {
		_bar->setTitle(title);
		setTopBar(_bar);
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		const auto area = QRect(0, _bar->height(), width(), std::max(height() - _bar->height(), 0));
		_scroll->setGeometry(area);
		if (_state) {
			const auto column = Column(width());
			_state->setGeometry((width() - column) / 2, area.y(), column, area.height());
		}
		areaResized(area);
	}

	virtual void areaResized(const QRect &area) {
		Q_UNUSED(area);
	}

	void showSkeleton(Ui::SkeletonList::Kind kind) {
		delete _state;
		_state = new Ui::SkeletonList(this, kind);
		_state->show();
		_scroll->hide();
		resizeEvent(nullptr);
	}

	void showPlaceholder(Ui::EmptyPlaceholderDescriptor descriptor) {
		delete _state;
		_state = new Ui::EmptyPlaceholder(this, std::move(descriptor));
		_state->show();
		_scroll->hide();
		resizeEvent(nullptr);
	}

	void showContent() {
		delete _state;
		_state = nullptr;
		_scroll->show();
		resizeEvent(nullptr);
	}

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::RpWidget *_state = nullptr;

};

class TrendsSection final : public SimpleSection {
public:
	TrendsSection(QWidget *parent, Window::SessionController *controller)
	: SimpleSection(parent, controller, tr::lng_eli_trending(tr::now)) {
		_list = _scroll->setOwnedWidget(
			Ui::object_ptr<TrendsList>(_scroll, controller));
		load();
	}

	QString sectionKey() const override {
		return Routes::Trends();
	}

private:
	void load() {
		showSkeleton(Ui::SkeletonList::Kind::Lines);
		controller()->session().data().feed().trending(
			base::Guard(this),
			[this](const QVector<Data::Trend> &trends) {
				if (trends.isEmpty()) {
					auto descriptor = Ui::EmptyPlaceholderDescriptor();
					descriptor.icon = &st::eliEmptyIconDiscover;
					descriptor.title = tr::lng_eli_nothing_trending_right_now(tr::now);
					descriptor.text = tr::lng_eli_check_back_little_later(tr::now);
					showPlaceholder(std::move(descriptor));
					return;
				}
				_list->setTrends(trends);
				showContent();
			},
			[this](const Api::Error &error) {
				auto descriptor = Ui::EmptyPlaceholderDescriptor();
				descriptor.icon = &st::emptyIconError;
				descriptor.title = tr::lng_error_load(tr::now);
				descriptor.text = Support::ErrorText(error);
				descriptor.buttonText = tr::lng_try_again(tr::now);
				descriptor.buttonCallback = [this] {
					Ui::PostponeCall(this, [this] { load(); });
				};
				showPlaceholder(std::move(descriptor));
			});
	}

	TrendsList *_list = nullptr;

};

// ---- sound

class SoundContent final : public Ui::RpWidget {
public:
	SoundContent(QWidget *parent, Window::SessionController *controller)
	: RpWidget(parent)
	, _controller(controller) {
		setMouseTracking(true);
	}

	void setSound(const Data::PostSound &sound) {
		_sound = sound;
		_cover = nullptr;
		_coverPrepared = QPixmap();
		if (!sound.originPost.isEmpty() && !sound.media.isEmpty()) {
			_cover = Support::MakeImageSource(
				&_controller->session(),
				Data::MediaRef::PostMedia(sound.originPost, sound.media),
				Data::MediaVariant::Poster);
			if (_cover) {
				connect(_cover.get(), &Ui::ImageSource::updated, this, [this] {
					_coverPrepared = QPixmap();
					update();
				});
			}
		}
		resizeToWidth(width());
		update();
	}

	void addPosts(const QStringList &ids) {
		auto &feed = _controller->session().data().feed();
		for (const auto &id : ids) {
			const auto post = feed.post(id);
			const auto known = std::any_of(_tiles.begin(), _tiles.end(), [&](const Tile &tile) {
				return tile.postId == id;
			});
			if (!post || known) {
				continue;
			}
			auto tile = Tile();
			tile.postId = id;
			const Data::PostMedia *media = nullptr;
			for (const auto &entry : post->media) {
				if (entry.kind == u"video") {
					media = &entry;
					break;
				}
			}
			if (!media && !post->media.isEmpty()) {
				media = &post->media.front();
			}
			if (media) {
				auto options = Support::ImageOptions();
				options.blurhash = media->blurhash;
				options.aspect = QSize(media->width, media->height);
				tile.source = Support::MakeImageSource(
					&_controller->session(),
					Data::MediaRef::PostMedia(id, media->id),
					Data::MediaVariant::Poster,
					std::move(options));
				if (tile.source) {
					connect(tile.source.get(), &Ui::ImageSource::updated, this, [this, id] {
						for (auto &tile : _tiles) {
							if (tile.postId == id) {
								tile.prepared = QPixmap();
							}
						}
						update();
					});
				}
			}
			// The video the sound comes from leads the first page.
			if (id == _sound.originPost) {
				_tiles.insert(_tiles.begin(), std::move(tile));
			} else {
				_tiles.push_back(std::move(tile));
			}
		}
		resizeToWidth(width());
		update();
	}

	void setTail(const QString &text) {
		_tailText = text;
		resizeToWidth(width());
		update();
	}

	[[nodiscard]] int tilesCount() const {
		return int(_tiles.size());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_column = Column(newWidth);
		_left = (newWidth - _column) / 2;
		const auto &header = st::eliSoundHeaderPadding;
		const auto &grid = st::eliSoundGridPadding;
		_gridTop = header.top() + st::eliSoundCover + header.bottom();
		const auto inner = _column - grid.left() - grid.right();
		const auto skip = st::eliSoundTileSkip;
		_perRow = std::clamp((inner + skip) / (st::eliSoundTileMinWidth + skip), 2, 5);
		_tileWidth = (inner - (_perRow - 1) * skip) / _perRow;
		_tileHeight = _tileWidth * 16 / 9;
		for (auto &tile : _tiles) {
			if (tile.prepared.size() != QSize()) {
				tile.prepared = QPixmap();
			}
		}
		const auto rows = (int(_tiles.size()) + _perRow - 1) / _perRow;
		auto height = _gridTop + rows * (_tileHeight + skip);
		if (!_tailText.isEmpty()) {
			height += 120;
		}
		return height + grid.bottom();
	}

	[[nodiscard]] QRect tileRect(int index) const { // logical
		const auto skip = st::eliSoundTileSkip;
		return QRect(
			_left + st::eliSoundGridPadding.left() + (index % _perRow) * (_tileWidth + skip),
			_gridTop + (index / _perRow) * (_tileHeight + skip),
			_tileWidth,
			_tileHeight);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		PainterHighQualityEnabler hq(p);
		const auto &header = st::eliSoundHeaderPadding;
		const auto ratio = Ui::DevicePixelRatio(this);
		const auto cover = style::rtlrect(
			_left + header.left(),
			header.top(),
			st::eliSoundCover,
			st::eliSoundCover,
			width());
		if (_cover && _coverPrepared.isNull()) {
			const auto image = _cover->request(cover.size() * ratio);
			if (!image.isNull()) {
				_coverPrepared = QPixmap::fromImage(Ui::Images::Round(
					Ui::Images::PrepareCover(image, cover.size(), ratio),
					st::eliSoundCoverRadius));
				_coverPrepared.setDevicePixelRatio(ratio);
			}
		}
		if (!_coverPrepared.isNull()) {
			p.drawPixmap(cover.topLeft(), _coverPrepared);
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliAudioCoverBg);
			p.drawRoundedRect(cover, st::eliSoundCoverRadius, st::eliSoundCoverRadius);
			st::eliSoundCoverIcon.paintInCenter(p, cover);
		}
		const auto textLeft = _left + header.left() + st::eliSoundCover + 16;
		const auto available = _column - (textLeft - _left) - header.right();
		const auto title = _sound.title.isEmpty()
			? tr::lng_eli_original_sound(tr::now)
			: (_sound.artist.isEmpty()
				? _sound.title
				: (_sound.title + tr::lng_eli_meta_separator(tr::now) + _sound.artist));
		auto y = header.top() + 10;
		p.setPen(st::textPrimary);
		p.setFont(st::eliSoundTitleFont);
		p.drawTextLeft(textLeft, y, width(), st::eliSoundTitleFont->elided(title, available));
		y += st::eliSoundTitleFont->height + 6;
		p.setPen(st::textSecondary);
		p.setFont(st::eliSoundSubFont);
		p.drawTextLeft(
			textLeft,
			y,
			width(),
			st::eliSoundSubFont->elided(
				AuthorName(_controller->session(), _sound.owner),
				available));
		y += st::eliSoundSubFont->height + 4;
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			textLeft,
			y,
			width(),
			tr::lng_eli_sound_videos(tr::now, lt_n, Lang::CompactCount(_sound.uses)));

		for (auto i = 0; i != int(_tiles.size()); ++i) {
			auto &tile = _tiles[i];
			const auto rect = style::rtlrect(tileRect(i), width());
			if (!rect.intersects(e->rect())) {
				continue;
			}
			if (tile.source && tile.prepared.isNull()) {
				auto image = tile.source->request(rect.size() * ratio);
				if (image.isNull()) {
					image = tile.source->preview();
				}
				if (!image.isNull()) {
					tile.prepared = QPixmap::fromImage(Ui::Images::Round(
						Ui::Images::PrepareCover(image, rect.size(), ratio),
						st::eliSoundTileRadius));
					tile.prepared.setDevicePixelRatio(ratio);
				}
			}
			if (!tile.prepared.isNull()) {
				p.drawPixmap(rect.topLeft(), tile.prepared);
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(st::eliMediaBg);
				p.drawRoundedRect(rect, st::eliSoundTileRadius, st::eliSoundTileRadius);
			}
			if (i == _over) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::eliMediaMoreBg);
				p.setOpacity(0.4);
				p.drawRoundedRect(rect, st::eliSoundTileRadius, st::eliSoundTileRadius);
				p.setOpacity(1.);
			}
			const auto circle = QRect(
				rect.center().x() - 20,
				rect.center().y() - 20,
				40,
				40);
			p.setPen(Qt::NoPen);
			p.setBrush(st::eliMediaPlayBg);
			p.drawEllipse(circle);
			PaintPlayGlyph(p, circle, st::staticWhite->c);
			if (tile.postId == _sound.originPost) {
				const auto text = tr::lng_eli_sound_origin(tr::now);
				const auto badgeWidth = st::eliSoundOriginFont->width(text) + 14;
				const auto badge = QRect(
					style::RightToLeft() ? (rect.right() - 7 - badgeWidth) : (rect.x() + 8),
					rect.y() + 8,
					badgeWidth,
					20);
				p.setBrush(st::brandPrimary);
				p.drawRoundedRect(badge, 10, 10);
				p.setPen(st::textOnBrand);
				p.setFont(st::eliSoundOriginFont);
				p.drawTextCentered(badge, text);
			}
		}
		if (!_tailText.isEmpty()) {
			const auto rows = (int(_tiles.size()) + _perRow - 1) / _perRow;
			const auto top = _gridTop + rows * (_tileHeight + st::eliSoundTileSkip);
			p.setPen(st::textTertiary);
			p.setFont(st::normalFont);
			p.drawTextCentered(QRect(_left, top, _column, 120), _tailText);
		}
	}

	[[nodiscard]] int tileAt(QPoint point) const {
		for (auto i = 0; i != int(_tiles.size()); ++i) {
			if (style::rtlrect(tileRect(i), width()).contains(point)) {
				return i;
			}
		}
		return -1;
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = tileAt(e->pos());
		if (over != _over) {
			_over = over;
			setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	void leaveEvent(QEvent *e) override {
		RpWidget::leaveEvent(e);
		_over = -1;
		update();
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = tileAt(e->pos());
		if (e->button() == Qt::LeftButton && index >= 0) {
			OpenPardeh(_controller, _tiles[index].postId);
		}
	}

private:
	struct Tile {
		QString postId;
		std::shared_ptr<Ui::ImageSource> source;
		QPixmap prepared;
	};

	Window::SessionController *_controller = nullptr;
	Data::PostSound _sound;
	std::shared_ptr<Ui::ImageSource> _cover;
	QPixmap _coverPrepared;
	std::vector<Tile> _tiles;
	QString _tailText;
	int _column = 0;
	int _left = 0;
	int _gridTop = 0;
	int _perRow = 3;
	int _tileWidth = 0;
	int _tileHeight = 0;
	int _over = -1;

};

class SoundSection final : public SimpleSection {
public:
	SoundSection(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &soundId)
	: SimpleSection(parent, controller, tr::lng_eli_sound(tr::now))
	, _soundId(soundId) {
		_edit = _bar->addAction(st::eliSoundEdit);
		_edit->hide();
		_edit->setClickedCallback([this] { editSound(); });
		_content = _scroll->setOwnedWidget(
			Ui::object_ptr<SoundContent>(_scroll, controller));
		_scroll->setNearEdgeThreshold(600);
		connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
			loadPosts();
		});
		load();
	}

	QString sectionKey() const override {
		return Routes::Sound(_soundId);
	}

private:
	void load() {
		showSkeleton(Ui::SkeletonList::Kind::Lines);
		controller()->session().data().feed().sound(
			_soundId,
			base::Guard(this),
			[this](const Data::PostSound &sound) {
				_sound = sound;
				_content->setSound(sound);
				_edit->setVisible(sound.mine);
				showContent();
				loadPosts();
			},
			[this](const Api::Error &error) {
				auto descriptor = Ui::EmptyPlaceholderDescriptor();
				descriptor.icon = &st::eliEmptyIconMusic;
				descriptor.title = tr::lng_eli_sound_unavailable(tr::now);
				descriptor.text = error.notFound() ? QString() : Support::ErrorText(error);
				if (!error.notFound()) {
					descriptor.buttonText = tr::lng_try_again(tr::now);
					descriptor.buttonCallback = [this] {
						Ui::PostponeCall(this, [this] { load(); });
					};
				}
				showPlaceholder(std::move(descriptor));
			});
	}

	void loadPosts() {
		if (_loading || _done || _sound.id.isEmpty()) {
			return;
		}
		_loading = true;
		controller()->session().data().feed().soundPosts(
			_soundId,
			_before,
			base::Guard(this),
			[this](const Data::PostsPage &page) {
				_loading = false;
				_content->addPosts(page.posts);
				_before = page.nextBefore;
				_done = _before.isEmpty() || page.posts.isEmpty();
				_content->setTail((_content->tilesCount() == 0)
					? tr::lng_eli_no_videos_with_sound_yet(tr::now)
					: QString());
			},
			[this](const Api::Error &error) {
				_loading = false;
				_done = true;
				_content->setTail(Support::ErrorText(error, tr::lng_error_load(tr::now)));
			});
	}

	void editSound() {
		const auto controller = this->controller();
		const auto sound = _sound;
		const auto weak = QPointer<SoundSection>(this);
		controller->show(Ui::Box([=](Ui::GenericBox *box) {
			box->setTitle(tr::lng_eli_sound_edit(tr::now));
			const auto title = box->addRow(Ui::object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				tr::lng_name(tr::now),
				sound.title));
			const auto artist = box->addRow(Ui::object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				tr::lng_eli_sound_artist(tr::now),
				sound.artist));
			const auto reuse = box->addRow(Ui::object_ptr<Ui::Checkbox>(
				box,
				tr::lng_eli_sound_reuse(tr::now),
				sound.reuseAllowed));
			box->setFocusCallback([=] { title->setFocusFast(); });
			box->addButton(tr::lng_eli_save(tr::now), [=] {
				const auto newTitle = title->text().trimmed();
				const auto newArtist = artist->text().trimmed();
				const auto allowed = reuse->checked();
				controller->session().data().feed().updateSound(sound.id, QJsonObject{
					{ u"title"_s, newTitle },
					{ u"artist"_s, newArtist },
					{ u"reuse_allowed"_s, allowed },
				});
				if (weak) {
					weak->_sound.title = newTitle;
					weak->_sound.artist = newArtist;
					weak->_sound.reuseAllowed = allowed;
					weak->_content->setSound(weak->_sound);
				}
				controller->showToast(tr::lng_eli_saved_toast(tr::now));
				box->closeBox();
			}, st::defaultRoundButton);
			box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
		}));
	}

	QString _soundId;
	Data::PostSound _sound;
	Ui::IconButton *_edit = nullptr;
	SoundContent *_content = nullptr;
	QString _before;
	bool _loading = false;
	bool _done = false;

};

// ---- bookmarks

class BookmarksSection final : public SimpleSection {
public:
	BookmarksSection(QWidget *parent, Window::SessionController *controller)
	: SimpleSection(parent, controller, tr::lng_eli_bookmarks_title(tr::now)) {
		const auto content = _scroll->setOwnedWidget(
			Ui::object_ptr<Ui::VerticalLayout>(_scroll));
		_caption = content->add(Ui::object_ptr<Ui::RpWidget>(content));
		_caption->resize(_caption->width(), st::smallFont->height + 20);
		connect(_caption, &Ui::RpWidget::paintRequest, _caption, [this] {
			auto p = Painter(_caption);
			const auto column = Column(_caption->width());
			p.setPen(st::textTertiary);
			p.setFont(st::smallFont);
			p.drawTextLeft(
				(_caption->width() - column) / 2 + st::eliCaptionPadding.left(),
				st::eliCaptionPadding.top(),
				_caption->width(),
				tr::lng_eli_only_can_see_saved_posts(tr::now));
		});
		auto descriptor = ListDescriptor();
		descriptor.loader = [=](
				const QString &cursor,
				base::Guard guard,
				Fn<void(const ListPage &)> done,
				Data::FailCallback fail) {
			controller->session().data().feed().bookmarks(
				cursor,
				guard,
				[=](const Data::PostsPage &page) { done(ListPage::From(page)); },
				fail);
		};
		descriptor.empty = [] {
			auto result = Ui::EmptyPlaceholderDescriptor();
			result.icon = &st::emptyIconBookmarks;
			result.title = tr::lng_eli_havent_saved_anything(tr::now);
			return result;
		};
		_list = content->add(
			Ui::object_ptr<PostList>(content, controller, std::move(descriptor)));
		connect(_list, &PostList::scrollToRequested, this, [this](int top, int bottom) {
			_scroll->scrollToY(top + _list->y(), bottom + _list->y());
		});
		_scroll->setNearEdgeThreshold(600);
		connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
			_list->loadMore();
		});
	}

	QString sectionKey() const override {
		return Routes::Bookmarks();
	}

	void setInnerFocus() override {
		_list->setFocus();
	}

protected:
	void areaResized(const QRect &area) override {
		_list->setMinHeight(std::max(area.height() - _caption->height(), 0));
	}

	void keyPressEvent(QKeyEvent *e) override {
		if (!_list->handleKey(e)) {
			SimpleSection::keyPressEvent(e);
		}
	}

private:
	Ui::RpWidget *_caption = nullptr;
	PostList *_list = nullptr;

};

} // namespace

Window::SectionFactory TopicFactory(const QString &slug) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new TopicSection(parent, controller, slug));
	};
}

Window::SectionFactory TrendsFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new TrendsSection(parent, controller));
	};
}

Window::SectionFactory SoundFactory(const QString &soundId) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new SoundSection(parent, controller, soundId));
	};
}

Window::SectionFactory BookmarksFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new BookmarksSection(parent, controller));
	};
}

} // namespace Eli
