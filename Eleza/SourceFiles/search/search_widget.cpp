#include "search/search_widget.h"

#include "conversation/conversation_boxes.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "storage/storage_database.h"
#include "support/error_text.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/chips.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_embeds.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "window/window_shortcuts.h"
#include "styles/style_conversation.h"
#include "styles/style_search.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QTextEdit>

namespace Search {
namespace {

constexpr auto kNearBottom = 400;
constexpr auto kBrowseParts = 3;
constexpr auto kBrowseTrends = 5;
constexpr auto kBrowseVideos = 6;
constexpr auto kBrowsePeople = 5;
constexpr auto kPeopleLowWater = 3;
constexpr auto kAllPeople = 5;
constexpr auto kAllTopics = 5;
constexpr auto kAllPosts = 5;
constexpr auto kAllConversations = 3;
constexpr auto kSuggestionsPage = 20;
constexpr auto kBrowseStaleMs = qint64(5) * 60 * 1000;
constexpr auto kMaxHiddenPeople = 500;
constexpr auto kTopicInfoLimit = 12;
constexpr auto kPosterFit = QSize(480, 640);

const auto kRecentKey = u"search.recent"_s;
const auto kHiddenPeopleKey = u"search.hidden_suggestions"_s;
const auto kEmbedId = u"eli.posts"_s;

const auto kScopeAll = u"all"_s;
const auto kScopeUsers = u"users"_s;
const auto kScopeTopics = u"topics"_s;
const auto kScopePosts = u"posts"_s;
const auto kScopeConversations = u"conversations"_s;

enum RowKind {
	kHeader,
	kRecent,
	kTrend,
	kTopic,
	kVideos,
	kPerson,
	kConversation,
	kPost,
	kNote,
	kState,
	kLoader,
};

enum RowAction {
	kActNone = 0,
	kActLink,
	kActRemove,
	kActFollow,
	kActDismiss,
	kActButton,
	kActCard = 100,  // + Conversation::CardAction
	kActVideo = 200, // + cell
};

// What a header link / a state button does.
enum Command {
	kCmdNone = 0,
	kCmdClearRecent,
	kCmdAllTrends,
	kCmdScopeUsers,
	kCmdScopeTopics,
	kCmdScopePosts,
	kCmdScopeConversations,
	kCmdRetryBrowse,
	kCmdRetrySearch,
	kCmdNewConversation,
	kCmdAllPeople,
	kCmdRetryAllPeople,
};

const auto kAllPeopleRoute = u"search/people"_s;

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

// "#tag" that keeps its own direction inside a line of the other one: the
// sign stays in front of a Persian tag in the English interface too.
[[nodiscard]] QString Hashtag(const QString &tag) {
	return QChar(0x2068) + (QLatin1Char('#') + tag) + QChar(0x2069);
}

} // namespace

struct Widget::Row {
	int kind = kHeader;
	QString id; // user / slug / post / conversation id, or the recent query
	QString title;
	QString subtitle;
	QString link; // header link / state button text
	int command = kCmdNone;
	int rank = 0;
	bool rising = false;
	bool suggestion = false;
	Data::Profile profile;
	QStringList posts; // video cells
	std::unique_ptr<Conversation::Card> card;
	Ui::Text::String text; // post text, note, state text
	Ui::Text::String heading; // state title
	const style::icon *icon = nullptr;
};

struct Widget::Browse {
	std::optional<QVector<Data::Trend>> trends;
	std::optional<QStringList> videos;
	// The five rows and the line behind them.
	SuggestionQueue people = SuggestionQueue(kBrowsePeople, kPeopleLowWater);
	bool peopleLoaded = false;
	QString peopleCursor;
	bool peopleFromStart = false; // the line was refilled from page 1 again
	RefreshThrottle peopleThrottle;
	int peopleGeneration = 0;
	std::optional<Api::Error> error;
	int pending = 0;
	int generation = 0;
	qint64 loadedAt = 0;
	bool loaded = false;
	bool allTrends = false;
	bool fetchingPeople = false;
};

struct Widget::Results {
	QVector<Data::Profile> users;
	QStringList topics;
	QVector<Data::OpenConversation> conversations;
	QStringList posts;
	std::optional<Api::Error> error;
	int nextOffset = 0;
	int pending = 0;
	int generation = 0;
	bool loaded = false;
	bool loadingMore = false;
};

struct Widget::AllPeople {
	QVector<Data::Profile> people;
	QString cursor;
	std::optional<Api::Error> error;
	int generation = 0;
	bool loading = false;
	bool loaded = false;
	bool done = false;
};

struct Widget::Poster {
	Data::MediaHandlePtr handle;
	QPixmap prepared;
	QSize preparedFor;
};

Widget::Widget(
	QWidget *parent,
	Window::SessionController *controller,
	Mode mode)
: SectionWidget(parent, controller)
, _mode(mode)
, _field(new Ui::SearchField(
	this,
	tr::lng_search_search_people_topics_posts(tr::now)))
, _scopes(new Ui::ChipsRow(this, st::searchScopes))
, _scroll(new Ui::ScrollArea(this))
, _skeleton(new Ui::SkeletonList(this, Ui::SkeletonList::Kind::Chats))
, _cards(&controller->session(), [this](const QString &) {
	if (_list) {
		_list->update();
	}
})
, _userpics(&controller->session(), [this](const QString &) {
	if (_list) {
		_list->update();
	}
})
, _browse(std::make_unique<Browse>())
, _results(std::make_unique<Results>())
, _all(std::make_unique<AllPeople>())
, _scope(kScopeAll) {
	setBackground(st::searchBg);
	_inner = _scroll->setOwnedWidget(Ui::object_ptr<Ui::VerticalLayout>(_scroll));
	_list = _inner->add(Ui::object_ptr<Ui::ActionList>(
		_inner,
		static_cast<Ui::ActionListDelegate*>(this),
		st::searchList));
	_list->setSkips(0, st::searchListBottomSkip);
	_scroll->setNearEdgeThreshold(kNearBottom);
	_skeleton->setAttribute(Qt::WA_TransparentForMouseEvents);
	_skeleton->hide();

	_scopes->addChip(kScopeAll, tr::lng_all(tr::now));
	_scopes->addChip(kScopeUsers, tr::lng_search_people(tr::now));
	_scopes->addChip(kScopeTopics, tr::lng_conv_topics(tr::now));
	_scopes->addChip(kScopePosts, tr::lng_search_posts(tr::now));
	_scopes->addChip(kScopeConversations, tr::lng_search_conversations(tr::now));
	_scopes->setActive(kScopeAll, anim::type::instant);
	_scopes->hide();
	QObject::connect(_scopes, &Ui::ChipsRow::activated, this, [this](
			const QString &id) {
		setScope(id);
	});

	// ---- the field
	QObject::connect(
		_field,
		&Ui::SearchField::changed,
		&_debounce,
		&Debouncer::edited);
	QObject::connect(_field, &Ui::SearchField::submitted, this, [this] {
		if (_list->selectedRow() >= 0) {
			_list->chooseSelected();
			return;
		}
		_debounce.flush();
		rememberQuery();
	});
	QObject::connect(&_debounce, &Debouncer::fired, this, [this](
			const QString &query) {
		queryFired(query);
	});
	_field->field()->rawTextEdit()->installEventFilter(this);
	// ---- the list
	QObject::connect(_scroll, &Ui::ScrollArea::nearBottom, this, [this] {
		loadMoreResults();
	});
	QObject::connect(_scroll, &Ui::ScrollArea::scrolled, this, [this] {
		updateEmbedRange();
	});
	QObject::connect(
		_list,
		&Ui::ListWidget::rowClicked,
		this,
		[this](int index) { rowActivated(index); });
	QObject::connect(
		_list,
		&Ui::ActionList::actionClicked,
		this,
		[this](int index, int action) { actionTriggered(index, action); });
	QObject::connect(
		_list,
		&Ui::ListWidget::rowContextMenu,
		this,
		[this](int index, QPoint position) { showRowMenu(index, position); });
	QObject::connect(
		_list,
		&Ui::ListWidget::scrollToRequested,
		this,
		[this](int top, int bottom) {
			_scroll->scrollToY(_list->y() + top, _list->y() + bottom);
		});

	// ---- what changes under the rows
	QObject::connect(
		&data().users(),
		&Data::Users::changed,
		this,
		[this] {
			for (const auto &row : _rows) {
				if (row->card) {
					_cards.refreshName(*row->card);
				}
			}
			_list->update();
		});
	QObject::connect(
		&data().graph(),
		&Data::Graph::relationshipChanged,
		this,
		[this](const QString &userId, const Data::GraphState &state) {
			if (!_followBusy.contains(userId)) {
				setFollowState(userId, FollowStateOf(state), false);
			}
		});
	QObject::connect(
		&data().graph(),
		&Data::Graph::suggestionDismissed,
		this,
		[this](const QString &userId) { hidePerson(userId); });
	if (_mode == Mode::Browse) {
		// Back to the app: the suggestions may be due again.
		QObject::connect(
			qApp,
			&QGuiApplication::applicationStateChanged,
			this,
			[this](Qt::ApplicationState state) {
				if (state == Qt::ApplicationActive && isVisible()) {
					maybeRefresh();
				}
			});
	}
	QObject::connect(
		Conversation::Events(),
		&Conversation::Notifier::topicHidden,
		this,
		[this](const QString &id) {
			auto &list = _results->conversations;
			const auto was = list.size();
			list.erase(
				std::remove_if(list.begin(), list.end(), [&](
						const Data::OpenConversation &item) {
					return item.id == id;
				}),
				list.end());
			if (list.size() != was && !_query.isEmpty()) {
				rebuild();
			}
		});
	QObject::connect(
		&data().feed(),
		&Data::Feed::postRemoved,
		this,
		[this](const QString &id) {
			if (_results->posts.removeAll(id) && !_query.isEmpty()) {
				rebuild();
			}
		});
	style::OnPaletteChanged(this, [this] {
		_cards.clearUserpics();
		_userpics.clear();
		update();
	});

	// ---- what this device remembers
	if (const auto db = data().local()) {
		db->value(kRecentKey, this, [this](QJsonValue value) {
			_recent.fromJson(value);
			if (_query.isEmpty() && !_recent.empty()) {
				rebuild();
			}
		});
		db->value(kHiddenPeopleKey, this, [this](QJsonValue value) {
			for (const auto &item : value.toArray()) {
				const auto id = item.toString();
				if (!id.isEmpty() && !_hiddenPeople.contains(id)) {
					_hiddenPeople.insert(id);
					_hiddenPeopleOrder.push_back(id);
				}
			}
			auto changed = false;
			for (const auto &id : _hiddenPeople) {
				changed = _browse->people.remove(id) || changed;
			}
			const auto before = _all->people.size();
			_all->people.erase(
				std::remove_if(_all->people.begin(), _all->people.end(), [&](
						const Data::Profile &person) {
					return _hiddenPeople.contains(person.id);
				}),
				_all->people.end());
			changed = changed || (_all->people.size() != before);
			if (changed && _query.isEmpty()) {
				refillSuggestions();
				rebuild();
			}
		});
	}
	if (_mode == Mode::AllPeople) {
		_field->hide();
		_bar = new Ui::TopBar(this);
		_bar->setTitle(tr::lng_search_suggested(tr::now));
		setTopBar(_bar);
		data().graph().retryDismissals();
		loadAllPeople(true);
		return;
	}
	data().conversations().loadDismissed(this, nullptr);
	loadBrowse();
}

Widget::~Widget() = default;

Data::Session &Widget::data() const {
	return controller()->session().data();
}

QString Widget::sectionKey() const {
	return (_mode == Mode::AllPeople)
		? kAllPeopleRoute
		: Window::Routes::Search();
}

void Widget::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) {
		// Esc from the field arrives here too (not through its `cancelled`
		// signal, or the window would go back a second time): the query
		// first, then what is open in the main column.
		if (!onBackRequest()) {
			controller()->showBack();
		}
		return;
	}
	SectionWidget::keyPressEvent(e);
}

