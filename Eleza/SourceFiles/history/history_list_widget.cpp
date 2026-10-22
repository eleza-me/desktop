#include "history/history_list_widget.h"
#include "support/media_image.h"
#include "data/data_json.h"
#include "data/data_groups.h"
#include "data/data_group_rules.h"

#include "api/api_client_config.h"
#include "data/data_chats.h"
#include "data/data_feed.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "history/history_album.h"
#include "history/history_downloads.h"
#include "history/history_view_media.h"
#include "history/history_themes.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_audio_player.h"
#include "media/media_video_player.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimerEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QPaintEvent>
#include <QtGui/QWheelEvent>

#include <algorithm>

namespace History {
namespace {

// Up to this many rows are laid out at once; more are estimated first and
// made exact in slices, the rows around the viewport before the others.
constexpr auto kSyncLayoutLimit = 320;
constexpr auto kLayoutSliceMs = 7;
constexpr auto kKeepLoadedAround = 260; // rows on each side of the viewport
constexpr auto kTrimInterval = 1500;
constexpr auto kReadDelay = 120;
constexpr auto kDateHideDelay = 1400;
constexpr auto kLoadRetryDelay = 4000;
constexpr auto kHighlightHold = 900;
constexpr auto kHighlightFade = 700;
constexpr auto kBottomSlack = 2;

[[nodiscard]] int PillHeight() {
	const auto &padding = st::historyServicePadding;
	return st::historyServiceStyle.lineHeight + padding.top() + padding.bottom();
}

} // namespace

ListWidget::ListWidget(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &chatId,
	Ui::ScrollArea *scroll)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _chatId(chatId)
, _scroll(scroll)
, _downloads(Downloads::For(_session))
, _group(Data::IsGroupId(chatId))
, _topLoader([this] {
	update(0, _visibleTop, width(), 60);
}) {
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent);

	auto &data = _session->data();
	if (_group) {
		_userpics = std::make_unique<Support::UserpicPainter>(
			_session,
			[this](const QString &) { update(); });
		connect(
			&data.groups(),
			&Data::Groups::membersChanged,
			this,
			[this](const QString &group) {
				if (group == _chatId) {
					loadRoles();
				}
			});
		loadRoles();
	}
	connect(
		&data.messages(),
		&Data::Messages::historyChanged,
		this,
		[this](const QString &chat) {
			if (chat == _chatId && !_debugMode && _started) {
				refreshRows();
			}
		});
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](const QString &id) {
		if (id != _chatId || _unreadResolved || !_started || _debugMode) {
			return;
		}
		const auto chat = _session->data().chats().chat(_chatId);
		if (!chat) {
			return;
		}
		// The chat was opened before its row was known (a link, a start
		// route): the unread divider is placed now.
		_unreadResolved = true;
		if (chat->unread > 0
			|| (chat->lastReadSeq > 0 && chat->lastReadSeq < chat->lastSeq)) {
			_firstUnreadSeq = chat->lastReadSeq + 1;
			refreshRows();
			if (_positioned && _atBottom) {
				positionInitially();
			}
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this](const QStringList &ids) {
		// Names in reply headers and shared posts; in a group also above
		// the bubbles and in the notes; the lines of self-destructing
		// media and its screenshot notice.
		auto changed = false;
		for (auto &row : _rows) {
			const auto &message = row.element->data();
			const auto system = (message.kind == Data::MessageKind::System);
			if ((_group
					&& (ids.contains(message.sender)
						|| (system && ids.contains(Data::GroupRules::NoteUser(message.system)))))
				|| (system && ids.contains(Data::SelfDestructScreenshotTaker(message.system)))
				|| (message.selfDestruct && ids.contains(message.sender))) {
				row.element->unloadHeavy();
				row.element->invalidate();
				row.exact = false;
				changed = true;
			}
		}
		if (changed) {
			relayoutKeepingAnchor();
		}
		invalidateAll();
	});
	connect(&data.feed(), &Data::Feed::postChanged, this, [this](const QString &id) {
		auto changed = false;
		for (auto &row : _rows) {
			if (row.element->data().forwardedPost == id) {
				row.element->invalidate();
				row.exact = false;
				changed = true;
			}
		}
		if (changed) {
			relayoutKeepingAnchor();
			update();
		}
	});
	connect(&data.stories(), &Data::Stories::storyGone, this, [this](
			const QString &storyId) {
		// Its card turns into «استوری دیگر در دسترس نیست».
		auto changed = false;
		for (auto &row : _rows) {
			const auto &message = row.element->data();
			if (message.story && message.story->storyId == storyId) {
				row.element->invalidate();
				row.exact = false;
				changed = true;
			}
		}
		if (changed) {
			relayoutKeepingAnchor();
			update();
		}
	});
	connect(_downloads, &Downloads::changed, this, [this](const QString &chat, qint64 seq) {
		if (chat == _chatId) {
			updateRow(indexOfSeq(seq));
		}
	});
	connect(_downloads, &Downloads::failed, this, [this](const QString &chat, qint64 seq) {
		if (chat == _chatId) {
			const auto item = message(seq);
			_controller->showToast(
				(item && _downloads->state(*item).oversized)
					? tr::lng_chat_file_larger_than_declared(tr::now)
					: tr::lng_chat_file_isnt_available(tr::now));
		}
	});
	const auto player = &Media::Player();
	const auto repaintTrack = [this, player] {
		static const auto kPrefix = u"chat:"_s;
		const auto &id = player->current().id;
		const auto prefix = kPrefix + _chatId + u':';
		if (id.startsWith(prefix)) {
			updateRow(indexOfSeq(id.mid(prefix.size()).toLongLong()));
		}
	};
	connect(player, &Media::AudioPlayer::stateChanged, this, [=, this] {
		// The previous track's row goes back to «play» too.
		update(0, _visibleTop, width(), _visibleBottom - _visibleTop);
	});
	connect(player, &Media::AudioPlayer::trackChanged, this, [=, this] {
		update(0, _visibleTop, width(), _visibleBottom - _visibleTop);
	});
	connect(player, &Media::AudioPlayer::positionChanged, this, repaintTrack);
	connect(player, &Media::AudioPlayer::speedChanged, this, repaintTrack);
	style::OnPaletteChanged(this, [this] {
		_colorsVersion = -1;
		update();
	});
	if (_scroll) {
		connect(_scroll, &Ui::ScrollArea::scrolled, this, [this] { scrolled(); });
	}
	_liveTimer.start(30 * 1000, this);
}

