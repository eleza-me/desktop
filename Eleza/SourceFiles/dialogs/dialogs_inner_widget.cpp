#include "dialogs/dialogs_inner_widget.h"

#include "api/api_client_config.h"
#include "data/data_chats.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "dialogs/dialogs_menu.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_key_value.h"
#include "support/chat_text.h"
#include "ui/effects/shadow.h"
#include "ui/image/userpic.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/ui_utility.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"

#include <QtCore/QJsonArray>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>

namespace Dialogs {
namespace {

constexpr auto kClockInterval = 30 * 1000;
constexpr auto kPresenceWatchLimit = 60;
constexpr auto kBottomSkip = 8;

[[nodiscard]] QString PinnedOrderKey(const QString &userId) {
	return u"dialogs.pinned_order."_s + userId;
}

} // namespace

InnerWidget::InnerWidget(
	QWidget *parent,
	Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _userpics(&controller->session(), [this](const QString &) { update(); }) {
	setMouseTracking(true);

	auto &data = _controller->session().data();
	const auto chats = &data.chats();
	connect(chats, &Data::Chats::listChanged, this, [this] {
		scheduleRebuild();
	});
	connect(chats, &Data::Chats::chatChanged, this, [this](const QString &id) {
		chatChanged(id);
	});
	connect(chats, &Data::Chats::chatRemoved, this, [this](const QString &id) {
		_thumbs.remove(id);
		scheduleRebuild();
	});
	connect(chats, &Data::Chats::unreadChanged, this, [this] {
		// The counters of the folder entries.
		if (_folder == Folder::Main && !searching()) {
			scheduleRebuild();
		}
	});
	connect(chats, &Data::Chats::activityChanged, this, [this](
			const QString &chat) {
		const auto i = _indexById.constFind(chat);
		if (i != _indexById.cend()) {
			_rows[*i].prepared = false;
			updateRow(*i);
		}
	});
	connect(chats, &Data::Chats::presenceChanged, this, [this](
			const QString &user) {
		for (auto i = 0, count = rowsCount(); i != count; ++i) {
			if (_rows[i].type == Row::Type::Chat
				&& _rows[i].chat.peerUserId == user) {
				updateRow(i);
			}
		}
	});
	connect(&data.users(), &Data::Users::changed, this, [this](
			const QStringList &ids) {
		auto changed = false;
		for (auto &row : _rows) {
			if (row.type == Row::Type::Chat
				&& ids.contains(row.chat.peerUserId)) {
				row.prepared = false;
				changed = true;
			}
		}
		if (changed) {
			if (searching()) {
				scheduleRebuild();
			}
			update();
		}
	});

	_clock.setInterval(kClockInterval);
	connect(&_clock, &QTimer::timeout, this, [this] {
		// «۲۱:۴۱» becomes «دیروز» while the list just sits there.
		for (auto &row : _rows) {
			row.prepared = false;
		}
		update();
	});
	_clock.start();

	style::OnPaletteChanged(this, [this] {
		_userpics.clear();
		for (auto &thumb : _thumbs) {
			thumb.prepared = QImage();
		}
		update();
	});

	rebuild();
}

InnerWidget::~InnerWidget() {
	if (!_watched.isEmpty()) {
		_controller->session().data().chats().unwatchPresence(
			QStringList(_watched.cbegin(), _watched.cend()));
	}
	for (const auto &thumb : std::as_const(_thumbs)) {
		QObject::disconnect(thumb.connection);
	}
}

void InnerWidget::setFolder(Folder folder) {
	if (_folder == folder) {
		return;
	}
	finishDrag(false);
	_folder = folder;
	_selected = -1;
	rebuild();
}

void InnerWidget::setQuery(const QString &query) {
	const auto normalized = NormalizeQuery(query);
	if (_query == normalized) {
		return;
	}
	finishDrag(false);
	_query = normalized;
	_selected = -1;
	rebuild();
}

void InnerWidget::setActiveChat(const QString &id) {
	if (_activeChat == id) {
		return;
	}
	const auto was = _indexById.value(_activeChat, -1);
	_activeChat = id;
	updateRow(was);
	updateRow(_indexById.value(_activeChat, -1));
}

void InnerWidget::scheduleRebuild() {
	if (_dragging) {
		_rebuildAfterDrag = true;
		return;
	} else if (_rebuildScheduled) {
		return;
	}
	_rebuildScheduled = true;
	Ui::PostponeCall(this, [this] {
		if (_rebuildScheduled) {
			rebuild();
		}
	});
}

QStringList InnerWidget::pinnedOrder() const {
	auto &session = _controller->session();
	auto result = QStringList();
	const auto stored = session.account().settings().value(
		PinnedOrderKey(session.userId())).toArray();
	for (const auto &value : stored) {
		result.push_back(value.toString());
	}
	return result;
}

void InnerWidget::savePinnedOrder(const QStringList &order) {
	auto &session = _controller->session();
	auto list = QJsonArray();
	for (const auto &id : order) {
		list.push_back(id);
	}
	session.account().settings().set(PinnedOrderKey(session.userId()), list);
}

void InnerWidget::rebuild() {
	_rebuildScheduled = false;
	_rebuildAfterDrag = false;
	stopRipple();

	auto &session = _controller->session();
	auto &chats = session.data().chats();
	auto &users = session.data().users();
	const auto selectedChat = (_selected >= 0
		&& _selected < rowsCount()
		&& _rows[_selected].type == Row::Type::Chat)
		? _rows[_selected].chat.id
		: QString();
	const auto selectedType = (_selected >= 0 && _selected < rowsCount())
		? _rows[_selected].type
		: Row::Type::Chat;
	const auto wasCount = rowsCount();

	auto list = chats.list(Data::Chats::Section::All);
	auto requests = 0;
	auto requestsUnread = 0;
	auto archived = 0;
	auto shown = QVector<Data::Chat>();
	shown.reserve(list.size());
	for (auto &chat : list) {
		if (InFolder(chat, Folder::Requests)) {
			++requests;
			if (chat.unread > 0) {
				++requestsUnread;
			}
		} else if (InFolder(chat, Folder::Archived)) {
			++archived;
		}
		// Searching from the main list also looks into the archive.
		const auto fits = InFolder(chat, _folder)
			|| (searching()
				&& _folder == Folder::Main
				&& InFolder(chat, Folder::Archived));
		if (fits) {
			shown.push_back(std::move(chat));
		}
	}
	SortChats(shown, pinnedOrder());

	auto rows = std::vector<Row>();
	rows.reserve(shown.size() + 2);
	if (_folder == Folder::Main && !searching()) {
		if (requests > 0) {
			auto row = Row();
			row.type = Row::Type::Requests;
			row.count = requests;
			row.accent = (requestsUnread > 0);
			rows.push_back(std::move(row));
		}
		if (archived > 0) {
			auto row = Row();
			row.type = Row::Type::Archive;
			row.count = archived;
			rows.push_back(std::move(row));
		}
	}
	for (auto &chat : shown) {
		auto row = Row();
		row.type = Row::Type::Chat;
		if (!chat.peerUserId.isEmpty() && !chat.anonymous) {
			row.handle = users.peek(chat.peerUserId).handle;
		}
		if (searching()) {
			const auto title = Support::ChatTitle(&session, chat);
			if (!MatchesQuery(chat, title, row.handle, _query)) {
				continue;
			}
		}
		row.chat = std::move(chat);
		rows.push_back(std::move(row));
	}
	_rows = std::move(rows);

	_indexById.clear();
	_selected = -1;
	for (auto i = 0, count = rowsCount(); i != count; ++i) {
		const auto &row = _rows[i];
		if (row.type == Row::Type::Chat) {
			_indexById.insert(row.chat.id, i);
			if (!selectedChat.isEmpty() && row.chat.id == selectedChat) {
				_selected = i;
			}
		} else if (selectedChat.isEmpty()
			&& selectedType == row.type
			&& selectedType != Row::Type::Chat) {
			_selected = i;
		}
	}
	for (auto i = _thumbs.begin(); i != _thumbs.end();) {
		if (!_indexById.contains(i.key())) {
			QObject::disconnect(i->connection);
			i = _thumbs.erase(i);
		} else {
			++i;
		}
	}
	_over = -1;
	_pressed = -1;
	refreshPresenceWatch();

	resizeToWidth(width());
	update();
	if (underMouse()) {
		setOver(rowAt(mapFromGlobal(QCursor::pos())));
	}
	if (wasCount != rowsCount() || !wasCount) {
		Q_EMIT rowsChanged();
	}
}

void InnerWidget::chatChanged(const QString &id) {
	const auto chat = _controller->session().data().chats().chat(id);
	const auto i = _indexById.constFind(id);
	if (i == _indexById.cend()) {
		// A new chat, or one that may enter this folder / the counters.
		if (chat) {
			scheduleRebuild();
		}
		return;
	} else if (!chat) {
		scheduleRebuild();
		return;
	}
	auto &row = _rows[*i];
	const auto reorder = (row.chat.pinned != chat->pinned)
		|| (SortTime(row.chat) != SortTime(*chat))
		|| (InFolder(row.chat, Folder::Requests)
			!= InFolder(*chat, Folder::Requests))
		|| (row.chat.archived != chat->archived);
	row.chat = *chat;
	row.prepared = false;
	if (reorder || searching()) {
		scheduleRebuild();
	} else {
		updateRow(*i);
	}
}

void InnerWidget::refreshPresenceWatch() {
	auto &session = _controller->session();
	const auto limit = std::min(
		kPresenceWatchLimit,
		std::max(session.account().clientConfig().presenceMaxSubscriptions(), 0));
	auto wanted = QSet<QString>();
	for (const auto &row : _rows) {
		if (wanted.size() >= limit) {
			break;
		}
		const auto &chat = row.chat;
		if (row.type == Row::Type::Chat
			&& !chat.isConversation
			&& !chat.saved
			&& !chat.isBot
			&& !chat.peerDeleted
			&& !chat.anonymous
			&& !chat.peerUserId.isEmpty()) {
			wanted.insert(chat.peerUserId);
		}
	}
	auto add = QStringList();
	auto remove = QStringList();
	for (const auto &id : std::as_const(wanted)) {
		if (!_watched.contains(id)) {
			add.push_back(id);
		}
	}
	for (const auto &id : std::as_const(_watched)) {
		if (!wanted.contains(id)) {
			remove.push_back(id);
		}
	}
	auto &chats = session.data().chats();
	if (!remove.isEmpty()) {
		chats.unwatchPresence(remove);
	}
	if (!add.isEmpty()) {
		chats.watchPresence(add);
	}
	_watched = std::move(wanted);
}

void InnerWidget::prepare(Row &row) {
	row.prepared = true;
	row.titleElidedFor = -1;
	row.textElidedFor = -1;
	if (row.type != Row::Type::Chat) {
		row.title = (row.type == Row::Type::Requests)
			? tr::lng_dialogs_requests(tr::now)
			: tr::lng_dialogs_archived(tr::now);
		row.preview = Preview();
		row.preview.kind = Preview::Kind::Service;
		row.preview.text = (row.type == Row::Type::Requests)
			? tr::lng_dialogs_requests_about(tr::now)
			: tr::lng_dialogs_archived_count(tr::now, lt_count, row.count);
		row.date = QString();
		row.dateWidth = 0;
		row.tick = Tick::None;
		row.badge = (row.type == Row::Type::Requests)
			? Ui::BadgeText(row.count)
			: QString();
		return;
	}
	auto &session = _controller->session();
	const auto &chat = row.chat;
	row.title = Support::ChatTitle(&session, chat);
	const auto activity = session.data().chats().activity(chat.id);
	row.preview = PreviewFor(chat, activity ? activity->kind : QString());
	if (!row.preview.text.isEmpty()) {
		// For painting: "@sara" / "#tag" keep their direction inside the
		// line, and the line reads in the direction of its own text (a
		// Persian message in the English interface and the reverse).
		row.preview.text = Lang::Isolated(Ui::Text::IsolateLinks(
			Ui::Text::ParseEntities(
				row.preview.text,
				Ui::Text::ParseAll)).text);
	}
	const auto time = SortTime(chat);
	row.date = (time > 0)
		? Lang::ListTime(QDateTime::fromMSecsSinceEpoch(time))
		: QString();
	row.dateWidth = st::dialogsDateFont->width(row.date);
	row.tick = TickFor(chat);
	row.badge = (chat.unread > 0)
		? (chat.unreadCapped
			? (Lang::Num(chat.unread) + QLatin1Char('+'))
			: Ui::BadgeText(chat.unread, 999))
		: QString();
}

int InnerWidget::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return rowsCount() * st::dialogsRowHeight
		+ (_rows.empty() ? 0 : kBottomSkip);
}

void InnerWidget::visibleTopBottomUpdated(int visibleTop, int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;
}

int InnerWidget::rowTop(int index) const {
	return index * st::dialogsRowHeight;
}

QRect InnerWidget::rowRect(int index) const {
	return QRect(0, rowTop(index), width(), st::dialogsRowHeight);
}

int InnerWidget::rowAt(QPoint position) const {
	if (position.y() < 0 || position.x() < 0 || position.x() >= width()) {
		return -1;
	}
	const auto index = position.y() / st::dialogsRowHeight;
	return (index < rowsCount()) ? index : -1;
}

void InnerWidget::updateRow(int index) {
	if (index >= 0 && index < rowsCount()) {
		update(rowRect(index));
	}
}

style::color InnerWidget::rowBg(const RowState &state) const {
	return state.active
		? st::dialogsBgActive
		: state.over
		? st::dialogsBgOver
		: st::dialogsBg;
}

const QImage &InnerWidget::thumbFor(const Row &row) {
	static const auto kEmpty = QImage();
	if (!row.chat.lastMessage) {
		return kEmpty;
	}
	const auto &message = *row.chat.lastMessage;
	const auto key = message.key() + QLatin1Char(':') + message.attachment;
	auto &thumb = _thumbs[row.chat.id];
	if (thumb.key != key) {
		QObject::disconnect(thumb.connection);
		thumb = Thumb();
		thumb.key = key;
		thumb.source = Support::MakeMessageImageSource(
			&_controller->session(),
			message);
		if (thumb.source) {
			const auto id = row.chat.id;
			thumb.connection = connect(
				thumb.source.get(),
				&Ui::ImageSource::updated,
				this,
				[this, id] {
					const auto i = _thumbs.find(id);
					if (i != _thumbs.end()) {
						i->prepared = QImage();
					}
					updateRow(_indexById.value(id, -1));
				});
		}
	}
	if (!thumb.source) {
		return kEmpty;
	}
	if (thumb.prepared.isNull()) {
		const auto ratio = devicePixelRatioF();
		const auto size = int(std::ceil(st::dialogsThumbSize * ratio));
		auto image = thumb.source->request(QSize(size, size));
		if (image.isNull()) {
			image = thumb.source->preview();
		}
		if (image.isNull()) {
			return kEmpty;
		}
		auto result = QImage(
			size,
			size,
			QImage::Format_ARGB32_Premultiplied);
		result.fill(Qt::transparent);
		{
			auto p = QPainter(&result);
			p.setRenderHints(
				QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
			auto path = QPainterPath();
			const auto radius = st::dialogsThumbRadius * ratio;
			path.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
			p.setClipPath(path);
			const auto side = std::min(image.width(), image.height());
			const auto source = QRect(
				(image.width() - side) / 2,
				(image.height() - side) / 2,
				side,
				side);
			p.drawImage(QRect(0, 0, size, size), image, source);
		}
		result.setDevicePixelRatio(ratio);
		thumb.prepared = std::move(result);
	}
	return thumb.prepared;
}

void InnerWidget::paintUserpic(
		Painter &p,
		const Row &row,
		const QRect &rect,
		const RowState &state) {
	if (row.type != Row::Type::Chat) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::dialogsFolderBg);
		p.drawEllipse(rect);
		((row.type == Row::Type::Requests)
			? st::dialogsFolderRequestsIcon
			: st::dialogsFolderArchiveIcon).paintInCenter(p, rect);
		return;
	}
	const auto &chat = row.chat;
	if (chat.saved) {
		_userpics.paintSpecial(p, rect, Ui::UserpicKind::Saved, chat.id);
	} else if (chat.peerDeleted) {
		_userpics.paintSpecial(p, rect, Ui::UserpicKind::Deleted, chat.id);
	} else if (chat.anonymous || chat.peerUserId.isEmpty()) {
		_userpics.paintSpecial(
			p,
			rect,
			chat.isConversation
				? Ui::UserpicKind::Anonymous
				: Ui::UserpicKind::User,
			chat.peerRef.isEmpty() ? chat.id : chat.peerRef,
			row.title);
	} else {
		_userpics.paint(p, rect, chat.peerUserId);
		if (!chat.isBot && !chat.isConversation) {
			const auto presence
				= _controller->session().data().chats().presence(
					chat.peerUserId);
			if (presence && presence->online()) {
				// The dot's border is the row's own background.
				const auto bg = state.dragged
					? st::dialogsDragBg
					: state.active
					? st::dialogsBgActive
					: st::dialogsBg;
				Ui::PaintOnlineDot(p, rect, bg);
			}
		}
	}
}

