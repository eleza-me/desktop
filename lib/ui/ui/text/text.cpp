#include "ui/text/text.h"

#include "ui/rtl.h"

#include <QtGui/QGlyphRun>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QTextLayout>
#include <QtGui/QTextOption>

#include <algorithm>
#include <cmath>
#include <climits>

namespace Ui::Text {
namespace {

constexpr auto kHugeWidth = 1'000'000;
constexpr auto kSpoilerRadius = 4.;
const auto kEllipsis = QChar(0x2026);

struct Block {
	int offset = 0;
	int length = 0;
	bool rtl = false;
	std::unique_ptr<QTextLayout> layout; // null for an empty paragraph
	int firstLine = 0; // index in `lines`
	int lineCount = 0;
};

struct LineInfo {
	int block = 0;
	int index = -1; // line inside the block's layout, -1 for an empty paragraph
	int top = 0;
	int x = 0;
	int width = 0;
	int from = 0; // absolute offset of the first character
	int length = 0;
};

// The first strong character decides, as the bidi algorithm does it (P2):
// what is inside an isolate (U+2066…U+2068 up to its U+2069) does not count,
// so «{name} left the group» with an isolated Persian name stays left to
// right in English.
[[nodiscard]] bool DetectRtl(QStringView text) {
	auto isolates = 0;
	for (const auto ch : text) {
		const auto code = ch.unicode();
		if (code >= 0x2066 && code <= 0x2068) {
			++isolates;
			continue;
		} else if (code == 0x2069) {
			isolates = std::max(isolates - 1, 0);
			continue;
		} else if (isolates > 0) {
			continue;
		}
		switch (ch.direction()) {
		case QChar::DirL: return false;
		case QChar::DirR:
		case QChar::DirAL: return true;
		default: break;
		}
	}
	return style::RightToLeft();
}

[[nodiscard]] bool IsWordChar(QChar ch) {
	return ch.isLetterOrNumber()
		|| ch == QLatin1Char('_')
		|| ch == QChar(0x200C) // zero width non-joiner, inside Persian words
		|| ch == QChar(0x200D)
		|| ch.isMark()
		|| ch.isSurrogate();
}

[[nodiscard]] QImage GenerateDust(double ratio) {
	const auto size = 48;
	auto result = QImage(
		QSize(size, size) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	// A fixed pseudo-random scatter, the same on every run.
	auto seed = quint32(0x9E3779B9U);
	const auto next = [&] {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		return seed;
	};
	for (auto i = 0; i != 150; ++i) {
		const auto x = (next() % (size * 16)) / 16.;
		const auto y = (next() % (size * 16)) / 16.;
		const auto radius = 0.45 + (next() % 60) / 100.;
		const auto alpha = 110 + int(next() % 146);
		p.setBrush(QColor(255, 255, 255, alpha));
		p.drawEllipse(QPointF(x, y), radius, radius);
	}
	return result;
}

void PaintSpoilerDust(QPainter &p, const QRectF &rect, const QColor &color) {
	static auto cache = QPixmap();
	static auto cacheColor = QRgb(0);
	static auto cacheRatio = 0.;
	const auto ratio = p.device()->devicePixelRatioF();
	if (cache.isNull() || cacheColor != color.rgba() || cacheRatio != ratio) {
		auto image = GenerateDust(ratio);
		{
			auto q = QPainter(&image);
			q.setCompositionMode(QPainter::CompositionMode_SourceIn);
			q.fillRect(QRect(0, 0, 48, 48), color);
		}
		cache = QPixmap::fromImage(std::move(image));
		cacheColor = color.rgba();
		cacheRatio = ratio;
	}
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	auto path = QPainterPath();
	path.addRoundedRect(rect, kSpoilerRadius, kSpoilerRadius);
	auto faded = color;
	faded.setAlphaF(color.alphaF() * 0.22f);
	p.fillPath(path, faded);
	p.setClipPath(path, Qt::IntersectClip);
	p.drawTiledPixmap(rect, cache, QPointF(rect.x(), rect.y()));
	p.restore();
}

} // namespace

struct String::Private {
	const style::TextStyle *st = nullptr;
	QString text;
	Entities entities;
	std::vector<Link> links;
	std::vector<int> linkEntity; // link index -> entity index
	std::vector<Block> blocks;
	int maxWidth = 0;
	int lineHeight = 0;
	bool hasSpoilers = false;
	bool spoilersRevealed = false;
	int paletteVersion = 0;