bool Widget::handleCommand(Window::Command command) {
	if (_mode == Mode::AllPeople) {
		if (command == Window::Command::Refresh) {
			loadAllPeople(true);
			return true;
		}
		return false;
	} else if (command == Window::Command::Find) {
		_field->setFocusFast();
		_field->field()->selectAll();
		return true;
	}
	return false;
}

void Widget::setInnerFocus() {
	if (_mode == Mode::AllPeople) {
		_list->setFocus();
		return;
	}
	_field->setFocusFast();
}

bool Widget::onBackRequest() {
	if (_mode == Mode::AllPeople
		|| (_field->text().isEmpty() && _query.isEmpty())) {
		return false;
	}
	_field->clear();
	_debounce.set(QString());
	return true;
}

void Widget::setQuery(const QString &query) {
	const auto trimmed = query.trimmed();
	if (_field->text().trimmed() != trimmed) {
		_field->setText(trimmed);
	}
	_debounce.set(trimmed);
	_field->setFocusFast();
}

void Widget::queryFired(const QString &query) {
	if (_query == query) {
		return;
	}
	const auto wasEmpty = _query.isEmpty();
	_query = query;
	_scroll->scrollToY(0);
	_list->clearSelection();
	if (_query.isEmpty()) {
		++_results->generation;
		resetEmbed();
		_scopes->hide();
		resizeEvent(nullptr);
		if (!_browse->loaded && !_browse->pending) {
			loadBrowse();
		}
		rebuild();
		return;
	}
	if (wasEmpty) {
		_scopes->show();
		resizeEvent(nullptr);
	}
	runSearch();
}

void Widget::setScope(const QString &scope) {
	if (_scope == scope) {
		return;
	}
	_scope = scope;
	_scopes->setActive(scope);
	_scroll->scrollToY(0);
	_list->clearSelection();
	if (!_query.isEmpty()) {
		runSearch();
	}
}

// ---- browse

void Widget::loadBrowse() {
	// Read again after a while: what is on screen stays until the new one
	// arrives (and when it fails).
	const auto refresh = _browse->loaded;
	const auto generation = ++_browse->generation;
	_browse->error = std::nullopt;
	_browse->pending = refresh ? (kBrowseParts - 1) : kBrowseParts;
	if (!refresh) {
		_browse->trends = std::nullopt;
		_browse->videos = std::nullopt;
	}
	const auto done = [=, this] {
		if (generation == _browse->generation) {
			browsePartDone();
		}
	};
	const auto failed = [=, this](const Api::Error &error) {
		if (generation == _browse->generation) {
			if (!_browse->error && !refresh) {
				_browse->error = error;
			}
			browsePartDone();
		}
	};
	data().feed().trending(this, [=, this](const QVector<Data::Trend> &trends) {
		if (generation != _browse->generation) {
			return;
		}
		auto list = QVector<Data::Trend>();
		for (const auto &trend : trends) {
			if (!trend.slug.isEmpty()) {
				list.push_back(trend);
			}
		}
		_browse->trends = list;
		done();
	}, failed);
	data().feed().loadVideoFeed(QString(), this, [=, this](
			const Data::FeedPage &page) {
		if (generation != _browse->generation) {
			return;
		}
		auto list = QStringList();
		for (const auto &item : page.items) {
			if (list.size() >= kBrowseVideos) {
				break;
			} else if (data().feed().post(item.postId)) {
				list.push_back(item.postId);
				requestPoster(item.postId);
			}
		}
		_browse->videos = list;
		done();
	}, failed);
	if (refresh) {
		// The suggestions keep their own pace.
		refreshSuggestions();
		return;
	}
	// Dismissals that did not reach the server go out again first.
	data().graph().retryDismissals();
	const auto people = ++_browse->peopleGeneration;
	_browse->fetchingPeople = true;
	data().graph().suggestions(QString(), kSuggestionsPage, this, [=, this](
			const Data::SuggestionsPage &page) {
		if (people != _browse->peopleGeneration) {
			return;
		}
		auto &browse = *_browse;
		browse.fetchingPeople = false;
		browse.people.replace(page.people, _hiddenPeople, data().userId());
		browse.peopleCursor = page.nextCursor;
		browse.peopleFromStart = false;
		browse.peopleLoaded = true;
		browse.peopleThrottle.loaded(NowMs());
		done();
	}, [=, this](const Api::Error &error) {
		if (people == _browse->peopleGeneration) {
			_browse->fetchingPeople = false;
			failed(error);
		}
	});
	if (_query.isEmpty()) {
		rebuild();
	}
}

void Widget::browsePartDone() {
	if (--_browse->pending > 0) {
		return;
	}
	_browse->loaded = true;
	_browse->loadedAt = NowMs();
	refillSuggestions();
	if (_query.isEmpty()) {
		rebuild();
	}
}

// Shown again (the tab, the app back in front): what is stale is read again.
void Widget::maybeRefresh() {
	if (_mode != Mode::Browse || !_query.isEmpty() || !_browse->loaded) {
		return;
	} else if (_browse->pending <= 0
		&& (NowMs() - _browse->loadedAt > kBrowseStaleMs)) {
		loadBrowse();
	} else {
		refreshSuggestions();
	}
}

// A fresh first page of suggestions, at most every ten minutes after the
// last one that arrived: swapped in place.
void Widget::refreshSuggestions() {
	auto &browse = *_browse;
	if (!browse.peopleThrottle.due(NowMs())) {
		return;
	}
	const auto generation = ++browse.peopleGeneration;
	browse.fetchingPeople = true;
	data().graph().suggestions(QString(), kSuggestionsPage, this, [=, this](
			const Data::SuggestionsPage &page) {
		if (generation != _browse->peopleGeneration) {
			return;
		}
		auto &browse = *_browse;
		browse.fetchingPeople = false;
		browse.people.replace(page.people, _hiddenPeople, data().userId());
		browse.peopleCursor = page.nextCursor;
		browse.peopleFromStart = false;
		browse.peopleLoaded = true;
		browse.peopleThrottle.loaded(NowMs());
		refillSuggestions();
		if (_query.isEmpty()) {
			rebuild();
		}
	}, [=, this](const Api::Error &) {
		// Still due: the next showing tries again.
		if (generation == _browse->peopleGeneration) {
			_browse->fetchingPeople = false;
		}
	});
}

// Few left waiting behind the rows: the next page, or page 1 again once
// the server's list ended (it ends after `feed.suggest_max_total` people).
void Widget::refillSuggestions() {
	auto &browse = *_browse;
	if (_mode != Mode::Browse
		|| !browse.peopleLoaded
		|| browse.fetchingPeople
		|| !browse.people.needsRefill()) {
		return;
	}
	const auto cursor = browse.peopleCursor;
	if (cursor.isEmpty()) {
		if (browse.peopleFromStart) {
			return;
		}
		browse.peopleFromStart = true;
	}
	browse.fetchingPeople = true;
	const auto generation = browse.peopleGeneration;
	data().graph().suggestions(cursor, kSuggestionsPage, this, [=, this](
			const Data::SuggestionsPage &page) {
		if (generation != _browse->peopleGeneration) {
			return;
		}
		auto &browse = *_browse;
		browse.fetchingPeople = false;
		const auto added = browse.people.refill(
			page.people,
			_hiddenPeople,
			data().userId());
		browse.peopleCursor = page.people.isEmpty()
			? QString()
			: page.nextCursor;
		if (added && _query.isEmpty()) {
			rebuild();
		}
		refillSuggestions();
	}, [=, this](const Api::Error &) {
		if (generation == _browse->peopleGeneration) {
			_browse->fetchingPeople = false;
			_browse->peopleCursor = QString();
			_browse->peopleFromStart = true;
		}
	});
}