void InnerWidget::paintRow(
		Painter &p,
		Row &row,
		int top,
		const RowState &state) {
	if (!row.prepared) {
		prepare(row);
	}
	const auto outer = width();
	const auto height = int(st::dialogsRowHeight);
	const auto &padding = st::dialogsPadding;
	p.translate(0, top);

	// ---- background
	const auto highlight = QRect(
		st::dialogsRowMargin,
		1,
		outer - 2 * st::dialogsRowMargin,
		height - 2);
	if (state.dragged) {
		Ui::Shadow::Paint(
			p,
			highlight,
			st::dialogsRowRadius,
			st::dialogsDragShadow);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::dialogsDragBg);
		p.drawRoundedRect(
			highlight,
			st::dialogsRowRadius,
			st::dialogsRowRadius);
	} else if (state.active || state.over) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(rowBg(state));
		p.drawRoundedRect(
			highlight,
			st::dialogsRowRadius,
			st::dialogsRowRadius);
	}

	// ---- userpic
	const auto size = int(st::dialogsUserpicSize);
	const auto userpic = style::rtlrect(
		padding.left(),
		(height - size) / 2,
		size,
		size,
		outer);
	paintUserpic(p, row, userpic, state);

	const auto left = padding.left() + size + st::dialogsUserpicSkip;
	const auto right = outer - padding.right();
	const auto isChat = (row.type == Row::Type::Chat);
	const auto &chat = row.chat;

	// ---- the first line: name, marks ... ticks, time
	auto nameRight = right;
	if (!row.date.isEmpty()) {
		const auto unread = isChat && chat.unread > 0 && !chat.muted;
		p.setFont(st::dialogsDateFont);
		p.setPen(unread ? st::dialogsDateFgUnread : st::dialogsDateFg);
		p.drawTextRight(
			padding.right(),
			st::dialogsDateTop,
			outer,
			row.date,
			row.dateWidth);
		nameRight -= row.dateWidth + st::dialogsDateSkip;
	}
	if (row.tick != Tick::None) {
		const auto &icon = (row.tick == Tick::Read)
			? st::dialogsTickRead
			: (row.tick == Tick::Sent)
			? st::dialogsTickSent
			: (row.tick == Tick::Failed)
			? st::dialogsTickFailed
			: st::dialogsTickPending;
		const auto x = nameRight
			+ st::dialogsDateSkip
			- st::dialogsTickSkip
			- icon.width();
		icon.paint(
			p,
			x,
			st::dialogsDateTop
				+ (st::dialogsDateFont->height - icon.height()) / 2,
			outer);
		nameRight = x - st::dialogsTickSkip;
	}
	const auto verified = isChat && chat.verified;
	const auto muted = isChat && chat.muted;
	const auto marks = (verified
		? (st::dialogsVerifiedIcon.width() + st::dialogsNameIconSkip)
		: 0)
		+ (muted
			? (st::dialogsMuteIcon.width() + st::dialogsNameIconSkip)
			: 0);
	const auto topic = isChat && chat.isConversation;
	const auto &nameFont = topic ? st::dialogsTopicFont : st::dialogsNameFont;
	const auto nameAvailable = std::max(nameRight - left - marks, 0);
	if (row.titleElidedFor != nameAvailable) {
		row.titleElidedFor = nameAvailable;
		row.titleElided = nameFont->elided(row.title, nameAvailable);
		row.titleElidedWidth = nameFont->width(row.titleElided);
	}
	const auto nameTop = st::dialogsNameTop
		+ (st::dialogsNameFont->height - nameFont->height) / 2;
	p.setFont(nameFont);
	p.setPen((isChat && chat.anonymous)
		? st::dialogsAnonFg
		: st::dialogsNameFg);
	p.drawTextLeft(
		left,
		nameTop,
		outer,
		row.titleElided,
		row.titleElidedWidth);
	auto markLeft = left + row.titleElidedWidth + st::dialogsNameIconSkip;
	const auto markTop = [&](const style::icon &icon) {
		return st::dialogsNameTop
			+ (st::dialogsNameFont->height - icon.height()) / 2;
	};
	if (verified) {
		st::dialogsVerifiedIcon.paint(
			p,
			markLeft,
			markTop(st::dialogsVerifiedIcon),
			outer);
		markLeft += st::dialogsVerifiedIcon.width() + st::dialogsNameIconSkip;
	}
	if (muted) {
		st::dialogsMuteIcon.paint(
			p,
			markLeft,
			markTop(st::dialogsMuteIcon),
			outer);
	}

	// ---- the second line: preview ... badge / pin
	auto textRight = right;
	if (!row.badge.isEmpty()) {
		const auto mutedBadge = isChat ? chat.muted : !row.accent;
		const auto badgeWidth = Ui::PaintBadge(
			p,
			row.badge,
			padding.right(),
			st::dialogsBadgeTop,
			outer,
			st::dialogsBadge,
			mutedBadge);
		textRight -= badgeWidth + st::dialogsBadgeSkip;
	} else if (isChat && chat.pinned && !searching()) {
		const auto &icon = st::dialogsPinIcon;
		icon.paint(
			p,
			right - icon.width(),
			st::dialogsBadgeTop + (st::dialogsBadge.height - icon.height()) / 2,
			outer);
		textRight -= icon.width() + st::dialogsBadgeSkip;
	}
	const auto &preview = row.preview;
	if (preview.kind == Preview::Kind::Empty) {
		p.translate(0, -top);
		return;
	}
	const auto &textFont = st::dialogsTextFont;
	auto textLeft = left;
	p.setFont(textFont);
	const auto prefixFg = (preview.kind == Preview::Kind::Draft)
		? st::dialogsTextFgDraft
		: st::dialogsTextFg;
	auto prefixWidth = 0;
	if (!preview.prefix.isEmpty()) {
		prefixWidth = textFont->width(preview.prefix);
		if (prefixWidth + textFont->spacew < textRight - textLeft) {
			p.setPen(prefixFg);
			p.drawTextLeft(
				textLeft,
				st::dialogsTextTop,
				outer,
				preview.prefix,
				prefixWidth);
			textLeft += prefixWidth + textFont->spacew;
		}
	}
	if (preview.thumbnail && isChat) {
		const auto &image = thumbFor(row);
		if (!image.isNull()
			&& textLeft + st::dialogsThumbSize + st::dialogsThumbSkip < textRight) {
			const auto rect = style::rtlrect(
				textLeft,
				st::dialogsThumbTop,
				st::dialogsThumbSize,
				st::dialogsThumbSize,
				outer);
			p.drawImage(rect.topLeft(), image);
			textLeft += st::dialogsThumbSize + st::dialogsThumbSkip;
		}
	}
	const auto textAvailable = std::max(textRight - textLeft, 0);
	if (row.textElidedFor != textAvailable) {
		row.textElidedFor = textAvailable;
		row.textElided = textFont->elided(preview.text, textAvailable);
	}
	switch (preview.kind) {
	case Preview::Kind::Activity:
		p.setPen(st::dialogsTextFgActivity);
		break;
	case Preview::Kind::Media:
		p.setPen(st::dialogsTextFgService);
		break;
	case Preview::Kind::Service:
		p.setPen(isChat ? st::dialogsTextFgService : st::dialogsFolderTextFg);
		break;
	default:
		p.setPen(st::dialogsTextFg);
	}
	p.drawTextLeft(textLeft, st::dialogsTextTop, outer, row.textElided);
	p.translate(0, -top);
}

void InnerWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	if (_rows.empty()) {
		return;
	}
	const auto height = int(st::dialogsRowHeight);
	const auto from = std::clamp(clip.top() / height, 0, rowsCount() - 1);
	const auto till = std::clamp(clip.bottom() / height, 0, rowsCount() - 1);
	for (auto i = from; i <= till; ++i) {
		if (_dragging && i == _dragIndex) {
			continue;
		}
		auto &row = _rows[i];
		auto state = RowState();
		const auto isChat = (row.type == Row::Type::Chat);
		state.active = isChat && (row.chat.id == _activeChat);
		state.over = !_dragging
			&& ((i == _over && _mouseSelection)
				|| (i == _selected)
				|| (i == _pressed)
				|| (isChat
					&& !_menuChat.isEmpty()
					&& row.chat.id == _menuChat));
		paintRow(p, row, rowTop(i), state);
		if (_ripple && _rippleRow == i) {
			_ripple->paint(p, st::dialogsRowMargin, rowTop(i) + 1);
			if (_ripple->empty()) {
				_ripple = nullptr;
				_rippleRow = -1;
			}
		}
	}
	if (_dragging && _dragIndex >= 0 && _dragIndex < rowsCount()) {
		auto state = RowState();
		state.dragged = true;
		paintRow(p, _rows[_dragIndex], _dragTop, state);
	}
}

