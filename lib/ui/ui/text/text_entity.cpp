#include "ui/text/text_entity.h"

#include <QtCore/QHash>
#include <QtCore/QRegularExpression>

#include <algorithm>

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
	const auto overlapsLink = [&](int offset, int length) {
		for (const auto &entity : result.entities) {
			const auto blocks = IsLinkType(entity.type)
				|| entity.type == EntityType::Code
				|| entity.type == EntityType::Pre;
			if (blocks
				&& entity.offset < offset + length
				&& offset < entity.offset + entity.length) {
				return true;
			}
		}
		return false;
	};
	auto found = Entities();
	const auto add = [&](EntityType type, int offset, int length) {
		if (length > 0 && !overlapsLink(offset, length)) {
			for (const auto &entity : found) {
				if (entity.offset < offset + length
					&& offset < entity.offset + entity.length) {
					return;
				}
			}
			found.push_back(Entity{ type, offset, length, QString() });
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
				"(?<![\\w@.])(?:(?:https?://|www\\.)[^\\s<>\"«»]+"
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

} // namespace Ui::Text