// A followed suggestion gives its place a moment after the click.
void Widget::leaveSuggestions(const QString &userId) {
	auto &browse = *_browse;
	const auto person = browse.people.find(userId);
	if (!person || !LeavesSuggestions(FollowStateOf(*person))) {
		return;
	}
	browse.people.remove(userId);
	refillSuggestions();
	if (_query.isEmpty()) {
		rebuild();
	}
}

// ---- «دیدن همه»

void Widget::loadAllPeople(bool reset) {
	auto &all = *_all;
	if (reset) {
		++all.generation;
		all.loading = false;
		all.done = false;
		all.cursor = QString();
		all.error = std::nullopt;
	}
	if (all.loading || all.done) {
		return;
	}
	all.loading = true;
	const auto generation = all.generation;
	const auto first = all.cursor.isEmpty();
	data().graph().suggestions(all.cursor, kSuggestionsPage, this, [=, this](
			const Data::SuggestionsPage &page) {
		if (generation != _all->generation) {
			return;
		}
		auto &all = *_all;
		all.loading = false;
		all.loaded = true;
		all.error = std::nullopt;
		if (first) {
			// (a reload: the people on screen stayed until now)
			all.people.clear();
		}
		const auto me = data().userId();
		auto added = 0;
		for (const auto &person : page.people) {
			const auto known = std::any_of(
				all.people.cbegin(),
				all.people.cend(),
				[&](const Data::Profile &other) { return other.id == person.id; });
			if (!known
				&& !person.me
				&& person.id != me
				&& !_hiddenPeople.contains(person.id)) {
				all.people.push_back(person);
				++added;
			}
		}
		all.cursor = page.nextCursor;
		all.done = all.cursor.isEmpty() || page.people.isEmpty();
		rebuild();
		// A page with nobody new (hidden here): go on.
		if (!added && !all.done) {
			loadAllPeople(false);
		}
	}, [=, this](const Api::Error &error) {
		if (generation != _all->generation) {
			return;
		}
		_all->loading = false;
		_all->error = error;
		rebuild();
	});
	rebuild();
}

void Widget::buildAll() {
	const auto &all = *_all;
	const auto retry = [&](const Api::Error &error) {
		addState(
			&st::searchStateErrorIcon,
			tr::lng_error_load(tr::now),
			Support::ErrorText(error),
			tr::lng_retry(tr::now),
			kCmdRetryAllPeople);
	};
	if (!all.loaded) {
		if (all.error && !all.loading) {
			retry(*all.error);
		}
		return;
	} else if (all.people.isEmpty() && all.done) {
		addState(
			&st::searchStateDiscoverIcon,
			tr::lng_search_no_suggestions(tr::now),
			QString());
		return;
	}
	for (const auto &person : all.people) {
		addPerson(person, true);
	}
	if (all.loading) {
		addRow(kLoader);
	} else if (all.error) {
		retry(*all.error);
	}
}

// ---- results

void Widget::runSearch() {
	auto &results = *_results;
	const auto generation = ++results.generation;
	results = Results();
	results.generation = generation;
	resetEmbed();
	const auto query = _query;
	const auto embedded = Window::HasEmbed(kEmbedId);
	auto types = QStringList();
	if (_scope == kScopeAll) {
		types = QStringList{ kScopeUsers, kScopeTopics, kScopeConversations };
		if (!embedded) {
			types.push_back(kScopePosts);
		}
	} else if (_scope != kScopePosts || !embedded) {
		types.push_back(_scope);
	}
	results.pending = int(types.size());
	if (types.isEmpty()) {
		// The posts scope is the embedded list alone.
		results.loaded = true;
	}
	const auto single = (_scope != kScopeAll);
	for (const auto &type : types) {
		data().graph().search(query, type, 0, this, [=, this](
				const Data::SearchResults &page) {
			if (generation != _results->generation) {
				return;
			}
			if (type == kScopeUsers) {
				_results->users = page.users;
			} else if (type == kScopeTopics) {
				_results->topics = page.topics;
			} else if (type == kScopePosts) {
				_results->posts = page.posts;
			} else if (type == kScopeConversations) {
				_results->conversations = page.conversations;
			}
			if (single) {
				_results->nextOffset = page.nextOffset;
			}
			searchPartDone(generation);
		}, [=, this](const Api::Error &error) {
			if (generation != _results->generation) {
				return;
			}
			if (!_results->error) {
				_results->error = error;
			}
			searchPartDone(generation);
		});
	}
	rebuild();
}

void Widget::searchPartDone(int generation) {
	if (generation != _results->generation || --_results->pending > 0) {
		return;
	}
	_results->loaded = true;
	rebuild();
}

void Widget::loadMoreResults() {
	if (_mode == Mode::AllPeople) {
		loadAllPeople(false);
		return;
	} else if (_query.isEmpty()) {
		return;
	}
	if (_embed) {
		_embed->loadMore();
	}
	auto &results = *_results;
	const auto paged = (_scope == kScopeUsers)
		|| (_scope == kScopePosts && !_embed)
		|| (_scope == kScopeTopics)
		|| (_scope == kScopeConversations);
	if (!paged
		|| !results.loaded
		|| results.loadingMore
		|| results.nextOffset <= 0
		|| results.error) {
		return;
	}
	results.loadingMore = true;
	const auto generation = results.generation;
	const auto scope = _scope;
	data().graph().search(_query, scope, results.nextOffset, this, [=, this](
			const Data::SearchResults &page) {
		if (generation != _results->generation) {
			return;
		}
		auto &results = *_results;
		results.loadingMore = false;
		auto added = false;
		if (scope == kScopeUsers) {
			for (const auto &user : page.users) {
				const auto known = std::any_of(
					results.users.cbegin(),
					results.users.cend(),
					[&](const Data::Profile &old) { return old.id == user.id; });
				if (!known) {
					results.users.push_back(user);
					added = true;
				}
			}
		} else if (scope == kScopePosts) {
			for (const auto &id : page.posts) {
				if (!results.posts.contains(id)) {
					results.posts.push_back(id);
					added = true;
				}
			}
		} else if (scope == kScopeTopics) {
			for (const auto &slug : page.topics) {
				if (!results.topics.contains(slug)) {
					results.topics.push_back(slug);
					added = true;
				}
			}
		} else {
			for (const auto &item : page.conversations) {
				const auto known = std::any_of(
					results.conversations.cbegin(),
					results.conversations.cend(),
					[&](const Data::OpenConversation &old) {
						return old.id == item.id;
					});
				if (!known) {
					results.conversations.push_back(item);
					added = true;
				}
			}
		}
		// A page that brought nothing new ends the list.
		results.nextOffset = added ? page.nextOffset : 0;
		rebuild();
	}, [=, this](const Api::Error &error) {
		if (generation != _results->generation) {
			return;
		}
		_results->loadingMore = false;
		_results->nextOffset = 0;
		Support::ShowError(controller(), error);
		rebuild();
	});
	rebuild();
}

// ---- the embedded posts list

void Widget::resetEmbed() {
	if (_embed) {
		_inner->removeChild(_embed);
		_embed = nullptr;
	}
	_embedQuery = QString();
	_embedEmpty = false;
}

void Widget::ensureEmbed() {
	if (_embed && _embedQuery == _query) {
		return;
	}
	resetEmbed();
	auto embed = Window::CreateEmbed(kEmbedId, _inner, controller(), {
		{ u"source"_s, u"search"_s },
		{ u"query"_s, _query },
	});
	if (!embed) {
		return;
	}
	_embedQuery = _query;
	_embed = _inner->add(std::move(embed));
	const auto raw = _embed;
	QObject::connect(raw, &Window::EmbeddedList::heightChanged, this, [=, this] {
		if (raw == _embed) {
			raw->resizeToWidth(_inner->width());
			updateEmbedRange();
		}
	});
	QObject::connect(raw, &Window::EmbeddedList::emptyChanged, this, [=, this](
			bool empty) {
		if (raw == _embed && _embedEmpty != empty) {
			_embedEmpty = empty;
			raw->setVisible(!empty);
			rebuild();
		}
	});
	updateEmbedRange();
}

void Widget::updateEmbedRange() {
	if (!_embed || _embed->isHidden()) {
		return;
	}
	const auto top = _scroll->scrollTop() - _embed->y();
	_embed->setVisibleRange(
		std::clamp(top, 0, _embed->height()),
		std::clamp(top + _scroll->height(), 0, _embed->height()));
}

// ---- building the rows

Widget::Row &Widget::addRow(int kind) {
	_rows.push_back(std::make_unique<Row>());
	_rows.back()->kind = kind;
	return *_rows.back();
}

void Widget::addHeader(const QString &title, const QString &link, int action) {
	auto &row = addRow(kHeader);
	row.title = title;
	row.link = link;
	row.command = action;
}

void Widget::addPerson(const Data::Profile &profile, bool suggestion) {
	auto &row = addRow(kPerson);
	row.id = profile.id;
	row.profile = profile;
	row.suggestion = suggestion;
	row.title = profile.deleted
		? tr::lng_deleted_account(tr::now)
		: profile.displayName;
	row.subtitle = suggestion
		? SuggestionReason(profile)
		: HandleText(profile.handle);
	if (!suggestion && !profile.bio.trimmed().isEmpty()) {
		// The person's own words keep their own direction in the line.
		row.link = QChar(0x2068) + profile.bio.simplified() + QChar(0x2069);
	}
}

void Widget::addState(
		const style::icon *icon,
		const QString &title,
		const QString &text,
		const QString &button,
		int action) {
	auto &row = addRow(kState);
	row.icon = icon;
	row.heading.setText(st::searchStateTitleStyle, title);
	if (!text.isEmpty()) {
		row.text.setText(st::searchStateTextStyle, text);
	}
	row.link = button;
	row.command = action;
}

