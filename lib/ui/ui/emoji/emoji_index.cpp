#include "ui/emoji/emoji_index.h"

#include "ui/emoji/emoji_tables.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

#include <array>

namespace Ui::Emoji {
namespace {

constexpr auto kHandshake = 0x10000;
constexpr auto kZwj = QChar(0x200D);
constexpr auto kVs16 = QChar(0xFE0F);
constexpr auto kVs15 = QChar(0xFE0E);
constexpr auto kKeycap = QChar(0x20E3);
constexpr auto kSymbolsEnd = 0x3300;

// 🤝, 🫱, 🫲, ZWJ ➡.
const auto kHandshakeCode = QStringView(u"\U0001F91D");
const auto kRightHand = QStringView(u"\U0001FAF1");
const auto kLeftHand = QStringView(u"\U0001FAF2");
const auto kRightArrowSuffix = QStringView(u"‍➡");

// The emoji on the panel's category tabs.
const char16_t *const kTabEmoji[kPanelCategoryCount] = {
	u"\U0001F600", // 😀
	u"\U0001F43B", // 🐻
	u"\U0001F354", // 🍔
	u"⚽", // ⚽
	u"\U0001F697", // 🚗
	u"\U0001F4A1", // 💡
	u"\U0001F523", // 🔣
	u"\U0001F3F3", // 🏳
};

[[nodiscard]] QStringList Split(const Tables::List &list) {
	auto result = QStringList();
	result.reserve(list.count);
	for (auto i = 0; i != list.chunkCount; ++i) {
		const auto chunk = QStringView(list.chunks[i]);
		for (const auto part : chunk.tokenize(u' ', Qt::SkipEmptyParts)) {
			result.push_back(part.toString());
		}
	}
	return result;
}

struct Data {
	Data();

