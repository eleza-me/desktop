// Rich text model: a string plus formatting ranges.
//
// Offsets and lengths are in UTF-16 code units (the same units QString uses and the
// server sends), so a character outside the BMP — most emoji — counts as two.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

namespace Ui::Text {

enum class EntityType : uchar {
	Invalid,
	Bold,
	Italic,
	Underline,
	StrikeOut,
	Code, // inline monospace
	Pre, // monospace block
	Spoiler, // hidden until clicked
	Url, // the text itself is the address
	CustomUrl, // data = the address
	Email,
	Phone,
	Mention, // @username
	MentionName, // data = user id
	Hashtag,
	BotCommand,
};

struct Entity {
	EntityType type = EntityType::Invalid;
	int offset = 0;
	int length = 0;
	QString data;

	friend bool operator==(const Entity &a, const Entity &b) {
		return (a.type == b.type)
			&& (a.offset == b.offset)
			&& (a.length == b.length)
			&& (a.data == b.data);
	}
};

using Entities = QList<Entity>;

struct TextWithEntities {
	QString text;
	Entities entities;

	[[nodiscard]] bool empty() const {
		return text.isEmpty();
	}
};

[[nodiscard]] inline bool IsLinkType(EntityType type) {
	switch (type) {
	case EntityType::Url:
	case EntityType::CustomUrl:
	case EntityType::Email:
	case EntityType::Phone:
	case EntityType::Mention:
	case EntityType::MentionName:
	case EntityType::Hashtag:
	case EntityType::BotCommand:
		return true;
	default:
		return false;
	}
}

// Entity names used by the API ("bold", "text_link", "spoiler", ...).
[[nodiscard]] EntityType EntityTypeFromName(const QString &name);

// Sorts, clamps to the text, drops empty / invalid ranges and never leaves an
// edge inside a surrogate pair.
[[nodiscard]] Entities NormalizeEntities(const QString &text, Entities entities);

enum ParseFlag : uint {
	ParseLinks = 0x01, // https://…, www.…, name@mail.tld
	ParseMentions = 0x02, // @username
	ParseHashtags = 0x04, // #tag
	ParseAll = ParseLinks | ParseMentions | ParseHashtags,
};

// Detects links / mentions / hashtags in plain text and merges them with `existing`
// (existing link entities win where they overlap).
[[nodiscard]] TextWithEntities ParseEntities(
	const QString &text,
	uint flags = ParseAll,
	Entities existing = {});

// A selection range [from, to) in UTF-16 units.
struct Selection {
	int from = 0;
	int to = 0;

	[[nodiscard]] bool empty() const {
		return from >= to;
	}
	friend bool operator==(Selection a, Selection b) {
		return (a.from == b.from) && (a.to == b.to);
	}
	friend bool operator!=(Selection a, Selection b) {
		return !(a == b);
	}
};

// A clickable range of a laid out text.
struct Link {
	EntityType type = EntityType::Invalid;
	QString text; // the visible text of the range
	QString data; // url / user id / the text for mentions and hashtags
	int offset = 0;
	int length = 0;
};

// The address to open for Url / CustomUrl / Email / Phone links, empty otherwise.
[[nodiscard]] QString LinkUrl(const Link &link);

} // namespace Ui::Text
