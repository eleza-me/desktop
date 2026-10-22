#include "history/history_logic.h"

#include "data/data_group_rules.h"

#include <QtCore/QHash>

#include <algorithm>
#include <cmath>

namespace History {
namespace {

[[nodiscard]] bool EntitiesEqual(
		const QVector<Data::Entity> &a,
		const QVector<Data::Entity> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0; i != a.size(); ++i) {
		if (a[i].type != b[i].type
			|| a[i].offset != b[i].offset
			|| a[i].length != b[i].length
			|| a[i].url != b[i].url
			|| a[i].user != b[i].user) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool MediaEqual(
		const std::optional<Data::MediaMeta> &a,
		const std::optional<Data::MediaMeta> &b) {
	if (a.has_value() != b.has_value()) {
		return false;
	} else if (!a) {
		return true;
	}
	return (a->width == b->width)
		&& (a->height == b->height)
		&& (a->durationMs == b->durationMs)
		&& (a->size == b->size)
		&& (a->blurhash == b->blurhash)
		&& (a->waveform == b->waveform);
}

[[nodiscard]] bool MarkupEqual(
		const std::optional<Data::ReplyMarkup> &a,
		const std::optional<Data::ReplyMarkup> &b) {
	if (a.has_value() != b.has_value()) {
		return false;
	} else if (!a) {
		return true;
	} else if (a->inlineKeyboard.size() != b->inlineKeyboard.size()) {
		return false;
	}
	for (auto i = 0; i != a->inlineKeyboard.size(); ++i) {
		const auto &rowA = a->inlineKeyboard[i];
		const auto &rowB = b->inlineKeyboard[i];
		if (rowA.size() != rowB.size()) {
			return false;
		}
		for (auto j = 0; j != rowA.size(); ++j) {
			if (rowA[j].text != rowB[j].text
				|| rowA[j].kind != rowB[j].kind
				|| rowA[j].url != rowB[j].url) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] bool IsSkinTone(char32_t code) {
	return code >= 0x1F3FB && code <= 0x1F3FF;
}

[[nodiscard]] bool IsRegionalIndicator(char32_t code) {
	return code >= 0x1F1E6 && code <= 0x1F1FF;
}

[[nodiscard]] bool IsTag(char32_t code) {
	return code >= 0xE0020 && code <= 0xE007F;
}

[[nodiscard]] std::vector<char32_t> Codepoints(const QString &text) {
	auto result = std::vector<char32_t>();
	result.reserve(text.size());
	for (auto i = 0; i < text.size(); ++i) {
		const auto ch = text[i];
		if (ch.isHighSurrogate()
			&& i + 1 < text.size()
			&& text[i + 1].isLowSurrogate()) {
			result.push_back(QChar::surrogateToUcs4(ch, text[i + 1]));
			++i;
		} else {
			result.push_back(ch.unicode());
		}
	}
	return result;
}

[[nodiscard]] bool SameReplyParts(
		const Data::Message &a,
		const Data::Message &b) {
	if (a.replyToSeq != b.replyToSeq
		|| a.quote.has_value() != b.quote.has_value()) {
		return false;
	}
	return !a.quote
		|| (a.quote->text == b.quote->text && a.quote->offset == b.quote->offset);
}

[[nodiscard]] bool SamePreview(
		const Data::Message &a,
		const Data::Message &b) {
	if (a.linkPreview.has_value() != b.linkPreview.has_value()) {
		return false;
	} else if (!a.linkPreview) {
		return true;
	}
	return (a.linkPreview->url == b.linkPreview->url)
		&& (a.linkPreview->title == b.linkPreview->title)
		&& (a.linkPreview->description == b.linkPreview->description)
		&& (a.linkPreview->siteName == b.linkPreview->siteName)
		&& (a.linkPreview->imageUrl == b.linkPreview->imageUrl);
}

[[nodiscard]] bool SameLocation(
		const Data::Message &a,
		const Data::Message &b) {
	if (a.location.has_value() != b.location.has_value()
		|| a.live.has_value() != b.live.has_value()) {
		return false;
	}
	if (a.location
		&& (a.location->lat != b.location->lat
			|| a.location->lng != b.location->lng
			|| a.location->label != b.location->label
			|| a.location->address != b.location->address)) {
		return false;
	}
	if (a.live
		&& (a.live->until != b.live->until
			|| a.live->stopped != b.live->stopped)) {
		return false;
	}
	return true;
}

} // namespace

QDateTime MessageDateTime(const Data::Message &message) {
	return (message.createdAt > 0)
		? QDateTime::fromMSecsSinceEpoch(message.createdAt)
		: QDateTime::currentDateTime();
}

QDate MessageDate(const Data::Message &message) {
	return MessageDateTime(message).date();
}

bool IsService(const Data::Message &message) {
	return (message.kind == Data::MessageKind::System) && !message.call;
}

bool CanAttach(const Data::Message &previous, const Data::Message &next) {
	if (IsService(previous) || IsService(next)) {
		return false;
	} else if (previous.mine != next.mine || previous.sender != next.sender) {
		return false;
	}
	const auto delta = (next.createdAt > 0 && previous.createdAt > 0)
		? std::abs(next.createdAt - previous.createdAt)
		: qint64(0);
	return (delta < kGroupWindowMs)
		&& (MessageDate(previous) == MessageDate(next));
}

std::vector<RowFlags> ComputeRowFlags(
		const QVector<Data::Message> &items,
		qint64 firstUnreadSeq) {
	auto result = std::vector<RowFlags>(items.size());
	auto unreadShown = false;
	auto previousDate = QDate();
	for (auto i = 0; i != int(items.size()); ++i) {
		const auto &message = items[i];
		auto &flags = result[i];
		const auto date = MessageDate(message);
		flags.showDate = (i == 0) || (date != previousDate);
		previousDate = date;
		if (!unreadShown
			&& firstUnreadSeq > 0
			&& message.seq >= firstUnreadSeq
			&& !message.mine
			&& !IsService(message)) {
			// Nothing to divide when everything on screen is unread.
			flags.showUnread = (i > 0);
			unreadShown = true;
		}
		if (i > 0
			&& !flags.showDate
			&& !flags.showUnread
			&& CanAttach(items[i - 1], message)) {
			flags.attachedToPrevious = true;
			result[i - 1].attachedToNext = true;
		}
	}
	return result;
}

RowId RowId::From(const Data::Message &message) {
	return RowId{ message.key(), message.clientMessageId };
}

ListDiff DiffRows(
		const std::vector<RowId> &was,
		const std::vector<RowId> &now) {
	auto result = ListDiff();
	result.newToOld.assign(now.size(), -1);
	result.oldToNew.assign(was.size(), -1);
	auto index = QHash<QString, int>();
	index.reserve(int(was.size()));
	for (auto i = 0; i != int(was.size()); ++i) {
		index.insert(was[i].key, i);
	}
	auto lastOld = -1;
	for (auto i = 0; i != int(now.size()); ++i) {
		auto found = index.constFind(now[i].key);
		if (found == index.cend() && !now[i].clientMessageId.isEmpty()) {
			found = index.constFind(
				Data::Message::KeyForLocal(now[i].clientMessageId));
		}
		if (found == index.cend() || result.oldToNew[*found] >= 0) {
			++result.added;
			continue;
		}
		result.newToOld[i] = *found;
		result.oldToNew[*found] = i;
		if (*found < lastOld) {
			result.reordered = true;
		}
		lastOld = std::max(lastOld, *found);
	}
	for (const auto mapped : result.oldToNew) {
		if (mapped < 0) {
			++result.removed;
		}
	}
	auto first = -1;
	auto last = -1;
	for (auto i = 0; i != int(now.size()); ++i) {
		if (result.newToOld[i] >= 0) {
			if (first < 0) {
				first = i;
			}
			last = i;
		}
	}
	if (first < 0) {
		result.addedAtBack = result.added;
	} else {
		result.addedAtFront = first;
		result.addedAtBack = int(now.size()) - 1 - last;
	}
	return result;
}

bool SameLayout(const Data::Message &a, const Data::Message &b) {
	const auto failedA = (a.status == Data::MessageStatus::Failed);
	const auto failedB = (b.status == Data::MessageStatus::Failed);
	const auto uploadingA = (a.uploadProgress >= 0.);
	const auto uploadingB = (b.uploadProgress >= 0.);
	return (a.kind == b.kind)
		&& (a.kindName == b.kindName)
		&& (a.text == b.text)
		&& (a.mine == b.mine)
		&& (a.sender == b.sender)
		&& (a.edited == b.edited)
		&& (a.forwarded == b.forwarded)
		&& (a.forwardedPost == b.forwardedPost)
		&& (a.system == b.system)
		&& (a.mediaSpoiler == b.mediaSpoiler)
		&& (a.album == b.album)
		&& (a.captionAbove == b.captionAbove)
		&& (a.underReview == b.underReview)
		&& (a.createdAt == b.createdAt)
		&& (failedA == failedB)
		&& (uploadingA == uploadingB)
		&& (a.errorText == b.errorText)
		&& (a.reactions == b.reactions)
		&& (a.myReaction == b.myReaction)
		&& (a.sticker.has_value() == b.sticker.has_value())
		&& (!a.sticker || a.sticker->id == b.sticker->id)
		&& (a.call.has_value() == b.call.has_value())
		&& (!a.call
			|| (a.call->outcome == b.call->outcome
				&& a.call->durationS == b.call->durationS))
		&& (a.audio.has_value() == b.audio.has_value())
		&& (!a.audio
			|| (a.audio->title == b.audio->title
				&& a.audio->performer == b.audio->performer))
		&& (a.viaBot.has_value() == b.viaBot.has_value())
		&& (a.selfDestruct == b.selfDestruct)
		&& EntitiesEqual(a.entities, b.entities)
		&& MediaEqual(a.media, b.media)
		&& MarkupEqual(a.replyMarkup, b.replyMarkup)
		&& SameReplyParts(a, b)
		&& SamePreview(a, b)
		&& SameLocation(a, b);
}

bool SameForView(const Data::Message &a, const Data::Message &b) {
	return SameLayout(a, b)
		&& (a.seq == b.seq)
		&& (a.status == b.status)
		&& (a.played == b.played)
		&& (a.attachment == b.attachment)
		&& (a.localPath == b.localPath)
		&& (std::abs(a.uploadProgress - b.uploadProgress) < 0.001)
		&& (a.errorCode == b.errorCode);
}

Ui::Text::Entities ConvertEntities(
		const QString &text,
		const QVector<Data::Entity> &entities) {
	auto result = Ui::Text::Entities();
	result.reserve(entities.size());
	for (const auto &entity : entities) {
		auto converted = Ui::Text::Entity();
		converted.type = Ui::Text::EntityTypeFromName(entity.type);
		if (converted.type == Ui::Text::EntityType::Invalid) {
			continue;
		}
		converted.offset = entity.offset;
		converted.length = entity.length;
		converted.data = entity.url;
		if (converted.type == Ui::Text::EntityType::Mention
			&& !entity.user.isEmpty()) {
			// A member of a group picked by name: the text is whatever the
			// sender's app wrote, the person is the id.
			converted.type = Ui::Text::EntityType::MentionName;
			converted.data = entity.user;
		}
		result.push_back(std::move(converted));
	}
	return Ui::Text::NormalizeEntities(text, std::move(result));
}

Ui::Text::TextWithEntities TextForMessage(
		const Data::Message &message,
		bool botCommands) {
	// For showing: links are isolated (Ui::Text::IsolateLinks), so quoting and
	// copying go through Ui::Text::SourceSelection / StripIsolates.
	return Ui::Text::IsolateLinks(Ui::Text::ParseEntities(
		message.text,
		Ui::Text::ParseAll | (botCommands ? Ui::Text::ParseBotCommands : 0u),
		ConvertEntities(message.text, message.entities)));
}

bool IsEmojiCodepoint(char32_t code) {
	return (code >= 0x1F000 && code <= 0x1FAFF)
		|| (code >= 0x2600 && code <= 0x27BF)
		|| (code >= 0x2300 && code <= 0x23FF)
		|| (code >= 0x2B05 && code <= 0x2B55)
		|| (code >= 0x2194 && code <= 0x21AA)
		|| (code >= 0x25AA && code <= 0x25FE)
		|| code == 0x2934
		|| code == 0x2935
		|| code == 0x3030
		|| code == 0x303D
		|| code == 0x3297
		|| code == 0x3299
		|| code == 0x00A9
		|| code == 0x00AE
		|| code == 0x203C
		|| code == 0x2049
		|| code == 0x2122
		|| code == 0x2139;
}

int JumboEmojiCount(const QString &text) {
	const auto trimmed = text.trimmed();
	if (trimmed.isEmpty() || trimmed.size() > 48) {
		return 0;
	}
	const auto codes = Codepoints(trimmed);
	const auto size = int(codes.size());
	auto count = 0;
	auto i = 0;
	const auto skipModifiers = [&] {
		while (i < size
			&& (codes[i] == 0xFE0F || IsSkinTone(codes[i]) || IsTag(codes[i]))) {
			++i;
		}
	};
	while (i < size) {
		const auto code = codes[i];
		if (code == U' ' || code == U'\n' || code == 0x200C) {
			++i;
			continue;
		}
		if (IsRegionalIndicator(code)) {
			// A flag is a pair of regional indicators.
			i += (i + 1 < size && IsRegionalIndicator(codes[i + 1])) ? 2 : 1;
		} else if (IsEmojiCodepoint(code) && !IsSkinTone(code)) {
			++i;
			skipModifiers();
			// Joined sequences (a family, a profession) are one emoji.
			while (i + 1 < size
				&& codes[i] == 0x200D
				&& IsEmojiCodepoint(codes[i + 1])) {
				i += 2;
				skipModifiers();
			}
		} else {
			return 0;
		}
		if (++count > 3) {
			return 0;
		}
	}
	return count;
}

std::optional<Data::Quote> MakeQuote(
		const QString &messageText,
		Ui::Text::Selection selection,
		int maxLength) {
	const auto size = int(messageText.size());
	auto from = std::clamp(selection.from, 0, size);
	auto till = std::clamp(selection.to, from, size);
	const auto insidePair = [&](int position) {
		return position > 0
			&& position < size
			&& messageText[position].isLowSurrogate()
			&& messageText[position - 1].isHighSurrogate();
	};
	if (insidePair(from)) {
		--from;
	}
	if (insidePair(till)) {
		++till;
	}
	while (from < till && messageText[from].isSpace()) {
		++from;
	}
	while (till > from && messageText[till - 1].isSpace()) {
		--till;
	}
	if (maxLength > 0 && till - from > maxLength) {
		till = from + maxLength;
		if (insidePair(till)) {
			--till;
		}
		while (till > from && messageText[till - 1].isSpace()) {
			--till;
		}
	}
	if (till <= from) {
		return std::nullopt;
	}
	auto result = Data::Quote();
	result.text = messageText.mid(from, till - from);
	result.offset = from;
	return result;
}

std::optional<Ui::Text::Selection> FindQuote(
		const QString &messageText,
		const Data::Quote &quote) {
	const auto length = int(quote.text.size());
	if (!length) {
		return std::nullopt;
	}
	if (quote.offset >= 0
		&& quote.offset + length <= messageText.size()
		&& QStringView(messageText).mid(quote.offset, length) == quote.text) {
		return Ui::Text::Selection{ quote.offset, quote.offset + length };
	}
	const auto found = int(messageText.indexOf(quote.text));
	if (found >= 0) {
		return Ui::Text::Selection{ found, found + length };
	}
	return std::nullopt;
}

QString FormatForCopy(const std::vector<CopyItem> &items) {
	if (items.empty()) {
		return QString();
	} else if (items.size() == 1) {
		return items.front().text;
	}
	auto parts = QStringList();
	parts.reserve(int(items.size()));
	for (const auto &item : items) {
		auto head = item.sender;
		const auto time = item.time.isValid()
			? item.time.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
			: QString();
		if (!time.isEmpty()) {
			head += head.isEmpty() ? time : (QStringLiteral(", ") + time);
		}
		parts.push_back(QLatin1Char('[') + head + QStringLiteral("]\n") + item.text);
	}
	return parts.join(QStringLiteral("\n\n"));
}

QString MessagePlainText(
		const Data::Message &message,
		const QString &kindLabel) {
	if (kindLabel.isEmpty()) {
		return message.text;
	} else if (message.text.trimmed().isEmpty()) {
		return kindLabel;
	}
	return kindLabel + QLatin1Char('\n') + message.text;
}

QString NormalizeForSearch(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	for (const auto ch : text.toCaseFolded()) {
		const auto code = ch.unicode();
		if (code == 0x200C
			|| code == 0x200D
			|| code == 0x0640
			|| (code >= 0x064B && code <= 0x0652)) {
			continue;
		} else if (code == 0x064A || code == 0x0649) {
			result.push_back(QChar(0x06CC));
		} else if (code == 0x0643) {
			result.push_back(QChar(0x06A9));
		} else if (code >= 0x06F0 && code <= 0x06F9) {
			result.push_back(QChar(ushort('0' + (code - 0x06F0))));
		} else if (code >= 0x0660 && code <= 0x0669) {
			result.push_back(QChar(ushort('0' + (code - 0x0660))));
		} else if (ch.isSpace()) {
			result.push_back(QLatin1Char(' '));
		} else {
			result.push_back(ch);
		}
	}
	return result;
}

std::vector<int> SearchMessages(
		const QVector<Data::Message> &items,
		const QString &query) {
	auto result = std::vector<int>();
	const auto words = NormalizeForSearch(query).split(
		QLatin1Char(' '),
		Qt::SkipEmptyParts);
	if (words.isEmpty()) {
		return result;
	}
	for (auto i = int(items.size()) - 1; i >= 0; --i) {
		const auto &message = items[i];
		if (IsService(message) || message.seq <= 0 || message.selfDestruct) {
			// Self-destructing media is never found by its words.
			continue;
		}
		auto haystack = message.text;
		if (message.audio) {
			haystack += QLatin1Char(' ') + message.audio->title
				+ QLatin1Char(' ') + message.audio->performer;
		}
		if (message.linkPreview) {
			haystack += QLatin1Char(' ') + message.linkPreview->title
				+ QLatin1Char(' ') + message.linkPreview->description;
		}
		if (message.location) {
			haystack += QLatin1Char(' ') + message.location->label
				+ QLatin1Char(' ') + message.location->address;
		}
		const auto normalized = NormalizeForSearch(haystack);
		const auto all = std::all_of(
			words.begin(),
			words.end(),
			[&](const QString &word) { return normalized.contains(word); });
		if (all) {
			result.push_back(i);
		}
	}
	return result;
}

std::vector<ReactionCount> SortedReactions(const Data::Message &message) {
	auto result = std::vector<ReactionCount>();
	for (auto i = message.reactions.cbegin(); i != message.reactions.cend(); ++i) {
		if (i.value() > 0) {
			result.push_back({ i.key(), i.value(), i.key() == message.myReaction });
		}
	}
	std::stable_sort(
		result.begin(),
		result.end(),
		[](const ReactionCount &a, const ReactionCount &b) {
			return (a.count > b.count)
				|| (a.count == b.count && a.emoji < b.emoji);
		});
	return result;
}

void ApplyMyReaction(Data::Message &message, const QString &emoji) {
	const auto next = (message.myReaction == emoji) ? QString() : emoji;
	if (!message.myReaction.isEmpty()) {
		const auto left = message.reactions.value(message.myReaction, 1) - 1;
		if (left > 0) {
			message.reactions[message.myReaction] = left;
		} else {
			message.reactions.remove(message.myReaction);
		}
	}
	if (!next.isEmpty()) {
		message.reactions[next] = message.reactions.value(next, 0) + 1;
	}
	message.myReaction = next;
}

QSize MediaBoxSize(
		int width,
		int height,
		Data::MessageKind kind,
		int maxWidth) {
	constexpr auto kMinSide = 120;
	constexpr auto kMaxHeight = 380;
	constexpr auto kNoteSize = 220;
	if (kind == Data::MessageKind::VideoNote) {
		const auto side = std::min(kNoteSize, std::max(maxWidth, kMinSide));
		return QSize(side, side);
	}
	maxWidth = std::max(maxWidth, kMinSide);
	auto ratio = (width > 0 && height > 0) ? (double(width) / height) : 1.5;
	ratio = std::clamp(ratio, 0.45, 2.4);
	// Small pictures are not blown up to the full bubble width.
	auto boxWidth = (width > 0) ? std::min(width, maxWidth) : maxWidth;
	boxWidth = std::max(boxWidth, std::min(maxWidth, 200));
	auto boxHeight = int(std::lround(boxWidth / ratio));
	if (boxHeight > kMaxHeight) {
		boxHeight = kMaxHeight;
		boxWidth = int(std::lround(boxHeight * ratio));
	}
	boxWidth = std::clamp(boxWidth, std::min(kMinSide, maxWidth), maxWidth);
	boxHeight = std::max(boxHeight, kMinSide / 2);
	return QSize(boxWidth, boxHeight);
}

std::vector<float> WaveformBars(const QByteArray &waveform, int bars) {
	auto result = std::vector<float>();
	if (bars <= 0) {
		return result;
	}
	result.reserve(bars);
	if (waveform.isEmpty()) {
		for (auto i = 0; i != bars; ++i) {
			result.push_back(float(
				(60. + 50. * std::abs(std::sin(i * 1.7))) / 255.));
		}
		return result;
	}
	const auto size = int(waveform.size());
	for (auto i = 0; i != bars; ++i) {
		const auto from = i * size / bars;
		const auto till = std::max((i + 1) * size / bars, from + 1);
		auto peak = 0;
		for (auto j = from; j < till && j < size; ++j) {
			peak = std::max(peak, int(uchar(waveform[j])));
		}
		result.push_back(std::max(float(peak) / 255.f, 0.12f));
	}
	return result;
}

FileType FileTypeFor(const QString &name, const QString &mime) {
	const auto dot = name.lastIndexOf(QLatin1Char('.'));
	const auto extension = (dot >= 0) ? name.mid(dot + 1).toLower() : QString();
	static const auto kImages = QStringList{
		u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"webp"_s, u"bmp"_s,
		u"svg"_s, u"heic"_s, u"tiff"_s, u"ico"_s,
	};
	static const auto kVideos = QStringList{
		u"mp4"_s, u"mov"_s, u"mkv"_s, u"webm"_s, u"avi"_s, u"m4v"_s,
	};
	static const auto kAudio = QStringList{
		u"mp3"_s, u"ogg"_s, u"oga"_s, u"wav"_s, u"flac"_s, u"m4a"_s,
		u"aac"_s, u"opus"_s,
	};
	static const auto kDocuments = QStringList{
		u"pdf"_s, u"doc"_s, u"docx"_s, u"xls"_s, u"xlsx"_s, u"ppt"_s,
		u"pptx"_s, u"txt"_s, u"rtf"_s, u"odt"_s, u"csv"_s, u"epub"_s,
		u"md"_s,
	};
	static const auto kArchives = QStringList{
		u"zip"_s, u"rar"_s, u"7z"_s, u"tar"_s, u"gz"_s, u"bz2"_s, u"xz"_s,
		u"apk"_s, u"dmg"_s, u"iso"_s,
	};
	static const auto kCode = QStringList{
		u"json"_s, u"xml"_s, u"html"_s, u"css"_s, u"js"_s, u"ts"_s,
		u"cpp"_s, u"h"_s, u"py"_s, u"go"_s, u"rs"_s, u"java"_s, u"kt"_s,
		u"swift"_s, u"sh"_s, u"yml"_s, u"yaml"_s, u"sql"_s,
	};
	if (kImages.contains(extension) || mime.startsWith(u"image/")) {
		return FileType::Image;
	} else if (kVideos.contains(extension) || mime.startsWith(u"video/")) {
		return FileType::Video;
	} else if (kAudio.contains(extension) || mime.startsWith(u"audio/")) {
		return FileType::Audio;
	} else if (kArchives.contains(extension)
		|| mime.contains(u"zip")
		|| mime.contains(u"compressed")) {
		return FileType::Archive;
	} else if (kCode.contains(extension)) {
		return FileType::Code;
	} else if (kDocuments.contains(extension)
		|| mime == u"application/pdf"
		|| mime.startsWith(u"text/")) {
		return FileType::Document;
	}
	return FileType::Generic;
}

QString FileExtensionLabel(const QString &name) {
	const auto dot = name.lastIndexOf(QLatin1Char('.'));
	if (dot < 0 || dot + 1 >= name.size()) {
		return QString();
	}
	const auto extension = name.mid(dot + 1);
	for (const auto ch : extension) {
		if (!ch.isLetterOrNumber() || ch.unicode() > 0x7F) {
			return QString();
		}
	}
	return (extension.size() <= 4) ? extension.toLower() : QString();
}

bool CanEdit(
		const Data::Message &message,
		qint64 nowMs,
		qint64 editWindowSeconds) {
	if (!message.mine
		|| message.seq <= 0
		|| message.forwarded
		|| message.viaBot
		|| message.selfDestruct
		|| message.status == Data::MessageStatus::Failed) {
		return false;
	}
	switch (message.kind) {
	case Data::MessageKind::Text:
	case Data::MessageKind::Photo:
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
		break;
	default:
		return false;
	}
	return (editWindowSeconds <= 0)
		|| (message.createdAt <= 0)
		|| (nowMs - message.createdAt <= editWindowSeconds * 1000);
}

bool HasCopyableText(const Data::Message &message) {
	if (message.selfDestruct) {
		// Its text is the server's «only on the phone»; its caption is never sent here.
		return false;
	}
	switch (message.kind) {
	case Data::MessageKind::Text:
	case Data::MessageKind::Unknown:
	case Data::MessageKind::Photo:
	case Data::MessageKind::Video:
	case Data::MessageKind::Gif:
	case Data::MessageKind::ForwardedPost:
	case Data::MessageKind::StoryReply:
		return !message.text.trimmed().isEmpty();
	default:
		return false;
	}
}

bool CanForward(const Data::Message &message) {
	if (message.seq <= 0 || message.selfDestruct) {
		return false;
	}
	switch (message.kind) {
	case Data::MessageKind::System:
	case Data::MessageKind::Call:
	case Data::MessageKind::LiveLocation:
	case Data::MessageKind::StoryReply:
	case Data::MessageKind::StoryReaction:
	case Data::MessageKind::Unknown:
		return false;
	default:
		return true;
	}
}

namespace {

[[nodiscard]] const Data::Message *ReplyKeyboardMessage(
		const QVector<Data::Message> &items) {
	for (auto i = items.crbegin(); i != items.crend(); ++i) {
		if (i->mine || i->seq <= 0 || i->deleted || !i->replyMarkup) {
			continue;
		}
		const auto &markup = *i->replyMarkup;
		if (markup.removeKeyboard || !markup.keyboard.isEmpty()) {
			return &*i;
		}
	}
	return nullptr;
}

} // namespace

std::optional<Data::ReplyMarkup> CurrentReplyKeyboard(
		const QVector<Data::Message> &items,
		qint64 after) {
	const auto message = ReplyKeyboardMessage(items);
	if (!message
		|| message->seq <= after
		|| message->replyMarkup->removeKeyboard
		|| message->replyMarkup->keyboard.isEmpty()) {
		return std::nullopt;
	}
	return message->replyMarkup;
}

qint64 ReplyKeyboardSeq(const QVector<Data::Message> &items) {
	const auto message = ReplyKeyboardMessage(items);
	return message ? message->seq : 0;
}

bool ConversationEnded(const QString &state) {
	return state == u"ended"
		|| state == u"expired"
		|| state == u"closed"
		|| state == u"purged";
}

qint64 DownFirstStop(
		const QVector<Data::Message> &items,
		int unread,
		qint64 lastReadSeq,
		qint64 bottomSeq) {
	if (unread <= 0) {
		return 0;
	}
	auto counted = 0;
	auto stop = qint64(0);
	for (auto i = int(items.size()) - 1; i >= 0; --i) {
		const auto &message = items[i];
		if (message.seq <= 0 || message.mine || !message.visible()) {
			continue;
		} else if (message.seq <= lastReadSeq) {
			break;
		}
		stop = message.seq;
		if (++counted == unread) {
			break;
		}
	}
	return (stop > bottomSeq) ? stop : 0;
}

bool FloatingButtonsAtPhysicalRight(bool group, bool rtl) {
	return group || !rtl;
}

qint64 NextMention(const QVector<qint64> &seqs, qint64 lastReadSeq) {
	for (const auto seq : seqs) {
		if (seq > lastReadSeq) {
			return seq;
		}
	}
	return 0;
}

bool SelfDestructAllowed(const Data::Chat &chat) {
	return !chat.isGroup
		&& !Data::IsGroupId(chat.id)
		&& !chat.isConversation
		&& !chat.info.conversation
		&& !chat.anonymous
		&& !chat.saved
		&& !chat.info.saved
		&& !chat.isBot
		&& !chat.peerDeleted
		&& !chat.peerUserId.isEmpty()
		&& (chat.info.kind.isEmpty() || chat.info.kind == u"direct");
}

SelfDestructLine SelfDestructLineFor(const Data::Message &message) {
	if (!message.selfDestruct || message.selfDestruct->expired()) {
		return SelfDestructLine::Expired;
	} else if (message.selfDestruct->opened()) {
		return SelfDestructLine::Opened;
	}
	return message.mine ? SelfDestructLine::NotOpened : SelfDestructLine::OnPhone;
}

} // namespace History