void Widget::rebuild() {
	_rows.clear();
	if (_mode == Mode::AllPeople) {
		buildAll();
	} else if (_query.isEmpty()) {
		buildBrowse();
	} else {
		buildResults();
	}
	refreshList();
}

void Widget::refreshList() {
	_list->refresh();
	updateSkeleton();
	const auto loader = std::any_of(_rows.cbegin(), _rows.cend(), [](
			const std::unique_ptr<Row> &row) {
		return row->kind == kLoader;
	});
	if (loader) {
		if (!_loader) {
			_loader = std::make_unique<Ui::InfiniteRadialAnimation>([this] {
				if (!_rows.empty() && _rows.back()->kind == kLoader) {
					_list->updateRow(int(_rows.size()) - 1);
				}
			});
		}
		_loader->start();
	} else if (_loader) {
		_loader->stop();
	}
	updateEmbedRange();
}

void Widget::updateSkeleton() {
	const auto loading = (_mode == Mode::AllPeople)
		? (!_all->loaded && !_all->error)
		: _query.isEmpty()
		? (!_browse->loaded && _recent.empty())
		: (!_results->loaded && !_embed);
	_skeleton->setVisible(loading && _rows.empty());
}

void Widget::buildBrowse() {
	auto &browse = *_browse;
	if (!_recent.empty()) {
		addHeader(
			tr::lng_search_recent(tr::now),
			tr::lng_search_recent_clear(tr::now),
			kCmdClearRecent);
		for (const auto &query : _recent.list()) {
			auto &row = addRow(kRecent);
			row.id = query;
			row.title = query;
		}
	}
	if (!browse.loaded) {
		return;
	}
	if (!browse.trends && !browse.videos && !browse.peopleLoaded) {
		addState(
			&st::searchStateErrorIcon,
			tr::lng_error_load(tr::now),
			browse.error ? Support::ErrorText(*browse.error) : QString(),
			tr::lng_retry(tr::now),
			kCmdRetryBrowse);
		return;
	}
	auto any = false;
	if (browse.trends) {
		any = true;
		const auto &trends = *browse.trends;
		const auto more = (trends.size() > kBrowseTrends) && !browse.allTrends;
		addHeader(
			tr::lng_search_hot_hashtags(tr::now),
			more ? tr::lng_all(tr::now) : QString(),
			more ? kCmdAllTrends : kCmdNone);
		if (trends.isEmpty()) {
			auto &row = addRow(kNote);
			row.text.setText(
				st::searchNoteStyle,
				tr::lng_search_nothing_trending_yet(tr::now));
		}
		const auto count = browse.allTrends
			? int(trends.size())
			: std::min(int(trends.size()), kBrowseTrends);
		for (auto i = 0; i != count; ++i) {
			const auto &trend = trends[i];
			auto &row = addRow(kTrend);
			row.id = trend.slug;
			row.title = Hashtag(trend.title.isEmpty() ? trend.slug : trend.title);
			row.subtitle = tr::lng_search_trend_counts(
				tr::now,
				lt_posts,
				Lang::CompactCount(trend.posts),
				lt_people,
				Lang::CompactCount(trend.authors),
				lt_open,
				Lang::Num(trend.openConversations));
			row.rank = i + 1;
			row.rising = trend.rising;
			row.link = trend.id;
		}
	}
	if (browse.videos && !browse.videos->isEmpty()) {
		any = true;
		addHeader(tr::lng_search_new_videos(tr::now));
		const auto &videos = *browse.videos;
		const auto columns = int(st::searchVideoColumns);
		for (auto i = 0; i < int(videos.size()); i += columns) {
			auto &row = addRow(kVideos);
			row.posts = videos.mid(i, columns);
		}
	}
	if (const auto people = browse.people.visible(); !people.isEmpty()) {
		any = true;
		addHeader(
			tr::lng_search_people_may_know(tr::now),
			tr::lng_search_see_all(tr::now),
			kCmdAllPeople);
		for (const auto &person : people) {
			addPerson(person, true);
		}
	}
	if (!any) {
		addState(
			&st::searchStateDiscoverIcon,
			tr::lng_search_nothing_trending_yet(tr::now),
			QString());
	}
}

void Widget::buildResults() {
	auto &results = *_results;
	const auto all = (_scope == kScopeAll);
	const auto embedded = Window::HasEmbed(kEmbedId)
		&& (all || _scope == kScopePosts);
	if (!results.loaded) {
		return;
	} else if (results.error) {
		resetEmbed();
		addState(
			&st::searchStateErrorIcon,
			tr::lng_search_search_failed(tr::now),
			Support::ErrorText(*results.error),
			tr::lng_retry(tr::now),
			kCmdRetrySearch);
		return;
	}
	if (embedded) {
		ensureEmbed();
	}
	const auto &dismissed = data().conversations();
	auto conversations = QVector<Data::OpenConversation>();
	for (const auto &item : results.conversations) {
		if (!dismissed.isDismissed(item.id)
			&& item.expiresAt * 1000 > NowMs()) {
			conversations.push_back(item);
		}
	}
	const auto embedHasRows = _embed && !_embedEmpty;
	const auto any = !results.users.isEmpty()
		|| !results.topics.isEmpty()
		|| !conversations.isEmpty()
		|| !results.posts.isEmpty()
		|| embedHasRows;
	if (!any) {
		if (_scope == kScopeTopics) {
			addState(
				&st::searchStateSearchIcon,
				tr::lng_search_no_topic(tr::now, lt_query, _query),
				QString());
		} else if (_scope == kScopeConversations) {
			addState(
				&st::searchStateConversationIcon,
				tr::lng_search_no_conversation(tr::now, lt_query, _query),
				QString(),
				tr::lng_search_be_first(tr::now),
				kCmdNewConversation);
		} else {
			addState(
				&st::searchStateSearchIcon,
				tr::lng_nothing_found(tr::now),
				tr::lng_search_nothing_for(tr::now, lt_query, _query),
				tr::lng_search_start_about_this(tr::now),
				kCmdNewConversation);
		}
		return;
	}
	if (!results.users.isEmpty()) {
		const auto limited = all && (results.users.size() > kAllPeople);
		addHeader(
			tr::lng_search_people(tr::now),
			limited ? tr::lng_all(tr::now) : QString(),
			limited ? kCmdScopeUsers : kCmdNone);
		const auto count = limited ? kAllPeople : int(results.users.size());
		for (auto i = 0; i != count; ++i) {
			addPerson(results.users[i], false);
		}
	}
	if (!results.topics.isEmpty()) {
		const auto limited = all && (results.topics.size() > kAllTopics);
		addHeader(
			tr::lng_conv_topics(tr::now),
			limited ? tr::lng_all(tr::now) : QString(),
			limited ? kCmdScopeTopics : kCmdNone);
		const auto count = limited ? kAllTopics : int(results.topics.size());
		auto wanted = QStringList();
		for (auto i = 0; i != count; ++i) {
			const auto &slug = results.topics[i];
			auto &row = addRow(kTopic);
			row.id = slug;
			row.title = Hashtag(slug);
			if (const auto info = _topicInfo.constFind(slug);
				info != _topicInfo.cend()) {
				row.subtitle = tr::lng_search_trend_counts(
					tr::now,
					lt_posts,
					Lang::CompactCount(info->postsToday),
					lt_people,
					Lang::CompactCount(info->peopleToday),
					lt_open,
					Lang::Num(info->openConversations));
			} else {
				wanted.push_back(slug);
			}
		}
		requestTopicInfo(wanted);
	}
	if (!conversations.isEmpty()) {
		const auto limited = all && (conversations.size() > kAllConversations);
		addHeader(
			tr::lng_eli_open_conversations(tr::now),
			limited ? tr::lng_all(tr::now) : QString(),
			limited ? kCmdScopeConversations : kCmdNone);
		const auto count = limited
			? kAllConversations
			: int(conversations.size());
		for (auto i = 0; i != count; ++i) {
			const auto &item = conversations[i];
			auto &row = addRow(kConversation);
			row.id = item.id;
			// Search never names the speaker: the result's mask only.
			row.card = std::make_unique<Conversation::Card>(
				_cards.make(item, true));
		}
	}
	if (embedHasRows) {
		addHeader(tr::lng_search_posts(tr::now));
	} else if (!results.posts.isEmpty()) {
		const auto limited = all && (results.posts.size() > kAllPosts);
		addHeader(
			tr::lng_search_posts(tr::now),
			limited ? tr::lng_all(tr::now) : QString(),
			limited ? kCmdScopePosts : kCmdNone);
		const auto count = limited ? kAllPosts : int(results.posts.size());
		for (auto i = 0; i != count; ++i) {
			const auto post = data().feed().post(results.posts[i]);
			if (!post || post->deleted) {
				continue;
			}
			auto &row = addRow(kPost);
			row.id = post->id;
			row.profile.id = post->author;
			row.text.setText(
				st::searchPostTextStyle,
				post->text.trimmed().isEmpty()
					? tr::lng_profile_media(tr::now)
					: post->text.trimmed());
			row.subtitle = Lang::RelativeTime(
				QDateTime::fromMSecsSinceEpoch(post->createdAt));
			data().users().request(post->author);
		}
	}
	if (results.loadingMore) {
		addRow(kLoader);
	}
}

void Widget::requestTopicInfo(const QStringList &slugs) {
	for (const auto &slug : slugs) {
		if (_topicInfoRequested.contains(slug)
			|| _topicInfoRequested.size() >= kTopicInfoLimit * 8) {
			continue;
		}
		_topicInfoRequested.insert(slug);
		data().feed().topic(slug, this, [=, this](const Data::TopicInfo &info) {
			_topicInfo.insert(slug, info);
			for (auto i = 0; i != int(_rows.size()); ++i) {
				auto &row = *_rows[i];
				if (row.kind == kTopic && row.id == slug) {
					row.subtitle = tr::lng_search_trend_counts(
						tr::now,
						lt_posts,
						Lang::CompactCount(info.postsToday),
						lt_people,
						Lang::CompactCount(info.peopleToday),
						lt_open,
						Lang::Num(info.openConversations));
					_list->updateRow(i);
				}
			}
		}, [](const Api::Error &) {
			// The row stays without counts.
		});
	}
}

