#include "support/links.h"

#include "lang_auto.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"

#include <QtGui/QDesktopServices>

namespace Support {

bool OpenLinkExternally(const QString &link) {
	const auto parsed = ParseLink(link);
	switch (parsed.type) {
	case ParsedLink::Type::OwnWeb:
	case ParsedLink::Type::External:
	case ParsedLink::Type::System:
		return QDesktopServices::openUrl(parsed.url);
	case ParsedLink::Type::Route:
	case ParsedLink::Type::Invalid:
		break;
	}
	return false;
}

bool OpenLink(Window::SessionController *controller, const QString &link) {
	const auto parsed = ParseLink(link);
	switch (parsed.type) {
	case ParsedLink::Type::Route:
		return controller && controller->openRoute(parsed.route.toString());
	case ParsedLink::Type::OwnWeb:
	case ParsedLink::Type::System:
		return QDesktopServices::openUrl(parsed.url);
	case ParsedLink::Type::External: {
		if (!controller) {
			return false;
		}
		const auto url = parsed.url;
		auto args = Ui::ConfirmBoxArgs();
		args.title = tr::lng_support_open_link_title(tr::now);
		// The address is isolated so its direction does not mix with the text.
		args.text = tr::lng_support_open_link_text(tr::now)
			+ QStringLiteral("\n\n")
			+ QChar(0x2066) // left-to-right isolate
			+ DisplayUrl(url)
			+ QChar(0x2069);
		args.confirmText = tr::lng_support_open_link_open(tr::now);
		args.confirmed = [url](std::function<void()> close) {
			close();
			QDesktopServices::openUrl(url);
		};
		controller->show(Ui::MakeConfirmBox(std::move(args)));
		return true;
	}
	case ParsedLink::Type::Invalid:
		break;
	}
	return false;
}

bool ActivateTextLink(
		Window::SessionController *controller,
		const Ui::Text::Link &link,
		Qt::MouseButton button) {
	if (button != Qt::LeftButton && button != Qt::MiddleButton) {
		return false;
	}
	using Type = Ui::Text::EntityType;
	switch (link.type) {
	case Type::Url:
	case Type::CustomUrl:
	case Type::Email:
	case Type::Phone: {
		const auto url = Ui::Text::LinkUrl(link);
		// Handled either way: a refused address must not reach the default
		// handler of the label.
		OpenLink(controller, url.isEmpty() ? link.text : url);
		return true;
	}
	case Type::Mention: {
		const auto handle = link.data.isEmpty() ? link.text : link.data;
		return controller
			&& controller->openRoute(Window::Routes::Handle(handle));
	}
	case Type::MentionName:
		return controller
			&& !link.data.isEmpty()
			&& controller->openRoute(Window::Routes::User(link.data));
	case Type::Hashtag: {
		const auto tag = link.data.isEmpty() ? link.text : link.data;
		return controller
			&& controller->openRoute(Window::Routes::Topic(tag));
	}
	default:
		break;
	}
	return false;
}

std::function<bool(const Ui::Text::Link&, Qt::MouseButton)> LinkHandler(
		Window::SessionController *controller) {
	const auto weak = QPointer<Window::SessionController>(controller);
	return [weak](const Ui::Text::Link &link, Qt::MouseButton button) {
		return ActivateTextLink(weak.data(), link, button);
	};
}

} // namespace Support