// ---- mouse

void InnerWidget::setOver(int index) {
	if (_over == index) {
		return;
	}
	updateRow(_over);
	_over = index;
	updateRow(_over);
	setCursor((_over >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void InnerWidget::setSelected(int index) {
	if (_selected == index) {
		return;
	}
	updateRow(_selected);
	_selected = index;
	updateRow(_selected);
}

void InnerWidget::addRipple(int index, QPoint position) {
	if (index < 0) {
		return;
	}
	if (!_ripple || _rippleRow != index) {
		const auto size = QSize(
			width() - 2 * st::dialogsRowMargin,
			st::dialogsRowHeight - 2);
		_ripple = std::make_unique<Ui::RippleAnimation>(
			st::dialogsRipple,
			Ui::RippleAnimation::RoundRectMask(size, st::dialogsRowRadius),
			[this] { updateRow(_rippleRow); });
		_rippleRow = index;
	}
	_ripple->add(position - QPoint(st::dialogsRowMargin, rowTop(index) + 1));
}

void InnerWidget::stopRipple() {
	_ripple = nullptr;
	_rippleRow = -1;
}

void InnerWidget::mouseMoveEvent(QMouseEvent *e) {
	const auto position = e->pos();
	if (_dragging) {
		updateDrag(position);
		return;
	}
	_mouseSelection = true;
	if (_selected >= 0) {
		setSelected(-1);
	}
	if (_pressed >= 0
		&& (e->buttons() & Qt::LeftButton)
		&& canDrag(_pressed)
		&& (position - _pressPosition).manhattanLength()
			>= QApplication::startDragDistance()) {
		startDrag();
		updateDrag(position);
		return;
	}
	setOver(rowAt(position));
}

void InnerWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		// The right button opens the menu (contextMenuEvent); the middle one
		// does nothing.
		return;
	}
	_mouseSelection = true;
	setOver(rowAt(e->pos()));
	_pressed = _over;
	_pressPosition = e->pos();
	if (_pressed >= 0) {
		addRipple(_pressed, e->pos());
		updateRow(_pressed);
	}
}

void InnerWidget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	if (_dragging) {
		finishDrag(true);
		setOver(rowAt(e->pos()));
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	if (_ripple) {
		_ripple->lastStop();
	}
	updateRow(pressed);
	setOver(rowAt(e->pos()));
	if (pressed >= 0 && pressed == _over) {
		activate(pressed);
	}
}

void InnerWidget::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (!_dragging) {
		setOver(-1);
	}
}

