#include "media/media_shared.h"

#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_messages.h"
#include "data/data_session.h"
#include "data/data_shared_media.h"
#include "data/data_users.h"
#include "history/history_boxes.h"
#include "history/history_downloads.h"
#include "history/history_logic.h"
#include "lang/lang_format.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_audio_player.h"
#include "media/media_image_viewer.h"
#include "media/media_shared_logic.h"
#include "media/media_video_player.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/top_bar.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_media_shared.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSet>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>

#include <array>

namespace Media {
namespace {

using namespace Qt::StringLiterals;
using Data::SharedMediaTab;
using Controller = Window::SessionController;

constexpr auto kPageSize = 40;
constexpr auto kLoadMoreThreshold = 900;
constexpr auto kMaxGifs = 2;
// How far around the visible part a gif keeps playing.
constexpr auto kGifMargin = 100;

[[nodiscard]] QString TabTitle(SharedMediaTab tab) {
	switch (tab) {
	case SharedMediaTab::Media: return tr::lng_media_shared_tab_media(tr::now);
	case SharedMediaTab::Files: return tr::lng_media_shared_tab_files(tr::now);
	case SharedMediaTab::Links: return tr::lng_media_shared_tab_links(tr::now);
	case SharedMediaTab::Music: return tr::lng_media_shared_tab_music(tr::now);
	case SharedMediaTab::Voice: return tr::lng_media_shared_tab_voice(tr::now);
	case SharedMediaTab::Gifs: return tr::lng_media_shared_tab_gifs(tr::now);
	}
	return QString();
}

[[nodiscard]] QString TabEmpty(SharedMediaTab tab) {
	switch (tab) {
	case SharedMediaTab::Media: return tr::lng_media_shared_empty_media(tr::now);
	case SharedMediaTab::Files: return tr::lng_media_shared_empty_files(tr::now);
	case SharedMediaTab::Links: return tr::lng_media_shared_empty_links(tr::now);
	case SharedMediaTab::Music: return tr::lng_media_shared_empty_music(tr::now);
	case SharedMediaTab::Voice: return tr::lng_media_shared_empty_voice(tr::now);
	case SharedMediaTab::Gifs: return tr::lng_media_shared_empty_gifs(tr::now);
	}
	return QString();
}

// «۱۲۸ عکس و ویدیو», «۱۰٬۰۰۰+ فایل» when the server stopped counting.
[[nodiscard]] QString CountText(SharedMediaTab tab, int count, bool capped) {
	if (capped) {
		const auto n = Lang::Num(qint64(count));
		switch (tab) {
		case SharedMediaTab::Media: return tr::lng_media_shared_capped_media(tr::now, lt_n, n);
		case SharedMediaTab::Files: return tr::lng_media_shared_capped_files(tr::now, lt_n, n);
		case SharedMediaTab::Links: return tr::lng_media_shared_capped_links(tr::now, lt_n, n);
		case SharedMediaTab::Music: return tr::lng_media_shared_capped_music(tr::now, lt_n, n);
		case SharedMediaTab::Voice: return tr::lng_media_shared_capped_voice(tr::now, lt_n, n);
		case SharedMediaTab::Gifs: return tr::lng_media_shared_capped_gifs(tr::now, lt_n, n);
		}
		return QString();
	}
	switch (tab) {
	case SharedMediaTab::Media: return tr::lng_media_shared_count_media(tr::now, lt_count, count);
	case SharedMediaTab::Files: return tr::lng_media_shared_count_files(tr::now, lt_count, count);
	case SharedMediaTab::Links: return tr::lng_media_shared_count_links(tr::now, lt_count, count);
	case SharedMediaTab::Music: return tr::lng_media_shared_count_music(tr::now, lt_count, count);
	case SharedMediaTab::Voice: return tr::lng_media_shared_count_voice(tr::now, lt_count, count);
	case SharedMediaTab::Gifs: return tr::lng_media_shared_count_gifs(tr::now, lt_count, count);
	}
	return QString();
}

[[nodiscard]] QString MonthTitle(const QDate &date) {
	if (!date.isValid()) {
		return QString();
	} else if (Lang::IsPersian()) {
		const auto jalali = Lang::ToJalali(date);
		return Lang::JalaliMonthName(jalali.month)
			+ u' '
			+ Lang::Num(qint64(jalali.year));
	}
	return Lang::GregorianMonthName(date.month())
		+ u' '
		+ QString::number(date.year());
}

[[nodiscard]] QDate MessageDate(const Data::Message &message) {
	return QDateTime::fromMSecsSinceEpoch(message.createdAt).date();
}

// The extension of a file's name for its tile («PDF»), at most 4 letters.
[[nodiscard]] QString Extension(const QString &name) {
	const auto suffix = QFileInfo(name).suffix().toUpper();
	return (suffix.isEmpty() || suffix.size() > 4) ? QString() : suffix;
}

// One of a few colours for a file's tile, by its extension (as Telegram).
[[nodiscard]] QColor ExtensionColor(const QString &extension) {
	static const auto kColors = std::array<QColor, 4>{
		QColor(0x3d, 0x8b, 0xd8), // documents
		QColor(0x47, 0xa4, 0x4f), // tables, archives
		QColor(0xe0, 0x5a, 0x4f), // pdf, presentations
		QColor(0xe0, 0x9b, 0x2d), // everything else
	};
	const auto e = extension.toLower();
	if (e == u"doc" || e == u"docx" || e == u"txt" || e == u"rtf" || e == u"odt") {
		return kColors[0];
	} else if (e == u"xls" || e == u"xlsx" || e == u"csv" || e == u"zip"
		|| e == u"rar" || e == u"7z" || e == u"gz" || e == u"tar") {
		return kColors[1];
	} else if (e == u"pdf" || e == u"ppt" || e == u"pptx" || e == u"key") {
		return kColors[2];
	}
	return kColors[3];
}

[[nodiscard]] QString FileName(const Data::Message &message) {
	const auto name = message.text.trimmed();
	return name.isEmpty()
		? History::Downloads::SuggestedName(message)
		: name;
}

void ShowInFileManager(const QString &path) {
	const auto info = QFileInfo(path);
#ifdef Q_OS_WIN
	auto explorer = QProcess();
	explorer.setProgram(u"explorer.exe"_s);
	explorer.setNativeArguments(u"/select,\"%1\""_s.arg(
		QDir::toNativeSeparators(info.absoluteFilePath())));
	explorer.startDetached();
#else // Q_OS_WIN
	QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif // Q_OS_WIN
}

void LaunchFile(const QString &path) {
	// Programs and scripts are shown, never started by a click.
	if (History::Downloads::IsExecutable(path)) {
		ShowInFileManager(path);
	} else {
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	}
}

// «۲۱٫۴ MB · ۲۰ مهر ۱۴۰۵»: each part keeps its own direction.
[[nodiscard]] QString StatusLine(const QStringList &parts) {
	auto isolated = QStringList();
	for (const auto &part : parts) {
		isolated.push_back(Lang::Isolated(part));
	}
	return isolated.join(u" · "_s);
}

// ---- the list of one tab at a time

class List final : public Ui::RpWidget {
public:
	List(QWidget *parent, Controller *controller, const QString &chatId);

	// Called when the rows of a tab appear or the counts change what to show.
	void setTabsChangedCallback(Fn<void()> callback) {
		_tabsChanged = std::move(callback);
	}
	void setSelectionChangedCallback(Fn<void()> callback) {
		_selectionChanged = std::move(callback);
	}

	void setTab(SharedMediaTab tab);
	[[nodiscard]] SharedMediaTab tab() const {
		return _tab;
	}
	void setVisibleRange(int top, int bottom);

	[[nodiscard]] const std::optional<Data::SharedMediaCounts> &counts() const {
		return _counts;
	}
	[[nodiscard]] std::array<bool, Data::kSharedMediaTabs> hasRows() const;
	[[nodiscard]] std::array<bool, Data::kSharedMediaTabs> unknown() const;

