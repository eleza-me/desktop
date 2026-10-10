// Opening links: inside the app when they point into Eleza, in the browser
// otherwise (after a confirmation for foreign hosts).
//
//   Support::OpenLink(controller, u"https://eleza.me/u/sara"_s);   // -> route "u/sara"
//   Support::OpenLink(controller, u"@sara"_s);                     // -> route "u/sara"
//   Support::OpenLink(controller, u"#تهران"_s);                    // -> route "eli/topic/تهران"
//   Support::OpenLink(controller, u"https://example.com/x"_s);     // -> «این پیوند باز شود؟» box
//
//   label->setLinkHandler(Support::LinkHandler(controller));       // Ui::FlatLabel
//   Support::ActivateTextLink(controller, text.link(index));       // custom painted Text::String
//
// The canonical route strings are in window/window_routes.h. Only http(s), mailto
// and tel addresses ever leave the app; anything else (file:, javascript:, …) is
// refused.
#pragma once

#include "ui/text/text_entity.h"
#include "window/window_routes.h"

#include <QtCore/QUrl>

#include <functional>
#include <optional>

namespace Window {
class SessionController;
} // namespace Window

namespace Support {

// Our own hosts, opened without a question: eleza.me, www.eleza.me and
// web.eleza.me, nothing else (no other subdomain).
[[nodiscard]] bool IsElezaHost(const QString &host);

// What a piece of link text means.
struct ParsedLink {
	enum class Type {
		Invalid, // not something we open
		Route, // inside the app: `route`
		OwnWeb, // a page of ours without a screen here (terms, share links): the browser, no questions
		External, // a foreign web page: the browser after a confirmation
		System, // mailto: / tel: for the system handler
	};
	Type type = Type::Invalid;
	Window::Route route;
	QUrl url;
};
[[nodiscard]] ParsedLink ParseLink(const QString &link);

// The address as shown in the confirmation (an internationalized host is shown
// in its ASCII form so look-alike letters cannot disguise it).
[[nodiscard]] QString DisplayUrl(const QUrl &url);

// Returns false when the link was refused.
bool OpenLink(Window::SessionController *controller, const QString &link);
// Without a session (the intro): our own pages and system links only open
// directly; a foreign page opens too — the caller only passes fixed addresses.
bool OpenLinkExternally(const QString &link);

// A click on a link inside rich text. Returns false for types the caller has to
// handle itself (bot commands).
bool ActivateTextLink(
	Window::SessionController *controller,
	const Ui::Text::Link &link,
	Qt::MouseButton button = Qt::LeftButton);
[[nodiscard]] std::function<bool(const Ui::Text::Link&, Qt::MouseButton)> LinkHandler(
	Window::SessionController *controller);

} // namespace Support
