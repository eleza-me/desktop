// ListWidget: the message context menu (a strip of reactions on
// top, then reply / edit / pin / copy / media actions / forward / select /
// delete; with text selected inside the message — «copy selected» and
// «quote»).
#include "history/history_list_widget.h"
#include "window/window_routes.h"
#include "data/data_groups.h"

#include "api/api_client_config.h"
#include "boxes/report_box.h"
#include "data/data_chats.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "history/history_downloads.h"
#include "history/history_reactions_strip.h"
#include "history/history_view_media.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/links.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_history.h"

#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace History {

void ListWidget::showContextMenu(
		const MouseTarget &target,
		QPoint globalPosition,
		bool allReactions) {
	using View::Zone;
	if (_menu) {
		_menu->hideMenu(true);
	}
	if (target.index < 0 || target.index >= int(_rows.size())) {
		return;
	}
	const auto element = _rows[target.index].element.get();
	const auto zone = target.hit.zone;
	// Of the centred lines only self-destructing media is a message one
	// may reply to, pin or delete.
	const auto service = element->isService();
	if ((service && (!element->data().selfDestruct || zone == Zone::Side))
		|| zone == Zone::Date
		|| zone == Zone::None) {
		return;
	}
	// A copy: the row may change while the menu is open.
	const auto message = element->data();
	const auto key = target.key;
	// An album acts as a whole (forward, delete, select); a tile under the
	// pointer adds the actions of its own picture.
	const auto album = element->isAlbum();
	const auto all = element->messages();
	const auto tile = album && target.hit.item >= 0;
	const auto item = element->messageAt(target.hit);
	const auto captioned = element->captionMessage();
	_menu = new Ui::PopupMenu(this);
	const auto menu = _menu.data();
	const auto config = _session->data().clientConfig();

	if (!_selected.isEmpty()) {
		// Selection mode: the menu is about the selected messages.
		if (!_selected.contains(key)) {
			menu->addAction(tr::lng_select(tr::now), [=, this] {
				toggleSelected(key);
			}, &st::menuIconSelect);
			menu->addSeparator();
		}
		menu->addAction(tr::lng_history_copy_selected(tr::now), [this] {
			copySelection();
		}, &st::menuIconCopy);
		const auto selected = selectedMessages();
		const auto forwardable = std::all_of(
			selected.begin(),
			selected.end(),
			[](const Data::Message &item) { return CanForward(item); });
		if (forwardable && !_anonymousPeer) {
			menu->addAction(tr::lng_history_forward_selected(tr::now), [this] {
				Q_EMIT forwardRequested(selectedMessages());
			}, &st::menuIconForward);
		}
		auto remove = Ui::MenuItem();
		remove.text = tr::lng_history_delete_selected(tr::now);
		remove.icon = &st::menuIconDelete;
		remove.destructive = true;
		remove.callback = [this] {
			Q_EMIT deleteRequested(selectedMessages());
		};
		menu->addItem(std::move(remove));
		menu->addSeparator();
		menu->addAction(tr::lng_history_clear_selection(tr::now), [this] {
			clearSelection();
		});
		menu->popup(globalPosition);
		return;
	}

	const auto anyLocal = std::any_of(all.begin(), all.end(), [](
			const Data::Message &one) {
		return one.local();
	});
	if (anyLocal) {
		// Not sent yet (or failed): only what makes sense for the queue.
		auto ids = QStringList();
		auto failed = QStringList();
		for (const auto &one : all) {
			if (one.local() && !one.clientMessageId.isEmpty()) {
				ids.push_back(one.clientMessageId);
				if (one.status == Data::MessageStatus::Failed) {
					failed.push_back(one.clientMessageId);
				}
			}
		}
		const auto session = QPointer<Main::Session>(_session);
		if (!failed.isEmpty()) {
			menu->addAction(tr::lng_try_again(tr::now), [=] {
				if (session) {
					for (const auto &id : failed) {
						session->data().outbox().retry(id);
					}
				}
			}, &st::menuIconRetry);
		}
		if (HasCopyableText(message)) {
			const auto text = message.text;
			menu->addAction(tr::lng_copy_text(tr::now), [=] {
				QGuiApplication::clipboard()->setText(text);
			}, &st::menuIconCopy);
		}
		auto remove = Ui::MenuItem();
		remove.text = tr::lng_delete(tr::now);
		remove.icon = &st::menuIconDelete;
		remove.destructive = true;
		remove.callback = [=] {
			if (session) {
				for (const auto &id : ids) {
					session->data().outbox().discard(id);
				}
			}
		};
		menu->addItem(std::move(remove));
		menu->popup(globalPosition);
		return;
	}

	// ---- reactions (a centred line shows none)
	if (_canReact && !service) {
		auto emoji = config ? config->reactionEmojiSet() : QStringList();
		if (!emoji.isEmpty()) {
			const auto strip = menu->setTopWidget(Ui::object_ptr<ReactionsStrip>(
				menu,
				emoji,
				message.myReaction,
				allReactions));
			strip->setChosenCallback([=, this](const QString &chosen) {
				menu->hideMenu(true);
				const auto index = indexOfKey(key);
				if (index >= 0) {
					toggleReaction(_rows[index].element->data(), chosen);
				}
			});
			strip->setExpandCallback([=, this] {
				const auto again = target;
				menu->hideMenu(true);
				Ui::PostponeCall(this, [=, this] {
					auto fresh = again;
					fresh.index = indexOfKey(again.key);
					showContextMenu(fresh, globalPosition, !allReactions);
				});
			});
		}
	}

	// ---- the selected part of the text
	const auto selectedHere = hasTextSelection() && (_textSelectionKey == key);
	if (selectedHere) {
		const auto selection = _textSelection;
		const auto text = element->selectedText(selection);
		auto copy = Ui::MenuItem();
		copy.text = tr::lng_history_copy_selected_text(tr::now);
		copy.icon = &st::menuIconCopy;
		copy.shortcut = QKeySequence(QKeySequence::Copy).toString(
			QKeySequence::NativeText);
		copy.callback = [=] {
			QGuiApplication::clipboard()->setText(text);
		};
		menu->addItem(std::move(copy));
		// The quote must be an exact part of the message text.
		// (The shown text has direction marks around its links.)
		if (_canWrite
			&& Ui::Text::StripIsolates(element->text()) == message.text) {
			const auto quote = MakeQuote(
				message.text,
				Ui::Text::SourceSelection(element->text(), selection),
				config ? config->maxQuoteLength() : 0);
			if (quote) {
				menu->addAction(tr::lng_chat_quote(tr::now), [=, this] {
					setTextSelection(QString(), {});
					Q_EMIT replyRequested(message, quote);
				}, &st::menuIconQuote);
			}
		}
		menu->addSeparator();
	}

	if (_canWrite) {
		// A tile of an album: the reply is to that picture.
		menu->addAction(tr::lng_reply(tr::now), [=, this] {
			Q_EMIT replyRequested(tile ? item : message, std::nullopt);
		}, &st::menuIconReply);
	}
	const auto editWindow = config ? config->editWindow() : qint64(0);
	// An album's caption lives on one of its items.
	if (_canWrite
		&& CanEdit(captioned, QDateTime::currentMSecsSinceEpoch(), editWindow)) {
		menu->addAction(tr::lng_edit(tr::now), [=, this] {
			Q_EMIT editRequested(captioned);
		}, &st::menuIconEdit);
	}
	const auto groupRow = _group
		? _session->data().chats().chat(_chatId)
		: nullptr;
	const auto groupHeader = (groupRow && groupRow->group)
		? &*groupRow->group
		: nullptr;
	// In a group pinning is a permission (the server has the last word).
	const auto mayPin = !_group
		|| (groupHeader
			&& _session->data().groups().administration()
			&& groupHeader->can(u"pin_messages"));
	if (!_anonymousPeer && mayPin && !message.underReview) {
		const auto pinned = isPinned(message.seq);
		menu->addAction(
			pinned ? tr::lng_unpin(tr::now) : tr::lng_pin(tr::now),
			[=, this] { requestPin(message, !pinned); },
			&st::menuIconPin);
	}
	if (!selectedHere && HasCopyableText(message)) {
		const auto text = message.text;
		menu->addAction(tr::lng_copy_text(tr::now), [=] {
			QGuiApplication::clipboard()->setText(text);
		}, &st::menuIconCopy);
	}
	if (zone == Zone::Link) {
		if (const auto link = element->link(target.hit.link)) {
			auto address = Ui::Text::LinkUrl(*link);
			if (address.isEmpty()) {
				address = link->text;
			}
			menu->addAction(tr::lng_copy_link(tr::now), [=, this] {
				QGuiApplication::clipboard()->setText(address);
				_controller->showToast(tr::lng_link_copied(tr::now));
			}, &st::menuIconLink);
		}
	} else if (zone == Zone::Preview && message.linkPreview) {
		const auto address = message.linkPreview->url;
		menu->addAction(tr::lng_copy_link(tr::now), [=, this] {
			QGuiApplication::clipboard()->setText(address);
			_controller->showToast(tr::lng_link_copied(tr::now));
		}, &st::menuIconLink);
	}

	// ---- media and files (of the tile under the pointer in an album)
	const auto &media = tile ? item : message;
	const auto hidden = media.mediaSpoiler && !_revealed.contains(media.key());
	// Self-destructing media is never saved or copied (nothing of it is here).
	switch (((album && !tile) || media.selfDestruct) ? Data::MessageKind::Unknown : media.kind) {
	case Data::MessageKind::Photo:
		if (!hidden) {
			menu->addAction(tr::lng_history_copy_image(tr::now), [=, this] {
				copyImage(media);
			}, &st::menuIconImage);
			menu->addAction(tr::lng_history_save_as(tr::now), [=, this] {
				saveAs(media);
			}, &st::menuIconSaveAs);
		}
		break;
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
	case Data::MessageKind::VideoNote:
		if (!hidden) {
			menu->addAction(tr::lng_history_save_as(tr::now), [=, this] {
				saveAs(media);
			}, &st::menuIconSaveAs);
		}
		break;
	case Data::MessageKind::File:
	case Data::MessageKind::Audio:
	case Data::MessageKind::Voice: {
		const auto saved = !_downloads->savedPath(media).isEmpty();
		if (media.kind == Data::MessageKind::File) {
			menu->addAction(
				saved
					? tr::lng_history_open_file(tr::now)
					: tr::lng_history_download(tr::now),
				[=, this] { openFile(media); },
				saved ? &st::menuIconOpen : &st::menuIconDownload);
		}
		if (saved) {
			menu->addAction(tr::lng_history_show_in_folder(tr::now), [=, this] {
				showInFolder(media);
			}, &st::menuIconFolder);
		}
		menu->addAction(tr::lng_history_save_as(tr::now), [=, this] {
			saveAs(media);
		}, &st::menuIconSaveAs);
	} break;
	case Data::MessageKind::Location:
	case Data::MessageKind::LiveLocation:
		menu->addAction(tr::lng_history_open_map(tr::now), [=, this] {
			openLocation(message);
		}, &st::menuIconLocation);
		if (message.kind == Data::MessageKind::LiveLocation
			&& message.mine
			&& message.live
			&& !message.live->stopped
			&& message.live->until > QDateTime::currentSecsSinceEpoch()) {
			menu->addAction(tr::lng_history_live_stop(tr::now), [=, this] {
				_session->data().messages().stopLiveLocation(_chatId, message.seq);
			}, &st::menuIconStop);
		}
		break;
	default:
		break;
	}

	const auto forwardable = std::all_of(all.begin(), all.end(), [](
			const Data::Message &one) {
		return CanForward(one) && !one.underReview;
	});
	if (forwardable && !_anonymousPeer) {
		menu->addAction(tr::lng_forward(tr::now), [=, this] {
			Q_EMIT forwardRequested(all);
		}, &st::menuIconForward);
	}
	// Who reacted (a group of the third release that shows its members).
	if (_group
		&& groupHeader
		&& !groupHeader->membersHidden
		&& !message.reactions.isEmpty()
		&& message.seq > 0
		&& _session->data().groups().thirdRelease()) {
		const auto seq = message.seq;
		menu->addAction(tr::lng_groups_reactions(tr::now), [=, this] {
			_controller->openRoute(Window::Routes::GroupReactions(_chatId, seq));
		}, &st::menuIconReactions);
	}
	if (_group && !message.mine && !message.sender.isEmpty()) {
		const auto sender = message.sender;
		menu->addAction(tr::lng_groups_sender_profile(tr::now), [=, this] {
			_controller->openRoute(Window::Routes::User(sender));
		}, &st::menuIconUser);
		if (_session->data().groups().administration() && message.seq > 0) {
			const auto seq = message.seq;
			menu->addAction(tr::lng_groups_report_message(tr::now), [=, this] {
				auto args = Boxes::ReportArgs();
				args.kind = u"group_message"_s;
				args.id = _chatId;
				args.messageSeq = seq;
				Boxes::ShowReportBox(_controller, std::move(args));
			}, &st::menuIconReport);
		}
	}
	if (selectable(target.index)) {
		menu->addAction(tr::lng_select(tr::now), [=, this] {
			toggleSelected(key);
		}, &st::menuIconSelect);
	}
	auto remove = Ui::MenuItem();
	remove.text = tr::lng_delete(tr::now);
	remove.icon = &st::menuIconDelete;
	remove.destructive = true;
	remove.callback = [=, this] {
		Q_EMIT deleteRequested(all);
	};
	menu->addItem(std::move(remove));
	menu->popup(globalPosition);
}

} // namespace History