ListWidget::~ListWidget() = default;

// ---- View::Delegate

Main::Session *ListWidget::elementSession() {
	return _session;
}

QObject *ListWidget::elementGuard() {
	return this;
}

double ListWidget::elementRatio() {
	return devicePixelRatioF();
}

const ChatColors &ListWidget::elementColors() {
	if (_colorsVersion != style::PaletteVersion()) {
		_colorsVersion = style::PaletteVersion();
		_colors = ColorsForTheme(_theme, _anonymousPeer);
	}
	return _colors;
}

void ListWidget::elementRepaint(View::Element *element) {
	updateRow(indexOfKey(element->key()));
}

void ListWidget::elementRelayout(View::Element *element) {
	const auto key = element->key();
	// May be called while painting: never change geometry right now.
	Ui::PostponeCall(this, [=, this] {
		const auto index = indexOfKey(key);
		if (index < 0) {
			return;
		}
		_rows[index].exact = false;
		relayoutKeepingAnchor();
		update();
	});
}

const Data::Message *ListWidget::elementReplyTarget(const Data::Message &message) {
	const auto seq = message.replyToSeq;
	if (seq <= 0) {
		return nullptr;
	}
	if (const auto found = this->message(seq)) {
		return found;
	}
	const auto cached = _replyTargets.constFind(seq);
	if (cached != _replyTargets.cend()) {
		return &*cached;
	} else if (_debugMode || _requestedReplies.contains(seq)) {
		return nullptr;
	}
	_requestedReplies.insert(seq);
	_session->data().messages().fetchOne(_chatId, seq, this, [=, this](
			const std::optional<Data::Message> &found) {
		if (!found) {
			return;
		}
		_replyTargets.insert(seq, *found);
		auto changed = false;
		for (auto &row : _rows) {
			if (row.element->data().replyToSeq == seq) {
				row.element->unloadHeavy();
				row.element->invalidate();
				row.exact = false;
				changed = true;
			}
		}
		if (changed) {
			relayoutKeepingAnchor();
			update();
		}
	});
	return nullptr;
}

QString ListWidget::elementSenderName(const Data::Message &message) {
	return senderName(message);
}

bool ListWidget::elementGroup() {
	return _group;
}

View::SenderInfo ListWidget::elementSender(const Data::Message &message) {
	auto result = View::SenderInfo();
	if (!_group || message.sender.isEmpty()) {
		return result;
	}
	result.userId = message.sender;
	result.name = Support::MemberName(_session, message.sender);
	result.tag = _roles.value(message.sender);
	result.colorIndex = Data::GroupRules::NameColorIndex(message.sender, 8);
	return result;
}

void ListWidget::elementPaintUserpic(
		Painter &p,
		const QRect &rect,
		const QString &userId) {
	if (!_userpics) {
		// Outside a group only a story sent on shows a face (its owner's).
		_userpics = std::make_unique<Support::UserpicPainter>(
			_session,
			[this](const QString &) { update(); });
	}
	_userpics->paint(p, rect, userId);
}

Support::NoteLine ListWidget::elementServiceText(const Data::Message &message) {
	// The notes of a group, the screenshot notice of a direct chat.
	return Support::ServiceNoteLine(_session, message);
}

// Who is the owner and who an admin (with a title of their own or without):
// the tag next to a name. One page of admins is all a chat screen needs.
void ListWidget::loadRoles() {
	auto &groups = _session->data().groups();
	if (!_group || !groups.enabled()) {
		return;
	} else if (_rolesLoading) {
		_rolesAgain = true;
		return;
	}
	_rolesLoading = true;
	const auto filter = groups.administration() ? u"admins"_s : u"all"_s;
	const auto finish = [this] {
		_rolesLoading = false;
		if (std::exchange(_rolesAgain, false)) {
			loadRoles();
		}
	};
	groups.loadMembers(_chatId, filter, QString(), this, [=, this](
			const Data::GroupMembersPage &page) {
		auto roles = QHash<QString, QString>();
		for (const auto &member : page.members) {
			if (member.role == u"owner") {
				roles.insert(member.user, tr::lng_groups_role_owner(tr::now));
			} else if (member.role == u"admin") {
				roles.insert(member.user, member.adminTitle.trimmed().isEmpty()
					? tr::lng_groups_role_admin(tr::now)
					: Data::Json::CleanName(member.adminTitle.simplified()));
			}
		}
		if (roles != _roles) {
			_roles = roles;
			for (auto &row : _rows) {
				if (!row.element->data().mine && !row.element->isService()) {
					row.element->unloadHeavy();
					row.element->invalidate();
					row.exact = false;
				}
			}
			relayoutKeepingAnchor();
			update();
		}
		finish();
	}, [=](const Api::Error &) {
		finish();
	});
}

QString ListWidget::senderName(const Data::Message &message) {
	auto &data = _session->data();
	if (message.mine) {
		const auto &me = data.users().me();
		return me.displayName.isEmpty() ? tr::lng_you(tr::now) : me.displayName;
	} else if (_group) {
		return Support::MemberName(_session, message.sender);
	}
	if (const auto chat = data.chats().chat(_chatId)) {
		return Support::ChatTitle(_session, *chat);
	}
	return QString();
}

const Data::Post *ListWidget::elementPost(const QString &postId) {
	static const auto kUnavailable = Data::Post();
	if (postId.isEmpty() || _failedPosts.contains(postId)) {
		return &kUnavailable;
	}
	auto &feed = _session->data().feed();
	if (const auto post = feed.post(postId)) {
		return post;
	} else if (_requestedPosts.contains(postId)) {
		return nullptr;
	}
	_requestedPosts.insert(postId);
	const auto relayout = [=, this] {
		for (auto &row : _rows) {
			if (row.element->data().forwardedPost == postId) {
				row.element->invalidate();
				row.exact = false;
			}
		}
		relayoutKeepingAnchor();
		update();
	};
	feed.fetchPost(postId, this, [=, this](const Data::Post &post) {
		// The author's card may still be on its way (users.changed follows).
		(void)_session->data().users().get(post.author);
		relayout();
	}, [=, this](const Api::Error &) {
		_failedPosts.insert(postId);
		relayout();
	});
	return nullptr;
}

