#include "data/data_note_types.h"

#include "data/data_json.h"

#include <QtCore/QSet>
#include <QtCore/QTextBoundaryFinder>

#include <algorithm>

namespace Data {
namespace {

using namespace Json;

// U+21A9 LEFTWARDS ARROW WITH HOOK: every note reply starts with it.
constexpr auto kReplyArrow = char16_t(0x21A9);
// U+FE0E VARIATION SELECTOR-15: the arrow is drawn as text, not as an emoji.
constexpr auto kTextStyle = char16_t(0xFE0E);
constexpr auto kQuoteOpen = char16_t(0x00AB);  // «
constexpr auto kQuoteClose = char16_t(0x00BB); // »
// At most this many characters between the arrow and «: the old format's
// label («نوتت: », "Your note: "), with a little room for other languages.
constexpr auto kLongestLabel = 24;

[[nodiscard]] QString ArrowMark() {
	return QString(QChar(kReplyArrow)) + QChar(kTextStyle);
}

} // namespace

QString NoteAudienceWire(NoteAudience audience) {
	return (audience == NoteAudience::CloseFriends)
		? u"close_friends"_s
		: u"mutuals"_s;
}

NoteAudience NoteAudienceFromWire(const QString &value) {
	return (value == u"close_friends")
		? NoteAudience::CloseFriends
		: NoteAudience::Mutuals;
}

std::optional<Note> Note::FromJson(const QJsonValue &json) {
	if (!json.isObject()) {
		return std::nullopt;
	}
	const auto object = json.toObject();
	auto result = Note();
	// Shown next to our own words, like a name: one line, without direction
	// overrides or invisible marks.
	result.text = CleanName(NoteText::Cleaned(String(object, "text"_L1)));
	if (result.text.isEmpty()) {
		return std::nullopt;
	}
	result.id = String(object, "id"_L1);
	result.audience = NoteAudienceFromWire(String(object, "audience"_L1));
	result.createdAt = Int64(object, "created_at"_L1);
	result.expiresAt = Int64(object, "expires_at"_L1);
	return result;
}

NotesFeed NotesFeed::FromJson(const QJsonObject &json, int limit) {
	auto result = NotesFeed();
	result.me = Note::FromJson(json.value("me"_L1));
	auto seen = QSet<QString>();
	for (const auto &value : json.value("items"_L1).toArray()) {
		if (result.items.size() >= limit) {
			break;
		}
		const auto object = value.toObject();
		const auto user = object.value("user"_L1);
		auto note = Note::FromJson(object.value("note"_L1));
		if (!user.isObject() || !note) {
			continue;
		}
		auto card = User::FromJson(user.toObject());
		if (!card.valid() || card.deleted || seen.contains(card.id)) {
			continue;
		}
		seen.insert(card.id);
		result.items.push_back({ std::move(card), std::move(*note) });
	}
	return result;
}

NotesFeed NotesFeed::fresh(qint64 now) const {
	auto result = NotesFeed();
	if (me && !me->expired(now)) {
		result.me = me;
	}
	result.items.reserve(items.size());
	for (const auto &item : items) {
		if (!item.note.expired(now)) {
			result.items.push_back(item);
		}
	}
	return result;
}

qint64 NotesFeed::nextExpiry(qint64 now) const {
	auto result = qint64(0);
	const auto consider = [&](const Note &note) {
		if (note.expiresAt > now && (!result || note.expiresAt < result)) {
			result = note.expiresAt;
		}
	};
	if (me) {
		consider(*me);
	}
	for (const auto &item : items) {
		consider(item.note);
	}
	return result;
}

const NoteItem *NotesFeed::item(const QString &userId) const {
	const auto i = std::find_if(items.begin(), items.end(), [&](
			const NoteItem &item) {
		return item.user.id == userId;
	});
	return (i != items.end()) ? &*i : nullptr;
}

namespace NoteText {

int Length(const QString &text) {
	auto result = 0;
	const auto size = text.size();
	for (auto i = 0; i != size; ++i) {
		if (text[i].isHighSurrogate()
			&& i + 1 < size
			&& text[i + 1].isLowSurrogate()) {
			++i;
		}
		++result;
	}
	return result;
}

QString Cleaned(const QString &text) {
	return text.simplified();
}

bool IsValid(const QString &text, int maxChars) {
	const auto clean = Cleaned(text);
	return !clean.isEmpty() && Length(clean) <= maxChars;
}

int Left(const QString &text, int maxChars) {
	return maxChars - Length(Cleaned(text));
}

QString Limited(const QString &text, int maxChars) {
	if (Length(text) <= maxChars) {
		return text;
	}
	auto finder = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, text);
	auto count = 0;
	auto end = 0;
	for (auto next = finder.toNextBoundary(); next > 0; next = finder.toNextBoundary()) {
		const auto size = Length(text.mid(end, next - end));
		if (count + size > maxChars) {
			break;
		}
		count += size;
		end = int(next);
	}
	return text.left(end);
}

} // namespace NoteText

QString NoteReply::Build(const QString &note, const QString &reply) {
	return ArrowMark()
		+ u' '
		+ QChar(kQuoteOpen)
		+ NoteText::Cleaned(note)
		+ QChar(kQuoteClose)
		+ u'\n'
		+ reply.trimmed();
}

std::optional<NoteReply> NoteReply::Parse(const QString &text) {
	if (text.isEmpty() || text[0] != QChar(kReplyArrow)) {
		return std::nullopt;
	}
	auto newline = -1;
	for (auto i = 0, size = int(text.size()); i != size; ++i) {
		if (text[i] == u'\n' || text[i] == u'\r') {
			newline = i;
			break;
		}
	}
	if (newline < 0) {
		return std::nullopt;
	}
	auto start = newline + 1;
	while (start < text.size() && text[start].isSpace()) {
		++start;
	}
	const auto reply = text.mid(start).trimmed();
	if (reply.isEmpty()) {
		return std::nullopt;
	}
	const auto first = text.left(newline).trimmed();
	if (!first.endsWith(QChar(kQuoteClose))) {
		return std::nullopt;
	}
	const auto open = first.indexOf(QChar(kQuoteOpen));
	const auto close = int(first.size()) - 1;
	if (open < 0 || open >= close) {
		return std::nullopt;
	}
	// The arrow (with its selector), then nothing or a short label.
	auto label = first.left(open);
	label.remove(QChar(kTextStyle));
	if (NoteText::Length(label) > kLongestLabel + 2) {
		return std::nullopt;
	}
	const auto note = first.mid(open + 1, close - open - 1).trimmed();
	if (note.isEmpty()) {
		return std::nullopt;
	}
	return NoteReply{ note, reply, start };
}

QString NoteReply::Preview(const QString &text) {
	const auto parsed = Parse(text);
	return parsed ? (ArrowMark() + u' ' + parsed->reply) : text;
}

QVector<Entity> NoteReply::replyEntities(
		const QVector<Entity> &entities) const {
	auto result = QVector<Entity>();
	const auto from = replyOffset;
	const auto till = replyOffset + int(reply.size());
	for (auto entity : entities) {
		const auto begin = std::max(entity.offset, from);
		const auto end = std::min(entity.offset + entity.length, till);
		if (end <= begin) {
			continue;
		}
		entity.offset = begin - from;
		entity.length = end - begin;
		result.push_back(std::move(entity));
	}
	return result;
}

} // namespace Data