	[[nodiscard]] int selectedCount() const {
		return int(_selected.size());
	}
	[[nodiscard]] QVector<Data::Message> selectedMessages() const;
	[[nodiscard]] bool canForwardSelected() const;
	void clearSelection();
	void removeSeqs(const QSet<qint64> &seqs);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;

private:
	struct TabState {
		QVector<Data::Message> server; // newest first
		QVector<Data::Message> shown; // merged with the cache, newest first
		bool serverRead = false;
		bool loading = false;
		bool failed = false;
		bool more = true;
		QString error;
		int generation = 0;
	};
	struct Entry {
		int item = -1; // -1: a month heading
		QString title; // of a heading
		QRect rect;
	};
	struct InlineGif {
		qint64 seq = 0;
		Data::MediaHandlePtr handle;
		QPointer<VideoPlayerWidget> widget;
	};

	[[nodiscard]] TabState &state() {
		return _tabs[int(_tab)];
	}
	[[nodiscard]] const TabState &state() const {
		return _tabs[int(_tab)];
	}
	[[nodiscard]] bool grid() const {
		return Data::SharedMediaIsGrid(_tab);
	}
	[[nodiscard]] QVector<Data::Message> cached(SharedMediaTab tab) const;
	void rebuild(SharedMediaTab tab);
	void rebuildAll();
	void relayout();
	void load(SharedMediaTab tab);
	void loadCounts();
	void maybeLoadMore();
	void notifyTabs();

	[[nodiscard]] int entryAt(QPoint position) const;
	[[nodiscard]] QRect stateRect() const;
	[[nodiscard]] bool stateIsLink() const;
	[[nodiscard]] QString stateText() const;
	[[nodiscard]] int rowHeight() const;

	[[nodiscard]] std::shared_ptr<Ui::ImageSource> image(const Data::Message &message);
	[[nodiscard]] std::shared_ptr<Ui::ImageSource> linkImage(const Data::Message &message);
	void paintMonth(Painter &p, const Entry &entry);
	void paintTile(Painter &p, const Data::Message &message, const QRect &rect, bool over);
	void paintRow(Painter &p, const Data::Message &message, const QRect &rect, bool over);
	void paintCheck(Painter &p, QRect circle, bool checked, bool onMedia);
	void paintRowIcon(Painter &p, const Data::Message &message, const QRect &box);
	void paintCover(Painter &p, const QImage &picture, const QRectF &rect);

	void open(const Data::Message &message);
	void openViewer(const Data::Message &message);
	[[nodiscard]] bool inViewer(
		SharedMediaTab tab,
		const Data::Message &message,
		qint64 clicked) const;
	[[nodiscard]] ViewerItem viewerItem(const Data::Message &message);
	void viewerPage(
		SharedMediaTab tab,
		qint64 clicked,
		const QString &edgeKey,
		std::function<void(std::vector<ViewerItem>, bool)> done);
	[[nodiscard]] QRect viewerOrigin(const QString &key) const;
	void pageArrived(SharedMediaTab tab);
	void openFile(const Data::Message &message);
	void showMenu(const Data::Message &message, QPoint globalPosition);
	void showInChat(qint64 seq);
	void toggleSelected(const Data::Message &message);
	void saveAs(const Data::Message &message);

	void updateGifs();
	void placeGifs();
	void clearGifs();

