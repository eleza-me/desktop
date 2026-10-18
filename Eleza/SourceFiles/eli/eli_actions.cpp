#include "eli/eli_actions.h"

#include "api/api_client_config.h"
#include "boxes/peer_list_box.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_outbox.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "eli/eli_post_card.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "stories/stories_boxes.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"
#include "styles/style_stories.h"

#include <QtCore/QDateTime>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Eli {
namespace {

constexpr auto kDefaultEditWindow = qint64(48) * 3600;

[[nodiscard]] const Data::Post *Find(
		Window::SessionController *controller,
		const QString &postId) {
	return controller->session().data().feed().post(postId);
}

[[nodiscard]] bool Mine(
		Window::SessionController *controller,
		const Data::Post &post) {
	return post.author == controller->session().userId();
}

[[nodiscard]] bool CanEdit(
		Window::SessionController *controller,
		const Data::Post &post) {
	auto window = controller->session().account().clientConfig().postEditWindow();
	if (window <= 0) {
		window = kDefaultEditWindow;
	}
	const auto age = QDateTime::currentMSecsSinceEpoch() - post.createdAt;
	return (age < window * 1000)
		&& (post.type == u"text"
			|| post.type == u"short_text"
			|| post.type == u"media"
			|| post.type == u"quote");
}

void CopyText(
		Window::SessionController *controller,
		const QString &text,
		const QString &toast) {
	QGuiApplication::clipboard()->setText(text);
	controller->showToast(toast);
}

struct PolicyOption {
	QString key;
	QString label;
};

[[nodiscard]] QVector<PolicyOption> Policies() {
	return {
		{ u"everyone"_s, tr::lng_eli_policy_everyone(tr::now) },
		{ u"followers"_s, tr::lng_eli_policy_followers(tr::now) },
		{ u"following"_s, tr::lng_eli_policy_following(tr::now) },
		{ u"mentioned"_s, tr::lng_eli_policy_mentioned(tr::now) },
	};
}

void FillMore(
		Ui::PopupMenu *menu,
		Window::SessionController *controller,
		const Data::Post &post,
		bool withQuick) {
	const auto id = post.id;
	const auto author = post.author;
	menu->addAction(
		post.my.bookmarked ? tr::lng_eli_saved(tr::now) : tr::lng_eli_save(tr::now),
		[=] { ToggleBookmark(controller, id); },
		&st::menuIconBookmark);
	menu->addAction(
		tr::lng_copy_link(tr::now),
		[=] { CopyPostLink(controller, id); },
		&st::menuIconLink);
	menu->addAction(
		tr::lng_eli_send_in_chat(tr::now),
		[=] { SendPostToChats(controller, id); },
		&st::eliMenuIconSend);
	if (!post.deleted && controller->session().data().stories().enabled()) {
		menu->addAction(
			tr::lng_stories_share(tr::now),
			[=] { Stories::ShareToStory(controller, id); },
			&st::storiesMenuIconStory);
	}
	if (!post.text.isEmpty()) {
		const auto text = post.text;
		menu->addAction(
			tr::lng_copy_text(tr::now),
			[=] { CopyText(controller, text, tr::lng_text_copied(tr::now)); },
			&st::menuIconCopy);
	}
	menu->addSeparator();
	if (Mine(controller, post)) {
		const auto canEdit = CanEdit(controller, post);
		auto edit = Ui::MenuItem();
		edit.text = canEdit
			? tr::lng_edit(tr::now)
			: tr::lng_eli_edit_window_over(tr::now);
		edit.icon = &st::menuIconEdit;
		edit.disabled = !canEdit;
		edit.callback = [=] {
			auto args = ComposeArgs();
			args.kind = ComposeArgs::Kind::Edit;
			args.postId = id;
			OpenCompose(controller, args);
		};
		menu->addItem(std::move(edit));

		auto policy = Ui::MenuItem();
		policy.text = tr::lng_eli_who_can_reply(tr::now);
		policy.icon = &st::eliMenuIconPolicy;
		const auto current = post.replyPolicy.isEmpty()
			? u"everyone"_s
			: post.replyPolicy;
		policy.fillSubmenu = [=](Ui::PopupMenu *submenu) {
			for (const auto &option : Policies()) {
				auto item = Ui::MenuItem();
				item.text = option.label;
				item.checked = (option.key == current);
				const auto key = option.key;
				item.callback = [=] {
					if (key == current) {
						return;
					}
					controller->session().data().feed().setReplyPolicy(id, key);
					controller->showToast(tr::lng_eli_policy_changed(tr::now));
				};
				submenu->addItem(std::move(item));
			}
		};
		menu->addItem(std::move(policy));

		const auto pinned = post.pinned;
		menu->addAction(
			pinned ? tr::lng_unpin(tr::now) : tr::lng_eli_pin_profile(tr::now),
			[=] {
				controller->session().data().feed().setPinned(id, !pinned);
				controller->showToast(pinned
					? tr::lng_eli_unpinned_toast(tr::now)
					: tr::lng_eli_pinned_toast(tr::now));
			},
			&st::menuIconPin);
		auto remove = Ui::MenuItem();
		remove.text = tr::lng_eli_delete_post(tr::now);
		remove.icon = &st::menuIconDelete;
		remove.destructive = true;
		remove.callback = [=] { ConfirmDeletePost(controller, id); };
		menu->addItem(std::move(remove));
	} else {
		const auto user = controller->session().data().users().peek(author);
		const auto handle = (user.handle.isEmpty() || user.deleted)
			? QString()
			: IsolatedHandle(user.handle);
		menu->addAction(
			tr::lng_eli_not_interested(tr::now),
			[=] {
				controller->session().data().feed().notInterested(id);
				controller->showToast(tr::lng_eli_fewer_like_this(tr::now));
			},
			&st::eliMenuIconNotInterested);
		menu->addAction(
			handle.isEmpty()
				? tr::lng_profile_mute(tr::now)
				: tr::lng_eli_mute_handle(tr::now, lt_handle, handle),
			[=] {
				controller->session().data().graph().mute(author);
				controller->showToast(tr::lng_eli_muted_toast(
					tr::now,
					lt_handle,
					handle.isEmpty() ? tr::lng_eli_this_user(tr::now) : handle));
			},
			&st::menuIconMute);
		auto block = Ui::MenuItem();
		block.text = tr::lng_block_user(tr::now);
		block.icon = &st::menuIconBlock;
		block.destructive = true;
		block.callback = [=] { ConfirmBlock(controller, author); };
		menu->addItem(std::move(block));
		auto report = Ui::MenuItem();
		report.text = tr::lng_eli_report_post(tr::now);
		report.icon = &st::menuIconReport;
		report.destructive = true;
		report.callback = [=] {
			OpenReport(controller, { u"post"_s, id, author });
		};
		menu->addItem(std::move(report));
	}
	Q_UNUSED(withQuick);
}

} // namespace