void InnerWidget::contextMenuEvent(QContextMenuEvent *e) {
	if (_dragging) {
		return;
	}
	const auto index = (e->reason() == QContextMenuEvent::Mouse)
		? rowAt(e->pos())
		: _selected;
	if (index < 0) {
		return;
	}
	showMenu(
		index,
		(e->reason() == QContextMenuEvent::Mouse)
			? e->globalPos()
			: mapToGlobal(rowRect(index).center()));
}

void InnerWidget::showMenu(int index, QPoint globalPosition) {
	if (index < 0 || index >= rowsCount()) {
		return;
	}
	const auto &row = _rows[index];
	if (_menu) {
		_menu->hideMenu(true);
	}
	const auto menu = new Ui::PopupMenu(this);
	if (row.type == Row::Type::Chat) {
		FillChatMenu(menu, _controller, row.chat, _folder);
	} else {
		const auto folder = (row.type == Row::Type::Requests)
			? Folder::Requests
			: Folder::Archived;
		menu->addAction(tr::lng_dialogs_open(tr::now), [this, folder] {
			Q_EMIT folderChosen(folder);
		});
	}
	if (menu->empty()) {
		delete menu;
		return;
	}
	_menu = menu;
	_menuChat = (row.type == Row::Type::Chat) ? row.chat.id : QString();
	connect(menu, &Ui::PopupMenu::aboutToHide, this, [this, menu] {
		if (_menu == menu) {
			const auto index = _indexById.value(_menuChat, -1);
			_menuChat = QString();
			updateRow(index);
		}
	});
	updateRow(index);
	menu->popup(globalPosition);
}