	Controller *_controller = nullptr;
	Main::Session *_session = nullptr;
	QString _chatId;
	SharedMediaTab _tab = SharedMediaTab::Media;
	std::array<TabState, Data::kSharedMediaTabs> _tabs;
	std::optional<Data::SharedMediaCounts> _counts;
	bool _countsFailed = false;
	bool _unsupported = false; // the server has no media pages for this chat
	std::vector<Entry> _entries;
	QHash<qint64, std::shared_ptr<Ui::ImageSource>> _images;
	QHash<QString, std::shared_ptr<Ui::ImageSource>> _linkImages;
	QMap<qint64, Data::Message> _selected; // by seq
	std::vector<InlineGif> _gifs;
	QSet<qint64> _gifFailed;
	History::Downloads *_downloads = nullptr;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	int _zoom = 0;
	int _over = -1;
	bool _overState = false;
	int _pressed = -1;
	Fn<void()> _tabsChanged;
	Fn<void()> _selectionChanged;
	// Viewers waiting for the next page of a tab.
	std::vector<std::pair<SharedMediaTab, Fn<void()>>> _pageWaiters;

};

List::List(QWidget *parent, Controller *controller, const QString &chatId)
: RpWidget(parent)
, _controller(controller)
, _session(&controller->session())
, _chatId(chatId)
, _downloads(History::Downloads::For(_session)) {
	setMouseTracking(true);
	auto &data = _session->data();
	connect(&data.messages(), &Data::Messages::historyChanged, this, [this](
			const QString &chat) {
		if (chat == _chatId) {
			rebuildAll();
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this] { update(); });
	connect(_downloads, &History::Downloads::changed, this, [this](
			const QString &chat,
			qint64 seq) {
		Q_UNUSED(seq);
		if (chat == _chatId) {
			update();
		}
	});
	connect(_downloads, &History::Downloads::failed, this, [this](
			const QString &chat,
			qint64 seq) {
		if (chat != _chatId || !isVisible()) {
			return;
		}
		auto state = History::View::DownloadState();
		for (const auto &tab : _tabs) {
			for (const auto &message : tab.shown) {
				if (message.seq == seq) {
					state = _downloads->state(message);
				}
			}
		}
		_controller->showToast(state.oversized
			? tr::lng_chat_file_larger_than_declared(tr::now)
			: state.gone
			? tr::lng_chat_file_isnt_available(tr::now)
			: tr::lng_chat_file_download_failed(tr::now));
	});
	const auto player = &Player();
	connect(player, &AudioPlayer::stateChanged, this, [this] { update(); });
	connect(player, &AudioPlayer::trackChanged, this, [this] { update(); });
	connect(player, &AudioPlayer::positionChanged, this, [this] { update(); });
	style::OnPaletteChanged(this, [this] { update(); });
	for (auto i = 0; i != Data::kSharedMediaTabs; ++i) {
		rebuild(SharedMediaTab(i));
	}
	loadCounts();
}

QVector<Data::Message> List::cached(SharedMediaTab tab) const {
	auto result = QVector<Data::Message>();
	const auto &items = _session->data().messages().history(_chatId).items;
	for (auto i = items.size(); i != 0;) {
		const auto &message = items[--i];
		if (Data::SharedMediaMatches(tab, message)) {
			result.push_back(message);
		}
	}
	return result;
}

void List::rebuild(SharedMediaTab tab) {
	auto &state = _tabs[int(tab)];
	const auto &history = _session->data().messages().history(_chatId);
	const auto local = cached(tab);
	auto serverSeqs = QVector<qint64>();
	auto bySeq = QHash<qint64, const Data::Message*>();
	for (const auto &message : std::as_const(state.server)) {
		if (const auto known = history.find(message.seq)) {
			// Deleted or held back since the page came: it leaves the tab.
			if (!known->visible()) {
				continue;
			}
		}
		serverSeqs.push_back(message.seq);
		bySeq.insert(message.seq, &message);
	}
	auto localSeqs = QVector<qint64>();
	for (const auto &message : local) {
		localSeqs.push_back(message.seq);
		// The cache knows the message as it is now (a reaction, «played»).
		bySeq.insert(message.seq, &message);
	}
	const auto seqs = Data::MergeSharedMedia(
		serverSeqs,
		state.serverRead && !_unsupported,
		localSeqs,
		(tab == SharedMediaTab::Links));
	auto shown = QVector<Data::Message>();
	shown.reserve(seqs.size());
	for (const auto seq : seqs) {
		if (const auto message = bySeq.value(seq)) {
			shown.push_back(*message);
		}
	}
	state.shown = std::move(shown);
	for (auto i = _selected.begin(); i != _selected.end();) {
		const auto present = std::any_of(
			_tabs.begin(),
			_tabs.end(),
			[&](const TabState &other) {
				return std::any_of(
					other.shown.begin(),
					other.shown.end(),
					[&](const Data::Message &message) {
						return message.seq == i.key();
					});
			});
		i = present ? std::next(i) : _selected.erase(i);
	}
}

void List::rebuildAll() {
	const auto selected = _selected.size();
	auto hadRows = hasRows();
	for (auto i = 0; i != Data::kSharedMediaTabs; ++i) {
		rebuild(SharedMediaTab(i));
	}
	relayout();
	if (hadRows != hasRows()) {
		notifyTabs();
	}
	if (selected != _selected.size() && _selectionChanged) {
		_selectionChanged();
	}
}

std::array<bool, Data::kSharedMediaTabs> List::hasRows() const {
	auto result = std::array<bool, Data::kSharedMediaTabs>();
	for (auto i = 0; i != Data::kSharedMediaTabs; ++i) {
		result[i] = !_tabs[i].shown.isEmpty();
	}
	return result;
}

std::array<bool, Data::kSharedMediaTabs> List::unknown() const {
	auto result = std::array<bool, Data::kSharedMediaTabs>();
	for (auto i = 0; i != Data::kSharedMediaTabs; ++i) {
		const auto &state = _tabs[i];
		// Without the counts a tab is offered until its first page says
		// it is empty; a failed page still shows (and says so).
		result[i] = !_unsupported
			&& !_counts
			&& (!state.serverRead || state.failed);
	}
	if (_unsupported) {
		// Nothing is known to be empty: the cache is all there is.
		result.fill(true);
	}
	return result;
}

void List::notifyTabs() {
	if (_tabsChanged) {
		_tabsChanged();
	}
}

void List::loadCounts() {
	Data::LoadSharedMediaCounts(
		&_session->data(),
		_chatId,
		base::Guard(this),
		[=, this](const Data::SharedMediaCounts &counts) {
			_counts = counts;
			notifyTabs();
		},
		[=, this](const Api::Error &error) {
			_countsFailed = true;
			if (Data::SharedMediaUnsupported(error)) {
				_unsupported = true;
				rebuildAll();
			} else {
				// Without the counts every tab reads its first page and
				// shows once it has something.
				for (auto i = 0; i != Data::kSharedMediaTabs; ++i) {
					if (!_tabs[i].serverRead && !_tabs[i].loading) {
						load(SharedMediaTab(i));
					}
				}
			}
			notifyTabs();
		});
}

void List::setTab(SharedMediaTab tab) {
	if (_tab != tab) {
		clearGifs();
	}
	_tab = tab;
	_over = -1;
	_pressed = -1;
	auto &state = this->state();
	if (!state.serverRead && !state.loading && !_unsupported) {
		load(tab);
	}
	relayout();
}

void List::setVisibleRange(int top, int bottom) {
	_visibleTop = top;
	_visibleBottom = bottom;
	maybeLoadMore();
	updateGifs();
}

void List::maybeLoadMore() {
	const auto &state = this->state();
	if (state.serverRead
		&& state.more
		&& !state.loading
		&& !state.failed
		&& !_unsupported
		&& _visibleBottom + kLoadMoreThreshold >= height()) {
		load(_tab);
	}
}

void List::load(SharedMediaTab tab) {
	auto &state = _tabs[int(tab)];
	if (state.loading || _unsupported) {
		return;
	}
	state.loading = true;
	state.failed = false;
	const auto generation = ++state.generation;
	const auto before = state.server.isEmpty() ? 0 : state.server.back().seq;
	if (tab == _tab) {
		relayout();
	}
	Data::LoadSharedMedia(
		&_session->data(),
		_chatId,
		Data::SharedMediaKinds(tab),
		before,
		kPageSize,
		base::Guard(this),
		[=, this](const Data::SharedMediaPage &page) {
			auto &state = _tabs[int(tab)];
			if (state.generation != generation) {
				return;
			}
			state.loading = false;
			state.serverRead = true;
			state.more = page.hasMoreBefore;
			const auto oldest = state.server.isEmpty()
				? std::numeric_limits<qint64>::max()
				: state.server.back().seq;
			for (const auto &message : page.messages) {
				if (message.seq < oldest) {
					state.server.push_back(message);
				}
			}
			const auto had = hasRows();
			rebuild(tab);
			if (tab == _tab) {
				relayout();
				Ui::PostponeCall(this, [this] { maybeLoadMore(); });
			}
			if (had != hasRows() || !_counts) {
				notifyTabs();
			}
			pageArrived(tab);
		},
		[=, this](const Api::Error &error) {
			auto &state = _tabs[int(tab)];
			if (state.generation != generation) {
				return;
			}
			state.loading = false;
			if (Data::SharedMediaUnsupported(error)) {
				_unsupported = true;
				rebuildAll();
				notifyTabs();
				pageArrived(tab);
				return;
			}
			state.failed = true;
			state.error = Support::ErrorText(error, tr::lng_error_load(tr::now));
			if (tab == _tab) {
				relayout();
			}
			if (!_counts) {
				notifyTabs();
			}
			pageArrived(tab);
		});
}

int List::rowHeight() const {
	return (_tab == SharedMediaTab::Links)
		? int(st::sharedRowLinkHeight)
		: int(st::sharedRowHeight);
}

void List::relayout() {
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
	Ui::PostponeCall(this, [this] {
		placeGifs();
		updateGifs();
	});
}

int List::resizeGetHeight(int newWidth) {
	_entries.clear();
	const auto &padding = st::sharedPadding;
	const auto inner = std::max(newWidth - padding.left() - padding.right(), 1);
	const auto &items = state().shown;
	auto keys = QVector<int>();
	keys.reserve(items.size());
	const auto jalali = Lang::IsPersian();
	for (const auto &message : items) {
		keys.push_back(SharedMonthKey(MessageDate(message), jalali));
	}
	const auto starts = SharedMonthStarts(keys);
	auto top = padding.top();
	const auto columns = SharedGridColumns(
		inner,
		st::sharedTileMin,
		st::sharedTileSkip,
		st::sharedGridColumnsMin,
		st::sharedGridColumnsMax,
		_zoom);
	const auto tile = SharedTileSize(inner, columns, st::sharedTileSkip);
	const auto skip = int(st::sharedTileSkip);
	const auto row = rowHeight();
	for (auto s = 0; s != int(starts.size()); ++s) {
		const auto from = starts[s];
		const auto till = (s + 1 < int(starts.size()))
			? starts[s + 1]
			: int(items.size());
		auto heading = Entry();
		heading.title = MonthTitle(MessageDate(items[from]));
		heading.rect = QRect(padding.left(), top, inner, st::sharedMonthHeight);
		_entries.push_back(heading);
		top += st::sharedMonthHeight;
		if (grid()) {
			for (auto i = from; i != till; ++i) {
				const auto index = i - from;
				auto entry = Entry();
				entry.item = i;
				entry.rect = style::rtlrect(
					padding.left() + (index % columns) * (tile + skip),
					top + (index / columns) * (tile + skip),
					tile,
					tile,
					newWidth);
				_entries.push_back(entry);
			}
			const auto rows = (till - from + columns - 1) / columns;
			top += rows * tile + std::max(rows - 1, 0) * skip;
		} else {
			for (auto i = from; i != till; ++i) {
				auto entry = Entry();
				entry.item = i;
				entry.rect = QRect(padding.left(), top, inner, row);
				_entries.push_back(entry);
				top += row;
			}
		}
	}
	const auto &state = this->state();
	if (items.isEmpty()) {
		if (state.loading || state.failed || state.serverRead || _unsupported) {
			top += st::sharedEmptyHeight;
		}
	} else if (state.loading || state.failed) {
		top += st::sharedStateHeight;
	}
	return top + padding.bottom();
}

QRect List::stateRect() const {
	const auto &state = this->state();
	if (state.shown.isEmpty()) {
		if (!state.loading && !state.failed && !state.serverRead && !_unsupported) {
			return QRect();
		}
		const auto bottom = height() - st::sharedPadding.bottom();
		return QRect(0, bottom - st::sharedEmptyHeight, width(), st::sharedEmptyHeight);
	} else if (!state.loading && !state.failed) {
		return QRect();
	}
	const auto bottom = height() - st::sharedPadding.bottom();
	return QRect(0, bottom - st::sharedStateHeight, width(), st::sharedStateHeight);
}

bool List::stateIsLink() const {
	const auto &state = this->state();
	return !state.loading && state.failed;
}

QString List::stateText() const {
	const auto &state = this->state();
	return state.loading
		? tr::lng_loading(tr::now)
		: state.failed
		? (state.error + u" · "_s + tr::lng_try_again(tr::now))
		: TabEmpty(_tab);
}

int List::entryAt(QPoint position) const {
	for (auto i = 0; i != int(_entries.size()); ++i) {
		if (_entries[i].item >= 0 && _entries[i].rect.contains(position)) {
			return _entries[i].item;
		}
	}
	return -1;
}

std::shared_ptr<Ui::ImageSource> List::image(const Data::Message &message) {
	auto &result = _images[message.seq];
	if (!result) {
		result = Support::MakeMessageImageSource(_session, message);
		if (result) {
			connect(result.get(), &Ui::ImageSource::updated, this, [this] {
				update();
			});
		}
	}
	return result;
}

std::shared_ptr<Ui::ImageSource> List::linkImage(const Data::Message &message) {
	if (!message.linkPreview || message.linkPreview->imageUrl.isEmpty()) {
		return nullptr;
	}
	const auto url = message.linkPreview->imageUrl;
	auto &result = _linkImages[url];
	if (!result) {
		result = Support::MakeImageSource(
			_session,
			Data::MediaRef::Url(url),
			Data::MediaVariant::Thumb);
		if (result) {
			connect(result.get(), &Ui::ImageSource::updated, this, [this] {
				update();
			});
		}
	}
	return result;
}

void List::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto &items = state().shown;
	for (const auto &entry : _entries) {
		if (!entry.rect.intersects(clip)) {
			continue;
		} else if (entry.item < 0) {
			paintMonth(p, entry);
		} else if (grid()) {
			paintTile(p, items[entry.item], entry.rect, entry.item == _over);
		} else {
			paintRow(p, items[entry.item], entry.rect, entry.item == _over);
		}
	}
	const auto state = stateRect();
	if (!state.isEmpty() && state.intersects(clip)) {
		const auto empty = this->state().shown.isEmpty();
		p.setFont(empty ? st::sharedEmptyFont : st::sharedStateFont);
		p.setPen((stateIsLink() && _overState)
			? st::brandPrimary
			: stateIsLink()
			? st::brandText
			: st::textTertiary);
		p.drawText(state, Qt::AlignCenter, stateText());
	}
}

void List::paintMonth(Painter &p, const Entry &entry) {
	p.setFont(st::sharedMonthFont);
	p.setPen(st::sharedMonthFg);
	p.drawTextLeft(
		entry.rect.x() + st::sharedMonthLeft,
		entry.rect.y() + (entry.rect.height() - st::sharedMonthFont->height) / 2,
		width(),
		entry.title);
}

void List::paintCover(Painter &p, const QImage &picture, const QRectF &rect) {
	// The middle of the picture fills the rectangle.
	const auto size = picture.size();
	const auto scale = std::max(
		rect.width() / double(std::max(size.width(), 1)),
		rect.height() / double(std::max(size.height(), 1)));
	const auto part = QSizeF(rect.width() / scale, rect.height() / scale);
	const auto from = QRectF(
		(size.width() - part.width()) / 2.,
		(size.height() - part.height()) / 2.,
		part.width(),
		part.height());
	p.drawImage(rect, picture, from);
}

void List::paintCheck(Painter &p, QRect circle, bool checked, bool onMedia) {
	auto hq = PainterHighQualityEnabler(p);
	const auto border = double(st::sharedCheckBorder);
	const auto inner = QRectF(circle).marginsRemoved(
		QMarginsF(border / 2., border / 2., border / 2., border / 2.));
	if (checked) {
		p.setPen(QPen(st::staticWhite->c, border));
		p.setBrush(st::brandPrimary);
		p.drawEllipse(inner);
		st::sharedCheckIcon.paintInCenter(p, circle);
	} else {
		p.setPen(QPen(onMedia ? st::staticWhite->c : st::textTertiary->c, border));
		p.setBrush(onMedia ? QColor(0, 0, 0, 0x40) : Qt::transparent);
		p.drawEllipse(inner);
	}
}

void List::paintTile(
		Painter &p,
		const Data::Message &message,
		const QRect &rect,
		bool over) {
	const auto selected = _selected.contains(message.seq);
	auto hq = PainterHighQualityEnabler(p);
	auto path = QPainterPath();
	path.addRoundedRect(rect, st::sharedTileRadius, st::sharedTileRadius);
	p.save();
	p.setClipPath(path);
	p.fillRect(rect, st::skeletonBg);
	if (const auto source = image(message)) {
		auto picture = message.mediaSpoiler
			? source->preview()
			: source->request(rect.size() * devicePixelRatioF());
		if (picture.isNull()) {
			picture = source->preview();
		}
		if (!picture.isNull()) {
			paintCover(p, picture, QRectF(rect));
		}
	}
	if (over) {
		p.fillRect(rect, st::stateHover);
	}
	if (selected) {
		p.fillRect(rect, st::sharedSelectedDim);
	}
	p.restore();

	auto badge = QString();
	if (message.kind == Data::MessageKind::Video && message.media) {
		badge = Lang::Duration(int(message.media->durationMs / 1000));
	}
	const auto gif = (message.kind == Data::MessageKind::Gif);
	if (!badge.isEmpty() || (gif && _tab != SharedMediaTab::Gifs)) {
		const auto &font = st::sharedBadgeFont;
		const auto &padding = st::sharedBadgePadding;
		const auto text = gif ? tr::lng_media_gif(tr::now) : badge;
		const auto boxWidth = font->width(text) + padding.left() + padding.right();
		// At the leading bottom corner of the square.
		const auto box = QRect(
			style::RightToLeft()
				? (rect.x() + rect.width() - st::sharedBadgeSkip - boxWidth)
				: (rect.x() + st::sharedBadgeSkip),
			rect.y() + rect.height() - st::sharedBadgeSkip
				- font->height - padding.top() - padding.bottom(),
			boxWidth,
			font->height + padding.top() + padding.bottom());
		p.setPen(Qt::NoPen);
		p.setBrush(st::overlayScrim);
		p.drawRoundedRect(box, box.height() / 2., box.height() / 2.);
		p.setFont(font);
		p.setPen(st::staticWhite);
		p.drawText(box, Qt::AlignCenter, text);
	}
	if (!_selected.isEmpty()) {
		const auto size = int(st::sharedCheckSize);
		// At the trailing top corner of the square.
		const auto x = style::RightToLeft()
			? (rect.x() + st::sharedCheckSkip)
			: (rect.x() + rect.width() - size - st::sharedCheckSkip);
		paintCheck(
			p,
			QRect(x, rect.y() + st::sharedCheckSkip, size, size),
			selected,
			true);
	}
}

void List::paintRowIcon(
		Painter &p,
		const Data::Message &message,
		const QRect &box) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	switch (_tab) {
	case SharedMediaTab::Files: {
		const auto extension = Extension(FileName(message));
		const auto download = _downloads->state(message);
		const auto saved = !_downloads->savedPath(message).isEmpty();
		p.setBrush(ExtensionColor(extension));
		p.drawRoundedRect(box, st::sharedRowIconRadius, st::sharedRowIconRadius);
		if (download.loading) {
			const auto ring = QRectF(box).marginsRemoved(QMarginsF(10, 10, 10, 10));
			p.setPen(QPen(st::textOnBrand->c, st::sharedProgressWidth));
			p.setBrush(Qt::NoBrush);
			p.drawArc(
				ring,
				90 * 16,
				-int(std::lround(std::clamp(download.progress, 0.02, 1.) * 360 * 16)));
		} else if (!saved) {
			st::sharedIconDownload.paintInCenter(p, box);
		} else if (!extension.isEmpty()) {
			p.setFont(st::sharedRowExtFont);
			p.setPen(st::textOnBrand);
			p.drawText(box, Qt::AlignCenter, extension);
		} else {
			st::sharedIconFile.paintInCenter(p, box);
		}
	} break;
	case SharedMediaTab::Links: {
		p.setBrush(st::brandSoft);
		p.drawRoundedRect(box, st::sharedRowIconRadius, st::sharedRowIconRadius);
		auto picture = QImage();
		if (const auto source = linkImage(message)) {
			picture = source->request(box.size() * devicePixelRatioF());
		}
		if (!picture.isNull()) {
			auto path = QPainterPath();
			path.addRoundedRect(box, st::sharedRowIconRadius, st::sharedRowIconRadius);
			p.save();
			p.setClipPath(path);
			paintCover(p, picture, QRectF(box));
			p.restore();
		} else {
			const auto link = Data::SharedMediaLink(message);
			const auto host = QUrl(link).host();
			auto letter = host.startsWith(u"www."_s) ? host.mid(4) : host;
			letter = letter.left(1).toUpper();
			if (letter.isEmpty()) {
				st::sharedIconLink.paintInCenter(p, box);
			} else {
				p.setFont(st::sharedRowLetterFont);
				p.setPen(st::brandText);
				p.drawText(box, Qt::AlignCenter, letter);
			}
		}
	} break;
	case SharedMediaTab::Voice:
		if (message.kind == Data::MessageKind::VideoNote) {
			auto path = QPainterPath();
			path.addEllipse(QRectF(box));
			p.save();
			p.setClipPath(path);
			p.fillRect(box, st::skeletonBg);
			if (const auto source = image(message)) {
				auto picture = source->request(box.size() * devicePixelRatioF());
				if (picture.isNull()) {
					picture = source->preview();
				}
				if (!picture.isNull()) {
					paintCover(p, picture, QRectF(box));
				}
			}
			p.restore();
			st::sharedIconVideoNote.paintInCenter(p, box);
			break;
		}
		[[fallthrough]];
	case SharedMediaTab::Music: {
		const auto id = AudioTrack::IdForMessage(message.chat, message.seq);
		const auto &player = Player();
		p.setBrush(st::brandPrimary);
		p.drawEllipse(box);
		if (player.isLoading(id)) {
			p.setPen(QPen(st::textOnBrand->c, st::sharedProgressWidth));
			p.setBrush(Qt::NoBrush);
			const auto ring = QRectF(box).marginsRemoved(QMarginsF(5, 5, 5, 5));
			p.drawArc(ring, 90 * 16, -90 * 16);
			st::sharedIconPause.paintInCenter(p, box);
		} else {
			(player.isPlaying(id)
				? st::sharedIconPause
				: st::sharedIconPlay).paintInCenter(p, box);
		}
	} break;
	case SharedMediaTab::Media:
	case SharedMediaTab::Gifs:
		break;
	}
}

void List::paintRow(
		Painter &p,
		const Data::Message &message,
		const QRect &rect,
		bool over) {
	const auto outer = width();
	const auto selected = _selected.contains(message.seq);
	if (over || selected) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(selected ? st::stateSelected : st::stateHover);
		p.drawRoundedRect(rect, st::sharedRowRadius, st::sharedRowRadius);
	}
	const auto &padding = st::sharedRowPadding;
	auto left = rect.x() + padding.left();
	if (!_selected.isEmpty()) {
		const auto size = int(st::sharedCheckSize);
		paintCheck(
			p,
			style::rtlrect(left, rect.y() + (rect.height() - size) / 2, size, size, outer),
			selected,
			false);
		left += size + st::sharedRowIconSkip;
	}
	const auto size = int(st::sharedRowIcon);
	const auto iconTop = (_tab == SharedMediaTab::Links)
		? (rect.y() + st::sharedRowTitleTop - 2)
		: (rect.y() + (rect.height() - size) / 2);
	paintRowIcon(p, message, style::rtlrect(left, iconTop, size, size, outer));
	left += size + st::sharedRowIconSkip;
	const auto available = std::max(
		rect.x() + rect.width() - padding.right() - left,
		1);

