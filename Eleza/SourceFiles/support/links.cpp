#include "support/links.h"

#include "lang_auto.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"

#include <QtGui/QDesktopServices>

namespace Support {
namespace {

constexpr auto kMaxShownUrl = 300;

[[nodiscard]] bool WebScheme(const QString &scheme) {
	return (scheme == u"http") || (scheme == u"https");
}

[[nodiscard]] QString StripControl(const QString &text) {
	// Bidi overrides and other format characters could reorder what is shown.
	auto result = QString();
	result.reserve(text.size());
	for (const auto ch : text) {
		const auto category = ch.category();
		if (category != QChar::Other_Format && category != QChar::Other_Control) {
			result.append(ch);
		}
	}
	return result;
}

} // namespace

bool IsElezaHost(const QString &host) {
	const auto lower = host.toLower();
	return (lower == u"eleza.me") || lower.endsWith(u".eleza.me");
}

ParsedLink ParseLink(const QString &link) {
	auto result = ParsedLink();
	const auto text = link.trimmed();
	if (text.isEmpty()) {
		return result;
	}
	if (text.startsWith(QLatin1Char('@')) && text.size() > 1) {
		result.route = Window::ParseRoute(Window::Routes::Handle(text));
		result.type = result.route.valid()
			? ParsedLink::Type::Route
			: ParsedLink::Type::Invalid;
		return result;
	} else if (text.startsWith(QLatin1Char('#')) && text.size() > 1) {
		result.route = Window::ParseRoute(Window::Routes::Topic(text));
		result.type = result.route.valid()
			? ParsedLink::Type::Route
			: ParsedLink::Type::Invalid;
		return result;
	}
	auto url = QUrl(text, QUrl::TolerantMode);
	if (url.scheme().isEmpty() && !text.startsWith(QLatin1Char('/'))) {
		// "sara.photo/gallery" as people type it.
		url = QUrl(QStringLiteral("https://") + text, QUrl::TolerantMode);
	}
	if (!url.isValid()) {
		return result;
	}
	const auto scheme = url.scheme().toLower();
	if (scheme == u"eleza") {
		// eleza://chat/c1?seq=4
		auto route = url.host() + url.path(QUrl::FullyEncoded);
		if (url.hasQuery()) {
			route += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
		}
		result.route = Window::ParseRoute(route);
		result.type = result.route.valid()
			? ParsedLink::Type::Route
			: ParsedLink::Type::Invalid;
		return result;
	} else if (scheme == u"mailto" || scheme == u"tel") {
		result.type = ParsedLink::Type::System;
		result.url = url;
		return result;
	} else if (!WebScheme(scheme) || url.host().isEmpty()) {
		return result;
	}
	result.url = url;
	if (!IsElezaHost(url.host())) {
		result.type = ParsedLink::Type::External;
		return result;
	}
	const auto host = url.host().toLower();
	const auto appHost = (host == u"eleza.me")
		|| (host == u"www.eleza.me")
		|| (host == u"app.eleza.me");
	if (appHost) {
		if (const auto route = Window::RouteFromWebPath(
				url.path(QUrl::FullyEncoded),
				url.query(QUrl::FullyEncoded))) {
			// The bare site address is the home page, not the feed.
			const auto home = url.path().isEmpty() || url.path() == u"/";
			if (!home) {
				result.type = ParsedLink::Type::Route;
				result.route = *route;
				return result;
			}
		}
	}
	result.type = ParsedLink::Type::OwnWeb;
	return result;
}

QString DisplayUrl(const QUrl &url) {
	auto shown = url;
	const auto host = url.host(QUrl::FullyDecoded);
	const auto ace = QString::fromLatin1(QUrl::toAce(host));
	if (!ace.isEmpty() && ace != host) {
		shown.setHost(ace, QUrl::StrictMode);
	}
	auto text = StripControl(shown.toDisplayString(QUrl::PrettyDecoded));
	if (text.size() > kMaxShownUrl) {
		text = text.left(kMaxShownUrl) + QChar(0x2026);
	}
	return text;
}

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