	std::array<QStringList, Tables::kPageCount> pages;
	std::array<QStringList, Tables::kPageCount> panel;
	QHash<QString, Id> ids;
	QHash<QString, QString> aliases;
	QSet<QString> toned;
	std::array<bool, kSymbolsEnd> symbol = {};
	QStringView toFe0f;
	QStringView dataChars;
};

Data::Data()
: toFe0f(static_cast<const char16_t*>(Tables::kToFe0f))
, dataChars(static_cast<const char16_t*>(Tables::kDataChars)) {
	auto total = 0;
	for (auto p = 0; p != Tables::kPageCount; ++p) {
		pages[p] = Split(Tables::kPages[p]);
		panel[p] = Split(Tables::kPanel[p]);
		total += int(pages[p].size());
	}
	ids.reserve(total);
	for (auto p = 0; p != Tables::kPageCount; ++p) {
		for (auto s = 0; s != int(pages[p].size()); ++s) {
			// (Keyed without variation selectors: a few entries carry U+FE0F.)
			const auto key = Strip(pages[p][s]);
			if (!ids.contains(key)) {
				ids.insert(key, (p << 12) | s);
			}
		}
	}
	const auto from = Split(Tables::kAliasOld);
	const auto to = Split(Tables::kAliasNew);
	for (auto i = 0; i < from.size() && i < to.size(); ++i) {
		aliases.insert(Strip(from[i]), Strip(to[i]));
	}
	for (const auto &code : Split(Tables::kColored)) {
		toned.insert(Strip(code));
	}
	for (const auto ch : dataChars) {
		if (ch.unicode() < kSymbolsEnd) {
			symbol[ch.unicode()] = true;
		}
	}
}

[[nodiscard]] const Data &Instance() {
	static const auto result = Data();
	return result;
}

[[nodiscard]] Id MakeHandshake(int left, int right) {
	return kHandshake | ((left + 1) << 4) | (right + 1);
}

// The skin tone at `at` of `s` (0…4), or -1.
[[nodiscard]] int ToneAt(QStringView s, qsizetype at) {
	if (at + 1 >= s.size() || s[at].unicode() != 0xD83C) {
		return -1;
	}
	const auto t = int(s[at + 1].unicode()) - 0xDFFB;
	return (t >= 0 && t <= 4) ? t : -1;
}

[[nodiscard]] char32_t CodePointAt(QStringView s, qsizetype i) {
	const auto c = s[i];
	if (c.isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate()) {
		return QChar::surrogateToUcs4(c, s[i + 1]);
	}
	return c.unicode();
}

[[nodiscard]] int CharCount(char32_t code) {
	return (code >= 0x10000) ? 2 : 1;
}

[[nodiscard]] bool IsKeycapBase(char32_t code) {
	return (code >= U'0' && code <= U'9') || code == U'#' || code == U'*';
}

[[nodiscard]] bool IsRegional(char32_t code) {
	return code >= 0x1F1E6 && code <= 0x1F1FF;
}

[[nodiscard]] bool IsToneCode(char32_t code) {
	return code >= 0x1F3FB && code <= 0x1F3FF;
}

// Can an emoji start here? Pictographs; a symbol that is an emoji alone or is
// followed by U+FE0F / a skin tone.
[[nodiscard]] bool StartsEmoji(
		const Data &data,
		QStringView s,
		qsizetype i,
		qsizetype till,
		char32_t code) {
	if (code >= 0x1F000 && code <= 0x1FAFF) {
		return true;
	} else if (code >= kSymbolsEnd || (code > 0x00AE && code < 0x203C)) {
		return false;
	} else if (data.symbol[code]) {
		return true;
	}
	const auto next = i + 1;
	if (next >= till) {
		return false;
	}
	const auto n = s[next];
	return (n == kVs16)
		|| (n.unicode() == 0xD83C
			&& next + 1 < till
			&& IsToneCode(CodePointAt(s, next)));
}

// May this follow a ZWJ inside an emoji?
[[nodiscard]] bool Joins(char32_t code) {
	return (code >= 0x1F000 && code <= 0x1FAFF)
		|| (code >= 0x2190 && code <= 0x2BFF)
		|| code == 0x00A9
		|| code == 0x00AE;
}

// End of the element at `i`: the pictograph, tag letters, a variation
// selector, a skin tone.
[[nodiscard]] qsizetype ElementEnd(QStringView s, qsizetype i, qsizetype till) {
	const auto code = CodePointAt(s, i);
	auto j = i + CharCount(code);
	if (code == 0x1F3F4) {
		// 🏴 + tag letters (E0020…E007E) + cancel tag (E007F): England,
		// Scotland, Wales.
		auto k = j;
		while (k + 1 < till) {
			const auto tag = CodePointAt(s, k);
			if (tag < 0xE0020 || tag > 0xE007F) {
				break;
			}
			k += 2;
			if (tag == 0xE007F) {
				j = k;
				break;
			}
		}
	}
	if (j < till && (s[j] == kVs16 || s[j] == kVs15)) {
		++j;
	}
	if (j + 1 < till && IsToneCode(CodePointAt(s, j))) {
		j += 2;
		if (j < till && s[j] == kVs16) {
			++j;
		}
	}
	return j;
}

[[nodiscard]] qsizetype SequenceEnd(QStringView s, qsizetype i, qsizetype till) {
	auto j = ElementEnd(s, i, till);
	while (j + 1 < till && s[j] == kZwj) {
		if (!Joins(CodePointAt(s, j + 1))) {
			break;
		}
		j = ElementEnd(s, j + 1, till);
	}
	return j;
}

[[nodiscard]] Id HandshakeOf(const QString &key) {
	const auto view = QStringView(key);
	if (view.startsWith(kHandshakeCode)) {
		if (view.size() == 4) {
			const auto tone = ToneAt(view, 2);
			if (tone >= 0) {
				return MakeHandshake(tone, tone);
			}
		}
		return kNone;
	}
	const auto zwj = view.indexOf(kZwj);
	if (zwj < 0
		|| !view.startsWith(kRightHand)
		|| !view.mid(zwj + 1).startsWith(kLeftHand)) {
		return kNone;
	}
	const auto a = view.left(zwj);
	const auto b = view.mid(zwj + 1);
	if ((a.size() != 2 && a.size() != 4) || (b.size() != 2 && b.size() != 4)) {
		return kNone;
	}
	const auto ta = (a.size() == 4) ? ToneAt(a, 2) : -1;
	const auto tb = (b.size() == 4) ? ToneAt(b, 2) : -1;
	if ((a.size() == 4 && ta < 0) || (b.size() == 4 && tb < 0)) {
		return kNone;
	}
	return MakeHandshake(ta, tb);
}

[[nodiscard]] Id Lookup(const Data &data, const QString &key) {
	const auto i = data.ids.constFind(key);
	if (i != data.ids.cend()) {
		return *i;
	}
	const auto alias = data.aliases.constFind(key);
	if (alias != data.aliases.cend()) {
		const auto j = data.ids.constFind(*alias);
		if (j != data.ids.cend()) {
			return *j;
		}
	}
	return HandshakeOf(key);
}

// The candidate [from, till): the whole of it, or the parts the pack knows.
void Resolve(
		QStringView s,
		qsizetype start,
		qsizetype end,
		std::vector<Range> &out,
		int limit) {
	auto id = Find(s.mid(start, end - start));
	if (id != kNone) {
		out.push_back({ int(start), int(end), id });
		return;
	}
	// The elements of a ZWJ chain: the longest leading run the pack knows,
	// then the rest.
	auto ends = std::vector<qsizetype>();
	for (auto k = start; k < end;) {
		const auto code = CodePointAt(s, k);
		if (code == kZwj.unicode()) {
			ends.push_back(k);
		}
		k += CharCount(code);
	}
	ends.push_back(end);
	auto from = start;
	auto first = size_t(0);
	while (first < ends.size() && int(out.size()) < limit) {
		auto found = false;
		for (auto last = int(ends.size()) - 1; last >= int(first); --last) {
			const auto to = ends[last];
			if (to <= from) {
				continue;
			}
			id = Find(s.mid(from, to - from));
			if (id != kNone) {
				out.push_back({ int(from), int(to), id });
				first = size_t(last) + 1;
				from = to + 1; // Past the ZWJ.
				found = true;
				break;
			}
		}
		if (!found) {
			// One element the pack lacks with its tone: at least the person
			// without it.
			const auto to = ends[first];
			auto baseEnd = from + CharCount(CodePointAt(s, from));
			if (baseEnd < to && s[baseEnd] == kVs16) {
				++baseEnd;
			}
			if (baseEnd < to) {
				id = Find(s.mid(from, baseEnd - from));
				if (id != kNone) {
					out.push_back({ int(from), int(baseEnd), id });
				}
			}
			++first;
			from = to + 1;
		}
		if (from >= end) {
			break;
		}
	}
}

// Supplementary emoji that are text by default: written with U+FE0F (as other
// apps send them).
[[nodiscard]] bool TextDefaultSupplementary(char32_t code) {
	switch (code) {
	case 0x1F004: case 0x1F17F: case 0x1F21A: case 0x1F22F:
	case 0x1F3F3: case 0x1F441: case 0x1F5E8: case 0x1F3CC: case 0x1F3CB:
	case 0x1F575:
		return true;
	}
	return false;
}

} // namespace

bool IsHandshake(Id id) {
	return (id >= 0) && (id & kHandshake);
}

int HandshakeTone(Id id, bool right) {
	return ((right ? id : (id >> 4)) & 0xF) - 1;
}

Id Find(QStringView code) {
	if (code.isEmpty()) {
		return kNone;
	}
	const auto &data = Instance();
	auto key = Strip(code);
	auto flags = 0;
	if (key.size() > kRightArrowSuffix.size()
		&& QStringView(key).endsWith(kRightArrowSuffix)) {
		const auto base = key.left(key.size() - kRightArrowSuffix.size());
		if (Lookup(data, base) != kNone) {
			key = base;
			flags = kMirrored;
		}
	}
	const auto id = Lookup(data, key);
	return (id == kNone) ? kNone : (id | flags);
}

QString Code(Id id) {
	if (id < 0 || IsHandshake(id)) {
		return QString();
	}
	const auto &data = Instance();
	const auto page = Page(id);
	const auto slot = Slot(id);
	return (page < Tables::kPageCount && slot < data.pages[page].size())
		? data.pages[page][slot]
		: QString();
}

int PageSize(int page) {
	return (page >= 0 && page < Tables::kPageCount)
		? int(Instance().pages[page].size())
		: 0;
}

bool MayHaveEmoji(QStringView text) {
	for (const auto ch : text) {
		const auto code = ch.unicode();
		if (code >= 0x203C || IsKeycapBase(code) || code == 0xA9 || code == 0xAE) {
			return true;
		}
	}
	return false;
}

std::vector<Range> FindAll(QStringView s, int limit) {
	auto out = std::vector<Range>();
	if (s.isEmpty() || limit <= 0 || !MayHaveEmoji(s)) {
		return out;
	}
	const auto &data = Instance();
	const auto till = s.size();
	auto i = qsizetype(0);
	while (i < till && int(out.size()) < limit) {
		const auto c = s[i].unicode();
		// The fast path: plain letters, digits not before a keycap, spaces.
		if (c < 0x00A9 && !IsKeycapBase(c)) {
			++i;
			continue;
		}
		const auto code = CodePointAt(s, i);
		const auto length = CharCount(code);
		auto end = qsizetype();
		if (IsKeycapBase(code)) {
			auto j = i + 1;
			if (j < till && s[j] == kVs16) {
				++j;
			}
			if (j >= till || s[j] != kKeycap) {
				++i;
				continue;
			}
			end = j + 1;
		} else if (IsRegional(code)) {
			const auto j = i + length;
			if (j + 1 < till && IsRegional(CodePointAt(s, j))) {
				end = j + 2;
			} else {
				i += length;
				continue;
			}
		} else if (StartsEmoji(data, s, i, till, code)) {
			end = SequenceEnd(s, i, till);
			// A lone symbol asked to be text (U+FE0E) is left alone.
			if (code < 0x10000 && end == i + 2 && s[i + 1] == kVs15) {
				i = end;
				continue;
			}
		} else {
			i += length;
			continue;
		}
		Resolve(s, i, end, out, limit);
		i = end;
	}
	if (int(out.size()) > limit) {
		out.resize(limit);
	}
	return out;
}

QString Strip(QStringView code) {
	auto result = QString();
	result.reserve(code.size());
	for (const auto ch : code) {
		if (ch != kVs16 && ch != kVs15) {
			result.push_back(ch);
		}
	}
	return result;
}

QString Tone(int tone) {
	if (tone < 0 || tone >= kToneCount) {
		return QString();
	}
	const auto code = char32_t(0x1F3FB + tone);
	const QChar pair[] = {
		QChar(QChar::highSurrogate(code)),
		QChar(QChar::lowSurrogate(code)),
	};
	return QString(pair, 2);
}

bool TakesTone(QStringView base) {
	const auto key = Strip(base);
	return Instance().toned.contains(key) || (key == kHandshakeCode);
}

QString WithTone(const QString &base, int tone) {
	if (tone < 0 || tone >= kToneCount) {
		return base;
	}
	auto code = Strip(base);
	auto tail = QString();
	if (code.size() > kRightArrowSuffix.size()
		&& QStringView(code).endsWith(kRightArrowSuffix)) {
		tail = kRightArrowSuffix.toString();
		code.chop(kRightArrowSuffix.size());
	}
	const auto n = code.size();
	auto end = QString();
	if (n > 2 && code[n - 2] == kZwj) {
		end = code.mid(n - 2);
		code.chop(2);
	} else if (n > 3 && code[n - 3] == kZwj) {
		end = code.mid(n - 3);
		code.chop(3);
	}
	return code + Tone(tone) + end + tail;
}

QString Qualify(QStringView code) {
	const auto &data = Instance();
	const auto s = Strip(code);
	const auto view = QStringView(s);
	auto result = QString();
	result.reserve(s.size() + 4);
	const auto n = s.size();
	for (auto i = qsizetype(0); i < n;) {
		const auto cp = CodePointAt(view, i);
		const auto next = i + CharCount(cp);
		result.append(view.mid(i, next - i));
		const auto followedByTone = ToneAt(view, next) >= 0;
		auto add = false;
		if (cp < 0x10000) {
			const auto c = QChar(char16_t(cp));
			if (IsKeycapBase(cp)) {
				add = (next < n) && (s[next] == kKeycap);
			} else if (cp >= 0x00A9 && cp <= 0x3299 && c != kZwj && c != kKeycap) {
				const auto inZwj = (i > 0 && s[i - 1] == kZwj)
					|| (next < n && s[next] == kZwj);
				add = data.toFe0f.contains(c)
					|| inZwj
					|| (n == 1 && data.dataChars.contains(c));
			}
		} else {
			add = TextDefaultSupplementary(cp);
		}
		if (add && !followedByTone) {
			result.push_back(kVs16);
		}
		i = next;
	}
	return result;
}

QHash<QString, QStringList> SearchWords() {
	auto result = QHash<QString, QStringList>();
	result.reserve(Tables::kKeywords.count);
	for (auto i = 0; i != Tables::kKeywords.chunkCount; ++i) {
		const auto chunk = QStringView(Tables::kKeywords.chunks[i]);
		for (const auto line : chunk.tokenize(u'\n', Qt::SkipEmptyParts)) {
			const auto tab = line.indexOf(u'\t');
			if (tab <= 0) {
				continue;
			}
			auto phrases = QStringList();
			for (const auto phrase : line.mid(tab + 1).tokenize(u'|', Qt::SkipEmptyParts)) {
				phrases.push_back(phrase.toString());
			}
			result.insert(Strip(line.left(tab)), std::move(phrases));
		}
	}
	return result;
}

const QStringList &PanelCategory(int index) {
	static const auto empty = QStringList();
	return (index >= 0 && index < kPanelCategoryCount)
		? Instance().panel[index]
		: empty;
}

QString PanelCategoryIcon(int index) {
	return (index >= 0 && index < kPanelCategoryCount)
		? Qualify(QStringView(kTabEmoji[index]))
		: QString();
}

} // namespace Ui::Emoji