	const auto date = Lang::SolarDate(MessageDate(message), true);
	auto title = QString();
	auto status = QStringList();
	auto third = QString();
	const auto id = AudioTrack::IdForMessage(message.chat, message.seq);
	const auto &player = Player();
	const auto current = player.isCurrent(id) && !player.isLoading(id);
	const auto duration = message.media ? message.media->durationMs : 0;
	switch (_tab) {
	case SharedMediaTab::Files: {
		title = FileName(message);
		const auto download = _downloads->state(message);
		if (download.loading && message.media && message.media->size > 0) {
			status.push_back(Lang::Bytes(qint64(message.media->size * download.progress))
				+ u" / "_s
				+ Lang::Bytes(message.media->size));
		} else if (message.media && message.media->size > 0) {
			status.push_back(Lang::Bytes(message.media->size));
		}
		status.push_back(date);
	} break;
	case SharedMediaTab::Links: {
		const auto link = Data::SharedMediaLink(message);
		const auto &preview = message.linkPreview;
		title = (preview && !preview->title.isEmpty())
			? preview->title
			: (preview && !preview->siteName.isEmpty())
			? preview->siteName
			: QUrl(link).host();
		if (title.isEmpty()) {
			title = link;
		}
		status.push_back(Lang::Ltr(link));
		third = (preview && !preview->description.isEmpty())
			? preview->description
			: QString(message.text).replace(link, QString()).trimmed();
	} break;
	case SharedMediaTab::Music: {
		const auto track = AudioTrack::ForMessage(message);
		title = track.title.isEmpty() ? tr::lng_media_music(tr::now) : track.title;
		if (!track.performer.isEmpty()) {
			status.push_back(track.performer);
		}
		status.push_back(current
			? (Lang::Duration(int(player.positionMs() / 1000))
				+ u" / "_s
				+ Lang::Duration(int(duration / 1000)))
			: Lang::Duration(int(duration / 1000)));
	} break;
	case SharedMediaTab::Voice:
		title = Support::MemberName(_session, message.sender);
		if (title.isEmpty()) {
			title = (message.kind == Data::MessageKind::VideoNote)
				? tr::lng_media_video_message(tr::now)
				: tr::lng_media_voice(tr::now);
		}
		status.push_back(current
			? (Lang::Duration(int(player.positionMs() / 1000))
				+ u" / "_s
				+ Lang::Duration(int(duration / 1000)))
			: Lang::Duration(int(duration / 1000)));
		status.push_back(date);
		break;
	case SharedMediaTab::Media:
	case SharedMediaTab::Gifs:
		break;
	}
	if (title.isEmpty()) {
		title = Support::MessagePreview(_session, message);
	}
	p.setFont(st::sharedRowTitleFont);
	p.setPen(st::textPrimary);
	p.drawTextLeft(
		left,
		rect.y() + st::sharedRowTitleTop,
		outer,
		st::sharedRowTitleFont->elided(title.simplified(), available));
	p.setFont(st::sharedRowStatusFont);
	p.setPen((_tab == SharedMediaTab::Links) ? st::brandText : st::textTertiary);
	p.drawTextLeft(
		left,
		rect.y() + st::sharedRowStatusTop,
		outer,
		st::sharedRowStatusFont->elided(StatusLine(status), available));
	if (!third.isEmpty()) {
		p.setPen(st::textTertiary);
		p.drawTextLeft(
			left,
			rect.y() + st::sharedRowLinkTextTop,
			outer,
			st::sharedRowStatusFont->elided(third.simplified(), available));
	}
	if (current && (_tab == SharedMediaTab::Music || _tab == SharedMediaTab::Voice)) {
		// The progress of the one playing, under its row.
		auto hq = PainterHighQualityEnabler(p);
		const auto line = QRectF(
			left,
			rect.y() + rect.height() - st::sharedSeekHeight - 4,
			available,
			st::sharedSeekHeight);
		p.setPen(Qt::NoPen);
		p.setBrush(st::borderSubtle);
		p.drawRoundedRect(line, line.height() / 2., line.height() / 2.);
		const auto done = line.width() * std::clamp(player.progress(), 0., 1.);
		p.setBrush(st::brandPrimary);
		p.drawRoundedRect(
			style::RightToLeft()
				? QRectF(line.x() + line.width() - done, line.y(), done, line.height())
				: QRectF(line.x(), line.y(), done, line.height()),
			line.height() / 2.,
			line.height() / 2.);
	}
}