View::DownloadState ListWidget::elementDownload(const Data::Message &message) {
	return _downloads->state(message);
}

View::PlaybackState ListWidget::elementPlayback(const Data::Message &message) {
	auto result = View::PlaybackState();
	if (message.seq <= 0 && message.localPath.isEmpty()) {
		return result;
	}
	const auto &player = Media::Player();
	const auto id = (message.seq > 0)
		? Media::AudioTrack::IdForMessage(message.chat, message.seq)
		: (u"local:"_s + message.clientMessageId);
	if (!player.isCurrent(id)) {
		return result;
	}
	result.current = true;
	result.playing = player.isPlaying(id);
	result.loading = player.isLoading(id);
	result.progress = player.progress();
	result.positionMs = player.positionMs();
	result.speed = player.speed();
	return result;
}

bool ListWidget::elementRevealed(const QString &key) {
	return _revealed.contains(key);
}

bool ListWidget::elementButtonLoading(
		const Data::Message &message,
		int row,
		int column) {
	return !_loadingButtons.isEmpty()
		&& _loadingButtons.contains(
			u"%1:%2:%3"_s.arg(message.seq).arg(row).arg(column));
}

// ---- public

void ListWidget::start(qint64 aroundSeq) {
	if (_started) {
		return;
	}
	_started = true;
	_aroundSeq = aroundSeq;
	auto &data = _session->data();
	const auto chat = data.chats().chat(_chatId);
	// The divider goes above the first incoming message after what I read
	// (a row made from a header alone has no unread count yet).
	_firstUnreadSeq = (chat
		&& (chat->unread > 0
			|| (chat->lastReadSeq > 0 && chat->lastReadSeq < chat->lastSeq)))
		? (chat->lastReadSeq + 1)
		: 0;
	_unreadResolved = (chat != nullptr);
	refreshRows();
	_initialLoading = _rows.empty();
	data.messages().loadCached(_chatId, this, [this](int) {
		loadInitial();
	});
}

void ListWidget::loadInitial() {
	auto &messages = _session->data().messages();
	const auto &known = history();
	if (_aroundSeq > 0) {
		const auto seq = std::exchange(_aroundSeq, 0);
		if (!known.loaded && indexOfSeq(seq) < 0) {
			// Nothing on screen yet: open right at the message.
			_initialLoading = _rows.empty();
		}
		jumpTo(seq);
		return;
	}
	if (known.loaded && !known.hasMoreAfter) {
		// Read once in this run and kept current by updates since.
		_initialLoading = false;
		Q_EMIT stateChanged();
		checkRead();
		return;
	}
	messages.loadLatest(_chatId, this, [this] {
		_initialLoading = false;
		_loadFailed = false;
		Q_EMIT stateChanged();
		checkRead();
	}, [this](const Api::Error &error) {
		_initialLoading = false;
		_loadFailed = _rows.empty();
		if (!_loadFailed) {
			// What the cache had stays on screen; say why it may be stale.
			Support::ShowError(_controller, error);
		}
		Q_EMIT stateChanged();
	});
}

void ListWidget::retryLoad() {
	_loadFailed = false;
	_initialLoading = _rows.empty();
	Q_EMIT stateChanged();
	loadInitial();
}

void ListWidget::setActive(bool active) {
	if (_active == active) {
		return;
	}
	_active = active;
	if (_active) {
		checkRead();
	} else {
		_readTimer.stop();
	}
}

void ListWidget::setTheme(const QString &theme) {
	if (_theme == theme) {
		return;
	}
	_theme = theme;
	_colorsVersion = -1;
	update();
}

void ListWidget::setAnonymousPeer(bool anonymous) {
	if (_anonymousPeer == anonymous) {
		return;
	}
	_anonymousPeer = anonymous;
	_colorsVersion = -1;
	update();
}

void ListWidget::setCanWrite(bool canWrite) {
	_canWrite = canWrite;
}

void ListWidget::setCanReact(bool canReact) {
	_canReact = canReact;
}

void ListWidget::setBotChat(bool bot, const QString &botHandle) {
	_botHandle = botHandle;
	if (_botChat == bot) {
		return;
	}
	_botChat = bot;
	// The texts are laid out again with (or without) their commands.
	for (auto &row : _rows) {
		row.element->unloadHeavy();
		row.element->invalidate();
		row.exact = false;
	}
	if (!_rows.empty()) {
		relayoutKeepingAnchor();
		update();
	}
}

bool ListWidget::elementBotCommands() {
	return _botChat;
}

bool ListWidget::awayFromBottom() const {
	if (_rows.empty()) {
		return false;
	} else if (!_debugMode && history().hasMoreAfter) {
		return true;
	}
	// As soon as the reader leaves the end (Telegram): then the list no
	// longer follows new messages, and the button is the way back.
	return _scroll
		&& (_scroll->scrollTopMax() - _scroll->scrollTop() > kBottomSlack);
}

qint64 ListWidget::bottomVisibleSeq() const {
	if (_rows.empty()) {
		return 0;
	}
	for (auto i = rowNear(_visibleBottom - 1); i >= 0; --i) {
		if (const auto value = _rows[i].element->lastSeq(); value > 0) {
			return value;
		}
	}
	return 0;
}

void ListWidget::scrollByPage(bool up) {
	if (!_scroll) {
		return;
	}
	const auto page = std::max(viewportHeight() - 60, 60);
	_scroll->scrollToYAnimated(_scroll->scrollTop() + (up ? -page : page));
}

std::optional<Data::Message> ListWidget::lastEditable() const {
	const auto config = _session->data().clientConfig();
	const auto window = config ? config->editWindow() : qint64(0);
	const auto now = QDateTime::currentMSecsSinceEpoch();
	for (auto i = int(_rows.size()) - 1; i >= 0; --i) {
		const auto &message = _rows[i].element->data();
		if (message.mine && CanEdit(message, now, window)) {
			return message;
		} else if (message.mine && message.seq > 0) {
			// Only the last own message is edited this way.
			if (!IsService(message) || message.selfDestruct) {
				return std::nullopt;
			}
		}
	}
	return std::nullopt;
}

const Data::Message *ListWidget::message(qint64 seq) const {
	const auto index = indexOfSeq(seq);
	if (index < 0) {
		return nullptr;
	}
	const auto element = _rows[index].element.get();
	for (const auto &item : element->album()) {
		if (item.seq == seq) {
			return &item;
		}
	}
	return &element->data();
}