void Widget::requestPoster(const QString &postId) {
	if (_posters.contains(postId)) {
		return;
	}
	const auto post = data().feed().post(postId);
	if (!post || post->media.isEmpty()) {
		return;
	}
	auto media = post->media.front();
	for (const auto &item : post->media) {
		if (item.kind == u"video") {
			media = item;
			break;
		}
	}
	auto options = Data::Media::Options();
	options.fit = kPosterFit;
	auto poster = Poster();
	poster.handle = data().media().request(
		Data::MediaRef::PostMedia(postId, media.id),
		(media.kind == u"image")
			? Data::MediaVariant::Feed
			: Data::MediaVariant::Poster,
		options);
	QObject::connect(
		poster.handle.get(),
		&Data::MediaHandle::ready,
		this,
		[this] { _list->update(); });
	_posters.insert(postId, poster);
}

// ---- geometry of the rows (logical, leading-side coordinates)

QRect Widget::headerLinkRect(const Row &row, int width) const {
	if (row.link.isEmpty()) {
		return QRect();
	}
	const auto textWidth = st::searchHeadLinkFont->width(row.link);
	const auto padding = int(st::searchPadding);
	return QRect(
		width - padding - textWidth - 6,
		int(st::searchHeadTop) - 4,
		textWidth + 12,
		st::searchHeadLinkFont->height + 8);
}

QRect Widget::dismissRect(const Row &row, int width) const {
	const auto size = int(st::searchRemoveSize);
	const auto rowHeightValue = (row.kind == kRecent)
		? int(st::searchRecentHeight)
		: (row.link.isEmpty()
			? int(st::searchPersonHeight)
			: int(st::searchPersonBioHeight));
	if (row.kind == kPerson && !row.suggestion) {
		return QRect();
	}
	return QRect(
		width - int(st::searchPadding) / 2 - size,
		(rowHeightValue - size) / 2,
		size,
		size);
}

QRect Widget::followRect(const Row &row, int width) const {
	if (row.kind != kPerson
		|| row.profile.me
		|| row.profile.deleted
		|| row.profile.id == data().userId()) {
		return QRect();
	}
	const auto text = FollowText(FollowStateOf(row.profile));
	const auto buttonWidth = st::searchFollowFont->width(text)
		+ 2 * int(st::searchFollowPadding);
	const auto height = int(st::searchFollowHeight);
	const auto rowHeightValue = row.link.isEmpty()
		? int(st::searchPersonHeight)
		: int(st::searchPersonBioHeight);
	const auto dismiss = dismissRect(row, width);
	const auto right = dismiss.isEmpty()
		? (width - int(st::searchPadding))
		: (dismiss.x() - 2);
	return QRect(
		right - buttonWidth,
		(std::min(rowHeightValue, int(st::searchPersonHeight)) - height) / 2,
		buttonWidth,
		height);
}

QRect Widget::videoRect(int cell, int width) const {
	const auto padding = int(st::searchPadding);
	const auto skip = int(st::searchVideoSkip);
	const auto columns = int(st::searchVideoColumns);
	const auto cellWidth = std::max(
		(width - 2 * padding - (columns - 1) * skip) / columns,
		1);
	return QRect(
		padding + cell * (cellWidth + skip),
		0,
		cellWidth,
		cellWidth * 4 / 3);
}

int Widget::stateHeight(const Row &row, int width) const {
	const auto &padding = st::searchStatePadding;
	const auto available = std::max(
		std::min(
			width - padding.left() - padding.right(),
			int(st::searchStateMaxWidth)),
		1);
	auto result = padding.top();
	if (row.icon) {
		result += row.icon->height() + int(st::searchStateIconSkip);
	}
	result += row.heading.countHeight(available);
	if (!row.text.isEmpty()) {
		result += int(st::searchStateTextSkip) + row.text.countHeight(available);
	}
	if (!row.link.isEmpty()) {
		result += int(st::searchStateButtonSkip)
			+ int(st::searchStateButtonHeight);
	}
	return result + padding.bottom();
}

QRect Widget::stateButtonRect(const Row &row, int width) const {
	if (row.link.isEmpty()) {
		return QRect();
	}
	const auto buttonWidth = st::searchStateButtonFont->width(row.link)
		+ 2 * int(st::searchStateButtonPadding);
	const auto height = int(st::searchStateButtonHeight);
	return QRect(
		(width - buttonWidth) / 2,
		stateHeight(row, width) - st::searchStatePadding.bottom() - height,
		buttonWidth,
		height);
}

// ---- Ui::ActionListDelegate

int Widget::rowsCount() const {
	return int(_rows.size());
}

int Widget::rowHeight(int index, int width) const {
	const auto &row = *_rows[index];
	switch (row.kind) {
	case kHeader: return st::searchHeadHeight;
	case kRecent: return st::searchRecentHeight;
	case kTrend: return st::searchTrendHeight;
	case kTopic: return st::searchTopicHeight;
	case kVideos:
		return videoRect(0, width).height() + int(st::searchVideoSkip);
	case kPerson:
		return row.link.isEmpty()
			? int(st::searchPersonHeight)
			: int(st::searchPersonBioHeight);
	case kConversation: return _cards.height(*row.card, width);
	case kPost: {
		const auto &padding = st::searchPostPadding;
		const auto left = padding.left()
			+ int(st::searchPostUserpic)
			+ int(st::searchPostSkip);
		const auto available = std::max(width - left - padding.right(), 1);
		return padding.top()
			+ st::searchPostAuthorFont->height
			+ int(st::searchPostTextTop)
			+ row.text.countHeight(available, st::searchPostLines)
			+ padding.bottom();
	}
	case kNote: {
		const auto &padding = st::searchNotePadding;
		return padding.top()
			+ row.text.countHeight(
				std::max(width - padding.left() - padding.right(), 1))
			+ padding.bottom();
	}
	case kState: return stateHeight(row, width);
	case kLoader: return st::searchLoaderHeight;
	}
	return 0;
}

bool Widget::rowClickable(int index) const {
	switch (_rows[index]->kind) {
	case kRecent:
	case kTrend:
	case kTopic:
	case kPerson:
	case kConversation:
	case kPost:
		return true;
	}
	return false;
}

int Widget::rowActionAt(int index, QPoint point, int width) const {
	const auto &row = *_rows[index];
	const auto hit = [&](const QRect &logical) {
		return !logical.isEmpty()
			&& style::rtlrect(logical, width).contains(point);
	};
	switch (row.kind) {
	case kHeader:
		return hit(headerLinkRect(row, width)) ? kActLink : kActNone;
	case kRecent:
		return hit(dismissRect(row, width)) ? kActRemove : kActNone;
	case kPerson:
		if (hit(followRect(row, width))) {
			return kActFollow;
		} else if (hit(dismissRect(row, width))) {
			return kActDismiss;
		}
		return kActNone;
	case kVideos:
		for (auto i = 0; i != int(row.posts.size()); ++i) {
			if (hit(videoRect(i, width))) {
				return kActVideo + i;
			}
		}
		return kActNone;
	case kConversation: {
		const auto action = _cards.actionAt(*row.card, point, width);
		return action ? (kActCard + action) : kActNone;
	}
	case kState:
		return hit(stateButtonRect(row, width)) ? kActButton : kActNone;
	}
	return kActNone;
}

void Widget::paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) {
	const auto &row = *_rows[index];
	const auto width = rect.width();
	if (row.kind == kConversation) {
		auto cardState = Conversation::CardState();
		cardState.over = state.over;
		const auto over = _list->overAction(index);
		const auto pressed = _list->pressedAction(index);
		cardState.overAction = (over > kActCard) ? (over - kActCard) : 0;
		cardState.pressedAction = (pressed > kActCard)
			? (pressed - kActCard)
			: 0;
		_cards.paint(p, *row.card, rect, cardState, NowMs());
		return;
	}
	p.translate(rect.topLeft());
	switch (row.kind) {
	case kHeader: paintHeader(p, row, width, index); break;
	case kRecent: paintRecent(p, row, width, index); break;
	case kTrend:
	case kTopic: paintTrend(p, row, width); break;
	case kVideos: paintVideos(p, row, width, index); break;
	case kPerson: paintPerson(p, row, width, index); break;
	case kPost: paintPost(p, row, width, rect.height()); break;
	case kNote: {
		const auto &padding = st::searchNotePadding;
		p.setPen(st::searchNoteFg);
		row.text.drawLeft(
			p,
			padding.left(),
			padding.top(),
			std::max(width - padding.left() - padding.right(), 1),
			width);
	} break;
	case kState: paintState(p, row, width, index); break;
	case kLoader:
		if (_loader) {
			const auto size = int(st::searchLoaderSize);
			_loader->draw(
				p,
				QRect(
					(width - size) / 2,
					(rect.height() - size) / 2,
					size,
					size),
				2.5,
				st::brandPrimary->c);
		}
		break;
	}
}

void Widget::paintHeader(Painter &p, const Row &row, int width, int index) {
	const auto padding = int(st::searchPadding);
	const auto link = headerLinkRect(row, width);
	p.setFont(st::searchHeadFont);
	p.setPen(st::searchHeadFg);
	p.drawTextLeft(
		padding,
		st::searchHeadTop,
		width,
		st::searchHeadFont->elided(
			row.title,
			std::max(width - 2 * padding - link.width(), 0)));
	if (!link.isEmpty()) {
		const auto over = (_list->overAction(index) == kActLink);
		p.setFont(over ? st::searchHeadLinkFontOver : st::searchHeadLinkFont);
		p.setPen(st::searchHeadLinkFg);
		p.drawTextLeft(
			link.x() + 6,
			link.y() + 4 + (st::searchHeadFont->height
				- st::searchHeadLinkFont->height) / 2,
			width,
			row.link);
	}
}