void List::mouseMoveEvent(QMouseEvent *e) {
	const auto index = entryAt(e->pos());
	const auto state = stateRect();
	const auto onState = stateIsLink() && state.contains(e->pos());
	setCursor((index >= 0 || onState) ? style::cur_pointer : style::cur_default);
	if (_over != index || _overState != onState) {
		_over = index;
		_overState = onState;
		update();
	}
}

void List::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (_over >= 0 || _overState) {
		_over = -1;
		_overState = false;
		update();
	}
}

void List::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) ? entryAt(e->pos()) : -1;
}

void List::mouseReleaseEvent(QMouseEvent *e) {
	const auto index = entryAt(e->pos());
	const auto pressed = std::exchange(_pressed, -1);
	const auto &items = state().shown;
	if (e->button() == Qt::RightButton && index >= 0) {
		showMenu(items[index], e->globalPosition().toPoint());
		return;
	} else if (e->button() != Qt::LeftButton) {
		return;
	} else if (index >= 0 && index == pressed) {
		const auto message = items[index];
		if (!_selected.isEmpty() || (e->modifiers() & Qt::ControlModifier)) {
			toggleSelected(message);
		} else {
			open(message);
		}
	} else if (index < 0 && stateIsLink() && stateRect().contains(e->pos())) {
		load(_tab);
	}
}