#ifdef ELEZA_DEBUG_TOOLS
void ListWidget::setDebugItems(const QVector<Data::Message> &items) {
	_debugMode = true;
	_started = true;
	_debugItems = items;
	_initialLoading = false;
	applyRows(_debugItems);
}
#endif // ELEZA_DEBUG_TOOLS

// ---- rows

const Data::History &ListWidget::history() const {
	return _session->data().messages().history(_chatId);
}

const QVector<Data::Message> &ListWidget::items() const {
	return _debugMode ? _debugItems : history().items;
}

void ListWidget::refreshRows() {
	applyRows(items());
}

void ListWidget::applyRows(const QVector<Data::Message> &timeline) {
	// Consecutive items of an album are one row: its head stands for it.
	const auto config = _session->data().clientConfig();
	const auto runs = GroupAlbums(
		timeline,
		_group,
		config ? config->maxAlbumItems() : 10);
	auto grouped = QVector<Data::Message>();
	auto albums = std::vector<QVector<Data::Message>>();
	if (runs.empty()) {
		grouped = timeline;
		albums.resize(timeline.size());
	} else {
		grouped.reserve(timeline.size());
		auto run = runs.begin();
		for (auto i = 0; i < int(timeline.size());) {
			if (run != runs.end() && run->from == i) {
				const auto album = timeline.mid(run->from, run->count);
				grouped.push_back(AlbumHeadMessage(album));
				albums.push_back(album);
				i += run->count;
				++run;
			} else {
				grouped.push_back(timeline[i]);
				albums.emplace_back();
				++i;
			}
		}
	}
	const auto &items = grouped;
	auto was = std::vector<RowId>();
	was.reserve(_rows.size());
	for (const auto &row : _rows) {
		was.push_back(RowId::From(row.element->data()));
	}
	auto now = std::vector<RowId>();
	now.reserve(items.size());
	for (const auto &message : items) {
		now.push_back(RowId::From(message));
	}
	const auto diff = DiffRows(was, now);
	auto flags = ComputeRowFlags(items, _firstUnreadSeq);
	const auto firstBack = int(items.size()) - diff.addedAtBack;
	auto sentByMe = false;
	for (auto i = firstBack; i < int(items.size()); ++i) {
		if (items[i].mine && items[i].local()) {
			sentByMe = true;
		}
	}
	if (sentByMe && _firstUnreadSeq) {
		// Answering reads everything: the divider goes.
		_firstUnreadSeq = 0;
		flags = ComputeRowFlags(items, 0);
	}
	auto renamed = QHash<QString, QString>();
	auto rows = std::vector<Row>(items.size());
	for (auto i = 0; i != int(items.size()); ++i) {
		const auto old = diff.newToOld[i];
		auto &row = rows[i];
		if (old >= 0) {
			row = std::move(_rows[old]);
			const auto oldKey = row.element->key();
			auto changed = row.element->setData(items[i], albums[i]);
			changed = row.element->setFlags(flags[i]) || changed;
			if (changed) {
				row.exact = false;
			}
			if (oldKey != row.element->key()) {
				renamed.insert(oldKey, row.element->key());
			}
		} else {
			row.element = std::make_unique<View::Element>(
				static_cast<View::Delegate*>(this),
				items[i],
				albums[i]);
			row.element->setFlags(flags[i]);
		}
	}
	// Whatever pointed at a row by its index is stale now.
	_hover = MouseTarget();
	_rows = std::move(rows);
	rebuildIndex();

	const auto remap = [&](QString &key) {
		if (key.isEmpty()) {
			return;
		}
		const auto moved = renamed.constFind(key);
		if (moved != renamed.cend()) {
			key = *moved;
		} else if (!_indexByKey.contains(key)) {
			key = QString();
		}
	};
	if (!_selected.isEmpty()) {
		auto selected = QSet<QString>();
		for (auto key : std::as_const(_selected)) {
			remap(key);
			if (!key.isEmpty()) {
				selected.insert(key);
			}
		}
		if (selected.size() != _selected.size()) {
			_selected = selected;
			Q_EMIT selectionChanged();
		} else {
			_selected = selected;
		}
	}
	const auto textKey = _textSelectionKey;
	remap(_textSelectionKey);
	if (_textSelectionKey.isEmpty() && !textKey.isEmpty()) {
		_textSelection = Ui::Text::Selection();
	}
	remap(_highlightKey);
	remap(_press.key);
	remap(_anchor.key);
	for (auto &video : _videos) {
		remap(video.key);
	}
	if (!renamed.isEmpty() && !_revealed.isEmpty()) {
		for (auto i = renamed.cbegin(); i != renamed.cend(); ++i) {
			if (_revealed.contains(i.key())) {
				_revealed.insert(i.value());
			}
		}
	}
	if (sentByMe) {
		// What I send is always shown.
		_anchor.bottom = true;
	}
	relayoutKeepingAnchor();
	if (!_positioned && !_rows.empty() && !_pendingJump) {
		positionInitially();
	}
	if (_pendingJump && indexOfSeq(_pendingJump) >= 0) {
		const auto seq = std::exchange(_pendingJump, 0);
		const auto quote = std::exchange(_pendingJumpQuote, std::nullopt);
		_positioned = true;
		showAtSeq(seq, quote, false);
	}
	updateInlineVideos();
	update();
	if (!_lastMouse.isNull() && underMouse()) {
		updateHover(mapFromGlobal(_lastMouseGlobal));
	}
	if (_rows.size() > size_t(2 * kKeepLoadedAround) && !_trimTimer.isActive()) {
		_trimTimer.start(kTrimInterval, this);
	}
	checkRead();
	Ui::PostponeCall(this, [this] { checkLoadMore(); });
	Q_EMIT stateChanged();
}

void ListWidget::rebuildIndex() {
	_indexByKey.clear();
	_indexBySeq.clear();
	_indexByKey.reserve(int(_rows.size()));
	_indexBySeq.reserve(int(_rows.size()));
	for (auto i = 0; i != int(_rows.size()); ++i) {
		const auto element = _rows[i].element.get();
		const auto &message = element->data();
		_indexByKey.insert(element->key(), i);
		if (message.seq > 0) {
			_indexBySeq.insert(message.seq, i);
		}
		// Every item of an album finds its bubble (jumps, replies, updates).
		for (const auto &item : element->album()) {
			if (!_indexByKey.contains(item.key())) {
				_indexByKey.insert(item.key(), i);
			}
			if (item.seq > 0) {
				_indexBySeq.insert(item.seq, i);
			}
		}
	}
}

