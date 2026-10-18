#include "ui/text/text_entity.h"

#include <QtCore/QHash>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <map>
#include <vector>

namespace Ui::Text {

EntityType EntityTypeFromName(const QString &name) {
	static const auto map = QHash<QString, EntityType>{
		{ QStringLiteral("bold"), EntityType::Bold },
		{ QStringLiteral("italic"), EntityType::Italic },
		{ QStringLiteral("underline"), EntityType::Underline },
		{ QStringLiteral("strikethrough"), EntityType::StrikeOut },
		{ QStringLiteral("strike"), EntityType::StrikeOut },
		{ QStringLiteral("code"), EntityType::Code },
		{ QStringLiteral("pre"), EntityType::Pre },
		{ QStringLiteral("spoiler"), EntityType::Spoiler },
		{ QStringLiteral("url"), EntityType::Url },
		{ QStringLiteral("link"), EntityType::Url },
		{ QStringLiteral("text_link"), EntityType::CustomUrl },
		{ QStringLiteral("text_url"), EntityType::CustomUrl },
		{ QStringLiteral("email"), EntityType::Email },
		{ QStringLiteral("phone"), EntityType::Phone },
		{ QStringLiteral("phone_number"), EntityType::Phone },
		{ QStringLiteral("mention"), EntityType::Mention },
		{ QStringLiteral("text_mention"), EntityType::MentionName },
		{ QStringLiteral("mention_name"), EntityType::MentionName },
		{ QStringLiteral("hashtag"), EntityType::Hashtag },
		{ QStringLiteral("bot_command"), EntityType::BotCommand },
	};
	return map.value(name, EntityType::Invalid);
}

Entities NormalizeEntities(const QString &text, Entities entities) {
	const auto size = int(text.size());
	auto result = Entities();
	result.reserve(entities.size());
	for (auto &entity : entities) {
		if (entity.type == EntityType::Invalid) {
			continue;
		}
		auto from = std::clamp(entity.offset, 0, size);
		auto till = std::clamp(
			int(std::min(qint64(entity.offset) + entity.length, qint64(size))),
			from,
			size);
		// Never cut a surrogate pair in half.
		if (from > 0
			&& from < size
			&& text[from].isLowSurrogate()
			&& text[from - 1].isHighSurrogate()) {
			--from;
		}
		if (till > 0
			&& till < size
			&& text[till].isLowSurrogate()
			&& text[till - 1].isHighSurrogate()) {
			++till;
		}
		if (till <= from) {
			continue;
		}
		entity.offset = from;
		entity.length = till - from;
		result.push_back(std::move(entity));
	}
	std::stable_sort(
		result.begin(),
		result.end(),
		[](const Entity &a, const Entity &b) {
			return (a.offset < b.offset)
				|| (a.offset == b.offset && a.length > b.length);
		});
	return result;
}

TextWithEntities ParseEntities(
		const QString &text,
		uint flags,
		Entities existing) {
	auto result = TextWithEntities{ text, NormalizeEntities(text, existing) };
	// The ranges nothing may be detected in (links, code), merged and keyed
	// by their start: a lookup is logarithmic. The text and the number of
	// entities are the sender's - a scan of every range for every match is
	// quadratic and froze the window on a long message full of links.
	auto blocked = std::map<int, int>();
	for (const auto &entity : result.entities) { // sorted by offset
		const auto blocks = IsLinkType(entity.type)
			|| entity.type == EntityType::Code
			|| entity.type == EntityType::Pre;
		if (!blocks) {
			continue;
		}
		const auto till = entity.offset + entity.length;
		if (!blocked.empty() && std::prev(blocked.end())->second >= entity.offset) {
			auto &last = std::prev(blocked.end())->second;
			last = std::max(last, till);
		} else {
			blocked.emplace(entity.offset, till);
		}
	}
	const auto overlaps = [&](int offset, int length) {
		// The last range that starts before our end: ranges are disjoint, so
		// it is the only one that can reach into us.
		auto i = blocked.lower_bound(offset + length);
		if (i == blocked.begin()) {
			return false;
		}
		--i;
		return (i->second > offset);
	};
	auto found = Entities();
	const auto add = [&](EntityType type, int offset, int length) {
		if (length > 0 && !overlaps(offset, length)) {
			found.push_back(Entity{ type, offset, length, QString() });
			blocked.emplace(offset, offset + length);
		}
	};
	const auto options = QRegularExpression::UseUnicodePropertiesOption;
	if (flags & ParseLinks) {
		static const auto email = QRegularExpression(
			QStringLiteral(
				"(?<![\\w.+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+"
				"(?:\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}"),
			options);
		static const auto url = QRegularExpression(
			QStringLiteral(
				// "-" in the look-behind: a host label may hold hyphens, and
				// without it every "-" of "-a-a-a-..." starts a new attempt
				// that scans to the end (quadratic in the sender's text).
				"(?<![\\w@.-])(?:(?:https?://|www\\.)[^\\s<>\"«»]+"
				"|(?:[A-Za-z0-9-]+\\.)+(?:com|org|net|ir|me|io|app|dev|co|info"
				"|edu|gov|tv|ai|xyz)(?:/[^\\s<>\"«»]*)?)"),
			options | QRegularExpression::CaseInsensitiveOption);
		auto i = email.globalMatch(text);
		while (i.hasNext()) {
			const auto match = i.next();
			add(
				EntityType::Email,
				int(match.capturedStart()),
				int(match.capturedLength()));
		}
		i = url.globalMatch(text);
		while (i.hasNext()) {
			const auto match = i.next();
			auto length = int(match.capturedLength());
			const auto start = int(match.capturedStart());
			// Trailing punctuation is not a part of the address.
			static const auto trailing = QStringLiteral(".,;:!?)]}'،؛؟");
			while (length > 0 && trailing.contains(text[start + length - 1])) {
				--length;
			}
			add(EntityType::Url, start, length);
		}
	}
	if (flags & ParseMentions) {
		static const auto mention = QRegularExpression(
			QStringLiteral("(?<![\\w@])@[A-Za-z0-9_.]{3,32}"),
			options);
		auto i = mention.globalMatch(text);
		while (i.hasNext()) {
			const auto match = i.next();
			auto length = int(match.capturedLength());
			const auto start = int(match.capturedStart());
			while (length > 1 && text[start + length - 1] == QLatin1Char('.')) {
				--length;
			}
			add(EntityType::Mention, start, length);
		}
	}
	if (flags & ParseHashtags) {
		static const auto hashtag = QRegularExpression(
			QStringLiteral("(?<![\\w#])#[\\p{L}\\p{N}_‌]{1,64}"),
			options);
		auto i = hashtag.globalMatch(text);
		while (i.hasNext()) {
			const auto match = i.next();
			add(
				EntityType::Hashtag,
				int(match.capturedStart()),
				int(match.capturedLength()));
		}
	}
	if ((flags & ParseBotCommands) && text.contains(QLatin1Char('/'))) {
		// Last: a slash inside an address, a mention or a tag is not a command.
		static const auto command = QRegularExpression(
			QStringLiteral(
				"(?<![\\p{L}\\p{N}_/@.:<>\\-])"
				"/[A-Za-z0-9_]{1,32}(?:@[A-Za-z][A-Za-z0-9_.]{1,30}[A-Za-z0-9_])?"
				"(?![\\p{L}\\p{N}_/@])"),
			options);
		auto i = command.globalMatch(text);
		while (i.hasNext()) {
			const auto match = i.next();
			add(
				EntityType::BotCommand,
				int(match.capturedStart()),
				int(match.capturedLength()));
		}
	}
	result.entities.append(found);
	result.entities = NormalizeEntities(text, std::move(result.entities));
	return result;
}

QString LinkUrl(const Link &link) {
	switch (link.type) {
	case EntityType::Url: {
		const auto &url = link.data.isEmpty() ? link.text : link.data;
		return url.contains(QStringLiteral("://"))
			? url
			: (QStringLiteral("https://") + url);
	}
	case EntityType::CustomUrl:
		return link.data.contains(QStringLiteral("://"))
			? link.data
			: (QStringLiteral("https://") + link.data);
	case EntityType::Email:
		return QStringLiteral("mailto:") + link.text;
	case EntityType::Phone:
		return QStringLiteral("tel:") + link.text;
	default:
		return QString();
	}
}

TextWithEntities IsolateLinks(TextWithEntities text) {
	// The links in order, without the ones that overlap an earlier one.
	auto links = Entities();
	auto sorted = text.entities;
	std::stable_sort(sorted.begin(), sorted.end(), [](
			const Entity &a,
			const Entity &b) {
		return a.offset < b.offset;
	});
	const auto size = int(text.text.size());
	auto till = 0;
	for (const auto &entity : sorted) {
		if (IsLinkType(entity.type)
			&& entity.length > 0
			&& entity.offset >= till
			&& entity.offset + entity.length <= size) {
			links.push_back(entity);
			till = entity.offset + entity.length;
		}
	}
	if (links.isEmpty()) {
		return text;
	}
	// One character goes in before each link and one after it. A range
	// starts after the marks that are at or before its first character and
	// ends before the ones that are at or after its end.
	// `links` are in order and do not overlap: their starts and their ends
	// are both ascending, so the counts are binary searches.
	auto starts = std::vector<int>();
	auto ends = std::vector<int>();
	starts.reserve(links.size());
	ends.reserve(links.size());
	for (const auto &link : links) {
		starts.push_back(link.offset);
		ends.push_back(link.offset + link.length);
	}
	const auto notAfter = [](const std::vector<int> &list, int position) {
		return int(std::upper_bound(list.begin(), list.end(), position) - list.begin());
	};
	const auto before = [](const std::vector<int> &list, int position) {
		return int(std::lower_bound(list.begin(), list.end(), position) - list.begin());
	};
	const auto shiftStart = [&](int position) {
		return notAfter(starts, position) + notAfter(ends, position);
	};
	const auto shiftEnd = [&](int position) {
		return before(starts, position) + before(ends, position);
	};
	auto result = TextWithEntities();
	result.text.reserve(size + 2 * links.size());
	auto from = 0;
	for (const auto &link : links) {
		// Addresses and handles read left to right; a tag or a named link
		// reads in the direction of its own first letter.
		const auto own = (link.type == EntityType::Hashtag)
			|| (link.type == EntityType::CustomUrl)
			|| (link.type == EntityType::MentionName);
		result.text += QStringView(text.text).mid(from, link.offset - from);
		result.text += QChar(own ? 0x2068 : 0x2066);
		result.text += QStringView(text.text).mid(link.offset, link.length);
		result.text += QChar(0x2069);
		from = link.offset + link.length;
	}
	result.text += QStringView(text.text).mid(from);
	result.entities.reserve(text.entities.size());
	for (auto entity : text.entities) {
		const auto begin = std::clamp(entity.offset, 0, size);
		const auto end = std::clamp(entity.offset + entity.length, begin, size);
		entity.offset = begin + shiftStart(begin);
		entity.length = std::max((end + shiftEnd(end)) - entity.offset, 0);
		result.entities.push_back(entity);
	}
	return result;
}

bool IsIsolateChar(QChar ch) {
	return (ch.unicode() >= 0x2066) && (ch.unicode() <= 0x2069);
}

QString StripIsolates(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	for (const auto ch : text) {
		if (!IsIsolateChar(ch)) {
			result.push_back(ch);
		}
	}
	return result;
}

Selection SourceSelection(const QString &isolated, Selection selection) {
	const auto size = int(isolated.size());
	const auto from = std::clamp(selection.from, 0, size);
	const auto to = std::clamp(selection.to, from, size);
	auto before = 0;
	auto inside = 0;
	for (auto i = 0; i != to; ++i) {
		if (IsIsolateChar(isolated[i])) {
			++((i < from) ? before : inside);
		}
	}
	return { from - before, to - before - inside };
}

Selection IsolatedSelection(const QString &isolated, Selection source) {
	if (source.empty()) {
		return source;
	}
	// The place of the source character number `index` in the shown text.
	const auto size = int(isolated.size());
	auto result = Selection{ size, size };
	auto index = 0;
	for (auto i = 0; i != size; ++i) {
		if (IsIsolateChar(isolated[i])) {
			continue;
		}
		if (index == source.from) {
			result.from = i;
		}
		if (index == source.to - 1) {
			result.to = i + 1;
			break;
		}
		++index;
	}
	return result;
}

} // namespace Ui::Text