void Widget::paintRecent(Painter &p, const Row &row, int width, int index) {
	const auto padding = int(st::searchPadding);
	const auto height = int(st::searchRecentHeight);
	const auto &icon = st::searchRecentIcon;
	icon.paint(p, padding, (height - icon.height()) / 2, width);
	const auto remove = dismissRect(row, width);
	const auto left = padding + icon.width() + int(st::searchRecentSkip);
	p.setFont(st::searchRecentFont);
	p.setPen(st::searchRecentFg);
	p.drawTextLeft(
		left,
		(height - st::searchRecentFont->height) / 2,
		width,
		st::searchRecentFont->elided(
			row.title,
			std::max(remove.x() - left - 4, 0)));
	const auto over = (_list->overAction(index) == kActRemove);
	const auto physical = style::rtlrect(remove, width);
	if (over) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::searchRemoveBgOver);
		p.drawEllipse(physical);
	}
	(over ? st::searchRemoveIconOver : st::searchRemoveIcon).paintInCenter(
		p,
		physical);
}

void Widget::paintTrend(Painter &p, const Row &row, int width) {
	const auto padding = int(st::searchPadding);
	const auto trend = (row.kind == kTrend);
	const auto height = trend
		? int(st::searchTrendHeight)
		: int(st::searchTopicHeight);
	auto left = padding;
	if (trend) {
		const auto rank = Lang::Num(row.rank);
		p.setFont(st::searchTrendRankFont);
		p.setPen(st::searchTrendRankFg);
		const auto rankWidth = st::searchTrendRankFont->width(rank);
		p.drawTextLeft(
			left + (int(st::searchTrendRankWidth) - rankWidth) / 2,
			(height - st::searchTrendRankFont->height) / 2,
			width,
			rank,
			rankWidth);
		left += int(st::searchTrendRankWidth) + int(st::searchTrendSkip);
	} else {
		const auto size = int(st::searchTopicIconSize);
		const auto circle = style::rtlrect(
			left,
			(height - size) / 2,
			size,
			size,
			width);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::searchTopicIconBg);
			p.drawEllipse(circle);
		}
		st::searchTopicIcon.paintInCenter(p, circle);
		left += size + int(st::searchTrendSkip);
	}
	const auto &chevron = st::searchTrendChevron;
	chevron.paint(
		p,
		width - padding - chevron.width(),
		(height - chevron.height()) / 2,
		width);
	auto available = width - left - padding - chevron.width() - 8;
	const auto single = row.subtitle.isEmpty();
	const auto titleTop = single
		? ((height - st::searchTrendTitleFont->height) / 2)
		: (trend
			? int(st::searchTrendTitleTop)
			: (int(st::searchTrendTitleTop) - 2));
	auto risingWidth = 0;
	const auto rising = row.rising
		? tr::lng_search_trend_rising(tr::now)
		: QString();
	if (row.rising) {
		risingWidth = st::searchTrendRisingFont->width(rising)
			+ 2 * int(st::searchTrendRisingPadding);
	}
	const auto title = st::searchTrendTitleFont->elided(
		row.title,
		std::max(available - (risingWidth ? (risingWidth + 8) : 0), 0));
	const auto titleWidth = st::searchTrendTitleFont->width(title);
	p.setFont(st::searchTrendTitleFont);
	p.setPen(st::searchTrendTitleFg);
	p.drawTextLeft(left, titleTop, width, title, titleWidth);
	if (row.rising) {
		const auto badgeHeight = int(st::searchTrendRisingHeight);
		const auto badge = style::rtlrect(
			left + titleWidth + 8,
			titleTop + (st::searchTrendTitleFont->height - badgeHeight) / 2,
			risingWidth,
			badgeHeight,
			width);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::searchTrendRisingBg);
			p.drawRoundedRect(badge, badgeHeight / 2., badgeHeight / 2.);
		}
		p.setFont(st::searchTrendRisingFont);
		p.setPen(st::searchTrendRisingFgOn);
		p.drawTextCentered(badge, rising);
	}
	if (!single) {
		p.setFont(st::searchTrendSubFont);
		p.setPen(st::searchTrendSubFg);
		p.drawTextLeft(
			left,
			trend ? int(st::searchTrendSubTop) : (int(st::searchTrendSubTop) - 2),
			width,
			st::searchTrendSubFont->elided(row.subtitle, std::max(available, 0)));
	}
}

void Widget::paintVideos(Painter &p, const Row &row, int width, int index) {
	const auto over = _list->overAction(index);
	const auto ratio = devicePixelRatioF();
	for (auto i = 0; i != int(row.posts.size()); ++i) {
		const auto cell = style::rtlrect(videoRect(i, width), width);
		auto path = QPainterPath();
		path.addRoundedRect(cell, st::searchVideoRadius, st::searchVideoRadius);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::searchVideoBg);
		p.drawPath(path);
		const auto poster = _posters.find(row.posts[i]);
		if (poster != _posters.end()
			&& poster->handle
			&& !poster->handle->image().isNull()) {
			const auto target = cell.size() * ratio;
			if (poster->preparedFor != target) {
				// Cover the cell, cropped around the middle, in device pixels.
				const auto scaled = poster->handle->image().scaled(
					target,
					Qt::KeepAspectRatioByExpanding,
					Qt::SmoothTransformation);
				auto cropped = scaled.copy(
					(scaled.width() - target.width()) / 2,
					(scaled.height() - target.height()) / 2,
					target.width(),
					target.height());
				cropped.setDevicePixelRatio(ratio);
				poster->prepared = QPixmap::fromImage(std::move(cropped));
				poster->preparedFor = target;
			}
			p.save();
			p.setClipPath(path);
			p.drawPixmap(cell.topLeft(), poster->prepared);
			p.restore();
		}
		// The play glyph at the top trailing corner, on a soft shade.
		const auto glyphSize = int(st::searchVideoPlaySize);
		const auto shift = int(st::searchVideoPlayShift);
		const auto glyph = QRectF(
			style::RightToLeft()
				? (cell.x() + shift)
				: (cell.x() + cell.width() - shift - glyphSize),
			cell.y() + shift,
			glyphSize,
			glyphSize);
		p.setBrush(st::searchVideoShadeFg);
		p.drawEllipse(glyph);
		{
			const auto center = glyph.center();
			const auto half = glyphSize / 5.;
			auto triangle = QPainterPath();
			triangle.moveTo(center.x() - half * 0.7, center.y() - half);
			triangle.lineTo(center.x() + half, center.y());
			triangle.lineTo(center.x() - half * 0.7, center.y() + half);
			triangle.closeSubpath();
			p.setBrush(st::staticWhite);
			p.drawPath(triangle);
		}
		if (over == kActVideo + i) {
			p.setBrush(st::searchVideoOverFg);
			p.drawPath(path);
		}
	}
}

void Widget::paintPerson(Painter &p, const Row &row, int width, int index) {
	const auto padding = int(st::searchPadding);
	const auto size = int(st::searchPersonUserpic);
	const auto baseHeight = int(st::searchPersonHeight);
	const auto userpic = style::rtlrect(
		padding,
		(baseHeight - size) / 2,
		size,
		size,
		width);
	if (row.profile.deleted) {
		_userpics.paintSpecial(p, userpic, Ui::UserpicKind::Deleted, row.id);
	} else {
		_userpics.paint(p, userpic, row.profile.card());
	}
	const auto follow = followRect(row, width);
	const auto dismiss = dismissRect(row, width);
	const auto left = padding + size + int(st::searchPersonSkip);
	const auto right = !follow.isEmpty()
		? (follow.x() - int(st::searchFollowSkip))
		: !dismiss.isEmpty()
		? (dismiss.x() - 4)
		: (width - padding);
	const auto available = std::max(right - left, 0);

	// ---- name, badges
	const auto single = row.subtitle.isEmpty();
	const auto nameTop = single
		? ((baseHeight - st::searchPersonNameFont->height) / 2)
		: int(st::searchPersonNameTop);
	const auto badges = (row.profile.verified
		? (st::searchVerifiedIcon.width() + int(st::searchBadgeSkip))
		: 0) + (row.profile.isPrivate
		? (st::searchLockIcon.width() + int(st::searchBadgeSkip))
		: 0);
	const auto name = st::searchPersonNameFont->elided(
		row.title,
		std::max(available - badges, 0));
	const auto nameWidth = st::searchPersonNameFont->width(name);
	p.setFont(st::searchPersonNameFont);
	p.setPen(st::searchPersonNameFg);
	p.drawTextLeft(left, nameTop, width, name, nameWidth);
	auto badgeLeft = left + nameWidth + int(st::searchBadgeSkip);
	if (row.profile.verified) {
		const auto &icon = st::searchVerifiedIcon;
		icon.paint(
			p,
			badgeLeft,
			nameTop + (st::searchPersonNameFont->height - icon.height()) / 2,
			width);
		badgeLeft += icon.width() + int(st::searchBadgeSkip);
	}
	if (row.profile.isPrivate) {
		const auto &icon = st::searchLockIcon;
		icon.paint(
			p,
			badgeLeft,
			nameTop + (st::searchPersonNameFont->height - icon.height()) / 2,
			width);
	}
	if (!single) {
		p.setFont(st::searchPersonSubFont);
		p.setPen(st::searchPersonSubFg);
		p.drawTextLeft(
			left,
			st::searchPersonSubTop,
			width,
			st::searchPersonSubFont->elided(row.subtitle, available));
	}
	if (!row.link.isEmpty()) {
		// The bio line runs under the button too.
		p.setFont(st::searchPersonSubFont);
		p.setPen(st::searchPersonBioFg);
		p.drawTextLeft(
			left,
			st::searchPersonBioTop,
			width,
			st::searchPersonSubFont->elided(
				row.link,
				std::max(width - left - padding, 0)));
	}

	// ---- the follow button
	if (!follow.isEmpty()) {
		const auto state = FollowStateOf(row.profile);
		const auto busy = _followBusy.contains(row.id);
		const auto over = !busy
			&& ((_list->overAction(index) == kActFollow)
				|| (_list->pressedAction(index) == kActFollow));
		const auto button = style::rtlrect(follow, width);
		const auto filled = (state == FollowState::None)
			|| (state == FollowState::FollowsYou);
		if (busy) {
			p.setOpacity(st::searchFollowBusyOpacity);
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			if (filled) {
				p.setPen(Qt::NoPen);
				p.setBrush(over ? st::searchFollowBgOver : st::searchFollowBg);
			} else if (state == FollowState::Following) {
				p.setPen(QPen(st::searchFollowingBorder->c, 1.));
				p.setBrush(over
					? QBrush(st::searchFollowingBgOver->c)
					: QBrush(Qt::NoBrush));
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(st::searchRequestedBg);
			}
			const auto half = filled ? 0. : 0.5;
			p.drawPath(Conversation::TailedRect(
				QRectF(button).marginsRemoved(QMarginsF(half, half, half, half)),
				st::searchFollowRadius,
				st::searchFollowTail));
		}
		p.setFont(st::searchFollowFont);
		p.setPen(filled
			? st::searchFollowFg
			: (state == FollowState::Following)
			? st::searchFollowingFg
			: st::searchRequestedFg);
		p.drawTextCentered(button, FollowText(state));
		p.setOpacity(1.);
	}

	// ---- «دیگر نشان نده»
	if (!dismiss.isEmpty()) {
		const auto over = (_list->overAction(index) == kActDismiss);
		const auto physical = style::rtlrect(dismiss, width);
		if (over) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::searchRemoveBgOver);
			p.drawEllipse(physical);
		}
		(over ? st::searchRemoveIconOver : st::searchRemoveIcon).paintInCenter(
			p,
			physical);
	}
}

