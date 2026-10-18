#include "stories/stories_module.h"

#include "window/window_registry.h"

#ifdef ELEZA_DEBUG_TOOLS
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_users.h"
#include "main/main_session.h"
#include "stories/stories_boxes.h"
#include "stories/stories_settings.h"
#include "stories/stories_viewer.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QUrlQuery>
#endif // ELEZA_DEBUG_TOOLS

namespace Stories {
namespace {

#ifdef ELEZA_DEBUG_TOOLS
// "debug/…" routes (docs/DEVELOPING.md, section 2):
//   debug/stories-sample               a tray and stories made up here (no server)
//   debug/stories/<user>               the viewer at <user>'s stories (after the sample: u1)
//   debug/story/<owner>/<story>        the viewer at one story (a chat bubble's click)
//   debug/story-compose?file=<path>    «استوری تازه» with that file
//   debug/story-share/<post>           «اشتراک در استوری»
//   debug/story-viewers/<story>        «بیننده‌ها»
//   debug/story-settings               Settings → Privacy → «استوری»
void ApplySample(Window::SessionController *controller) {
	auto &stories = controller->session().data().stories();
	const auto now = QDateTime::currentSecsSinceEpoch();
	const auto user = [](const QString &id, const QString &name) {
		auto result = Data::User();
		result.id = id;
		result.handle = id;
		result.displayName = name;
		return result;
	};
	auto page = Data::StoryTrayPage();
	page.me.user = controller->session().data().users().me();
	page.me.count = 1;
	page.me.latestAt = now - 600;
	const auto names = QStringList{
		u"سارا"_s, u"علی"_s, u"مریم"_s, u"رضا"_s, u"نگار"_s, u"کاوه"_s,
	};
	for (auto i = 0; i != int(names.size()); ++i) {
		auto item = Data::StoryTrayItem();
		item.user = user(u"u"_s + QString::number(i + 1), names[i]);
		item.count = 2 + (i % 2);
		item.unseen = (i < 3) ? item.count : 0;
		item.latestAt = now - 300 * (i + 1);
		item.closeFriends = (i == 1);
		item.muted = (i == 5);
		page.items.push_back(item);
	}
	stories.applyTray(page, true);
	for (const auto &item : page.items) {
		auto list = Data::UserStories();
		list.user = item.user;
		for (auto j = 0; j != item.count; ++j) {
			auto story = Data::Story();
			story.id = u"st"_s + item.user.id + QString::number(j);
			story.owner = item.user.id;
			story.kind = (j == 1) ? Data::StoryKind::Post : Data::StoryKind::Image;
			if (story.kind == Data::StoryKind::Post) {
				auto post = Data::StoryPost();
				post.id = u"p1"_s;
				post.author = item.user;
				post.available = (item.user.id != u"u2");
				post.textExcerpt = u"«الی» برای حرف‌هایی است که ارزش شنیدن دارند."_s;
				story.post = post;
			} else {
				auto media = Data::StoryMedia();
				media.width = 1080;
				media.height = 1920;
				media.durationMs = 5000;
				media.blurhash = u"LEHV6nWB2yk8pyo0adR*.7kCMdnj"_s;
				story.media = media;
			}
			story.caption = (j == 0) ? u"یک روز خوب در تهران ☀️"_s : QString();
			story.createdAt = now - 3600 * (j + 1);
			story.expiresAt = now + 3600 * 20;
			story.seen = (item.unseen == 0);
			story.closeFriends = item.closeFriends;
			story.canReply = (item.user.id != u"u3");
			list.stories.push_back(story);
		}
		stories.applyUserStories(list);
	}
}

bool HandleDebugRoute(
		Window::SessionController *controller,
		const QString &route) {
	const auto name = route.startsWith(QLatin1Char('/')) ? route.mid(1) : route;
	const auto path = name.section(QLatin1Char('?'), 0, 0);
	const auto query = QUrlQuery(name.section(QLatin1Char('?'), 1));
	if (path == u"debug/stories-sample") {
		ApplySample(controller);
		return true;
	} else if (path.startsWith(u"debug/stories/")) {
		const auto user = path.section(QLatin1Char('/'), 2, 2);
		OpenUserStories(controller, user, controller->session().data().stories().trayUserIds());
		return true;
	} else if (path.startsWith(u"debug/story/")) {
		OpenStory(
			controller,
			path.section(QLatin1Char('/'), 2, 2),
			path.section(QLatin1Char('/'), 3, 3));
		return true;
	} else if (path == u"debug/story-compose") {
		ShowComposeBoxForFile(controller, query.queryItemValue(u"file"_s));
		return true;
	} else if (path.startsWith(u"debug/story-share/")) {
		ShareToStory(controller, path.section(QLatin1Char('/'), 2, 2));
		return true;
	} else if (path.startsWith(u"debug/story-viewers/")) {
		ShowViewersBox(controller, path.section(QLatin1Char('/'), 2, 2));
		return true;
	} else if (path == u"debug/story-settings") {
		controller->showMainSection(SettingsFactory(), { .tab = Window::Tab::Settings });
		return true;
	}
	return false;
}
#endif // ELEZA_DEBUG_TOOLS

} // namespace

void Register() {
#ifdef ELEZA_DEBUG_TOOLS
	Window::RegisterRouteHandler(HandleDebugRoute);
#endif // ELEZA_DEBUG_TOOLS
}

} // namespace Stories
