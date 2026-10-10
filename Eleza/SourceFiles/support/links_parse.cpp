// The part of Support::* links that decides what an address means and how it
// is shown. No widgets here: the unit tests and the fuzzers compile this file
// alone (with window/window_routes.cpp).
#include "support/links.h"

#include <QtCore/QUrlQuery>

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

// What is handed to the mail / phone handler of the system: the address and,
// for mail, a subject and a body. Mail programs accept more in a "mailto:"
// (some attach local files named by "attach="), a "tel:" may carry dialling
// codes after ";" or ",": none of that comes from somebody's message.
[[nodiscard]] QUrl SystemUrl(const QUrl &url) {
	const auto scheme = url.scheme().toLower();
	const auto address = StripControl(url.path(QUrl::FullyDecoded)).trimmed();
	if (address.isEmpty() || address.size() > 320) {
		return QUrl();
	}
	auto result = QUrl();
	result.setScheme(scheme);
	if (scheme == u"tel") {
		auto number = QString();
		for (const auto ch : address) {
			const auto code = ch.unicode();
			if ((code >= '0' && code <= '9')
				|| (code == '+' && number.isEmpty())) {
				number.append(ch);
			} else if (code != ' ' && code != '-' && code != '(' && code != ')' && code != '.') {
				return QUrl();
			}
		}
		if (number.isEmpty() || number == u"+") {
			return QUrl();
		}
		result.setPath(number);
		return result;
	}
	// One plain address: letters, digits and the few marks a mailbox and a
	// host name are made of - nothing that could read as a second recipient
	// or as a parameter to whatever receives it.
	auto at = 0;
	for (const auto ch : address) {
		const auto code = ch.unicode();
		if (code == '@') {
			++at;
		} else if (!ch.isLetterOrNumber()
			&& code != '.'
			&& code != '_'
			&& code != '-'
			&& code != '+'
			&& code != '%') {
			return QUrl();
		}
	}
	if (at != 1 || address.startsWith(u'@') || address.endsWith(u'@')) {
		return QUrl();
	}
	result.setPath(address);
	auto kept = QUrlQuery();
	const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
	for (const auto &[name, value] : items) {
		const auto key = name.toLower();
		if ((key == u"subject" || key == u"body") && !kept.hasQueryItem(key)) {
			kept.addQueryItem(
				key,
				QString::fromLatin1(QUrl::toPercentEncoding(
					StripControl(value).left(2000))));
		}
	}
	if (!kept.isEmpty()) {
		result.setQuery(kept);
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
		result.url = SystemUrl(url);
		return result.url.isValid() ? result : ParsedLink();
	} else if (!WebScheme(scheme) || url.host().isEmpty()) {
		return result;
	}
	// "https://eleza.me@evil.example/": the part before the "@" is only
	// there to be mistaken for the host. It is neither shown nor sent.
	url.setUserInfo(QString());
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
	shown.setUserInfo(QString());
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

} // namespace Support
