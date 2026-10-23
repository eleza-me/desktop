#include "eli/eli_posts_embed.h"

#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "eli/eli_post_list.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_eli.h"

#include <QtGui/QKeyEvent>

namespace Eli {

PostsEmbed::PostsEmbed(
	QWidget *parent,
	Window::SessionController *controller,
	const QVariantMap &params)
: EmbeddedList(parent) {
	const auto source = params.value(u"source"_s).toString();
	const auto user = params.value(u"user"_s).toString();
	const auto query = params.value(u"query"_s).toString();
	auto descriptor = ListDescriptor();
	descriptor.pinnedId = params.value(u"pinned"_s).toString();
	auto emptyTitle = tr::lng_profile_no_posts_yet(tr::now);
	auto emptyIcon = &st::emptyIconEli;
	const auto postsDone = [](Fn<void(const ListPage &)> done) {
		return [=](const Data::PostsPage &page) {
			done(ListPage::From(page));
		};
	};
	if (source == u"bookmarks") {
		emptyTitle = tr::lng_eli_havent_saved_anything(tr::now);
		emptyIcon = &st::emptyIconBookmarks;
		descriptor.loader = [=](
				const QString &cursor,
				base::Guard guard,
				Fn<void(const ListPage &)> done,
				Data::FailCallback fail) {
			controller->session().data().feed().bookmarks(
				cursor,
				guard,
				postsDone(done),
				fail);
		};
	} else if (source == u"search") {
		emptyTitle = tr::lng_nothing_found(tr::now);
		emptyIcon = &st::emptyIconSearch;
		descriptor.loader = [=](
				const QString &cursor,
				base::Guard guard,
				Fn<void(const ListPage &)> done,
				Data::FailCallback fail) {
			controller->session().data().graph().search(
				query,
				u"posts"_s,
				cursor.toInt(),
				guard,
				[=](const Data::SearchResults &results) {
					auto page = ListPage();
					for (const auto &id : results.posts) {
						auto item = Data::FeedItem();
						item.postId = id;
						page.items.push_back(item);
					}
					if (results.nextOffset > 0) {
						page.next = QString::number(results.nextOffset);
					}
					done(page);
				},
				fail);
		};
	} else {
		const auto tab = (source == u"user_replies")
			? u"replies"_s
			: (source == u"user_media")
			? u"media"_s
			: QString();
		if (source == u"user_replies") {
			emptyTitle = tr::lng_profile_no_replies_yet(tr::now);
		} else if (source == u"user_media") {
			emptyTitle = tr::lng_profile_no_media_yet(tr::now);
		}
		if (source != u"user") {
			descriptor.loader = [=](
					const QString &cursor,
					base::Guard guard,
					Fn<void(const ListPage &)> done,
					Data::FailCallback fail) {
				controller->session().data().feed().userPosts(
					user,
					tab,
					cursor,
					QString(),
					guard,
					postsDone(done),
					fail);
			};
		} else {
			// Their reposts among their posts. A server that does not know
			// the parameter pages by `next_before`, and so do we then.
			const auto byBefore = std::make_shared<bool>(false);
			descriptor.loader = [=](
					const QString &cursor,
					base::Guard guard,
					Fn<void(const ListPage &)> done,
					Data::FailCallback fail) {
				if (cursor.isEmpty()) {
					*byBefore = false;
				}
				controller->session().data().feed().userTimeline(
					user,
					cursor,
					*byBefore,
					guard,
					[=](const Data::PostsPage &page) {
						*byBefore = page.nextCursor.isEmpty()
							&& !page.nextBefore.isEmpty();
						done(ListPage::From(page));
					},
					fail);
			};
		}
	}
	descriptor.empty = [=] {
		auto result = Ui::EmptyPlaceholderDescriptor();
		result.icon = emptyIcon;
		result.title = emptyTitle;
		return result;
	};
	_list = new PostList(this, controller, std::move(descriptor));
	_list->setMinHeight(240);
	_list->show();
	setFocusPolicy(Qt::StrongFocus);
	connect(_list, &Ui::RpWidget::heightChanged, this, [this] {
		if (_inResize) {
			return;
		}
		resize(width(), _list->height());
		Q_EMIT heightChanged();
	});
	connect(_list, &PostList::emptyChanged, this, [this](bool empty) {
		Q_EMIT emptyChanged(empty);
	});
	if (source == u"user" && user == controller->session().userId()) {
		// My own profile: a repost I take back leaves at once; «واگرد» on
		// the toast (ToggleRepost) reposts it and the row comes back.
		const auto feed = &controller->session().data().feed();
		connect(feed, &Data::Feed::postChanged, this, [=, this](const QString &id) {
			const auto card = _list->card(id);
			const auto post = feed->post(id);
			if (!post) {
				return;
			} else if (card
				&& !post->my.reposted
				&& card->options().reason == u"repost"
				&& card->options().actor == user) {
				_takenBack.remember(id, int(_list->ids().indexOf(id)), Ui::Now());
				_list->remove(id);
			} else if (!card && post->my.reposted) {
				const auto index = _takenBack.take(id, Ui::Now());
				if (index >= 0) {
					auto item = Data::FeedItem();
					item.postId = id;
					item.reason = u"repost"_s;
					item.actor = user;
					_list->insert(index, item);
				}
			}
		});
	}
}

void PostsEmbed::setVisibleRange(int top, int bottom) {
	_list->setVisibleTopBottom(top, bottom);
}

void PostsEmbed::loadMore() {
	_list->loadMore();
}

void PostsEmbed::reload() {
	_list->reload();
}

int PostsEmbed::resizeGetHeight(int newWidth) {
	_inResize = true;
	_list->resizeToWidth(newWidth);
	_list->move(0, 0);
	_inResize = false;
	return _list->height();
}

void PostsEmbed::keyPressEvent(QKeyEvent *e) {
	if (!_list->handleKey(e)) {
		e->ignore();
	}
}

void PostsEmbed::focusInEvent(QFocusEvent *e) {
	EmbeddedList::focusInEvent(e);
	_list->setFocus();
}

} // namespace Eli