void List::wheelEvent(QWheelEvent *e) {
	if (!grid() || !(e->modifiers() & Qt::ControlModifier)) {
		RpWidget::wheelEvent(e);
		return;
	}
	// Ctrl + wheel zooms the grid (the pinch of the phones): up for bigger
	// squares, down for more of them.
	const auto delta = e->angleDelta().y();
	if (delta != 0) {
		const auto inner = std::max(
			width() - st::sharedPadding.left() - st::sharedPadding.right(),
			1);
		const auto now = SharedGridColumns(
			inner,
			st::sharedTileMin,
			st::sharedTileSkip,
			st::sharedGridColumnsMin,
			st::sharedGridColumnsMax,
			_zoom);
		const auto wanted = std::clamp(
			now + ((delta > 0) ? -1 : 1),
			int(st::sharedGridColumnsMin),
			int(st::sharedGridColumnsMax));
		if (wanted != now) {
			_zoom += (now - wanted);
			relayout();
		}
	}
	e->accept();
}

void List::toggleSelected(const Data::Message &message) {
	if (_selected.contains(message.seq)) {
		_selected.remove(message.seq);
	} else {
		_selected.insert(message.seq, message);
	}
	update();
	if (_selectionChanged) {
		_selectionChanged();
	}
}

QVector<Data::Message> List::selectedMessages() const {
	auto result = QVector<Data::Message>();
	for (const auto &message : _selected) {
		result.push_back(message);
	}
	return result;
}

bool List::canForwardSelected() const {
	return std::any_of(_selected.begin(), _selected.end(), [](
			const Data::Message &message) {
		return History::CanForward(message);
	});
}

void List::clearSelection() {
	if (_selected.isEmpty()) {
		return;
	}
	_selected.clear();
	update();
	if (_selectionChanged) {
		_selectionChanged();
	}
}

void List::removeSeqs(const QSet<qint64> &seqs) {
	for (auto &state : _tabs) {
		state.server.erase(
			std::remove_if(state.server.begin(), state.server.end(), [&](
					const Data::Message &message) {
				return seqs.contains(message.seq);
			}),
			state.server.end());
	}
	for (const auto seq : seqs) {
		_selected.remove(seq);
	}
	rebuildAll();
	if (_selectionChanged) {
		_selectionChanged();
	}
}

void List::showInChat(qint64 seq) {
	_controller->openRoute(Window::Routes::Chat(_chatId, seq));
}

void List::open(const Data::Message &message) {
	switch (_tab) {
	case SharedMediaTab::Media:
	case SharedMediaTab::Gifs:
		openViewer(message);
		break;
	case SharedMediaTab::Files:
		openFile(message);
		break;
	case SharedMediaTab::Links: {
		const auto link = Data::SharedMediaLink(message);
		if (!link.isEmpty()) {
			Support::OpenLink(_controller, link);
		} else {
			showInChat(message.seq);
		}
	} break;
	case SharedMediaTab::Music:
	case SharedMediaTab::Voice:
		if (message.kind == Data::MessageKind::VideoNote) {
			openViewer(message);
		} else {
			const auto track = AudioTrack::ForMessage(message);
			const auto starting = !Player().isCurrent(track.id);
			// The tab's songs are the queue, newest first; a voice goes on
			// to the voices after it.
			auto queue = QVector<AudioTrack>();
			const auto &items = state().shown;
			if (!track.voice) {
				for (const auto &other : items) {
					queue.push_back(AudioTrack::ForMessage(other));
				}
			} else {
				queue.push_back(track);
				for (auto i = items.size(); i != 0;) {
					const auto &other = items[--i];
					if (other.kind == Data::MessageKind::Voice
						&& other.seq > message.seq) {
						queue.push_back(AudioTrack::ForMessage(other));
					}
				}
			}
			Player().toggle(track, _session, std::move(queue));
			if (starting
				&& track.voice
				&& !message.mine
				&& !message.played) {
				_session->data().messages().markPlayed(_chatId, message.seq);
			}
		}
		break;
	}
}

bool List::inViewer(
		SharedMediaTab tab,
		const Data::Message &message,
		qint64 clicked) const {
	switch (message.kind) {
	case Data::MessageKind::Photo:
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
		return true;
	case Data::MessageKind::VideoNote:
		// In the voice tab round videos go through each other.
		return (tab == SharedMediaTab::Voice) || (message.seq == clicked);
	default:
		return false;
	}
}

ViewerItem List::viewerItem(const Data::Message &message) {
	auto item = ViewerItem();
	item.key = message.key();
	const auto preview = image(message);
	item.preview = preview;
	if (message.kind != Data::MessageKind::Photo) {
		if (preview) {
			item.videoPoster = preview->request(QSize());
			if (item.videoPoster.isNull()) {
				item.videoPoster = preview->preview();
			}
		}
		const auto downloads = QPointer<History::Downloads>(_downloads);
		item.loadVideo = [=](
				QObject *guard,
				std::function<void(QString)> done) {
			if (!downloads) {
				done(QString());
				return;
			}
			downloads->fetch(
				message,
				Data::MediaVariant::Mp4,
				guard,
				[=](const QString &path) { done(path); });
		};
	} else {
		auto options = Support::ImageOptions();
		options.original = true;
		if (message.media) {
			options.blurhash = message.media->blurhash;
			options.aspect = QSize(message.media->width, message.media->height);
		}
		item.source = Support::MakeImageSource(
			_session,
			Data::MediaRef::Attachment(message.chat, message.seq),
			Data::MediaVariant::Full,
			options);
	}
	item.caption = message.text;
	item.saveName = History::Downloads::SuggestedName(message);
	return item;
}

void List::openViewer(const Data::Message &clicked) {
	// Every picture and video of the tab, ← → through them, and on into the
	// pages the tab has not read yet.
	const auto tab = _tab;
	const auto &items = state().shown;
	auto viewerItems = std::vector<ViewerItem>();
	auto index = 0;
	for (const auto &message : items) {
		if (!inViewer(tab, message, clicked.seq)) {
			continue;
		} else if (message.seq == clicked.seq) {
			index = int(viewerItems.size());
		}
		viewerItems.push_back(viewerItem(message));
	}
	if (viewerItems.empty()) {
		return;
	}
	const auto weak = QPointer<List>(this);
	auto options = ViewerOptions();
	if (clicked.kind != Data::MessageKind::VideoNote
		|| tab == SharedMediaTab::Voice) {
		const auto seq = clicked.seq;
		options.paging.moreAfter = !_unsupported;
		options.paging.load = [=](
				int direction,
				const QString &edgeKey,
				QObject *guard,
				std::function<void(std::vector<ViewerItem>, bool)> done) {
			Q_UNUSED(guard);
			// Newest first: what lies before the first item is the newest
			// there is (anything newer joins the tab, not the viewer).
			if (!weak || direction < 0) {
				done({}, false);
				return;
			}
			weak->viewerPage(tab, seq, edgeKey, std::move(done));
		};
	}
	options.origin = [=](const ViewerItem &item) {
		return weak ? weak->viewerOrigin(item.key) : QRect();
	};
	ShowImageViewer(
		_controller->window(),
		std::move(viewerItems),
		index,
		std::move(options));
}

void List::viewerPage(
		SharedMediaTab tab,
		qint64 clicked,
		const QString &edgeKey,
		std::function<void(std::vector<ViewerItem>, bool)> done) {
	auto &state = _tabs[int(tab)];
	auto result = std::vector<ViewerItem>();
	auto after = false;
	for (const auto &message : std::as_const(state.shown)) {
		if (!after) {
			after = (message.key() == edgeKey);
		} else if (inViewer(tab, message, clicked)) {
			result.push_back(viewerItem(message));
		}
	}
	const auto more = state.more && !_unsupported;
	if (!after || !result.empty() || !more) {
		done(std::move(result), more && after);
		return;
	}
	// The rest of the tab first, then the server.
	const auto weak = QPointer<List>(this);
	_pageWaiters.emplace_back(tab, [=] {
		if (weak) {
			auto &state = weak->_tabs[int(tab)];
			if (state.failed) {
				done({}, false);
			} else {
				weak->viewerPage(tab, clicked, edgeKey, done);
			}
		}
	});
	if (!state.loading) {
		load(tab);
	}
}