int ListWidget::indexOfKey(const QString &key) const {
	return key.isEmpty() ? -1 : _indexByKey.value(key, -1);
}

int ListWidget::indexOfSeq(qint64 seq) const {
	return (seq > 0) ? _indexBySeq.value(seq, -1) : -1;
}

int ListWidget::rowNear(int y) const {
	if (_rows.empty()) {
		return -1;
	}
	const auto relative = y - _rowsTop;
	const auto i = std::upper_bound(
		_rows.begin(),
		_rows.end(),
		relative,
		[](int value, const Row &row) { return value < row.top; });
	return std::max(int(i - _rows.begin()) - 1, 0);
}

int ListWidget::rowAt(int y) const {
	const auto index = rowNear(y);
	if (index < 0) {
		return -1;
	}
	const auto top = rowTop(index);
	return (y >= top && y < top + _rows[index].height) ? index : -1;
}

QRect ListWidget::rowRect(int index) const {
	return (index >= 0 && index < int(_rows.size()))
		? QRect(0, rowTop(index), width(), _rows[index].height)
		: QRect();
}

void ListWidget::updateRow(int index) {
	if (index >= 0 && index < int(_rows.size())) {
		update(rowRect(index));
	}
}

void ListWidget::invalidateAll() {
	if (_rows.empty()) {
		return;
	}
	for (auto &row : _rows) {
		if (row.element->data().replyToSeq > 0
			|| !row.element->data().forwardedPost.isEmpty()) {
			row.element->unloadHeavy();
			row.element->invalidate();
			row.exact = false;
		}
	}
	relayoutKeepingAnchor();
	update();
}

// ---- layout

int ListWidget::viewportHeight() const {
	return _scroll ? _scroll->height() : 0;
}

int ListWidget::layoutRows(int width) {
	_layoutWidth = width;
	auto timer = QElapsedTimer();
	timer.start();
	auto dirty = 0;
	for (const auto &row : _rows) {
		if (!row.exact || !row.element->laidOutFor(width)) {
			++dirty;
		}
	}
	if (dirty <= kSyncLayoutLimit) {
		for (auto &row : _rows) {
			if (!row.exact || !row.element->laidOutFor(width)) {
				row.height = row.element->resizeGetHeight(width);
				row.exact = true;
			}
		}
		_pendingLayout.clear();
	} else {
		// Old heights are a better guess than nothing after a resize.
		_pendingLayout.clear();
		_pendingLayout.reserve(dirty);
		for (auto i = 0; i != int(_rows.size()); ++i) {
			auto &row = _rows[i];
			if (row.exact && row.element->laidOutFor(width)) {
				continue;
			}
			if (row.height <= 0) {
				row.height = row.element->estimateHeight(width);
			}
			row.exact = false;
			_pendingLayout.push_back(i);
		}
		if (!_layoutTimer.isActive()) {
			_layoutTimer.start(0, this);
		}
	}
	recountTops();
	_stats.layoutUs += timer.nsecsElapsed() / 1000;
	const auto &padding = st::historyPadding;
	const auto content = padding.top() + _rowsHeight + padding.bottom();
	const auto result = std::max(content, viewportHeight());
	// Short chats sit at the bottom of the viewport.
	_rowsTop = result - padding.bottom() - _rowsHeight;
	return result;
}

void ListWidget::recountTops() {
	auto top = 0;
	for (auto &row : _rows) {
		row.top = top;
		top += row.height;
	}
	_rowsHeight = top;
}

void ListWidget::layoutExactly(int from, int till) {
	from = std::max(from, 0);
	till = std::min(till, int(_rows.size()));
	for (auto i = from; i < till; ++i) {
		auto &row = _rows[i];
		if (!row.exact || !row.element->laidOutFor(_layoutWidth)) {
			row.height = row.element->resizeGetHeight(_layoutWidth);
			row.exact = true;
		}
	}
}

// Rows around the place the viewport will be at get their real heights
// before anything is painted.
void ListWidget::ensureExactNearViewport() {
	if (_pendingLayout.empty() || _rows.empty()) {
		return;
	}
	const auto count = int(_rows.size());
	auto center = count - 1;
	if (!_anchor.bottom) {
		const auto index = indexOfKey(_anchor.key);
		if (index >= 0) {
			center = index;
		}
	}
	const auto viewport = std::max(viewportHeight(), 200);
	// Walk up and down from the anchor until two viewports are covered.
	auto from = center;
	auto covered = 0;
	while (from > 0 && covered < 2 * viewport) {
		--from;
		layoutExactly(from, from + 1);
		covered += _rows[from].height;
	}
	auto till = center;
	covered = 0;
	while (till < count && covered < 2 * viewport) {
		layoutExactly(till, till + 1);
		covered += _rows[till].height;
		++till;
	}
	recountTops();
	const auto &padding = st::historyPadding;
	const auto content = padding.top() + _rowsHeight + padding.bottom();
	_rowsTop = std::max(content, viewportHeight()) - padding.bottom() - _rowsHeight;
}

void ListWidget::relayoutKeepingAnchor() {
	if (_inLayout) {
		return;
	}
	const auto useWidth = (_layoutWidth > 0) ? _layoutWidth : width();
	if (useWidth <= 0) {
		return;
	}
	_inLayout = true;
	layoutRows(useWidth);
	ensureExactNearViewport();
	const auto &padding = st::historyPadding;
	const auto wanted = std::max(
		padding.top() + _rowsHeight + padding.bottom(),
		viewportHeight());
	if (wanted != height()) {
		resize(width(), wanted);
	}
	_inLayout = false;
	applyAnchor();
	placeInlineVideos();
}

int ListWidget::resizeGetHeight(int newWidth) {
	const auto guard = _inLayout;
	_inLayout = true;
	layoutRows(newWidth);
	ensureExactNearViewport();
	_inLayout = guard;
	const auto &padding = st::historyPadding;
	// The resize event applies the anchor; nothing arrives when the size
	// stays the same, so make sure it happens either way.
	Ui::PostponeCall(this, [this] {
		applyAnchor();
		placeInlineVideos();
	});
	return std::max(
		padding.top() + _rowsHeight + padding.bottom(),
		viewportHeight());
}

