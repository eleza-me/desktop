// Routes and links from message text: Window::ParseRoute / RouteFromWebPath and
// Support::ParseLink / DisplayUrl. Whatever the text, a link is only ever a
// route of ours, a web page, or a mail / phone address; our own host is never
// claimed by a foreign one; what is shown has no control characters.
#include "fuzz_driver.h"

#include "support/links.h"
#include "window/window_routes.h"

#include <QtCore/QUrlQuery>

namespace {

[[nodiscard]] QString MakeText(Fuzz::Input &input) {
	static const char *const kPieces[] = {
		"https://", "http://", "eleza://", "mailto:", "tel:", "file:///", "javascript:",
		"eleza.me", ".eleza.me", "eleza.me.", "@", ":", "/", "//", "\\", "?", "#", "&", "=",
		"%2e", "%2f", "%40", "%00", "%0a", "..", "evil.example", "xn--", "u/", "chat/", "debug/",
		"eli/post/", "settings/", "conversation/", "s/", "p/", "seq=", "q=", "attach=", "subject=",
		"\xe2\x80\xae", "\xe3\x80\x82", "\xef\xbc\x8e", "\xd0\xb5", " ", "\t", "\n", "[::1]", "127.0.0.1",
		"www.", "app.", "+98", "0912", ",", ";", "-",
	};
	auto result = QString();
	const auto count = input.below(40);
	for (auto i = uint32_t(0); i != count && !input.empty(); ++i) {
		const auto pick = input.byte();
		if (pick < 220) {
			result += QString::fromUtf8(kPieces[pick % (sizeof(kPieces) / sizeof(kPieces[0]))]);
		} else {
			result += QChar(char16_t((input.byte() << 8) | input.byte()));
		}
	}
	return result;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	auto input = Fuzz::Input(data, size);
	const auto raw = (input.byte() & 1) != 0;
	const auto text = raw ? QString::fromUtf8(input.rest()) : MakeText(input);

	const auto route = Window::ParseRoute(text);
	if (route.valid()) {
		// The same place again (ids with lone surrogates or noncharacters do
		// not keep their exact text through percent-encoding, the type does).
		const auto again = Window::ParseRoute(route.toString());
		if (again.type != route.type) {
			qFatal("fuzz_links: a route does not survive its own string: %s", qPrintable(route.toString()));
		}
	}
	(void)Window::RouteFromWebPath(text, text);

	const auto parsed = Support::ParseLink(text);
	using Type = Support::ParsedLink::Type;
	switch (parsed.type) {
	case Type::Invalid:
		break;
	case Type::Route:
		if (!parsed.route.valid() || parsed.route.toString().startsWith(u"debug")) {
			qFatal("fuzz_links: a bad route from %s", qPrintable(text));
		}
		break;
	case Type::OwnWeb:
		if (!Support::IsElezaHost(parsed.url.host())
			|| (parsed.url.scheme() != u"https" && parsed.url.scheme() != u"http")) {
			qFatal("fuzz_links: a foreign page taken for ours: %s", qPrintable(text));
		}
		break;
	case Type::External: {
		const auto scheme = parsed.url.scheme();
		if ((scheme != u"https" && scheme != u"http")
			|| parsed.url.host().isEmpty()
			|| !parsed.url.userInfo().isEmpty()
			|| Support::IsElezaHost(parsed.url.host())) {
			qFatal("fuzz_links: a bad external link from %s", qPrintable(text));
		}
		const auto shown = Support::DisplayUrl(parsed.url);
		if (shown.size() > 301) {
			qFatal("fuzz_links: the shown address is not cut");
		}
		for (const auto ch : shown) {
			if (ch.category() == QChar::Other_Format || ch.category() == QChar::Other_Control) {
				qFatal("fuzz_links: a control character in the shown address");
			}
		}
	} break;
	case Type::System: {
		const auto scheme = parsed.url.scheme();
		const auto full = parsed.url.toString(QUrl::FullyEncoded);
		auto extra = false;
		const auto items = QUrlQuery(parsed.url).queryItems(QUrl::FullyDecoded);
		for (const auto &[name, value] : items) {
			extra = extra || (name != u"subject" && name != u"body");
		}
		const auto address = parsed.url.path(QUrl::FullyDecoded);
		if ((scheme != u"mailto" && scheme != u"tel")
			|| extra
			|| address.contains(u'&') || address.contains(u'=') || address.contains(u',') || address.contains(u'/')
			|| (scheme == u"tel" && (!items.isEmpty() || full.contains(u';') || full.contains(u'*') || full.contains(u'#')))) {
			qFatal("fuzz_links: a bad system link %s from %s", qPrintable(full), qPrintable(text));
		}
	} break;
	}
	return 0;
}

FUZZ_SEEDS() {
	return {
		QByteArray("\x01https://eleza.me/u/sara"),
		QByteArray("\x01https://eleza.me@evil.example/u/sara"),
		QByteArray("\x01" "eleza://chat/c1?seq=4"),
		QByteArray("\x01mailto:a@b.co?subject=x&attach=/etc/passwd"),
		QByteArray("\x01tel:+98 912 123 4567"),
		QByteArray("\x01" "eli/post/p%2F1?x=1&x=2"),
		QByteArray("\x01@sara"),
		QByteArray("\x01#tag"),
		QByteArray("\x00\x00\x00\x00\x08\x00\x07\x08\x1a\x09\x19\x0b\x1b", 13),
	};
}