void List::pageArrived(SharedMediaTab tab) {
	auto waiting = std::vector<Fn<void()>>();
	for (auto i = _pageWaiters.begin(); i != _pageWaiters.end();) {
		if (i->first == tab) {
			waiting.push_back(std::move(i->second));
			i = _pageWaiters.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &callback : waiting) {
		callback();
	}
}

QRect List::viewerOrigin(const QString &key) const {
	if (!isVisible() || !grid()) {
		return QRect();
	}
	const auto &items = state().shown;
	for (const auto &entry : _entries) {
		if (entry.item >= 0 && items[entry.item].key() == key) {
			const auto visible = entry.rect.intersected(QRect(
				0,
				_visibleTop,
				width(),
				_visibleBottom - _visibleTop));
			if (visible.height() < entry.rect.height() / 2) {
				return QRect();
			}
			return QRect(mapToGlobal(entry.rect.topLeft()), entry.rect.size());
		}
	}
	return QRect();
}

void List::openFile(const Data::Message &message) {
	const auto saved = _downloads->savedPath(message);
	if (!saved.isEmpty()) {
		LaunchFile(saved);
		return;
	} else if (_downloads->state(message).loading) {
		_downloads->cancel(message);
		return;
	}
	const auto controller = _controller;
	_downloads->save(message, controller, [=](const QString &path) {
		if (!path.isEmpty()) {
			controller->showToast(tr::lng_history_file_saved(
				tr::now,
				lt_name,
				QFileInfo(path).fileName()));
		}
	});
}

void List::saveAs(const Data::Message &message) {
	const auto name = History::Downloads::SuggestedName(message);
	const auto window = QPointer<QWidget>(_controller->window());
	const auto save = [=](const QString &path) {
		if (window && !path.isEmpty()) {
			Support::SaveFileAs(window, path, name);
		}
	};
	const auto saved = _downloads->savedPath(message);
	if (!saved.isEmpty()) {
		save(saved);
		return;
	}
	_downloads->fetch(
		message,
		History::Downloads::FileVariant(message),
		_controller,
		save);
}

void List::showMenu(const Data::Message &message, QPoint globalPosition) {
	const auto menu = new Ui::PopupMenu(this);
	const auto seq = message.seq;
	menu->addAction(tr::lng_media_shared_show_in_chat(tr::now), [=, this] {
		showInChat(seq);
	}, &st::sharedMenuShowInChat);
	if (_tab == SharedMediaTab::Links) {
		const auto link = Data::SharedMediaLink(message);
		if (!link.isEmpty()) {
			const auto controller = _controller;
			menu->addAction(tr::lng_copy_link(tr::now), [=] {
				QGuiApplication::clipboard()->setText(link);
				controller->showToast(tr::lng_link_copied(tr::now));
			}, &st::menuIconLink);
		}
	} else if (message.kind != Data::MessageKind::VideoNote) {
		menu->addAction(tr::lng_history_save_as(tr::now), [=, this] {
			saveAs(message);
		}, &st::menuIconDownload);
		if (!_downloads->savedPath(message).isEmpty()) {
			const auto path = _downloads->savedPath(message);
			menu->addAction(tr::lng_history_show_in_folder(tr::now), [=] {
				ShowInFileManager(path);
			}, &st::menuIconCopy);
		}
	}
	if (History::CanForward(message)) {
		const auto controller = _controller;
		menu->addAction(tr::lng_forward(tr::now), [=] {
			History::ShowForwardBox(controller, { message });
		}, &st::menuIconForward);
	}
	menu->addAction(tr::lng_media_shared_select(tr::now), [=, this] {
		if (!_selected.contains(message.seq)) {
			toggleSelected(message);
		}
	}, &st::menuIconSelect);
	{
		auto item = Ui::MenuItem();
		item.text = tr::lng_delete(tr::now);
		item.icon = &st::menuIconDelete;
		item.destructive = true;
		const auto weak = QPointer<List>(this);
		const auto controller = _controller;
		const auto chatId = _chatId;
		item.callback = [=] {
			History::ShowDeleteMessagesBox(controller, chatId, { message }, [=] {
				if (weak) {
					weak->removeSeqs({ seq });
				}
			});
		};
		menu->addItem(std::move(item));
	}
	menu->popup(globalPosition);
}

// ---- gifs play by themselves while on screen, as in the bubbles

void List::clearGifs() {
	for (auto &gif : _gifs) {
		if (gif.widget) {
			gif.widget->hide();
			gif.widget->deleteLater();
		}
	}
	_gifs.clear();
}

void List::placeGifs() {
	for (const auto &gif : _gifs) {
		if (!gif.widget) {
			continue;
		}
		for (const auto &entry : _entries) {
			if (entry.item >= 0 && state().shown[entry.item].seq == gif.seq) {
				gif.widget->setGeometry(entry.rect);
				break;
			}
		}
	}
}

void List::updateGifs() {
	if (_tab != SharedMediaTab::Gifs) {
		clearGifs();
		return;
	}
	const auto &items = state().shown;
	const auto visible = [&](const Entry &entry) {
		return (entry.rect.bottom() >= _visibleTop - kGifMargin)
			&& (entry.rect.top() <= _visibleBottom + kGifMargin);
	};
	// What scrolled away (or left the tab) stops.
	for (auto i = _gifs.begin(); i != _gifs.end();) {
		const auto still = std::any_of(_entries.begin(), _entries.end(), [&](
				const Entry &entry) {
			return entry.item >= 0
				&& items[entry.item].seq == i->seq
				&& visible(entry);
		});
		if (still) {
			++i;
			continue;
		}
		if (i->widget) {
			i->widget->hide();
			i->widget->deleteLater();
		}
		i = _gifs.erase(i);
	}
	// The bubbles' rule: animations on, a few at a time.
	if (Ui::AnimationsDisabled()) {
		return;
	}
	for (const auto &entry : _entries) {
		if (int(_gifs.size()) >= kMaxGifs) {
			break;
		} else if (entry.item < 0 || !visible(entry)) {
			continue;
		}
		const auto &message = items[entry.item];
		const auto seq = message.seq;
		if (_gifFailed.contains(seq) || message.mediaSpoiler) {
			continue;
		}
		const auto known = std::any_of(_gifs.begin(), _gifs.end(), [&](
				const InlineGif &gif) {
			return gif.seq == seq;
		});
		if (known) {
			continue;
		}
		auto options = Data::Media::Options();
		options.decode = false;
		auto handle = _session->data().media().request(
			Data::MediaRef::Attachment(message.chat, seq),
			Data::MediaVariant::Mp4,
			options);
		_gifs.push_back({ seq, handle, nullptr });
		const auto raw = handle.get();
		const auto start = [=, this] {
			const auto path = raw->filePath();
			const auto i = std::find_if(_gifs.begin(), _gifs.end(), [&](
					const InlineGif &gif) {
				return gif.seq == seq;
			});
			if (i == _gifs.end()) {
				return;
			} else if (path.isEmpty()) {
				_gifFailed.insert(seq);
				_gifs.erase(i);
				return;
			}
			const auto widget = new VideoPlayerWidget(this);
			widget->setAttribute(Qt::WA_TransparentForMouseEvents);
			widget->setControlsShown(false);
			widget->setFullscreenAllowed(false);
			widget->setLooped(true);
			widget->setMuted(true);
			widget->setRadius(st::sharedTileRadius);
			widget->setSource(path);
			i->widget = widget;
			placeGifs();
			widget->show();
			widget->play();
		};
		if (raw->isReady()) {
			Ui::PostponeCall(this, start);
		} else if (raw->isFailed()) {
			_gifFailed.insert(seq);
		} else {
			connect(raw, &Data::MediaHandle::ready, this, start);
			connect(raw, &Data::MediaHandle::failed, this, [=, this] {
				_gifFailed.insert(seq);
				const auto i = std::find_if(_gifs.begin(), _gifs.end(), [&](
						const InlineGif &gif) {
					return gif.seq == seq;
				});
				if (i != _gifs.end()) {
					_gifs.erase(i);
				}
			});
		}
	}
}

// Passes the visible range of the scroll area on to the list and keeps it
// in a centred column.
class ListWrap final : public Ui::RpWidget {
public:
	ListWrap(QWidget *parent, List *list)
	: RpWidget(parent)
	, _list(list) {
		_list->setParent(this);
		connect(_list, &Ui::RpWidget::heightChanged, this, [this] {
			if (!_inResize) {
				resizeToWidth(width());
			}
		});
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_inResize = true;
		const auto inner = std::min(newWidth, int(st::sharedColumnWidth));
		_list->resizeToWidth(inner);
		_list->move((newWidth - inner) / 2, 0);
		_inResize = false;
		return _list->height();
	}
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		_list->setVisibleRange(visibleTop, visibleBottom);
	}

private:
	List *_list = nullptr;
	bool _inResize = false;

};