void Widget::paintPost(Painter &p, const Row &row, int width, int height) {
	const auto &padding = st::searchPostPadding;
	const auto size = int(st::searchPostUserpic);
	const auto author = data().users().get(row.profile.id);
	_userpics.paint(
		p,
		style::rtlrect(padding.left(), padding.top(), size, size, width),
		row.profile.id);
	const auto left = padding.left() + size + int(st::searchPostSkip);
	const auto available = std::max(width - left - padding.right(), 1);
	const auto timeWidth = st::searchPostTimeFont->width(row.subtitle);
	const auto name = st::searchPostAuthorFont->elided(
		author.deleted ? tr::lng_deleted_account(tr::now) : author.displayName,
		std::max(available - timeWidth - 8, 0));
	const auto nameWidth = st::searchPostAuthorFont->width(name);
	p.setFont(st::searchPostAuthorFont);
	p.setPen(st::searchPostAuthorFg);
	p.drawTextLeft(left, padding.top(), width, name, nameWidth);
	p.setFont(st::searchPostTimeFont);
	p.setPen(st::searchPostTimeFg);
	p.drawTextLeft(
		left + nameWidth + 8,
		padding.top()
			+ (st::searchPostAuthorFont->height - st::searchPostTimeFont->height),
		width,
		row.subtitle,
		timeWidth);
	p.setPen(st::searchPostTextFg);
	auto context = Ui::Text::PaintContext();
	context.position = style::rtlrect(
		left,
		padding.top()
			+ st::searchPostAuthorFont->height
			+ int(st::searchPostTextTop),
		available,
		1,
		width).topLeft();
	context.availableWidth = available;
	context.elisionLines = st::searchPostLines;
	context.autoDirection = true;
	row.text.draw(p, context);
	p.fillRect(
		style::rtlrect(left, height - 1, width - left, 1, width),
		st::searchLineFg);
}

void Widget::paintState(Painter &p, const Row &row, int width, int index) {
	const auto &padding = st::searchStatePadding;
	const auto available = std::max(
		std::min(
			width - padding.left() - padding.right(),
			int(st::searchStateMaxWidth)),
		1);
	const auto left = (width - available) / 2;
	auto top = padding.top();
	if (row.icon) {
		row.icon->paintInCenter(p, QRect(0, top, width, row.icon->height()));
		top += row.icon->height() + int(st::searchStateIconSkip);
	}
	p.setPen(st::searchStateTitleFg);
	auto context = Ui::Text::PaintContext();
	context.position = QPoint(left, top);
	context.availableWidth = available;
	context.align = style::al_center;
	row.heading.draw(p, context);
	top += row.heading.countHeight(available);
	if (!row.text.isEmpty()) {
		top += int(st::searchStateTextSkip);
		p.setPen(st::searchStateTextFg);
		context.position = QPoint(left, top);
		row.text.draw(p, context);
	}
	const auto button = stateButtonRect(row, width);
	if (!button.isEmpty()) {
		const auto over = (_list->overAction(index) == kActButton)
			|| (_list->pressedAction(index) == kActButton);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::searchStateButtonBg);
			p.drawRoundedRect(button, button.height() / 2., button.height() / 2.);
			if (over) {
				p.setBrush(st::rippleBrandFg);
				p.drawRoundedRect(
					button,
					button.height() / 2.,
					button.height() / 2.);
			}
		}
		p.setFont(st::searchStateButtonFont);
		p.setPen(st::searchStateButtonFg);
		p.drawTextCentered(button, row.link);
	}
}

// ---- actions

void Widget::rememberQuery() {
	if (!_query.isEmpty() && _recent.add(_query)) {
		saveRecent();
	}
}

void Widget::saveRecent() {
	if (const auto db = data().local()) {
		db->setValue(kRecentKey, _recent.toJson());
	}
}

void Widget::runRecent(const QString &query) {
	_field->setText(query);
	_debounce.set(query);
	rememberQuery();
	_field->setFocusFast();
}

void Widget::openNewConversation() {
	Conversation::ShowNewConversationBox(controller(), _query);
}

void Widget::rowActivated(int index) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto &row = *_rows[index];
	const auto window = controller();
	switch (row.kind) {
	case kRecent:
		runRecent(row.id);
		return;
	case kTrend:
	case kTopic:
		rememberQuery();
		window->openRoute(Window::Routes::Topic(row.id));
		return;
	case kPerson:
		rememberQuery();
		window->openRoute(Window::Routes::User(row.id));
		return;
	case kPost:
		rememberQuery();
		window->openRoute(Window::Routes::Post(row.id));
		return;
	case kConversation: {
		rememberQuery();
		const auto id = row.id;
		if (row.card->own) {
			Conversation::OpenConversation(window, id);
		} else {
			Conversation::ShowJoinBox(window, id);
		}
	} return;
	}
}

void Widget::actionTriggered(int index, int action) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto &row = *_rows[index];
	if (action >= kActVideo) {
		const auto cell = action - kActVideo;
		if (cell < row.posts.size()) {
			controller()->openRoute(Window::Routes::Post(row.posts[cell]));
		}
		return;
	} else if (action > kActCard) {
		if (action - kActCard == Conversation::kCardSpeaker
			&& !row.card->item.speakerId.isEmpty()) {
			controller()->openRoute(
				Window::Routes::User(row.card->item.speakerId));
		} else {
			rowActivated(index);
		}
		return;
	}
	switch (action) {
	case kActFollow:
		toggleFollow(row.id);
		return;
	case kActDismiss:
		dismissSuggestion(row.id);
		return;
	case kActRemove:
		if (_recent.remove(row.id)) {
			saveRecent();
			rebuild();
		}
		return;
	case kActLink:
	case kActButton:
		break;
	default:
		return;
	}
	switch (row.command) {
	case kCmdClearRecent:
		if (_recent.clear()) {
			saveRecent();
			rebuild();
		}
		break;
	case kCmdAllTrends:
		_browse->allTrends = true;
		rebuild();
		break;
	case kCmdScopeUsers: setScope(kScopeUsers); break;
	case kCmdScopeTopics: setScope(kScopeTopics); break;
	case kCmdScopePosts: setScope(kScopePosts); break;
	case kCmdScopeConversations: setScope(kScopeConversations); break;
	case kCmdRetryBrowse:
		_browse->loaded = false;
		loadBrowse();
		break;
	case kCmdRetrySearch: runSearch(); break;
	case kCmdNewConversation: openNewConversation(); break;
	case kCmdAllPeople: controller()->openRoute(kAllPeopleRoute); break;
	case kCmdRetryAllPeople: loadAllPeople(true); break;
	}
}

void Widget::setFollowState(
		const QString &userId,
		FollowState state,
		bool busy) {
	if (busy) {
		_followBusy.insert(userId);
	} else {
		_followBusy.remove(userId);
	}
	const auto apply = [&](Data::Profile &profile) {
		if (profile.id == userId) {
			ApplyFollowState(profile, state);
		}
	};
	for (auto &profile : _results->users) {
		apply(profile);
	}
	if (const auto person = _browse->people.find(userId)) {
		apply(*person);
		// Followed: the row gives its place a moment later (unless the
		// follow is taken back or fails meanwhile).
		if (_mode == Mode::Browse
			&& LeavesSuggestions(state)
			&& !_leaving.contains(userId)) {
			_leaving.insert(userId);
			Ui::CallDelayed(SuggestionQueue::kFollowLeaveMs, this, [=, this] {
				_leaving.remove(userId);
				leaveSuggestions(userId);
			});
		}
	}
	for (auto &profile : _all->people) {
		apply(profile);
	}
	for (auto i = 0; i != int(_rows.size()); ++i) {
		auto &row = *_rows[i];
		if (row.kind == kPerson && row.id == userId) {
			apply(row.profile);
			_list->updateRow(i);
		}
	}
}