void InnerWidget::activate(int index) {
	if (index < 0 || index >= rowsCount()) {
		return;
	}
	const auto &row = _rows[index];
	switch (row.type) {
	case Row::Type::Chat: {
		const auto id = row.chat.id;
		Q_EMIT chatChosen(id);
	} break;
	case Row::Type::Requests:
		Q_EMIT folderChosen(Folder::Requests);
		break;
	case Row::Type::Archive:
		Q_EMIT folderChosen(Folder::Archived);
		break;
	}
}

// ---- keyboard

void InnerWidget::selectSkip(int direction) {
	if (_rows.empty()) {
		return;
	}
	_mouseSelection = false;
	auto index = _selected;
	if (index < 0) {
		// Start from the open chat when it is listed.
		const auto active = _indexById.value(_activeChat, -1);
		index = (active >= 0)
			? (active + direction)
			: (direction > 0)
			? 0
			: (rowsCount() - 1);
	} else {
		index += direction;
	}
	index = std::clamp(index, 0, rowsCount() - 1);
	setSelected(index);
	Q_EMIT scrollToRequested(rowTop(index), rowTop(index) + st::dialogsRowHeight);
}

void InnerWidget::selectSkipPage(int height, int direction) {
	const auto rows = std::max(height / st::dialogsRowHeight - 1, 1);
	selectSkip(direction * rows);
}