	int layoutWidth = -1;
	style::align layoutAlign = style::al_left;
	bool layoutAuto = false;
	std::vector<LineInfo> lines;

	struct Elided {
		int width = -1;
		int line = -1;
		style::align align = style::al_left;
		bool autoDirection = false;
		std::unique_ptr<QTextLayout> layout;
		int from = 0; // absolute offset of the first character
		int kept = 0; // source characters kept before the ellipsis
		int x = 0;
		int naturalWidth = 0;
	} elided;

	void rebuild();
	void applyFormats();
	[[nodiscard]] QList<QTextLayout::FormatRange> formatsFor(
		int from,
		int length) const;
	void ensureLayout(int width, style::align align, bool autoDirection);
	void ensureElided(
		int line,
		int width,
		style::align align,
		bool autoDirection);
	[[nodiscard]] int lineX(
		const Block &block,
		int natural,
		int width,
		style::align align,
		bool autoDirection) const;
	void positionLine(QTextLine &line, int x, int top) const;
	[[nodiscard]] std::vector<QRectF> rangeRects(
		int from,
		int till,
		int fullLines,
		bool withElided) const;
};

void String::Private::rebuild() {
	blocks.clear();
	lines.clear();
	layoutWidth = -1;
	elided = Elided();
	maxWidth = 0;
	lineHeight = st
		? std::max(st->lineHeight, st->font->height)
		: 0;
	if (!st || text.isEmpty()) {
		return;
	}
	auto from = 0;
	const auto size = int(text.size());
	while (from <= size) {
		auto till = int(text.indexOf(QLatin1Char('\n'), from));
		if (till < 0) {
			till = size;
		}
		auto block = Block();
		block.offset = from;
		block.length = till - from;
		const auto view = QStringView(text).mid(from, till - from);
		block.rtl = DetectRtl(view);
		if (block.length > 0) {
			block.layout = std::make_unique<QTextLayout>(
				view.toString(),
				st->font->f);
			auto option = QTextOption(Qt::AlignLeft | Qt::AlignAbsolute);
			option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
			option.setTextDirection(
				block.rtl ? Qt::RightToLeft : Qt::LeftToRight);
			block.layout->setTextOption(option);
			block.layout->setCacheEnabled(true);
		}
		blocks.push_back(std::move(block));
		from = till + 1;
	}
	applyFormats();
	for (auto &block : blocks) {
		if (!block.layout) {
			continue;
		}
		block.layout->beginLayout();
		auto line = block.layout->createLine();
		if (line.isValid()) {
			line.setLineWidth(kHugeWidth);
			maxWidth = std::max(
				maxWidth,
				int(std::ceil(line.naturalTextWidth())));
		}
		block.layout->endLayout();
	}
}

QList<QTextLayout::FormatRange> String::Private::formatsFor(
		int from,
		int length) const {
	auto result = QList<QTextLayout::FormatRange>();
	if (entities.isEmpty() || length <= 0) {
		return result;
	}
	const auto till = from + length;
	auto cuts = std::vector<int>{ from, till };
	for (const auto &entity : entities) {
		const auto start = entity.offset;
		const auto end = entity.offset + entity.length;
		if (start > from && start < till) {
			cuts.push_back(start);
		}
		if (end > from && end < till) {
			cuts.push_back(end);
		}
	}
	std::sort(cuts.begin(), cuts.end());
	cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
	for (auto i = size_t(0); i + 1 < cuts.size(); ++i) {
		const auto start = cuts[i];
		const auto end = cuts[i + 1];
		auto format = QTextCharFormat();
		auto changed = false;
		auto hidden = false;
		for (const auto &entity : entities) {
			if (entity.offset > start
				|| entity.offset + entity.length < end) {
				continue;
			}
			changed = true;
			switch (entity.type) {
			case EntityType::Bold:
				format.setFontWeight(QFont::DemiBold);
				break;
			case EntityType::Italic:
				format.setFontItalic(true);
				break;
			case EntityType::Underline:
				format.setFontUnderline(true);
				break;
			case EntityType::StrikeOut:
				format.setFontStrikeOut(true);
				break;
			case EntityType::Code:
			case EntityType::Pre:
				format.setFontFamilies({
					style::MonospaceFontFamily(),
					style::TextFontFamily(),
				});
				format.setForeground(st->monoFg->b);
				break;
			case EntityType::Spoiler:
				hidden = !spoilersRevealed;
				break;
			default:
				if (IsLinkType(entity.type)) {
					format.setForeground(st->linkFg->b);
					if (st->linkUnderline) {
						format.setFontUnderline(true);
					}
				}
				break;
			}
		}
		if (hidden) {
			format.setForeground(QBrush(Qt::transparent));
			format.setFontUnderline(false);
			format.setFontStrikeOut(false);
		}
		if (changed) {
			auto range = QTextLayout::FormatRange();
			range.start = start - from;
			range.length = end - start;
			range.format = format;
			result.push_back(range);
		}
	}
	return result;
}

void String::Private::applyFormats() {
	paletteVersion = style::PaletteVersion();
	for (auto &block : blocks) {
		if (block.layout) {
			block.layout->setFormats(formatsFor(block.offset, block.length));
		}
	}
	// Formats may change glyph widths (bold), layouts are stale now.
	layoutWidth = -1;
	elided.width = -1;
}

int String::Private::lineX(
		const Block &block,
		int natural,
		int width,
		style::align align,
		bool autoDirection) const {
	const auto startIsRight = autoDirection
		? block.rtl
		: style::RightToLeft();
	if (align & Qt::AlignHCenter) {
		return (width - natural) / 2;
	}
	const auto toRight = (align & Qt::AlignRight)
		? !startIsRight
		: startIsRight;
	return toRight ? (width - natural) : 0;
}

void String::Private::positionLine(QTextLine &line, int x, int top) const {
	// Keep the baseline where the style's font puts it, whatever fallback
	// fonts (emoji) do to the line metrics.
	const auto baseline = top
		+ (lineHeight - st->font->height) / 2.
		+ st->font->ascent;
	line.setPosition(QPointF(x, baseline - line.ascent()));
}

void String::Private::ensureLayout(
		int width,
		style::align align,
		bool autoDirection) {
	width = std::max(width, 1);
	if (paletteVersion != style::PaletteVersion()) {
		applyFormats();
	}
	const auto horizontal = align & Qt::AlignHorizontal_Mask;
	if (layoutWidth == width
		&& layoutAlign == horizontal
		&& layoutAuto == autoDirection) {
		return;
	}
	layoutWidth = width;
	layoutAlign = horizontal;
	layoutAuto = autoDirection;
	lines.clear();
	elided.width = -1;
	auto top = 0;
	auto blockIndex = 0;
	for (auto &block : blocks) {
		block.firstLine = int(lines.size());
		if (!block.layout) {
			auto info = LineInfo();
			info.block = blockIndex;
			info.index = -1;
			info.top = top;
			info.x = lineX(block, 0, width, horizontal, autoDirection);
			info.from = block.offset;
			lines.push_back(info);
			top += lineHeight;
		} else {
			block.layout->beginLayout();
			auto index = 0;
			while (true) {
				auto line = block.layout->createLine();
				if (!line.isValid()) {
					break;
				}
				line.setLineWidth(width);
				const auto natural = std::min(
					int(std::ceil(line.naturalTextWidth())),
					width);
				auto info = LineInfo();
				info.block = blockIndex;
				info.index = index++;
				info.top = top;
				info.width = natural;
				info.x = lineX(block, natural, width, horizontal, autoDirection);
				info.from = block.offset + line.textStart();
				info.length = line.textLength();
				positionLine(line, info.x, top);
				lines.push_back(info);
				top += lineHeight;
			}
			block.layout->endLayout();
		}
		block.lineCount = int(lines.size()) - block.firstLine;
		++blockIndex;
	}
}

void String::Private::ensureElided(
		int line,
		int width,
		style::align align,
		bool autoDirection) {
	const auto horizontal = align & Qt::AlignHorizontal_Mask;
	if (elided.width == width
		&& elided.line == line
		&& elided.align == horizontal
		&& elided.autoDirection == autoDirection
		&& elided.layout) {
		return;
	}
	const auto &info = lines[line];
	const auto &block = blocks[info.block];
	const auto blockEnd = block.offset + block.length;
	auto rest = text.mid(info.from, blockEnd - info.from);
	while (!rest.isEmpty() && rest.back().isSpace()) {
		rest.chop(1);
	}
	// A first guess by the plain font metrics, refined by real layout below.
	auto kept = int(rest.size());
	const auto guess = st->font->m.elidedText(
		rest + kEllipsis,
		Qt::ElideRight,
		width);
	if (guess.size() - 1 < kept) {
		kept = std::max(int(guess.size()) - 1, 0);
	}
	auto layout = std::unique_ptr<QTextLayout>();
	auto natural = 0;
	while (true) {
		if (kept > 0
			&& kept < rest.size()
			&& rest[kept].isLowSurrogate()
			&& rest[kept - 1].isHighSurrogate()) {
			--kept;
		}
		auto kept2 = kept;
		while (kept2 > 0 && rest[kept2 - 1].isSpace()) {
			--kept2;
		}
		const auto string = rest.left(kept2) + kEllipsis;
		layout = std::make_unique<QTextLayout>(string, st->font->f);
		auto option = QTextOption(Qt::AlignLeft | Qt::AlignAbsolute);
		option.setWrapMode(QTextOption::NoWrap);
		option.setTextDirection(
			block.rtl ? Qt::RightToLeft : Qt::LeftToRight);
		layout->setTextOption(option);
		auto formats = formatsFor(info.from, kept2);
		layout->setFormats(formats);
		layout->beginLayout();
		auto created = layout->createLine();
		if (created.isValid()) {
			created.setLineWidth(kHugeWidth);
			natural = int(std::ceil(created.naturalTextWidth()));
		}
		layout->endLayout();
		if (natural <= width || kept2 <= 0) {
			kept = kept2;
			break;
		}
		kept = kept2 - 1;
	}
	elided.width = width;
	elided.line = line;
	elided.align = horizontal;
	elided.autoDirection = autoDirection;
	elided.from = info.from;
	elided.kept = kept;
	elided.naturalWidth = std::min(natural, width);
	elided.x = lineX(block, elided.naturalWidth, width, horizontal, autoDirection);
	elided.layout = std::move(layout);
	if (elided.layout->lineCount() > 0) {
		auto created = elided.layout->lineAt(0);
		positionLine(created, elided.x, info.top);
	}
}

std::vector<QRectF> String::Private::rangeRects(
		int from,
		int till,
		int fullLines,
		bool withElided) const {
	auto result = std::vector<QRectF>();
	const auto addRuns = [&](const QTextLine &line, int start, int length, int top) {
		if (length <= 0) {
			return;
		}
		auto rects = std::vector<QRectF>();
		const auto runs = line.glyphRuns(start, length);
		for (const auto &run : runs) {
			const auto bounds = run.boundingRect();
			if (bounds.width() > 0.) {
				rects.push_back(QRectF(bounds.x(), top, bounds.width(), lineHeight));
			}
		}
		std::sort(rects.begin(), rects.end(), [](const QRectF &a, const QRectF &b) {
			return a.x() < b.x();
		});
		for (const auto &rect : rects) {
			if (!result.empty()
				&& result.back().y() == rect.y()
				&& rect.x() <= result.back().right() + 1.5) {
				result.back().setRight(std::max(result.back().right(), rect.right()));
			} else {
				result.push_back(rect);
			}
		}
	};
	for (auto i = 0; i < fullLines && i < int(lines.size()); ++i) {
		const auto &info = lines[i];
		if (info.index < 0) {
			continue;
		}
		const auto start = std::max(from, info.from);
		const auto end = std::min(till, info.from + info.length);
		if (start >= end) {
			continue;
		}
		const auto &block = blocks[info.block];
		addRuns(
			block.layout->lineAt(info.index),
			start - block.offset,
			end - start,
			info.top);
	}
	if (withElided && elided.layout && elided.layout->lineCount() > 0) {
		const auto start = std::max(from, elided.from);
		const auto end = std::min(till, elided.from + elided.kept);
		if (start < end) {
			addRuns(
				elided.layout->lineAt(0),
				start - elided.from,
				end - start,
				lines[elided.line].top);
		}
	}
	return result;
}

String::String() : _d(std::make_unique<Private>()) {
}

String::String(const style::TextStyle &st, const QString &text) : String() {
	setText(st, text);
}

String::String(const style::TextStyle &st, const TextWithEntities &text)
: String() {
	setMarkedText(st, text);
}

String::String(String &&other) noexcept : _d(std::move(other._d)) {
	other._d = std::make_unique<Private>();
}

String &String::operator=(String &&other) noexcept {
	if (this != &other) {
		_d = std::move(other._d);
		other._d = std::make_unique<Private>();
	}
	return *this;
}

String::~String() = default;

void String::setText(const style::TextStyle &st, const QString &text) {
	setMarkedText(st, TextWithEntities{ text, {} });
}

void String::setMarkedText(
		const style::TextStyle &st,
		const TextWithEntities &text) {
	_d->st = &st;
	_d->text = text.text;
	_d->entities = NormalizeEntities(text.text, text.entities);
	_d->links.clear();
	_d->linkEntity.clear();
	_d->hasSpoilers = false;
	_d->spoilersRevealed = false;
	auto index = 0;
	for (const auto &entity : _d->entities) {
		if (entity.type == EntityType::Spoiler) {
			_d->hasSpoilers = true;
		} else if (IsLinkType(entity.type)) {
			auto link = Link();
			link.type = entity.type;
			link.text = _d->text.mid(entity.offset, entity.length);
			link.data = entity.data.isEmpty() ? link.text : entity.data;
			link.offset = entity.offset;
			link.length = entity.length;
			_d->links.push_back(std::move(link));
			_d->linkEntity.push_back(index);
		}
		++index;
	}
	_d->rebuild();
}

void String::clear() {
	_d = std::make_unique<Private>();
}

bool String::isEmpty() const {
	return _d->text.isEmpty();
}

int String::length() const {
	return int(_d->text.size());
}

const QString &String::toString() const {
	return _d->text;
}

QString String::toString(Selection selection) const {
	const auto from = std::clamp(selection.from, 0, length());
	const auto till = std::clamp(selection.to, from, length());
	return _d->text.mid(from, till - from);
}

TextWithEntities String::toTextWithEntities() const {
	return TextWithEntities{ _d->text, _d->entities };
}

int String::maxWidth() const {
	return _d->maxWidth;
}

int String::minHeight() const {
	return _d->lineHeight;
}

int String::countLines(int width) const {
	if (isEmpty()) {
		return 0;
	}
	_d->ensureLayout(width, _d->layoutAlign, _d->layoutAuto);
	return int(_d->lines.size());
}

int String::countHeight(int width, int elisionLines) const {
	const auto lines = countLines(width);
	const auto shown = (elisionLines > 0) ? std::min(lines, elisionLines) : lines;
	return shown * _d->lineHeight;
}

int String::countMaxLineWidth(int width, int elisionLines) const {
	const auto lines = countLines(width);
	if (elisionLines > 0 && lines > elisionLines) {
		return std::max(width, 1);
	}
	auto result = 0;
	for (const auto &info : _d->lines) {
		result = std::max(result, info.width);
	}
	return result;
}

String::LastLine String::countLastLine(int width) const {
	auto result = LastLine();
	if (!countLines(width)) {
		result.rtl = style::RightToLeft();
		return result;
	}
	const auto &info = _d->lines.back();
	result.width = info.width;
	result.rtl = _d->blocks[info.block].rtl;
	return result;
}

bool String::hasLinks() const {
	return !_d->links.empty();
}

int String::linksCount() const {
	return int(_d->links.size());
}

const Link &String::link(int index) const {
	return _d->links[index];
}

bool String::hasSpoilers() const {
	return _d->hasSpoilers;
}

bool String::spoilersRevealed() const {
	return _d->spoilersRevealed || !_d->hasSpoilers;
}

void String::setSpoilersRevealed(bool revealed) {
	if (_d->spoilersRevealed != revealed) {
		_d->spoilersRevealed = revealed;
		_d->applyFormats();
	}
}

void String::draw(QPainter &p, const PaintContext &context) const {
	if (isEmpty() || context.availableWidth <= 0) {
		return;
	}
	const auto d = _d.get();
	const auto width = context.availableWidth;
	d->ensureLayout(width, context.align, context.autoDirection);
	const auto total = int(d->lines.size());
	const auto limit = (context.elisionLines > 0)
		? std::min(total, context.elisionLines)
		: total;
	const auto useElided = (context.elisionLines > 0)
		&& (total > context.elisionLines);
	const auto fullLines = useElided ? (limit - 1) : limit;
	if (useElided) {
		d->ensureElided(limit - 1, width, context.align, context.autoDirection);
	}
	const auto origin = QPointF(context.position);
	const auto selectionFormat = [&] {
		auto format = QTextCharFormat();
		format.setBackground(d->st->selectBg->b);
		return format;
	};
	const auto selectionsFor = [&](int from, int length) {
		auto result = QList<QTextLayout::FormatRange>();
		if (context.selection.empty()) {
			return result;
		}
		const auto start = std::max(context.selection.from, from);
		const auto end = std::min(context.selection.to, from + length);
		if (start < end) {
			auto range = QTextLayout::FormatRange();
			range.start = start - from;
			range.length = end - start;
			range.format = selectionFormat();
			result.push_back(range);
		}
		return result;
	};
	const auto clipTop = context.clip.isNull()
		? INT_MIN
		: (context.clip.top() - context.position.y());
	const auto clipBottom = context.clip.isNull()
		? INT_MAX
		: (context.clip.top() + context.clip.height() - context.position.y());

	for (const auto &block : d->blocks) {
		if (!block.layout || block.firstLine >= fullLines) {
			continue;
		}
		const auto last = std::min(
			block.firstLine + block.lineCount,
			fullLines) - 1;
		const auto top = d->lines[block.firstLine].top;
		const auto bottom = d->lines[last].top + d->lineHeight;
		if (bottom <= clipTop || top >= clipBottom) {
			continue;
		}
		const auto selections = selectionsFor(block.offset, block.length);
		const auto whole = (block.firstLine + block.lineCount <= fullLines);
		if (whole) {
			block.layout->draw(&p, origin, selections);
			continue;
		}
		// Only a part of the paragraph fits the lines limit. QTextLayout::draw
		// skips lines that start below the clip rect or end above it, so the
		// clip describes exactly the lines we want. With selections it also
		// clips painting, then it has to cover those lines completely.
		const auto firstY = block.layout->lineAt(0).position().y();
		const auto lastY = block.layout->lineAt(
			last - block.firstLine).position().y();
		const auto clip = selections.isEmpty()
			? QRectF(
				origin.x(),
				origin.y() + firstY - 0.25,
				width,
				(lastY - firstY) + 0.5)
			: QRectF(
				origin.x(),
				origin.y() + top,
				width,
				(bottom - top) - 0.5);
		block.layout->draw(&p, origin, selections, clip);
	}
	if (useElided && d->elided.layout) {
		const auto top = d->lines[limit - 1].top;
		if (top + d->lineHeight > clipTop && top < clipBottom) {
			d->elided.layout->draw(
				&p,
				origin,
				selectionsFor(d->elided.from, d->elided.kept));
		}
	}

	if (d->hasSpoilers && !d->spoilersRevealed) {
		for (const auto &entity : d->entities) {
			if (entity.type != EntityType::Spoiler) {
				continue;
			}
			const auto rects = d->rangeRects(
				entity.offset,
				entity.offset + entity.length,
				fullLines,
				useElided);
			for (const auto &rect : rects) {
				PaintSpoilerDust(
					p,
					rect.translated(origin).adjusted(-1., 1., 1., -1.),
					d->st->spoilerFg->c);
			}
		}
	}
	if (context.hoveredLink >= 0
		&& context.hoveredLink < int(d->links.size())
		&& !d->st->linkUnderline) {
		const auto &link = d->links[context.hoveredLink];
		const auto rects = d->rangeRects(
			link.offset,
			link.offset + link.length,
			fullLines,
			useElided);
		const auto baseline = (d->lineHeight - d->st->font->height) / 2.
			+ d->st->font->ascent;
		const auto thickness = std::max(1., d->st->font->f.pixelSize() / 14.);
		for (const auto &rect : rects) {
			p.fillRect(
				QRectF(
					origin.x() + rect.x(),
					origin.y() + rect.y() + baseline + 1.5,
					rect.width(),
					thickness),
				d->st->linkFg->c);
		}
	}
}

void String::draw(
		QPainter &p,
		int left,
		int top,
		int width,
		style::align align) const {
	draw(p, PaintContext{
		.position = QPoint(left, top),
		.availableWidth = width,
		.align = align,
	});
}

void String::drawElided(
		QPainter &p,
		int left,
		int top,
		int width,
		int lines,
		style::align align) const {
	draw(p, PaintContext{
		.position = QPoint(left, top),
		.availableWidth = width,
		.align = align,
		.elisionLines = lines,
	});
}

void String::drawLeft(
		QPainter &p,
		int x,
		int y,
		int width,
		int outerw,
		style::align align) const {
	draw(
		p,
		style::RightToLeft() ? (outerw - x - width) : x,
		y,
		width,
		align);
}

void String::drawLeftElided(
		QPainter &p,
		int x,
		int y,
		int width,
		int outerw,
		int lines,
		style::align align) const {
	drawElided(
		p,
		style::RightToLeft() ? (outerw - x - width) : x,
		y,
		width,
		lines,
		align);
}

StateResult String::getState(
		QPoint point,
		const PaintContext &geometry) const {
	auto result = StateResult();
	if (isEmpty() || geometry.availableWidth <= 0) {
		return result;
	}
	const auto d = _d.get();
	const auto width = geometry.availableWidth;
	d->ensureLayout(width, geometry.align, geometry.autoDirection);
	const auto total = int(d->lines.size());
	const auto limit = (geometry.elisionLines > 0)
		? std::min(total, geometry.elisionLines)
		: total;
	const auto useElided = (geometry.elisionLines > 0)
		&& (total > geometry.elisionLines);
	const auto local = point - geometry.position;
	if (local.y() < 0) {
		result.cursor = 0;
		result.symbol = 0;
		return result;
	}
	const auto lineIndex = d->lineHeight ? (local.y() / d->lineHeight) : 0;
	if (lineIndex >= limit) {
		result.cursor = length();
		result.symbol = std::max(length() - 1, 0);
		return result;
	}
	const auto &info = d->lines[lineIndex];
	auto lineLeft = info.x;
	auto lineWidth = info.width;
	auto symbolLimit = info.from + std::max(info.length - 1, 0);
	if (useElided && lineIndex == limit - 1) {
		d->ensureElided(lineIndex, width, geometry.align, geometry.autoDirection);
		if (d->elided.layout && d->elided.layout->lineCount() > 0) {
			const auto line = d->elided.layout->lineAt(0);
			lineLeft = d->elided.x;
			lineWidth = d->elided.naturalWidth;
			const auto onChar = line.xToCursor(
				local.x(),
				QTextLine::CursorOnCharacter);
			const auto between = line.xToCursor(
				local.x(),
				QTextLine::CursorBetweenCharacters);
			result.symbol = d->elided.from + std::min(
				onChar,
				std::max(d->elided.kept - 1, 0));
			result.cursor = d->elided.from + std::min(between, d->elided.kept);
			symbolLimit = d->elided.from + std::max(d->elided.kept - 1, 0);
		}
	} else if (info.index < 0) {
		result.symbol = result.cursor = info.from;
		return result;
	} else {
		const auto &block = d->blocks[info.block];
		const auto line = block.layout->lineAt(info.index);
		result.symbol = block.offset + line.xToCursor(
			local.x(),
			QTextLine::CursorOnCharacter);
		result.cursor = block.offset + line.xToCursor(
			local.x(),
			QTextLine::CursorBetweenCharacters);
	}
	result.symbol = std::clamp(result.symbol, info.from, symbolLimit);
	result.uponSymbol = (local.x() >= lineLeft)
		&& (local.x() < lineLeft + lineWidth);
	if (result.uponSymbol) {
		const auto symbol = result.symbol;
		if (d->hasSpoilers && !d->spoilersRevealed) {
			for (const auto &entity : d->entities) {
				if (entity.type == EntityType::Spoiler
					&& entity.offset <= symbol
					&& symbol < entity.offset + entity.length) {
					result.spoiler = true;
					break;
				}
			}
		}
		if (!result.spoiler) {
			for (auto i = 0; i != int(d->links.size()); ++i) {
				const auto &link = d->links[i];
				if (link.offset <= symbol && symbol < link.offset + link.length) {
					result.link = i;
					break;
				}
			}
		}
	}
	return result;
}

Selection String::adjustSelection(Selection selection, SelectType type) const {
	const auto size = length();
	auto from = std::clamp(selection.from, 0, size);
	auto till = std::clamp(selection.to, from, size);
	const auto &text = _d->text;
	if (type == SelectType::Words) {
		while (from > 0 && IsWordChar(text[from - 1])) {
			--from;
		}
		while (till < size && IsWordChar(text[till])) {
			++till;
		}
	} else if (type == SelectType::Paragraphs) {
		while (from > 0 && text[from - 1] != QLatin1Char('\n')) {
			--from;
		}
		while (till < size && text[till] != QLatin1Char('\n')) {
			++till;
		}
	} else {
		if (from > 0 && from < size && text[from].isLowSurrogate()) {
			--from;
		}
		if (till > 0 && till < size && text[till].isLowSurrogate()) {
			++till;
		}
	}
	return Selection{ from, till };
}

bool HasVisibleText(const QString &text) {
	for (const auto ch : text) {
		if (!ch.isSpace()) {
			return true;
		}
	}
	return false;
}

} // namespace Ui::Text