namespace Routes {

QString Thread(const QString &postId) {
	return u"eli/thread/"_s + postId;
}

QString Trends() {
	return u"eli/trends"_s;
}

QString Bookmarks() {
	return u"eli/bookmarks"_s;
}

QString Sound(const QString &soundId) {
	return u"eli/sound/"_s + soundId;
}

QString FollowRequests() {
	return u"eli/notifications/requests"_s;
}

QString Compose() {
	return u"eli/compose"_s;
}

} // namespace Routes

void OpenProfile(Window::SessionController *controller, const QString &userId) {
	if (userId.isEmpty()) {
		return;
	}
	const auto user = controller->session().data().users().peek(userId);
	if (user.valid() && user.deleted) {
		controller->showToast(tr::lng_eli_account_unavailable(tr::now));
		return;
	}
	controller->openRoute(Window::Routes::User(userId));
}

void ConfirmBlock(Window::SessionController *controller, const QString &userId) {
	const auto user = controller->session().data().users().peek(userId);
	const auto name = user.displayName.isEmpty()
		? tr::lng_eli_this_person(tr::now)
		: user.displayName;
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_eli_block_sure(tr::now, lt_name, name);
	args.text = tr::lng_eli_block_about(tr::now);
	args.confirmText = tr::lng_block_user(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		controller->session().data().graph().block(
			userId,
			base::Guard(controller),
			[=](const Data::GraphState &) {
				controller->showToast(tr::lng_blocked_toast(tr::now));
			},
			[=](const Api::Error &error) {
				Support::ShowError(controller, error);
			});
		close();
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ToggleLike(Window::SessionController *controller, const QString &postId) {
	if (const auto post = Find(controller, postId)) {
		controller->session().data().feed().react(
			postId,
			(post->my.reaction == u"like")
				? Data::Feed::Reaction::None
				: Data::Feed::Reaction::Like);
	}
}

void ToggleDislike(Window::SessionController *controller, const QString &postId) {
	if (const auto post = Find(controller, postId)) {
		controller->session().data().feed().react(
			postId,
			(post->my.reaction == u"dislike")
				? Data::Feed::Reaction::None
				: Data::Feed::Reaction::Dislike);
	}
}

void ToggleBookmark(Window::SessionController *controller, const QString &postId) {
	if (const auto post = Find(controller, postId)) {
		const auto on = !post->my.bookmarked;
		controller->session().data().feed().bookmark(postId, on);
		controller->showToast(on
			? tr::lng_eli_saved_toast(tr::now)
			: tr::lng_eli_unsaved_toast(tr::now));
	}
}

void ToggleRepost(Window::SessionController *controller, const QString &postId) {
	if (const auto post = Find(controller, postId)) {
		const auto on = !post->my.reposted;
		if (on && !post->canRepost) {
			return;
		}
		controller->session().data().feed().repost(postId, on);
		controller->showToast(on
			? tr::lng_eli_reposted(tr::now)
			: tr::lng_eli_repost_removed(tr::now));
	}
}

void CopyPostLink(Window::SessionController *controller, const QString &postId) {
	controller->session().data().feed().shareLink(
		postId,
		base::Guard(controller),
		[=](const QString &link) {
			CopyText(controller, link, tr::lng_link_copied(tr::now));
		},
		[=](const Api::Error &error) {
			Support::ShowError(controller, error, tr::lng_eli_link_failed(tr::now));
		});
}

void SendPostToChats(Window::SessionController *controller, const QString &postId) {
	auto args = Boxes::ChooseChatsArgs();
	args.title = tr::lng_eli_send_in_chat(tr::now);
	args.submitText = tr::lng_send(tr::now);
	args.multiSelect = true;
	controller->show(Boxes::ChooseChatsBox(
		&controller->session(),
		std::move(args),
		[=](const QStringList &chatIds) {
			if (chatIds.isEmpty()) {
				return;
			}
			auto &outbox = controller->session().data().outbox();
			for (const auto &chat : chatIds) {
				outbox.sendPost(chat, postId);
			}
			controller->showToast(tr::lng_chat_sent(tr::now));
		}));
}

void ConfirmDeletePost(Window::SessionController *controller, const QString &postId) {
	auto args = Ui::ConfirmBoxArgs();
	args.title = tr::lng_eli_delete_post_sure(tr::now);
	args.text = tr::lng_eli_delete_post_about(tr::now);
	args.confirmText = tr::lng_eli_delete_post(tr::now);
	args.destructive = true;
	args.confirmed = [=](std::function<void()> close) {
		controller->session().data().feed().deletePost(
			postId,
			base::Guard(controller),
			[=] { controller->showToast(tr::lng_eli_post_deleted(tr::now)); },
			[=](const Api::Error &error) { Support::ShowError(controller, error); });
		close();
	};
	controller->show(Ui::MakeConfirmBox(std::move(args)));
}

void ShowMoreMenu(
		Window::SessionController *controller,
		QWidget *parent,
		const QString &postId,
		QPoint globalPosition) {
	const auto post = Find(controller, postId);
	if (!post || post->deleted) {
		return;
	}
	const auto menu = new Ui::PopupMenu(parent);
	FillMore(menu, controller, *post, false);
	menu->popup(globalPosition);
}

void ShowContextMenu(
		Window::SessionController *controller,
		QWidget *parent,
		const QString &postId,
		QPoint globalPosition,
		const QString &linkToCopy) {
	const auto post = Find(controller, postId);
	if (!post || post->deleted) {
		return;
	}
	const auto id = post->id;
	const auto menu = new Ui::PopupMenu(parent);
	if (!linkToCopy.isEmpty()) {
		menu->addAction(
			tr::lng_copy_link(tr::now),
			[=] { CopyText(controller, linkToCopy, tr::lng_link_copied(tr::now)); },
			&st::menuIconLink);
		menu->addSeparator();
	}
	menu->addAction(
		tr::lng_eli_open_post(tr::now),
		[=] { OpenPost(controller, id); },
		&st::eliMenuIconOpen);
	auto reply = Ui::MenuItem();
	reply.text = tr::lng_reply(tr::now);
	reply.icon = &st::eliMenuIconReply;
	reply.disabled = !post->canReply;
	reply.shortcut = u"R"_s;
	reply.callback = [=] {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Reply;
		args.postId = id;
		OpenCompose(controller, args);
	};
	menu->addItem(std::move(reply));
	auto like = Ui::MenuItem();
	like.text = (post->my.reaction == u"like")
		? tr::lng_eli_unlike(tr::now)
		: tr::lng_eli_like(tr::now);
	like.icon = &st::eliMenuIconLike;
	like.shortcut = u"L"_s;
	like.callback = [=] { ToggleLike(controller, id); };
	menu->addItem(std::move(like));
	auto repost = Ui::MenuItem();
	repost.text = post->my.reposted
		? tr::lng_eli_repost_undo(tr::now)
		: tr::lng_eli_repost(tr::now);
	repost.icon = &st::eliMenuIconRepost;
	repost.disabled = !post->canRepost && !post->my.reposted;
	repost.callback = [=] { ToggleRepost(controller, id); };
	menu->addItem(std::move(repost));
	auto quote = Ui::MenuItem();
	quote.text = tr::lng_chat_quote(tr::now);
	quote.icon = &st::eliMenuIconQuote;
	quote.disabled = !post->canRepost;
	quote.callback = [=] {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Quote;
		args.postId = id;
		OpenCompose(controller, args);
	};
	menu->addItem(std::move(quote));
	menu->addSeparator();
	FillMore(menu, controller, *post, true);
	menu->popup(globalPosition);
}

void ShowRepostMenu(
		Window::SessionController *controller,
		QWidget *parent,
		const QString &postId,
		QPoint globalPosition) {
	const auto post = Find(controller, postId);
	if (!post || post->deleted) {
		return;
	}
	const auto menu = new Ui::PopupMenu(parent);
	menu->addAction(
		post->my.reposted
			? tr::lng_eli_repost_undo(tr::now)
			: tr::lng_eli_repost(tr::now),
		[=] { ToggleRepost(controller, postId); },
		&st::eliMenuIconRepost);
	auto quote = Ui::MenuItem();
	quote.text = tr::lng_chat_quote(tr::now);
	quote.icon = &st::eliMenuIconQuote;
	quote.disabled = !post->canRepost;
	quote.callback = [=] {
		auto args = ComposeArgs();
		args.kind = ComposeArgs::Kind::Quote;
		args.postId = postId;
		OpenCompose(controller, args);
	};
	menu->addItem(std::move(quote));
	menu->popup(globalPosition);
}

void ShowShareMenu(
		Window::SessionController *controller,
		QWidget *parent,
		const QString &postId,
		QPoint globalPosition) {
	const auto menu = new Ui::PopupMenu(parent);
	menu->addAction(
		tr::lng_copy_link(tr::now),
		[=] { CopyPostLink(controller, postId); },
		&st::menuIconLink);
	menu->addAction(
		tr::lng_eli_send_in_chat(tr::now),
		[=] { SendPostToChats(controller, postId); },
		&st::eliMenuIconSend);
	menu->popup(globalPosition);
}

void ShowReplyMenu(
		Window::SessionController *controller,
		QWidget *parent,
		const QString &rootId,
		const QString &replyId,
		QPoint globalPosition) {
	const auto reply = Find(controller, replyId);
	if (!reply || reply->deleted) {
		return;
	}
	const auto me = controller->session().userId();
	const auto mine = (reply->author == me);
	const auto root = Find(controller, rootId);
	const auto underMine = root && (root->author == me);
	const auto author = reply->author;
	const auto menu = new Ui::PopupMenu(parent);
	if (!reply->text.isEmpty()) {
		const auto text = reply->text;
		menu->addAction(
			tr::lng_copy_text(tr::now),
			[=] { CopyText(controller, text, tr::lng_eli_comment_copied(tr::now)); },
			&st::menuIconCopy);
	}
	menu->addAction(
		tr::lng_copy_link(tr::now),
		[=] { CopyPostLink(controller, replyId); },
		&st::menuIconLink);
	if (!mine) {
		auto report = Ui::MenuItem();
		report.text = tr::lng_report(tr::now);
		report.icon = &st::menuIconReport;
		report.destructive = true;
		report.callback = [=] {
			OpenReport(controller, { u"reply"_s, replyId, author });
		};
		menu->addItem(std::move(report));
	}
	if (mine || underMine) {
		auto remove = Ui::MenuItem();
		remove.text = tr::lng_eli_delete_comment(tr::now);
		remove.icon = &st::menuIconDelete;
		remove.destructive = true;
		remove.callback = [=] {
			auto args = Ui::ConfirmBoxArgs();
			args.title = tr::lng_eli_delete_comment_sure(tr::now);
			args.text = tr::lng_eli_delete_comment_about(tr::now);
			args.confirmText = tr::lng_eli_delete_comment(tr::now);
			args.destructive = true;
			args.confirmed = [=](std::function<void()> close) {
				controller->session().data().feed().deleteReply(rootId, replyId, mine);
				controller->showToast(tr::lng_eli_comment_deleted(tr::now));
				close();
			};
			controller->show(Ui::MakeConfirmBox(std::move(args)));
		};
		menu->addItem(std::move(remove));
	}
	menu->popup(globalPosition);
}

void OpenPost(Window::SessionController *controller, const QString &postId) {
	controller->openRoute(Window::Routes::Post(postId));
}

void OpenThread(Window::SessionController *controller, const QString &postId) {
	controller->showMainSection(
		ThreadFactory(postId),
		{ .tab = Window::Tab::Eli });
}

void OpenTrends(Window::SessionController *controller) {
	controller->showMainSection(TrendsFactory(), { .tab = Window::Tab::Eli });
}

void OpenBookmarks(Window::SessionController *controller) {
	controller->showMainSection(
		BookmarksFactory(),
		{ .tab = Window::Tab::Eli });
}

void OpenSound(Window::SessionController *controller, const QString &soundId) {
	controller->showMainSection(
		SoundFactory(soundId),
		{ .tab = Window::Tab::Eli });
}

void OpenFollowRequests(Window::SessionController *controller) {
	controller->showMainSection(
		FollowRequestsFactory(),
		{ .tab = Window::Tab::Eli });
}

} // namespace Eli