void InnerWidget::clearSelection() {
	setSelected(-1);
}

bool InnerWidget::chooseSelected() {
	if (_selected < 0 || _selected >= rowsCount()) {
		return false;
	}
	const auto index = _selected;
	setSelected(-1);
	activate(index);
	return true;
}

QString InnerWidget::neighbourChat(const QString &id, int direction) const {
	if (_rows.empty() || !direction) {
		return QString();
	}
	const auto count = rowsCount();
	auto index = _indexById.value(id, -1);
	if (index < 0) {
		index = (direction > 0) ? -1 : count;
	}
	for (index += direction; index >= 0 && index < count; index += direction) {
		if (_rows[index].type == Row::Type::Chat) {
			return _rows[index].chat.id;
		}
	}
	return QString();
}

// ---- dragging pinned chats

int InnerWidget::pinnedStart() const {
	auto result = 0;
	for (const auto &row : _rows) {
		if (row.type == Row::Type::Chat) {
			break;
		}
		++result;
	}
	return result;
}

int InnerWidget::pinnedCount() const {
	auto result = 0;
	for (auto i = pinnedStart(), count = rowsCount(); i != count; ++i) {
		if (!_rows[i].chat.pinned) {
			break;
		}
		++result;
	}
	return result;
}

bool InnerWidget::canDrag(int index) const {
	return (_folder == Folder::Main)
		&& !searching()
		&& (index >= 0)
		&& (index < rowsCount())
		&& (_rows[index].type == Row::Type::Chat)
		&& _rows[index].chat.pinned
		&& (pinnedCount() > 1);
}

