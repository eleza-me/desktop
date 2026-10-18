// ListWidget: the mouse, selection, and what a click on each part of a row does.
#include "history/history_list_widget.h"

#include "data/data_calls.h"
#include "data/data_chats.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "history/history_bot_logic.h"
#include "history/history_downloads.h"
#include "history/history_view_media.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "media/media_audio_player.h"
#include "media/media_image_viewer.h"
#include "media/media_video_player.h"
#include "stories/stories_viewer.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/links.h"
#include "support/media_image.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_calls.h"
#include "window/window_main_window.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_history.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>

namespace History {
namespace {

constexpr auto kDragScrollInterval = 30;
constexpr auto kDragScrollEdge = 24;
constexpr auto kMaxInlineGifs = 2;

[[nodiscard]] bool PointerZone(View::Zone zone) {
	using View::Zone;
	switch (zone) {
	case Zone::Link:
	case Zone::Spoiler:
	case Zone::Reply:
	case Zone::Media:
	case Zone::MediaSpoiler:
	case Zone::MediaCancel:
	case Zone::VoicePlay:
	case Zone::VoiceSeek:
	case Zone::VoiceSpeed:
	case Zone::File:
	case Zone::Location:
	case Zone::LiveStop:
	case Zone::Post:
	case Zone::Story:
	case Zone::Preview:
	case Zone::Reaction:
	case Zone::Button:
	case Zone::Retry:
	case Zone::ViaBot:
	case Zone::Sender:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool SameTarget(
		const View::HitResult &a,
		const View::HitResult &b) {
	return (a.zone == b.zone)
		&& (a.link == b.link)
		&& (a.row == b.row)
		&& (a.column == b.column)
		&& (a.text == b.text);
}

void ShowInFileManager(const QString &path) {
	const auto info = QFileInfo(path);
#ifdef Q_OS_WIN
	// The path is always quoted: QProcess quotes an argument only when it has
	// spaces, and Explorer splits an unquoted one at commas ("a,b.pdf").
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
	if (Downloads::IsExecutable(path)) {
		ShowInFileManager(path);
	} else {
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	}
}

} // namespace

ListWidget::MouseTarget ListWidget::targetAt(QPoint position) const {
	auto result = MouseTarget();
	const auto index = rowAt(position.y());
	if (index < 0 || !_rows[index].exact) {
		return result;
	}
	result.index = index;
	result.key = _rows[index].element->key();
	result.local = position - QPoint(0, rowTop(index));
	result.hit = _rows[index].element->hitTest(result.local);
	return result;
}

bool ListWidget::selectable(int index) const {
	return index >= 0
		&& index < int(_rows.size())
		&& !_rows[index].element->isService();
}

void ListWidget::updateHover(QPoint position) {
	const auto target = targetAt(position);
	if (target.index == _hover.index && SameTarget(target.hit, _hover.hit)) {
		_hover = target;
		return;
	}
	const auto was = _hover.index;
	const auto repaint = (target.index != was)
		|| PointerZone(target.hit.zone)
		|| PointerZone(_hover.hit.zone);
	_hover = target;
	if (repaint) {
		updateRow(was);
		updateRow(target.index);
	}
	refreshCursor();
}

void ListWidget::clearHover() {
	if (_hover.index < 0) {
		return;
	}
	const auto was = _hover.index;
	_hover = MouseTarget();
	updateRow(was);
	refreshCursor();
}

void ListWidget::refreshCursor() {
	using View::Zone;
	const auto zone = _hover.hit.zone;
	if (!_selected.isEmpty()) {
		setCursor((_hover.index >= 0 && selectable(_hover.index))
			? style::cur_pointer
			: style::cur_default);
	} else if (PointerZone(zone)) {
		setCursor(style::cur_pointer);
	} else if (zone == Zone::Text && _hover.hit.uponSymbol) {
		setCursor(style::cur_text);
	} else {
		setCursor(style::cur_default);
	}
}

void ListWidget::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	if (!_pressed) {
		clearHover();
	}
}

void ListWidget::mouseMoveEvent(QMouseEvent *e) {
	_lastMouse = e->pos();
	_lastMouseGlobal = e->globalPosition().toPoint();
	if (!_pressed) {
		updateHover(e->pos());
		return;
	}
	if (!_dragging
		&& (e->pos() - _pressPosition).manhattanLength()
			>= QApplication::startDragDistance()) {
		_dragging = true;
	}
	if (_dragging) {
		dragSelect(e->pos());
		checkDragScroll(e->pos());
	}
}

void ListWidget::mousePressEvent(QMouseEvent *e) {
	using View::Zone;
	_lastMouse = e->pos();
	_lastMouseGlobal = e->globalPosition().toPoint();
	if (e->button() != Qt::LeftButton) {
		return;
	}
	updateHover(e->pos());
	_pressed = true;
	_dragging = false;
	_dragSelectingRows = false;
	_dragStartedInText = false;
	_skipClick = false;
	_press = _hover;
	_pressPosition = e->pos();
	_selectType = Ui::Text::SelectType::Letters;
	if (!_selected.isEmpty() || _press.index < 0) {
		return;
	}
	const auto zone = _press.hit.zone;
	if (zone == Zone::Text || zone == Zone::Link || zone == Zone::Spoiler) {
		_dragStartedInText = true;
		_dragStartCursor = _press.hit.cursor;
	}
}

void ListWidget::mouseReleaseEvent(QMouseEvent *e) {
	_lastMouse = e->pos();
	_lastMouseGlobal = e->globalPosition().toPoint();
	if (e->button() != Qt::LeftButton || !_pressed) {
		return;
	}
	_pressed = false;
	_releaseModifiers = e->modifiers();
	_dragScrollTimer.stop();
	const auto dragged = std::exchange(_dragging, false);
	_dragSelectingRows = false;
	if (std::exchange(_skipClick, false) || dragged) {
		updateHover(e->pos());
		return;
	}
	const auto target = targetAt(e->pos());
	updateHover(e->pos());
	if (target.key != _press.key || target.index < 0) {
		// Released somewhere else: a click on nothing drops the selection.
		if (target.index < 0 && _press.index < 0 && hasTextSelection()) {
			setTextSelection(QString(), {});
		}
		return;
	}
	activate(target, e->button());
}

void ListWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	using View::Zone;
	_lastMouse = e->pos();
	_lastMouseGlobal = e->globalPosition().toPoint();
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto target = targetAt(e->pos());
	_pressPosition = e->pos();
	_press = target;
	_pressed = true;
	_dragging = false;
	_dragSelectingRows = false;
	_dragStartedInText = false;
	_skipClick = false;
	if (target.index < 0) {
		_skipClick = true;
		return;
	} else if (!_selected.isEmpty()) {
		// The second click of a pair toggles like the first one.
		return;
	}
	const auto element = _rows[target.index].element.get();
	const auto zone = target.hit.zone;
	if ((zone == Zone::Text || zone == Zone::Link) && target.hit.uponSymbol) {
		// A double click on text selects the word.
		_selectType = Ui::Text::SelectType::Words;
		const auto word = element->adjustSelection(
			{ target.hit.symbol, target.hit.symbol + 1 },
			_selectType);
		_selectBase = word;
		_dragStartedInText = true;
		_dragStartCursor = word.from;
		setTextSelection(target.key, word);
		_skipClick = true;
		return;
	}
	const auto &message = element->data();
	const auto plain = (zone == Zone::Bubble)
		|| (zone == Zone::Side)
		|| (zone == Zone::Text);
	if (plain && !element->isService() && message.seq > 0 && _canWrite) {
		// … and anywhere else on the message replies to it.
		_skipClick = true;
		if (hasTextSelection()) {
			setTextSelection(QString(), {});
		}
		Q_EMIT replyRequested(message, std::nullopt);
	}
}

void ListWidget::contextMenuEvent(QContextMenuEvent *e) {
	const auto position = (e->reason() == QContextMenuEvent::Mouse)
		? e->pos()
		: mapFromGlobal(_lastMouseGlobal);
	_pressed = false;
	_dragging = false;
	updateHover(position);
	showContextMenu(targetAt(position), e->globalPos());
}

void ListWidget::activate(const MouseTarget &target, Qt::MouseButton button) {
	using View::Zone;
	if (target.index < 0 || target.index >= int(_rows.size())) {
		return;
	}
	const auto element = _rows[target.index].element.get();
	const auto zone = target.hit.zone;
	if (!_selected.isEmpty()) {
		if (selectable(target.index) && zone != Zone::Date && zone != Zone::None) {
			toggleSelected(target.key);
		}
		return;
	}
	if ((_releaseModifiers & Qt::ControlModifier)
		&& selectable(target.index)
		&& (zone == Zone::Side
			|| zone == Zone::Bubble
			|| zone == Zone::Text
			|| zone == Zone::Media)) {
		// Ctrl+click starts selecting messages.
		if (hasTextSelection()) {
			setTextSelection(QString(), {});
		}
		toggleSelected(target.key);
		return;
	}
	if (hasTextSelection() && zone != Zone::Link) {
		// A click anywhere drops the selection first.
		setTextSelection(QString(), {});
		if (zone == Zone::Text || zone == Zone::Bubble || zone == Zone::Side) {
			return;
		}
	}
	// A copy: actions below may change the timeline under us.
	const auto message = element->data();
	// The tile of an album under the pointer (the message itself elsewhere).
	const auto item = element->messageAt(target.hit);
	switch (zone) {
	case Zone::Sender:
		// The name above a bubble of a group, or the picture next to it.
		if (!target.hit.text.isEmpty()) {
			_controller->openRoute(Window::Routes::User(target.hit.text));
		}
		break;
	case Zone::Link:
		if (const auto link = element->link(target.hit.link)) {
			if (link->type == Ui::Text::EntityType::BotCommand) {
				// Sent like a typed message (only where I may write).
				if (_canWrite && button == Qt::LeftButton) {
					Q_EMIT botCommandRequested(link->text);
				}
			} else {
				Support::ActivateTextLink(_controller, *link, button);
			}
		}
		break;
	case Zone::Spoiler:
		revealSpoiler(target.key);
		break;
	case Zone::Reply:
		jumpTo(message.replyToSeq, message.quote);
		break;
	case Zone::Media:
		openMedia(item);
		break;
	case Zone::MediaSpoiler:
		_revealed.insert(item.key());
		updateRow(target.index);
		updateInlineVideos();
		break;
	case Zone::MediaCancel:
		cancelTransfer(item);
		break;
	case Zone::VoicePlay:
		toggleVoice(message);
		break;
	case Zone::VoiceSeek:
		Media::Player().seek(target.hit.ratio);
		break;
	case Zone::VoiceSpeed:
		Media::Player().cycleSpeed();
		break;
	case Zone::File:
		openFile(message);
		break;
	case Zone::Location:
		openLocation(message);
		break;
	case Zone::LiveStop:
		_session->data().messages().stopLiveLocation(_chatId, message.seq);
		break;
	case Zone::Post:
		if (!message.forwardedPost.isEmpty()) {
			_controller->openRoute(Window::Routes::Post(message.forwardedPost));
		}
		break;
	case Zone::Story:
		if (message.story) {
			Stories::OpenStory(_controller, message.story->owner, message.story->storyId);
		}
		break;
	case Zone::Preview:
		if (message.linkPreview) {
			Support::OpenLink(_controller, message.linkPreview->url);
		}
		break;
	case Zone::Reaction:
		toggleReaction(message, target.hit.text);
		break;
	case Zone::Button:
		pressButton(message, target.hit.row, target.hit.column);
		break;
	case Zone::Retry:
		showRetryMenu(message, _lastMouseGlobal);
		break;
	case Zone::ViaBot:
		// «via @bot»: inline mode with that bot in this chat's composer
		// (the chat screen opens the bot's profile where there is none).
		if (message.viaBot && !message.viaBot->handle.isEmpty()) {
			Q_EMIT inlineQueryRequested(
				InlineQueryPrefill(message.viaBot->handle, QString()),
				false);
		}
		break;
	case Zone::Side:
		// A plain click next to a message does nothing (a
		// drag or Ctrl+click selects, a double click replies).
		Q_EMIT focusRequested();
		break;
	default:
		break;
	}
}

// ---- selection

void ListWidget::toggleSelected(const QString &key) {
	const auto index = indexOfKey(key);
	if (index < 0 || !selectable(index)) {
		return;
	}
	if (hasTextSelection()) {
		setTextSelection(QString(), {});
	}
	if (!_selected.remove(key)) {
		_selected.insert(key);
	}
	updateRow(index);
	refreshCursor();
	Q_EMIT selectionChanged();
}

void ListWidget::setSelectedRange(int from, int till) {
	auto selected = _dragBase;
	for (auto i = std::max(from, 0); i <= till && i < int(_rows.size()); ++i) {
		if (!selectable(i)) {
			continue;
		}
		const auto &key = _rows[i].element->key();
		if (_dragAdds) {
			selected.insert(key);
		} else {
			selected.remove(key);
		}
	}
	if (selected == _selected) {
		return;
	}
	_selected = selected;
	update(0, _visibleTop, width(), _visibleBottom - _visibleTop);
	Q_EMIT selectionChanged();
}

void ListWidget::setTextSelection(
		const QString &key,
		Ui::Text::Selection selection) {
	if (selection.empty()) {
		if (_textSelectionKey.isEmpty()) {
			return;
		}
		const auto index = indexOfKey(_textSelectionKey);
		_textSelectionKey = QString();
		_textSelection = Ui::Text::Selection();
		updateRow(index);
		Q_EMIT selectionChanged();
		return;
	}
	if (_textSelectionKey == key && _textSelection == selection) {
		return;
	}
	const auto previous = indexOfKey(_textSelectionKey);
	_textSelectionKey = key;
	_textSelection = selection;
	const auto index = indexOfKey(key);
	if (previous != index) {
		updateRow(previous);
	}
	updateRow(index);
	Q_EMIT selectionChanged();
}

void ListWidget::dragSelect(QPoint position) {
	const auto pressIndex = indexOfKey(_press.key);
	if (pressIndex < 0 || _rows.empty()) {
		return;
	}
	const auto current = rowNear(std::clamp(position.y(), 0, height() - 1));
	if (!_dragSelectingRows) {
		const auto leftTheRow = (current != pressIndex);
		if (!leftTheRow && (_dragStartedInText || _selected.isEmpty())) {
			if (_dragStartedInText && _selected.isEmpty()) {
				// Selecting text inside the message.
				const auto element = _rows[pressIndex].element.get();
				const auto local = position - QPoint(0, rowTop(pressIndex));
				const auto cursor = element->textCursorAt(local);
				auto selection = Ui::Text::Selection{
					std::min(_dragStartCursor, cursor),
					std::max(_dragStartCursor, cursor),
				};
				if (_selectType != Ui::Text::SelectType::Letters) {
					const auto adjusted = element->adjustSelection(
						selection,
						_selectType);
					selection.from = std::min(adjusted.from, _selectBase.from);
					selection.to = std::max(adjusted.to, _selectBase.to);
				}
				setTextSelection(_press.key, selection);
			}
			return;
		}
		// The drag went over other messages: whole rows from now on.
		_dragSelectingRows = true;
		_dragBase = _selected;
		_dragAdds = !_selected.contains(_press.key);
		if (hasTextSelection()) {
			setTextSelection(QString(), {});
		}
	}
	setSelectedRange(
		std::min(pressIndex, current),
		std::max(pressIndex, current));
}

void ListWidget::checkDragScroll(QPoint position) {
	if (!_scroll || !_pressed || !_dragging) {
		_dragScrollTimer.stop();
		return;
	}
	auto delta = 0;
	if (position.y() < _visibleTop + kDragScrollEdge) {
		delta = position.y() - (_visibleTop + kDragScrollEdge);
	} else if (position.y() > _visibleBottom - kDragScrollEdge) {
		delta = position.y() - (_visibleBottom - kDragScrollEdge);
	}
	if (!delta) {
		_dragScrollTimer.stop();
		return;
	}
	if (!_dragScrollTimer.isActive()) {
		_dragScrollTimer.start(kDragScrollInterval, this);
	}
	_scroll->scrollToY(_scroll->scrollTop() + std::clamp(delta, -60, 60));
}

bool ListWidget::hasTextSelection() const {
	return !_textSelectionKey.isEmpty() && !_textSelection.empty();
}

void ListWidget::clearSelection() {
	const auto had = !_selected.isEmpty() || hasTextSelection();
	_selected.clear();
	_textSelectionKey = QString();
	_textSelection = Ui::Text::Selection();
	if (had) {
		update(0, _visibleTop, width(), _visibleBottom - _visibleTop);
		refreshCursor();
		Q_EMIT selectionChanged();
	}
}

QVector<Data::Message> ListWidget::selectedMessages() const {
	auto result = QVector<Data::Message>();
	if (_selected.isEmpty()) {
		return result;
	}
	result.reserve(_selected.size());
	for (const auto &row : _rows) {
		if (_selected.contains(row.element->key())) {
			// An album is selected as a whole: every item of it.
			result.append(row.element->messages());
		}
	}
	return result;
}

QString ListWidget::copyText(const QVector<Data::Message> &messages) {
	auto items = std::vector<CopyItem>();
	items.reserve(messages.size());
	for (const auto &message : messages) {
		auto item = CopyItem();
		item.sender = senderName(message);
		item.time = MessageDateTime(message);
		item.text = IsService(message)
			? Support::MessagePreview(message)
			: MessagePlainText(message, Support::MessageKindLabel(message));
		items.push_back(std::move(item));
	}
	return FormatForCopy(items);
}

bool ListWidget::copySelection() {
	if (hasTextSelection()) {
		const auto index = indexOfKey(_textSelectionKey);
		if (index >= 0) {
			QGuiApplication::clipboard()->setText(
				_rows[index].element->selectedText(_textSelection));
			return true;
		}
	}
	const auto messages = selectedMessages();
	if (messages.isEmpty()) {
		return false;
	}
	QGuiApplication::clipboard()->setText(copyText(messages));
	_controller->showToast(tr::lng_copied(tr::now));
	clearSelection();
	return true;
}

// ---- actions

void ListWidget::revealSpoiler(const QString &key) {
	const auto index = indexOfKey(key);
	if (index < 0) {
		return;
	}
	_revealed.insert(key);
	_rows[index].element->setSpoilersRevealed(true);
	updateRow(index);
}

void ListWidget::openMedia(const Data::Message &message) {
	if (message.call) {
		// A call record: a click calls back, where a call is possible.
		const auto chat = _session->data().chats().chat(message.chat);
		if (chat
			&& Window::CallsSupported()
			&& (Data::CallAvailabilityFor(*chat)
				== Data::CallAvailability::Available)) {
			// The same kind of call again (where video calls still exist).
			Window::StartCall(
				_controller,
				chat->peerUserId,
				message.call->video);
		}
		return;
	}
	const auto index = indexOfKey(message.key());
	const auto inAlbum = (index >= 0) && _rows[index].element->isAlbum();
	switch (message.kind) {
	case Data::MessageKind::Photo:
		openPhoto(message);
		break;
	case Data::MessageKind::Video:
		// A video of an album opens in the viewer with its neighbours.
		if (inAlbum) {
			openPhoto(message);
		} else {
			playVideo(message);
		}
		break;
	case Data::MessageKind::Gif:
		playVideo(message);
		break;
	case Data::MessageKind::VideoNote:
		toggleInlineVideo(message);
		break;
	default:
		break;
	}
}

View::Media *ListWidget::mediaFor(const Data::Message &message) const {
	const auto index = indexOfKey(message.key());
	if (index < 0) {
		return nullptr;
	}
	const auto element = _rows[index].element.get();
	const auto media = element->media();
	if (!media || !element->isAlbum()) {
		return media;
	}
	const auto &album = element->album();
	for (auto i = 0; i != int(album.size()); ++i) {
		if (album[i].key() == message.key()) {
			return media->albumTile(i);
		}
	}
	return nullptr;
}

void ListWidget::openPhoto(const Data::Message &message) {
	// Every photo of the chat, and the videos of the albums among them: ← →
	// go through an album and on to the pictures around it.
	auto items = std::vector<Media::ViewerItem>();
	auto index = 0;
	for (const auto &row : _rows) {
	const auto album = row.element->isAlbum();
	for (const auto &other : row.element->messages()) {
		const auto video = album && (other.kind == Data::MessageKind::Video);
		if (other.kind != Data::MessageKind::Photo && !video) {
			continue;
		} else if (other.mediaSpoiler
			&& !_revealed.contains(other.key())
			&& other.key() != message.key()) {
			// Hidden pictures stay hidden while flipping through.
			continue;
		}
		auto item = Media::ViewerItem();
		if (video) {
			if (const auto media = mediaFor(other)) {
				item.videoPoster = media->currentImage();
			}
			if (!other.localPath.isEmpty() && QFileInfo::exists(other.localPath)) {
				item.videoPath = other.localPath;
			} else if (other.seq > 0) {
				const auto downloads = QPointer<Downloads>(_downloads);
				item.loadVideo = [=](
						QObject *guard,
						std::function<void(QString)> done) {
					if (!downloads) {
						done(QString());
						return;
					}
					downloads->fetch(
						other,
						Data::MediaVariant::Mp4,
						guard,
						[=](const QString &path) { done(path); });
				};
			} else {
				continue;
			}
		} else if (other.seq > 0) {
			auto options = Support::ImageOptions();
			options.original = true;
			if (other.media) {
				options.blurhash = other.media->blurhash;
				options.aspect = QSize(other.media->width, other.media->height);
			}
			item.source = Support::MakeImageSource(
				_session,
				Data::MediaRef::Attachment(other.chat, other.seq),
				Data::MediaVariant::Full,
				options);
			if (const auto media = mediaFor(other)) {
				item.preview = media->imageSource();
				item.image = QImage();
			}
		} else if (!other.localPath.isEmpty()) {
			item.filePath = other.localPath;
		} else {
			continue;
		}
		// An album has one caption: every item of it shows it.
		item.caption = album
			? row.element->captionMessage().text
			: other.text;
		item.saveName = Downloads::SuggestedName(other);
		if (other.key() == message.key()) {
			index = int(items.size());
		}
		items.push_back(std::move(item));
	}
	}
	if (!items.empty()) {
		Media::ShowImageViewer(_controller->window(), std::move(items), index);
	}
}

void ListWidget::playVideo(const Data::Message &message) {
	const auto show = [=, this](const QString &path, const QImage &poster) {
		auto item = Media::ViewerItem();
		item.videoPath = path;
		item.videoPoster = poster;
		item.caption = message.text;
		item.saveName = Downloads::SuggestedName(message);
		auto items = std::vector<Media::ViewerItem>();
		items.push_back(std::move(item));
		Media::ShowImageViewer(_controller->window(), std::move(items), 0);
	};
	auto poster = QImage();
	if (const auto media = mediaFor(message)) {
		poster = media->currentImage();
	}
	if (!message.localPath.isEmpty() && QFileInfo::exists(message.localPath)) {
		show(message.localPath, poster);
		return;
	} else if (message.seq <= 0) {
		return;
	}
	_downloads->fetch(message, Data::MediaVariant::Mp4, this, [=, this](
			const QString &path) {
		if (path.isEmpty()) {
			return;
		}
		// Do not pop up over another chat.
		if (isVisible()) {
			show(path, poster);
		}
	});
}

void ListWidget::startInlineVideo(
		const QString &key,
		const QString &path,
		bool gif) {
	const auto index = indexOfKey(key);
	if (index < 0 || path.isEmpty()) {
		removeInlineVideo(key);
		return;
	}
	auto found = std::find_if(_videos.begin(), _videos.end(), [&](
			const InlineVideo &video) {
		return video.key == key;
	});
	if (found == _videos.end()) {
		_videos.push_back(InlineVideo{ key, nullptr, nullptr, gif });
		found = _videos.end() - 1;
	}
	if (found->widget) {
		return;
	}
	const auto element = _rows[index].element.get();
	const auto widget = Ui::CreateChild<Media::VideoPlayerWidget>(this);
	found->widget = widget;
	found->gif = gif;
	// Clicks, menus and selection stay with the list.
	Ui::MakeTransparentForMouse(widget);
	widget->setControlsShown(false);
	widget->setFullscreenAllowed(false);
	widget->setLooped(gif);
	widget->setMuted(gif);
	if (const auto media = element->media()) {
		widget->setPoster(media->currentImage());
	}
	const auto rect = element->mediaRect().translated(0, rowTop(index));
	widget->setRadius(gif ? int(st::historyMediaRadius) : (rect.width() / 2));
	widget->setGeometry(rect);
	widget->setSource(path);
	widget->show();
	widget->play();
	connect(widget, &Media::VideoPlayerWidget::failed, this, [=, this] {
		_videoFailed.insert(key);
		removeInlineVideo(key);
	});
	if (!gif) {
		connect(widget, &Media::VideoPlayerWidget::finished, this, [=, this] {
			removeInlineVideo(key);
		});
	}
}

void ListWidget::toggleInlineVideo(const Data::Message &message) {
	const auto key = message.key();
	for (const auto &video : _videos) {
		if (video.key == key && video.widget) {
			video.widget->toggle();
			return;
		}
	}
	if (!message.localPath.isEmpty() && QFileInfo::exists(message.localPath)) {
		startInlineVideo(key, message.localPath, false);
		return;
	} else if (message.seq <= 0) {
		return;
	}
	if (!message.mine && !message.played) {
		_session->data().messages().markPlayed(_chatId, message.seq);
	}
	_downloads->fetch(message, Data::MediaVariant::Mp4, this, [=, this](
			const QString &path) {
		if (!path.isEmpty()) {
			startInlineVideo(key, path, false);
		}
	});
}

void ListWidget::removeInlineVideo(const QString &key) {
	for (auto i = _videos.begin(); i != _videos.end(); ++i) {
		if (i->key == key) {
			if (i->widget) {
				i->widget->hide();
				i->widget->deleteLater();
			}
			_videos.erase(i);
			updateRow(indexOfKey(key));
			return;
		}
	}
}

// GIFs on screen play by themselves (muted, looped), two at a time.
void ListWidget::updateInlineVideos() {
	if (_rows.empty()) {
		return;
	}
	// Whatever scrolled away or lost its row stops.
	for (auto i = _videos.begin(); i != _videos.end();) {
		const auto index = indexOfKey(i->key);
		const auto visible = (index >= 0)
			&& (rowTop(index) + _rows[index].height > _visibleTop - 200)
			&& (rowTop(index) < _visibleBottom + 200);
		if (visible) {
			++i;
			continue;
		}
		if (i->widget) {
			i->widget->hide();
			i->widget->deleteLater();
		}
		i = _videos.erase(i);
	}
	if (Ui::AnimationsDisabled()) {
		return;
	}
	auto gifs = 0;
	for (const auto &video : _videos) {
		gifs += video.gif ? 1 : 0;
	}
	const auto first = rowNear(_visibleTop);
	for (auto i = first; i < int(_rows.size()) && gifs < kMaxInlineGifs; ++i) {
		const auto top = rowTop(i);
		if (top > _visibleBottom) {
			break;
		}
		const auto element = _rows[i].element.get();
		const auto &message = element->data();
		const auto key = element->key();
		if (message.kind != Data::MessageKind::Gif
			|| message.seq <= 0
			|| !_rows[i].exact
			|| _videoFailed.contains(key)
			|| (message.mediaSpoiler && !_revealed.contains(key))) {
			continue;
		}
		const auto known = std::any_of(_videos.begin(), _videos.end(), [&](
				const InlineVideo &video) {
			return video.key == key;
		});
		if (known) {
			continue;
		}
		++gifs;
		auto options = Data::Media::Options();
		options.decode = false;
		auto handle = _session->data().media().request(
			Data::MediaRef::Attachment(message.chat, message.seq),
			Data::MediaVariant::Mp4,
			options);
		_videos.push_back(InlineVideo{ key, nullptr, handle, true });
		const auto raw = handle.get();
		const auto ready = [=, this] {
			const auto path = raw->filePath();
			if (path.isEmpty()) {
				_videoFailed.insert(key);
				removeInlineVideo(key);
			} else {
				startInlineVideo(key, path, true);
			}
		};
		if (raw->isReady()) {
			Ui::PostponeCall(this, ready);
		} else if (raw->isFailed()) {
			_videoFailed.insert(key);
		} else {
			connect(raw, &Data::MediaHandle::ready, this, ready);
			connect(raw, &Data::MediaHandle::failed, this, [=, this] {
				_videoFailed.insert(key);
				removeInlineVideo(key);
			});
		}
	}
}

void ListWidget::placeInlineVideos() {
	for (const auto &video : _videos) {
		if (!video.widget) {
			continue;
		}
		const auto index = indexOfKey(video.key);
		if (index >= 0 && _rows[index].exact) {
			video.widget->setGeometry(
				_rows[index].element->mediaRect().translated(0, rowTop(index)));
		}
	}
}

void ListWidget::toggleVoice(const Data::Message &message) {
	auto track = Media::AudioTrack();
	const auto local = !message.localPath.isEmpty()
		&& QFileInfo::exists(message.localPath);
	if (message.seq > 0) {
		track.id = Media::AudioTrack::IdForMessage(message.chat, message.seq);
	} else if (local) {
		track.id = u"local:"_s + message.clientMessageId;
	} else {
		return;
	}
	if (local) {
		track.filePath = message.localPath;
	} else {
		track.ref = Data::MediaRef::Attachment(message.chat, message.seq);
	}
	track.voice = (message.kind == Data::MessageKind::Voice);
	track.durationMs = message.media ? message.media->durationMs : 0;
	if (message.audio) {
		track.title = message.audio->title;
		track.performer = message.audio->performer;
	}
	if (track.title.isEmpty()) {
		track.title = track.voice ? tr::lng_media_voice(tr::now) : message.text;
	}
	const auto &player = Media::Player();
	const auto starting = !player.isCurrent(track.id);
	Media::Player().toggle(track, _session);
	if (starting
		&& track.voice
		&& !message.mine
		&& !message.played
		&& message.seq > 0) {
		_session->data().messages().markPlayed(_chatId, message.seq);
	}
	if (Media::Player().isLoading(track.id) && !_spinTimer.isActive()) {
		_spinTimer.start(40, this);
	}
}

void ListWidget::openFile(const Data::Message &message) {
	if (message.seq <= 0) {
		if (!message.localPath.isEmpty() && QFileInfo::exists(message.localPath)) {
			LaunchFile(message.localPath);
		}
		return;
	}
	const auto saved = _downloads->savedPath(message);
	if (!saved.isEmpty()) {
		LaunchFile(saved);
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

void ListWidget::showInFolder(const Data::Message &message) {
	const auto saved = _downloads->savedPath(message);
	if (!saved.isEmpty()) {
		ShowInFileManager(saved);
	} else if (!message.localPath.isEmpty() && QFileInfo::exists(message.localPath)) {
		ShowInFileManager(message.localPath);
	}
}

void ListWidget::saveAs(const Data::Message &message) {
	const auto name = Downloads::SuggestedName(message);
	const auto window = QPointer<QWidget>(_controller->window());
	const auto save = [=](const QString &path) {
		if (window && !path.isEmpty()) {
			Support::SaveFileAs(window, path, name);
		}
	};
	if (!message.localPath.isEmpty() && QFileInfo::exists(message.localPath)) {
		save(message.localPath);
		return;
	} else if (message.seq <= 0) {
		return;
	}
	const auto saved = _downloads->savedPath(message);
	if (!saved.isEmpty()) {
		save(saved);
		return;
	}
	_downloads->fetch(message, Downloads::FileVariant(message), _controller, save);
}

void ListWidget::copyImage(const Data::Message &message) {
	const auto media = mediaFor(message);
	const auto image = media ? media->currentImage() : QImage();
	if (image.isNull()) {
		_controller->showToast(tr::lng_chat_file_isnt_available(tr::now));
		return;
	}
	QGuiApplication::clipboard()->setImage(image);
	_controller->showToast(tr::lng_history_image_copied(tr::now));
}

void ListWidget::openLocation(const Data::Message &message) {
	if (!message.location) {
		return;
	}
	const auto lat = QString::number(message.location->lat, 'f', 6);
	const auto lng = QString::number(message.location->lng, 'f', 6);
	Support::OpenLink(
		_controller,
		u"https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2"_s.arg(lat, lng));
}

void ListWidget::pressButton(const Data::Message &message, int row, int column) {
	if (!message.replyMarkup
		|| row < 0
		|| row >= message.replyMarkup->inlineKeyboard.size()) {
		return;
	}
	const auto &buttons = message.replyMarkup->inlineKeyboard[row];
	if (column < 0 || column >= buttons.size()) {
		return;
	}
	const auto &button = buttons[column];
	switch (ButtonActionFor(button)) {
	case ButtonAction::None:
		return;
	case ButtonAction::Url:
		Support::OpenLink(_controller, button.url);
		return;
	case ButtonAction::SwitchInlineCurrent:
	case ButtonAction::SwitchInlinePick: {
		// Never asked of the bot: «@bot query» goes into a composer.
		const auto handle = ButtonBotHandle(message, _botHandle);
		if (!handle.isEmpty()) {
			Q_EMIT inlineQueryRequested(
				InlineQueryPrefill(handle, button.query.value_or(QString())),
				ButtonActionFor(button) == ButtonAction::SwitchInlinePick);
		}
		return;
	}
	case ButtonAction::Callback:
		break;
	}
	if (message.seq <= 0) {
		return;
	}
	const auto key = u"%1:%2:%3"_s.arg(message.seq).arg(row).arg(column);
	if (_loadingButtons.contains(key)) {
		return;
	}
	_loadingButtons.insert(key);
	if (!_spinTimer.isActive()) {
		_spinTimer.start(40, this);
	}
	const auto seq = message.seq;
	const auto finish = [=, this] {
		_loadingButtons.remove(key);
		updateRow(indexOfSeq(seq));
	};
	_session->data().messages().pressButton(_chatId, seq, row, column, this, [=, this](
			const Data::CallbackAnswer &answer) {
		finish();
		if (!answer.url.isEmpty()) {
			Support::OpenLink(_controller, answer.url);
		} else if (answer.text.isEmpty()) {
			return;
		} else if (answer.alert) {
			_controller->show(Ui::MakeInformBox(answer.text));
		} else {
			_controller->showToast(answer.text);
		}
	}, [=, this](const Api::Error &error) {
		finish();
		Support::ShowError(_controller, error);
	});
}

void ListWidget::repaintSpinners() {
	auto any = false;
	for (const auto &key : std::as_const(_loadingButtons)) {
		const auto seq = key.section(u':', 0, 0).toLongLong();
		updateRow(indexOfSeq(seq));
		any = true;
	}
	const auto &player = Media::Player();
	if (player.state() == Media::AudioPlayer::State::Loading) {
		static const auto kPrefix = u"chat:"_s;
		const auto prefix = kPrefix + _chatId + u':';
		const auto &id = player.current().id;
		if (id.startsWith(prefix)) {
			updateRow(indexOfSeq(id.mid(prefix.size()).toLongLong()));
			any = true;
		}
	}
	if (!any) {
		_spinTimer.stop();
	}
}

void ListWidget::toggleReaction(const Data::Message &message, const QString &emoji) {
	if (message.seq <= 0 || emoji.isEmpty() || !_canReact) {
		return;
	}
	_session->data().messages().react(
		_chatId,
		message.seq,
		(message.myReaction == emoji) ? QString() : emoji);
}

void ListWidget::cancelTransfer(const Data::Message &message) {
	if (message.local()) {
		if (!message.clientMessageId.isEmpty()) {
			_session->data().outbox().discard(message.clientMessageId);
		}
	} else {
		_downloads->cancel(message);
	}
}

bool ListWidget::isPinned(qint64 seq) const {
	if (seq <= 0 || _debugMode) {
		return false;
	}
	const auto &known = history();
	if (known.info && known.info->pins.contains(seq)) {
		return true;
	}
	for (const auto &pin : known.pins) {
		if (pin.message.seq == seq) {
			return true;
		}
	}
	return false;
}

void ListWidget::requestPin(const Data::Message &message, bool pin) {
	const auto seq = message.seq;
	if (seq <= 0) {
		return;
	}
	auto &data = _session->data();
	const auto chat = data.chats().chat(_chatId);
	if (!pin) {
		auto scope = u"both"_s;
		for (const auto &known : history().pins) {
			if (known.message.seq == seq && !known.scope.isEmpty()) {
				scope = known.scope;
			}
		}
		data.messages().unpin(_chatId, seq, scope);
		_controller->showToast(tr::lng_history_unpinned(tr::now));
		return;
	}
	// «Also pin for …» in chats with somebody else; in a group a pin is
	// always for everyone.
	const auto withPeer = chat && !chat->saved && !chat->peerDeleted && !chat->isGroup;
	const auto both = std::make_shared<bool>(withPeer);
	const auto session = QPointer<Main::Session>(_session);
	const auto chatId = _chatId;
	const auto controller = _controller;
	auto args = Ui::ConfirmBoxArgs();
	const auto group = chat && chat->isGroup;
	args.text = group
		? tr::lng_groups_pin_sure(tr::now)
		: tr::lng_history_pin_sure(tr::now);
	args.confirmText = tr::lng_pin(tr::now);
	if (group) {
		*both = true;
	}
	if (withPeer) {
		args.checkboxText = tr::lng_history_pin_also_for(
			tr::now,
			lt_name,
			Support::ChatTitle(_session, *chat));
		args.checkboxChecked = true;
		args.checkboxChanged = [=](bool checked) { *both = checked; };
	}
	args.confirmed = [=](std::function<void()> close) {
		if (session) {
			session->data().messages().pin(
				chatId,
				seq,
				*both ? u"both"_s : u"me"_s);
			controller->showToast(tr::lng_history_pinned(tr::now));
		}
		close();
	};
	_controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ListWidget::showRetryMenu(const Data::Message &message, QPoint globalPosition) {
	if (message.clientMessageId.isEmpty()) {
		return;
	}
	if (_menu) {
		_menu->hideMenu(true);
	}
	_menu = new Ui::PopupMenu(this);
	const auto id = message.clientMessageId;
	const auto session = QPointer<Main::Session>(_session);
	if (!message.errorText.isEmpty()) {
		auto item = Ui::MenuItem();
		item.text = message.errorText;
		item.disabled = true;
		_menu->addItem(std::move(item));
		_menu->addSeparator();
	}
	_menu->addAction(tr::lng_try_again(tr::now), [=] {
		if (session) {
			session->data().outbox().retry(id);
		}
	}, &st::menuIconRetry);
	auto remove = Ui::MenuItem();
	remove.text = tr::lng_delete(tr::now);
	remove.icon = &st::menuIconDelete;
	remove.destructive = true;
	remove.callback = [=] {
		if (session) {
			session->data().outbox().discard(id);
		}
	};
	_menu->addItem(std::move(remove));
	_menu->popup(globalPosition);
}

} // namespace History
