// "debug/…" routes for the shared helpers: open them with --debug-route or the
// "route:" step of --debug-actions to see a helper without the screen that will
// use it (docs/DEVELOPING.md lists them).
//
// Only in builds with the debug tools (ELEZA_DEBUG_TOOLS).
#ifdef ELEZA_DEBUG_TOOLS

#include "support/session_services.h"

#include "boxes/about_box.h"
#include "boxes/peer_list_box.h"
#include "boxes/photo_crop_box.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/media_image_viewer.h"
#include "support/error_text.h"
#include "support/links.h"
#include "support/notifications.h"
#include "ui/painter.h"
#include "window/window_main_window.h"
#include "window/window_registry.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace Support {
namespace {

// A picture painted on the spot: no test files needed.
[[nodiscard]] QImage SampleImage(QSize size, int variant) {
	auto result = QImage(size, QImage::Format_RGB32);
	auto p = QPainter(&result);
	auto gradient = QLinearGradient(0, 0, size.width(), size.height());
	const auto colors = (variant % 3 == 0)
		? std::pair(QColor(0x06, 0x41, 0x2C), QColor(0x7F, 0xDD, 0xA5))
		: (variant % 3 == 1)
		? std::pair(QColor(0xE0, 0x9A, 0x2B), QColor(0xD6, 0x4F, 0x45))
		: std::pair(QColor(0x5C, 0x8F, 0xD6), QColor(0x8E, 0x7B, 0xD0));
	gradient.setColorAt(0., colors.first);
	gradient.setColorAt(1., colors.second);
	p.fillRect(result.rect(), gradient);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 60));
	const auto step = std::max(size.width() / 6, 1);
	for (auto i = 0; i != 5; ++i) {
		p.drawEllipse(
			QPointF(step * (i + 1), size.height() * (0.3 + 0.1 * ((i + variant) % 4))),
			step * 0.35,
			step * 0.35);
	}
	p.setBrush(QColor(255, 255, 255, 220));
	p.drawEllipse(
		QPointF(size.width() / 2., size.height() / 2.),
		size.height() / 6.,
		size.height() / 6.);
	p.end();
	return result;
}

[[nodiscard]] NotificationData SampleNotification(int index) {
	const auto persian = Lang::IsPersian();
	auto result = NotificationData();
	switch (index % 3) {
	case 0:
		result.key = QStringLiteral("debug-c1");
		result.userId = QStringLiteral("u_sara");
		result.title = persian
			? QStringLiteral("سارا احمدی")
			: QStringLiteral("Sara Ahmadi");
		result.text = persian
			? QStringLiteral("فردا ساعت ۵ می‌بینمت؟ کافهٔ همیشگی، کنار پارک ☕️")
			: QStringLiteral("See you tomorrow at 5? The usual café by the park ☕️");
		break;
	case 1:
		result.key = QStringLiteral("debug-c2");
		result.userId = QStringLiteral("u_ali");
		result.title = persian
			? QStringLiteral("علی رضایی")
			: QStringLiteral("Ali Rezaei");
		result.text = tr::lng_media_photo(tr::now);
		break;
	default:
		result.key = QStringLiteral("debug-c4");
		result.anonymous = true;
		result.title = tr::lng_anonymous(tr::now);
		result.text = tr::lng_support_notify_new_message(tr::now);
		break;
	}
	// Not a real chat id: reading a chat removes its toast.
	result.route = Window::Routes::Chat(result.key.mid(6));
	return result;
}

[[nodiscard]] bool Handle(
		Window::SessionController *controller,
		const QString &route) {
	const auto prefix = QStringLiteral("debug/");
	if (!route.startsWith(prefix)) {
		return false;
	}
	const auto mark = route.indexOf(QLatin1Char('?'));
	const auto name = route.mid(
		prefix.size(),
		(mark < 0) ? -1 : (mark - prefix.size()));
	const auto query = QUrlQuery((mark < 0) ? QString() : route.mid(mark + 1));
	const auto window = controller->window();
	if (name == QStringLiteral("notification")
		|| name == QStringLiteral("notifications")) {
		const auto manager = SessionServices::For(controller)->notifications();
		const auto count = (name == QStringLiteral("notifications")) ? 3 : 1;
		for (auto i = 0; i != count; ++i) {
			manager->showCustom(SampleNotification(i));
		}
		return true;
	} else if (name == QStringLiteral("viewer")) {
		auto items = std::vector<Media::ViewerItem>();
		const auto file = query.queryItemValue(
			QStringLiteral("file"),
			QUrl::FullyDecoded);
		const auto video = query.queryItemValue(
			QStringLiteral("video"),
			QUrl::FullyDecoded);
		if (!video.isEmpty()) {
			auto item = Media::ViewerItem();
			item.videoPath = video;
			item.caption = tr::lng_media_video(tr::now);
			items.push_back(std::move(item));
		}
		if (!file.isEmpty()) {
			auto item = Media::ViewerItem();
			item.filePath = file;
			items.push_back(std::move(item));
		}
		for (auto i = 0; i != 3; ++i) {
			auto item = Media::ViewerItem();
			item.image = SampleImage(
				(i == 1) ? QSize(900, 1400) : QSize(1600, 1000),
				i);
			item.caption = (i == 0)
				? tr::lng_intro_tagline(tr::now)
				: QString();
			item.saveName = QStringLiteral("eleza-sample-%1.png").arg(i + 1);
			items.push_back(std::move(item));
		}
		Media::ShowImageViewer(window, std::move(items), 0);
		return true;
	} else if (name == QStringLiteral("crop")) {
		const auto weak = QPointer<Window::MainWindow>(window);
		controller->show(Ui::Box<Boxes::PhotoCropBox>(
			SampleImage(QSize(1600, 1000), 0),
			[weak](const QImage &square) {
				if (weak) {
					weak->showToast(QStringLiteral("%1 × %2")
						.arg(square.width())
						.arg(square.height()));
				}
			}));
		return true;
	} else if (name == QStringLiteral("about")) {
		controller->show(Ui::Box<Boxes::AboutBox>());
		return true;
	} else if (name == QStringLiteral("peers")
		|| name == QStringLiteral("peers-multi")) {
		auto args = Boxes::ChooseChatsArgs();
		args.multiSelect = (name == QStringLiteral("peers-multi"));
		args.includeConversations = true;
		const auto weak = QPointer<Window::MainWindow>(window);
		controller->show(Boxes::ChooseChatsBox(
			&controller->session(),
			args,
			[weak](const QStringList &ids) {
				if (weak) {
					weak->showToast(ids.join(QStringLiteral(", ")));
				}
			}));
		return true;
	} else if (name == QStringLiteral("logout")) {
		controller->requestLogout();
		return true;
	} else if (name == QStringLiteral("upgrade")) {
		window->showUpgradeRequired(QString());
		return true;
	} else if (name == QStringLiteral("link")) {
		OpenLink(controller, QStringLiteral("https://example.com/some/page?x=1"));
		return true;
	} else if (name == QStringLiteral("error")) {
		ShowError(controller, Api::Error::Offline());
		return true;
	} else if (name == QStringLiteral("session-expired")) {
		// Deferred: ending the session destroys the caller.
		const auto account = &controller->account();
		QMetaObject::invokeMethod(account, [account] {
			account->endSession(Main::SessionEndReason::Expired);
		}, Qt::QueuedConnection);
		return true;
	}
	return false;
}

} // namespace

void RegisterDebugRoutes() {
	Window::RegisterRouteHandler(&Handle);
}

} // namespace Support

#endif // ELEZA_DEBUG_TOOLS