void InnerWidget::startDrag() {
	_dragging = true;
	_dragIndex = _pressed;
	_dragGrab = _pressPosition.y() - rowTop(_pressed);
	_dragTop = rowTop(_pressed);
	_pressed = -1;
	stopRipple();
	setOver(-1);
	setCursor(Qt::ClosedHandCursor);
	update();
}

void InnerWidget::updateDrag(QPoint position) {
	const auto height = int(st::dialogsRowHeight);
	const auto first = pinnedStart();
	const auto last = first + pinnedCount() - 1;
	_dragTop = std::clamp(
		position.y() - _dragGrab,
		rowTop(first),
		rowTop(last));
	const auto target = std::clamp(
		(_dragTop + height / 2) / height,
		first,
		last);
	if (target != _dragIndex) {
		auto row = std::move(_rows[_dragIndex]);
		_rows.erase(_rows.begin() + _dragIndex);
		_rows.insert(_rows.begin() + target, std::move(row));
		_dragIndex = target;
		_indexById.clear();
		for (auto i = 0, count = rowsCount(); i != count; ++i) {
			if (_rows[i].type == Row::Type::Chat) {
				_indexById.insert(_rows[i].chat.id, i);
			}
		}
	}
	update();
}

void InnerWidget::finishDrag(bool commit) {
	if (!_dragging) {
		return;
	}
	_dragging = false;
	_dragIndex = -1;
	setCursor(Qt::ArrowCursor);
	if (commit) {
		auto order = QStringList();
		const auto first = pinnedStart();
		for (auto i = first, till = first + pinnedCount(); i != till; ++i) {
			order.push_back(_rows[i].chat.id);
		}
		// Pinned chats that are not in this list (archived ones) stay pinned.
		auto &chats = _controller->session().data().chats();
		for (const auto &chat : chats.all()) {
			if (chat.pinned && CanPin(chat) && !order.contains(chat.id)) {
				order.push_back(chat.id);
			}
		}
		if (order != pinnedOrder()) {
			savePinnedOrder(order);
			chats.setPinnedOrder(order);
		}
	}
	if (_rebuildAfterDrag || !commit) {
		rebuild();
	} else {
		update();
	}
}

} // namespace Dialogs