void ListWidget::wheelEvent(QWheelEvent *e) {
	// The area scrolls; handing the event over by hand also serves wheel
	// events that are not propagated (synthesized ones).
	if (_scroll) {
		QCoreApplication::sendEvent(_scroll, e);
	} else {
		e->ignore();
	}
}

void ListWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (!_inLayout) {
		applyAnchor();
	}
}

void ListWidget::rememberAnchor() {
	if (!_scroll || _rows.empty()) {
		_anchor = Anchor();
		return;
	}
	const auto top = _scroll->scrollTop();
	const auto bottom = top + viewportHeight();
	const auto atEnd = (top >= _scroll->scrollTopMax() - kBottomSlack);
	_anchor.bottom = atEnd && (_debugMode || !history().hasMoreAfter);
	const auto index = rowNear(bottom - 1);
	_anchor.key = _rows[index].element->key();
	_anchor.offset = bottom - (rowTop(index) + _rows[index].height);
}

void ListWidget::applyAnchor() {
	if (!_scroll || _inLayout) {
		return;
	}
	const auto viewport = viewportHeight();
	auto wanted = _scroll->scrollTop();
	if (_anchor.bottom || _rows.empty()) {
		wanted = height() - viewport;
	} else {
		const auto index = indexOfKey(_anchor.key);
		if (index >= 0) {
			wanted = rowTop(index) + _rows[index].height + _anchor.offset - viewport;
		}
	}
	_inLayout = true;
	_scroll->finishScrollAnimation();
	_scroll->scrollToY(std::max(wanted, 0));
	_inLayout = false;
	const auto atBottom = (_scroll->scrollTop() >= _scroll->scrollTopMax() - kBottomSlack)
		&& (_debugMode || !history().hasMoreAfter);
	if (_atBottom != atBottom) {
		_atBottom = atBottom;
		Q_EMIT stateChanged();
	}
}

void ListWidget::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
}

void ListWidget::scrolled() {
	if (_inLayout) {
		return;
	}
	const auto wasAtBottom = _atBottom;
	const auto wasAway = awayFromBottom();
	rememberAnchor();
	_atBottom = _anchor.bottom;
	if (!_pendingLayout.empty()) {
		// Estimated rows are coming into view: measure them now, keeping
		// the row at the bottom edge where it is.
		const auto was = _rowsHeight;
		const auto viewport = std::max(viewportHeight(), 200);
		const auto from = rowNear(_visibleTop - viewport);
		const auto till = rowNear(_visibleBottom + viewport) + 1;
		auto need = false;
		for (auto i = from; i < till; ++i) {
			if (!_rows[i].exact) {
				need = true;
				break;
			}
		}
		if (need) {
			layoutExactly(from, till);
			recountTops();
			if (_rowsHeight != was) {
				relayoutKeepingAnchor();
			}
		}
	}
	// The wallpaper stays in place while the rows move: paint everything.
	update();
	if (!_dateFloating) {
		_dateFloating = true;
		_dateOpacity.start(
			[this] { update(0, _visibleTop, width(), 80); },
			0.,
			1.,
			st::motionFast);
	}
	_dateTimer.start(kDateHideDelay, this);
	checkLoadMore();
	checkRead();
	updateInlineVideos();
	if (_pressed && _dragging) {
		dragSelect(mapFromGlobal(_lastMouseGlobal));
	}
	if (wasAtBottom != _atBottom || wasAway != awayFromBottom()) {
		Q_EMIT stateChanged();
	}
}

void ListWidget::processPendingLayout() {
	if (_pendingLayout.empty() || _inLayout) {
		_layoutTimer.stop();
		return;
	}
	auto timer = QElapsedTimer();
	timer.start();
	// Newest first: they are the ones the user is closest to.
	while (!_pendingLayout.empty() && timer.elapsed() < kLayoutSliceMs) {
		const auto index = _pendingLayout.back();
		_pendingLayout.pop_back();
		if (index < int(_rows.size())) {
			layoutExactly(index, index + 1);
		}
	}
	_stats.layoutUs += timer.nsecsElapsed() / 1000;
	const auto pending = std::move(_pendingLayout);
	_inLayout = true;
	recountTops();
	const auto &padding = st::historyPadding;
	const auto wanted = std::max(
		padding.top() + _rowsHeight + padding.bottom(),
		viewportHeight());
	_rowsTop = wanted - padding.bottom() - _rowsHeight;
	if (wanted != height()) {
		resize(width(), wanted);
	}
	_inLayout = false;
	_pendingLayout = std::move(pending);
	applyAnchor();
	placeInlineVideos();
	update();
	if (_pendingLayout.empty()) {
		_layoutTimer.stop();
	}
}

void ListWidget::trimLoaded() {
	if (_rows.size() <= size_t(2 * kKeepLoadedAround)) {
		_trimTimer.stop();
		return;
	}
	const auto first = rowNear(_visibleTop) - kKeepLoadedAround;
	const auto last = rowNear(_visibleBottom) + kKeepLoadedAround;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if ((i < first || i > last) && _rows[i].element->heavyLoaded()) {
			_rows[i].element->unloadHeavy();
		}
	}
}

void ListWidget::positionInitially() {
	_positioned = true;
	if (!_scroll) {
		return;
	}
	// At the first unread message when there is more unread than fits,
	// at the end otherwise.
	auto divider = -1;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].element->flags().showUnread) {
			divider = i;
			break;
		}
	}
	if (divider >= 0) {
		const auto below = _rowsHeight - _rows[divider].top;
		if (below > viewportHeight() - 40) {
			_anchor.bottom = false;
			_inLayout = true;
			_scroll->scrollToY(std::max(rowTop(divider) - 8, 0));
			_inLayout = false;
			rememberAnchor();
			if (_atBottom != _anchor.bottom) {
				_atBottom = _anchor.bottom;
				Q_EMIT stateChanged();
			}
			return;
		}
	}
	_anchor.bottom = true;
	applyAnchor();
}