void Widget::toggleFollow(const QString &userId) {
	if (_followBusy.contains(userId)) {
		return;
	}
	const auto i = std::find_if(_rows.cbegin(), _rows.cend(), [&](
			const std::unique_ptr<Row> &row) {
		return row->kind == kPerson && row->id == userId;
	});
	if (i == _rows.cend()) {
		return;
	}
	const auto &profile = (*i)->profile;
	const auto current = FollowStateOf(profile);
	if (current != FollowState::Following) {
		applyFollow(userId, ClickFollows(current));
		return;
	}
	auto args = Ui::ConfirmBoxArgs();
	args.text = tr::lng_search_unfollow_sure(
		tr::now,
		lt_name,
		QChar(0x2068) + profile.displayName + QChar(0x2069));
	args.confirmText = tr::lng_unfollow(tr::now);
	args.cancelText = tr::lng_never_mind(tr::now);
	args.destructive = true;
	const auto weak = QPointer<Widget>(this);
	args.confirmed = [=](std::function<void()> close) {
		if (weak) {
			weak->applyFollow(userId, false);
		}
		close();
	};
	controller()->show(Ui::MakeConfirmBox(std::move(args)));
}

void Widget::applyFollow(const QString &userId, bool follow) {
	const auto i = std::find_if(_rows.cbegin(), _rows.cend(), [&](
			const std::unique_ptr<Row> &row) {
		return row->kind == kPerson && row->id == userId;
	});
	if (i == _rows.cend() || _followBusy.contains(userId)) {
		return;
	}
	const auto &profile = (*i)->profile;
	const auto was = FollowStateOf(profile);
	if (ClickFollows(was) != follow) {
		return;
	}
	// Shown at once; the server's answer (or the failure) corrects it.
	setFollowState(
		userId,
		NextFollowState(was, profile.isPrivate, profile.followsYou),
		true);
	const auto done = [=, this](const Data::GraphState &state) {
		setFollowState(userId, FollowStateOf(state), false);
	};
	const auto fail = [=, this](const Api::Error &error) {
		setFollowState(userId, was, false);
		Support::ShowError(
			controller(),
			error,
			tr::lng_search_action_failed(tr::now));
	};
	if (follow) {
		data().graph().follow(userId, this, done, fail);
	} else {
		data().graph().unfollow(userId, this, done, fail);
	}
}

void Widget::dismissSuggestion(const QString &userId) {
	if (!_hiddenPeople.contains(userId)) {
		// Never again on this device, whatever the server remembers.
		hidePerson(userId);
		if (const auto db = data().local()) {
			db->setValue(
				kHiddenPeopleKey,
				QJsonArray::fromStringList(_hiddenPeopleOrder));
		}
	}
	// Every list of suggestions hears of it (hidePerson again: no-op here).
	data().graph().dismissSuggestion(userId);
}

void Widget::hidePerson(const QString &userId) {
	if (!_hiddenPeople.contains(userId)) {
		_hiddenPeople.insert(userId);
		_hiddenPeopleOrder.push_back(userId);
		while (_hiddenPeopleOrder.size() > kMaxHiddenPeople) {
			_hiddenPeople.remove(_hiddenPeopleOrder.takeFirst());
		}
	}
	auto &all = _all->people;
	const auto before = all.size();
	all.erase(
		std::remove_if(all.begin(), all.end(), [&](const Data::Profile &person) {
			return person.id == userId;
		}),
		all.end());
	if (all.size() != before && _mode == Mode::AllPeople) {
		rebuild();
		if (all.isEmpty()) {
			loadAllPeople(false);
		}
	}
	if (_browse->people.find(userId)) {
		// The click lands, then the next one takes the place.
		Ui::CallDelayed(SuggestionQueue::kDismissLeaveMs, this, [=, this] {
			if (_browse->people.remove(userId)) {
				refillSuggestions();
				if (_query.isEmpty()) {
					rebuild();
				}
			}
		});
	}
}

void Widget::showRowMenu(int index, QPoint globalPosition) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto &row = *_rows[index];
	const auto window = controller();
	const auto id = row.id;
	const auto weak = QPointer<Widget>(this);
	if (row.kind == kConversation) {
		Conversation::ShowTopicMenu(this, window, *row.card, globalPosition);
		return;
	}
	const auto menu = new Ui::PopupMenu(this);
	const auto copy = [](const QString &text) {
		return [text] {
			QGuiApplication::clipboard()->setText(text);
		};
	};
	switch (row.kind) {
	case kRecent:
		menu->addAction(tr::lng_tab_search(tr::now), [=] {
			if (weak) {
				weak->runRecent(id);
			}
		}, &st::searchMenuOpen);
		menu->addAction(tr::lng_search_recent_remove(tr::now), [=] {
			if (weak && weak->_recent.remove(id)) {
				weak->saveRecent();
				weak->rebuild();
			}
		}, &st::searchMenuRemove);
		break;
	case kTrend:
	case kTopic: {
		menu->addAction(tr::lng_search_open_topic(tr::now), [=] {
			window->openRoute(Window::Routes::Topic(id));
		}, &st::searchMenuOpen);
		menu->addAction(
			tr::lng_eli_start_conversation_about(tr::now),
			[=] { Conversation::ShowNewConversationBox(window, id); },
			&st::searchMenuConversation);
		menu->addAction(
			tr::lng_search_copy_hashtag(tr::now),
			copy(QLatin1Char('#') + id),
			&st::menuIconCopy);
		const auto topicId = row.link;
		if (row.kind == kTrend && !topicId.isEmpty()) {
			menu->addSeparator();
			menu->addAction(tr::lng_eli_topic_not_interested(tr::now), [=] {
				window->session().data().feed().hideTopic(topicId, true);
				if (!weak || !weak->_browse->trends) {
					return;
				}
				auto &trends = *weak->_browse->trends;
				trends.erase(
					std::remove_if(trends.begin(), trends.end(), [&](
							const Data::Trend &trend) {
						return trend.id == topicId;
					}),
					trends.end());
				window->showToast(tr::lng_search_topic_hidden(tr::now));
				if (weak->_query.isEmpty()) {
					weak->rebuild();
				}
			}, &st::searchMenuHide);
		}
	} break;
	case kPerson: {
		menu->addAction(tr::lng_search_open_profile(tr::now), [=] {
			window->openRoute(Window::Routes::User(id));
		}, &st::searchMenuProfile);
		if (!followRect(row, width()).isEmpty()) {
			const auto follows = ClickFollows(FollowStateOf(row.profile));
			menu->addAction(
				follows ? tr::lng_follow(tr::now) : tr::lng_unfollow(tr::now),
				[=] {
					if (weak) {
						weak->toggleFollow(id);
					}
				},
				follows ? &st::searchMenuFollow : &st::searchMenuUnfollow);
		}
		if (!row.profile.handle.isEmpty()) {
			menu->addAction(
				tr::lng_search_copy_handle(tr::now),
				copy(QLatin1Char('@') + row.profile.handle),
				&st::menuIconCopy);
		}
		if (row.suggestion) {
			menu->addSeparator();
			menu->addAction(tr::lng_conv_dont_show_again(tr::now), [=] {
				if (weak) {
					weak->dismissSuggestion(id);
				}
			}, &st::searchMenuHide);
		}
	} break;
	case kPost:
		menu->addAction(tr::lng_search_open_post(tr::now), [=] {
			window->openRoute(Window::Routes::Post(id));
		}, &st::searchMenuOpen);
		if (const auto post = data().feed().post(id);
			post && !post->text.isEmpty()) {
			menu->addAction(
				tr::lng_copy_text(tr::now),
				copy(post->text),
				&st::menuIconCopy);
		}
		break;
	default:
		delete menu;
		return;
	}
	menu->popup(globalPosition);
}

// ---- events

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (_bar) {
		_bar->resizeToWidth(width());
		_bar->move(0, 0);
		const auto area = QRect(
			0,
			_bar->height(),
			width(),
			std::max(height() - _bar->height(), 0));
		_scroll->setGeometry(area);
		_skeleton->setGeometry(area);
		return;
	}
	const auto &padding = st::searchFieldPadding;
	_field->resizeToWidth(std::max(width() - padding.left() - padding.right(), 1));
	_field->moveToLeft(
		padding.left(),
		(int(st::searchHeaderHeight) - _field->height()) / 2,
		width());
	auto top = int(st::searchHeaderHeight);
	if (!_scopes->isHidden()) {
		_scopes->resizeToWidth(width());
		_scopes->moveToLeft(0, top, width());
		top += _scopes->height();
	}
	const auto area = QRect(0, top, width(), std::max(height() - top, 0));
	_scroll->setGeometry(area);
	_skeleton->setGeometry(area);
	updateEmbedRange();
}

void Widget::paintEvent(QPaintEvent *e) {
	SectionWidget::paintEvent(e);
	auto p = QPainter(this);
	const auto top = _scroll->y();
	p.fillRect(0, top - st::lineWidth, width(), st::lineWidth, st::searchLineFg);
}

void Widget::showEvent(QShowEvent *e) {
	SectionWidget::showEvent(e);
	// Back on the tab after a while: the browse sections are read again.
	maybeRefresh();
}

bool Widget::eventFilter(QObject *object, QEvent *event) {
	if (event->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(event)->key();
		switch (key) {
		case Qt::Key_Down: _list->selectSkip(1); return true;
		case Qt::Key_Up:
			if (const auto was = _list->selectedRow(); was >= 0) {
				_list->selectSkip(-1);
				if (_list->selectedRow() == was) {
					// Above the first row is the field itself.
					_list->clearSelection();
					_scroll->scrollToY(0);
				}
				return true;
			}
			break;
		case Qt::Key_PageDown:
			_list->selectSkipPage(_scroll->height(), 1);
			return true;
		case Qt::Key_PageUp:
			_list->selectSkipPage(_scroll->height(), -1);
			return true;
		}
	}
	return SectionWidget::eventFilter(object, event);
}

} // namespace Search