// «۳ انتخاب‌شده» with forward and delete, over the top bar in select mode.
class SelectionBar final : public Ui::RpWidget {
public:
	explicit SelectionBar(QWidget *parent)
	: RpWidget(parent)
	, _cancel(new Ui::IconButton(this, st::sharedSelectionButton))
	, _forward(new Ui::IconButton(this, st::sharedSelectionForward))
	, _delete(new Ui::IconButton(this, st::sharedSelectionDelete)) {
		style::OnPaletteChanged(this, [this] { update(); });
	}

	void setState(int count, bool canForward) {
		_count = count;
		_forward->setVisible(canForward);
		updateControls();
		update();
	}
	[[nodiscard]] Ui::IconButton *cancel() const {
		return _cancel;
	}
	[[nodiscard]] Ui::IconButton *forward() const {
		return _forward;
	}
	[[nodiscard]] Ui::IconButton *remove() const {
		return _delete;
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		Q_UNUSED(e);
		updateControls();
	}
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		p.fillRect(rect(), st::chromeBar);
		p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::chromeHairline);
		const auto &padding = st::sharedSelectionPadding;
		const auto left = padding.left() + _cancel->width() + 8;
		p.setFont(st::sharedSelectionFont);
		p.setPen(st::textPrimary);
		p.drawTextLeft(
			left,
			(height() - st::sharedSelectionFont->height) / 2,
			width(),
			tr::lng_media_shared_selected(tr::now, lt_count, _count));
	}

private:
	void updateControls() {
		const auto &padding = st::sharedSelectionPadding;
		const auto top = (height() - _cancel->height()) / 2;
		_cancel->moveToLeft(padding.left(), top, width());
		auto right = padding.right();
		for (const auto button : { _delete, _forward }) {
			if (!button->isHidden()) {
				button->moveToRight(right, top, width());
				right += button->width();
			}
		}
	}

	Ui::IconButton *_cancel = nullptr;
	Ui::IconButton *_forward = nullptr;
	Ui::IconButton *_delete = nullptr;
	int _count = 0;

};

class Section final : public Window::SectionWidget {
public:
	Section(QWidget *parent, Controller *controller, const QString &chatId);

	QString sectionKey() const override {
		return Data::IsGroupId(_chatId)
			? Window::Routes::GroupMedia(_chatId)
			: Window::Routes::ChatMedia(_chatId);
	}
	void setInnerFocus() override {
		setFocus();
	}
	bool onBackRequest() override {
		if (_list->selectedCount() > 0) {
			_list->clearSelection();
			return true;
		}
		return false;
	}

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void refreshTabs();
	void refreshTitle();
	void refreshSelection();
	void closeLater();

	QString _chatId;
	Ui::TopBar *_bar = nullptr;
	SelectionBar *_selection = nullptr;
	Ui::TabBar *_tabs = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	List *_list = nullptr;
	QVector<SharedMediaTab> _shown;

};

Section::Section(QWidget *parent, Controller *controller, const QString &chatId)
: SectionWidget(parent, controller)
, _chatId(chatId)
, _bar(new Ui::TopBar(this))
, _selection(new SelectionBar(this))
, _tabs(new Ui::TabBar(this))
, _scroll(new Ui::ScrollArea(this)) {
	setFocusPolicy(Qt::StrongFocus);
	_bar->setTitle(tr::lng_media_shared_title(tr::now));
	setTopBar(_bar);
	_selection->hide();
	_list = new List(nullptr, controller, chatId);
	_scroll->setOwnedWidget(Ui::object_ptr<ListWrap>(_scroll, _list));
	_list->setTabsChangedCallback([this] { refreshTabs(); });
	_list->setSelectionChangedCallback([this] { refreshSelection(); });
	connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		if (index >= 0 && index < int(_shown.size())) {
			_list->clearSelection();
			_list->setTab(_shown[index]);
			_scroll->scrollToY(0);
			refreshTitle();
		}
	});
	_selection->cancel()->setClickedCallback([this] {
		_list->clearSelection();
	});
	_selection->forward()->setClickedCallback([this] {
		const auto weak = QPointer<Section>(this);
		History::ShowForwardBox(
			this->controller(),
			_list->selectedMessages(),
			[=] {
				if (weak) {
					weak->_list->clearSelection();
				}
			});
	});
	_selection->remove()->setClickedCallback([this] {
		const auto weak = QPointer<Section>(this);
		const auto messages = _list->selectedMessages();
		auto seqs = QSet<qint64>();
		for (const auto &message : messages) {
			seqs.insert(message.seq);
		}
		History::ShowDeleteMessagesBox(this->controller(), _chatId, messages, [=] {
			if (weak) {
				weak->_list->removeSeqs(seqs);
			}
		});
	});
	auto &data = controller->session().data();
	connect(&data.chats(), &Data::Chats::chatRemoved, this, [this](
			const QString &id) {
		if (id == _chatId) {
			closeLater();
		}
	});
	connect(&data.chats(), &Data::Chats::chatChanged, this, [this](
			const QString &id) {
		if (id == _chatId) {
			refreshTitle();
		}
	});
	refreshTabs();
}

void Section::refreshTabs() {
	const auto shown = Data::SharedMediaVisibleTabs(
		_list->counts(),
		_list->hasRows(),
		_list->unknown());
	if (shown == _shown) {
		refreshTitle();
		return;
	}
	const auto was = _list->tab();
	_shown = shown;
	auto titles = QStringList();
	for (const auto tab : std::as_const(_shown)) {
		titles.push_back(TabTitle(tab));
	}
	_tabs->setTabs(titles);
	_tabs->setStretch(_shown.size() <= 4);
	auto index = int(_shown.indexOf(was));
	if (index < 0) {
		index = 0;
	}
	_tabs->setActive(index, anim::type::instant);
	_tabs->setVisible(_shown.size() > 1);
	_list->setTab(_shown[index]);
	refreshTitle();
	resizeEvent(nullptr);
}

void Section::refreshTitle() {
	const auto tab = _list->tab();
	const auto &counts = _list->counts();
	if (counts && counts->of(tab) > 0) {
		_bar->setSubtitle(CountText(tab, counts->of(tab), counts->cappedAt(tab)));
	} else if (const auto chat = controller()->session().data().chats().chat(_chatId)) {
		_bar->setSubtitle(Support::ChatTitle(&controller()->session(), *chat));
	} else {
		_bar->setSubtitle(TabTitle(tab));
	}
}

void Section::refreshSelection() {
	const auto count = _list->selectedCount();
	_selection->setState(count, _list->canForwardSelected());
	_selection->setVisible(count > 0);
	if (count > 0) {
		_selection->raise();
	}
}

void Section::closeLater() {
	const auto weak = QPointer<Section>(this);
	const auto window = controller();
	Ui::PostponeCall(window, [=] {
		if (weak && window->mainSection() == weak.data()) {
			if (!window->showBack()) {
				window->clearMainStack();
			}
		}
	});
}

void Section::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_selection->setGeometry(0, 0, width(), _bar->height());
	const auto &padding = st::sharedTabsPadding;
	auto top = _bar->height();
	if (!_tabs->isHidden()) {
		const auto available = std::max(width() - padding.left() - padding.right(), 1);
		const auto tabsWidth = std::min(available, int(st::sharedColumnWidth));
		_tabs->resizeToWidth(tabsWidth);
		_tabs->move((width() - _tabs->width()) / 2, top + padding.top());
		top = _tabs->y() + _tabs->height() + padding.bottom();
	}
	_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
}

} // namespace

Window::SectionFactory SharedMediaFactory(const QString &chatId) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new Section(parent, controller, chatId));
	};
}

} // namespace Media