void ListWidget::checkLoadMore() {
	if (_debugMode || !_started || _rows.empty() || !_scroll) {
		return;
	}
	const auto &known = history();
	const auto viewport = std::max(viewportHeight(), 200);
	const auto now = Ui::Now();
	if (known.hasMoreBefore
		&& !_loadingOlder
		&& now >= _olderRetryAt
		&& (_scroll->scrollTop() - _rowsTop < 2 * viewport)) {
		loadOlder();
	}
	if (known.hasMoreAfter
		&& !_loadingNewer
		&& now >= _newerRetryAt
		&& (_scroll->scrollTopMax() - _scroll->scrollTop() < 2 * viewport)) {
		loadNewer();
	}
}

void ListWidget::loadOlder() {
	_loadingOlder = true;
	_topLoader.start();
	_session->data().messages().loadOlder(_chatId, this, [this](int added) {
		_loadingOlder = false;
		_topLoader.stop();
		if (added <= 0) {
			_olderRetryAt = Ui::Now() + kLoadRetryDelay;
		}
		update();
		Ui::PostponeCall(this, [this] { checkLoadMore(); });
	}, [this](const Api::Error &) {
		_loadingOlder = false;
		_topLoader.stop();
		_olderRetryAt = Ui::Now() + kLoadRetryDelay;
		update();
	});
}

void ListWidget::loadNewer() {
	_loadingNewer = true;
	_session->data().messages().loadNewer(_chatId, this, [this](int added) {
		_loadingNewer = false;
		if (added <= 0) {
			_newerRetryAt = Ui::Now() + kLoadRetryDelay;
		}
		Ui::PostponeCall(this, [this] { checkLoadMore(); });
		Q_EMIT stateChanged();
	}, [this](const Api::Error &) {
		_loadingNewer = false;
		_newerRetryAt = Ui::Now() + kLoadRetryDelay;
	});
}

// ---- navigation

void ListWidget::showAtSeq(
		qint64 seq,
		const std::optional<Data::Quote> &quote,
		bool animated) {
	const auto index = indexOfSeq(seq);
	if (index < 0 || !_scroll) {
		return;
	}
	const auto viewport = viewportHeight();
	const auto &row = _rows[index];
	const auto top = rowTop(index);
	// In the middle; a row taller than the viewport from its top.
	const auto wanted = (row.height + 40 > viewport)
		? (top - 20)
		: (top + row.height / 2 - viewport / 2);
	_anchor.bottom = false;
	if (animated && !Ui::AnimationsDisabled()) {
		_scroll->scrollToYAnimated(std::max(wanted, 0));
	} else {
		_inLayout = true;
		_scroll->finishScrollAnimation();
		_scroll->scrollToY(std::max(wanted, 0));
		_inLayout = false;
		rememberAnchor();
		_atBottom = _anchor.bottom;
		update();
		checkLoadMore();
		Q_EMIT stateChanged();
	}
	startHighlight(row.element->key(), quote);
}

void ListWidget::jumpTo(qint64 seq, const std::optional<Data::Quote> &quote) {
	if (seq <= 0) {
		return;
	}
	if (indexOfSeq(seq) >= 0) {
		_positioned = true;
		showAtSeq(seq, quote, true);
		return;
	} else if (_debugMode) {
		return;
	}
	_pendingJump = seq;
	_pendingJumpQuote = quote;
	_session->data().messages().loadAround(_chatId, seq, this, [=, this] {
		_initialLoading = false;
		if (_pendingJump == seq) {
			// Loaded, but the message itself is not there (deleted).
			if (indexOfSeq(seq) < 0) {
				_pendingJump = 0;
				_pendingJumpQuote = std::nullopt;
				_controller->showToast(tr::lng_history_message_not_found(tr::now));
				if (!_positioned && !_rows.empty()) {
					positionInitially();
				}
			} else {
				refreshRows();
			}
		}
		Q_EMIT stateChanged();
	}, [=, this](const Api::Error &error) {
		_initialLoading = false;
		if (_pendingJump == seq) {
			_pendingJump = 0;
			_pendingJumpQuote = std::nullopt;
		}
		_loadFailed = _rows.empty();
		Support::ShowError(_controller, error);
		Q_EMIT stateChanged();
	});
}

void ListWidget::jumpToBottom() {
	if (!_scroll) {
		return;
	}
	if (!_debugMode && history().hasMoreAfter) {
		_anchor.bottom = true;
		_session->data().messages().loadLatest(_chatId, this, [this] {
			_anchor.bottom = true;
			relayoutKeepingAnchor();
			Q_EMIT stateChanged();
		}, [this](const Api::Error &error) {
			Support::ShowError(_controller, error);
		});
		return;
	}
	_anchor.bottom = true;
	if (Ui::AnimationsDisabled()) {
		applyAnchor();
		update();
		checkRead();
	} else {
		_scroll->scrollToYAnimated(_scroll->scrollTopMax());
	}
}

void ListWidget::startHighlight(
		const QString &key,
		const std::optional<Data::Quote> &quote) {
	const auto previous = indexOfKey(_highlightKey);
	_highlightKey = key;
	_highlightRange = Ui::Text::Selection();
	const auto index = indexOfKey(key);
	if (index >= 0 && quote) {
		if (const auto found = FindQuote(_rows[index].element->data().text, *quote)) {
			_highlightRange = *found;
		}
	}
	_highlightStarted = Ui::Now();
	_highlightTimer.start(16, this);
	updateRow(previous);
	updateRow(index);
}

double ListWidget::highlightOpacity() const {
	if (_highlightKey.isEmpty()) {
		return 0.;
	}
	const auto passed = Ui::Now() - _highlightStarted;
	if (passed < kHighlightHold) {
		return 1.;
	} else if (passed >= kHighlightHold + kHighlightFade) {
		return 0.;
	}
	return 1. - double(passed - kHighlightHold) / kHighlightFade;
}

// ---- reading

void ListWidget::checkRead() {
	if (_active && !_debugMode && !_rows.empty()) {
		_readTimer.start(kReadDelay, this);
	}
}

void ListWidget::markReadNow() {
	if (!_active || _debugMode || _rows.empty() || !_scroll) {
		return;
	}
	auto &data = _session->data();
	const auto chat = data.chats().chat(_chatId);
	if (!chat) {
		return;
	}
	const auto &known = history();
	auto seq = qint64(0);
	if (_atBottom && !known.hasMoreAfter) {
		seq = known.lastSeq();
	} else {
		// The last message that is completely on screen.
		auto index = rowNear(_visibleBottom - 1);
		if (index > 0
			&& rowTop(index) + _rows[index].height > _visibleBottom + 4
			&& _rows[index].height < viewportHeight()) {
			--index;
		}
		for (auto i = index; i >= 0; --i) {
			if (const auto value = _rows[i].element->lastSeq(); value > 0) {
				seq = value;
				break;
			}
		}
	}
	if (seq > chat->lastReadSeq) {
		data.messages().markRead(_chatId, seq);
	}
}

// ---- painting

void ListWidget::paintEvent(QPaintEvent *e) {
	auto timer = QElapsedTimer();
	timer.start();
	auto p = Painter(this);
	const auto clip = e->rect();
	PaintWallpaper(p, clip, QPoint(0, _visibleTop), _theme);
	if (_rows.empty()) {
		return;
	}
	const auto highlight = highlightOpacity();
	const auto floating = _dateOpacity.value(_dateFloating ? 1. : 0.);
	const auto floatTop = _visibleTop + st::historyDateFloatTop;
	auto painted = 0;
	for (auto i = rowNear(clip.top()); i < int(_rows.size()); ++i) {
		auto &row = _rows[i];
		const auto top = rowTop(i);
		if (top > clip.bottom()) {
			break;
		} else if (!row.exact) {
			// Its real height is not known yet (the idle pass is on its way).
			continue;
		}
		auto context = View::PaintContext();
		context.clip = clip.translated(0, -top);
		const auto &key = row.element->key();
		context.selected = !_selected.isEmpty() && _selected.contains(key);
		if (key == _textSelectionKey) {
			context.textSelection = _textSelection;
		}
		if (highlight > 0. && key == _highlightKey) {
			context.highlight = highlight;
			context.highlightRange = _highlightRange;
		}
		if (_hover.index == i) {
			context.hovered = _hover.hit.zone;
			context.hoveredLink = _hover.hit.link;
			context.hoveredRow = _hover.hit.row;
			context.hoveredColumn = _hover.hit.column;
			context.hoveredText = _hover.hit.text;
		}
		if (floating > 0.
			&& row.element->flags().showDate
			&& top + st::historyServiceMargin.top() < floatTop) {
			// The floating pill stands in for this one.
			context.dateOpacity = 1. - floating;
		}
		p.translate(0, top);
		row.element->paint(p, context);
		p.translate(0, -top);
		++painted;
	}
	paintFloatingDate(p, clip);
	paintTopLoader(p);
	const auto elapsed = timer.nsecsElapsed() / 1000;
	++_stats.paints;
	_stats.totalUs += elapsed;
	_stats.maxUs = std::max(_stats.maxUs, elapsed);
	_stats.rows = painted;
}

void ListWidget::paintFloatingDate(Painter &p, const QRect &clip) {
	const auto opacity = _dateOpacity.value(_dateFloating ? 1. : 0.);
	if (opacity <= 0. || _rows.empty()) {
		return;
	}
	const auto first = rowNear(_visibleTop);
	const auto height = PillHeight();
	auto top = _visibleTop + st::historyDateFloatTop;
	const auto &head = *_rows[first].element;
	if (head.flags().showDate
		&& rowTop(first) + st::historyServiceMargin.top() >= top) {
		// The day's own pill is right there.
		return;
	}
	// The next day's pill pushes this one out.
	for (auto i = first + 1; i < int(_rows.size()); ++i) {
		const auto rowY = rowTop(i);
		if (rowY > top + 2 * height) {
			break;
		} else if (_rows[i].element->flags().showDate) {
			top = std::min(
				top,
				rowY + st::historyServiceMargin.top() - height - 3);
			break;
		}
	}
	const auto text = head.dateText();
	const auto &padding = st::historyServicePadding;
	const auto width = st::historyServiceFont->width(text)
		+ padding.left()
		+ padding.right();
	const auto rect = QRect((this->width() - width) / 2, top, width, height);
	if (!rect.intersects(clip)) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	p.setOpacity(opacity);
	p.setPen(Qt::NoPen);
	p.setBrush(ServiceBg());
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setPen(st::historyServiceFg);
	p.setFont(st::historyServiceFont);
	p.drawText(rect, Qt::AlignCenter, text);
	p.setOpacity(1.);
}

void ListWidget::paintTopLoader(Painter &p) {
	if (!_loadingOlder || _rows.empty() || _visibleTop - _rowsTop > 400) {
		return;
	}
	const auto size = st::historyLoadingSize;
	const auto circle = QRect(
		(width() - size) / 2,
		_visibleTop + 10,
		size,
		size);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(ServiceBg());
	p.drawEllipse(circle);
	_topLoader.draw(
		p,
		circle.marginsRemoved(QMargins(6, 6, 6, 6)),
		2.,
		st::historyServiceFg->c);
}

void ListWidget::timerEvent(QTimerEvent *e) {
	const auto id = e->timerId();
	if (id == _layoutTimer.timerId()) {
		processPendingLayout();
	} else if (id == _trimTimer.timerId()) {
		trimLoaded();
	} else if (id == _readTimer.timerId()) {
		_readTimer.stop();
		markReadNow();
	} else if (id == _dateTimer.timerId()) {
		_dateTimer.stop();
		if (_dateFloating) {
			_dateFloating = false;
			_dateOpacity.start(
				[this] { update(0, _visibleTop, width(), 80); },
				1.,
				0.,
				st::motion);
			update(0, _visibleTop, width(), 80);
		}
	} else if (id == _highlightTimer.timerId()) {
		const auto index = indexOfKey(_highlightKey);
		if (highlightOpacity() <= 0. || index < 0) {
			_highlightTimer.stop();
			_highlightKey = QString();
			_highlightRange = Ui::Text::Selection();
		}
		updateRow(index);
	} else if (id == _dragScrollTimer.timerId()) {
		checkDragScroll(mapFromGlobal(_lastMouseGlobal));
	} else if (id == _spinTimer.timerId()) {
		repaintSpinners();
	} else if (id == _liveTimer.timerId()) {
		// «زنده تا …» turns into «تمام شد» by itself.
		for (auto i = 0; i != int(_rows.size()); ++i) {
			if (_rows[i].element->data().kind == Data::MessageKind::LiveLocation) {
				_rows[i].element->invalidate();
				_rows[i].exact = false;
			}
		}
		relayoutKeepingAnchor();
		update();
	} else {
		RpWidget::timerEvent(e);
	}
}

} // namespace History
