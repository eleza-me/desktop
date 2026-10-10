// Ui::Text: entities with any offsets (negative, past the end, overlapping,
// inside a surrogate pair) over any UTF-16 text, through the same steps a
// message takes: NormalizeEntities -> ParseEntities -> IsolateLinks -> layout
// -> paint -> hit test -> selection mapping.
#include "fuzz_driver.h"

#include "ui/style/style_core.h"
#include "ui/text/text.h"
#include "ui/text/text_entity.h"
#include "styles/style_basic.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>

using namespace Qt::Literals::StringLiterals;

namespace {

using namespace Ui::Text;

void Start() {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;
	qputenv("QT_QPA_PLATFORM", "offscreen");
	static auto argc = 1;
	static char name[] = "fuzz_text";
	static char *argv[] = { name, nullptr };
	static auto application = new QGuiApplication(argc, argv);
	Q_UNUSED(application);
	style::Start(style::Scheme::Light);
	style::SetRightToLeft(false);
}

[[nodiscard]] QString MakeText(Fuzz::Input &input) {
	// Direction marks are written as numbers: no such character in the source.
	static const auto kPieces = QStringList{
		u"@sara "_s, u"#tag "_s, u"a.io "_s, u"https://"_s, u"www."_s, u"x@y.co "_s,
		u"\n"_s, u"-a"_s, u"."_s, QString::fromUtf8("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 "),
		QString(QChar(0x202E)), QString(QChar(0x2066)), QString(QChar(0x2069)),
		QString(QChar(0x200C)), QString::fromUcs4(U"\U0001F600", 1),
		QString(QChar(0xD83D)), QString(QChar(0xDE00)), u" "_s, u"\t"_s,
		QString(QChar(0x2028)), u"e"_s + QChar(0x0301),
	};
	auto result = QString();
	const auto count = input.below(400);
	for (auto i = uint32_t(0); i != count && !input.empty(); ++i) {
		const auto pick = input.byte();
		if (pick < 200) {
			result += kPieces[pick % kPieces.size()];
		} else {
			result += QChar(char16_t((input.byte() << 8) | input.byte()));
		}
	}
	return result;
}

[[nodiscard]] Entities MakeEntities(Fuzz::Input &input, int size) {
	static const int kOffsets[] = {
		0, 1, -1, 2, 3, 7, 0x7FFFFFFF, -0x7FFFFFFF - 1, 0x7FFFFFF0, 65535, 65536, -65536,
	};
	auto result = Entities();
	const auto count = input.below(40);
	for (auto i = uint32_t(0); i != count && !input.empty(); ++i) {
		auto entity = Entity();
		entity.type = EntityType(input.byte() % 17);
		const auto pick = [&] {
			const auto how = input.byte();
			return (how < 64)
				? kOffsets[how % (sizeof(kOffsets) / sizeof(kOffsets[0]))]
				: (how < 200)
				? int(input.below(uint32_t(size + 3)))
				: int(input.u32());
		};
		entity.offset = pick();
		entity.length = pick();
		if (input.byte() & 1) {
			entity.data = QString::fromUtf8(input.bytes(input.byte() % 32));
		}
		result.push_back(entity);
	}
	return result;
}

void Check(const QString &text, const Entities &entities) {
	const auto size = int(text.size());
	for (const auto &entity : entities) {
		if (entity.offset < 0
			|| entity.length <= 0
			|| entity.offset > size
			|| entity.length > size - entity.offset) {
			qFatal("fuzz_text: an entity outside the text (%d + %d of %d)",
				entity.offset, entity.length, size);
		}
		const auto from = entity.offset;
		const auto till = entity.offset + entity.length;
		if ((from > 0 && from < size && text[from].isLowSurrogate() && text[from - 1].isHighSurrogate())
			|| (till > 0 && till < size && text[till].isLowSurrogate() && text[till - 1].isHighSurrogate())) {
			qFatal("fuzz_text: an entity edge inside a surrogate pair");
		}
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	Start();
	auto input = Fuzz::Input(data, size);
	const auto flags = input.byte();
	const auto text = MakeText(input);
	const auto entities = MakeEntities(input, int(text.size()));

	const auto normalized = NormalizeEntities(text, entities);
	Check(text, normalized);
	const auto parsed = ParseEntities(text, flags & ParseAll, entities);
	Check(parsed.text, parsed.entities);
	const auto shown = IsolateLinks(parsed);
	for (const auto &entity : shown.entities) {
		if (entity.offset < 0
			|| entity.length < 0
			|| entity.offset > shown.text.size()
			|| entity.length > shown.text.size() - entity.offset) {
			qFatal("fuzz_text: an isolated entity outside the text");
		}
	}
	const auto from = int(input.u32());
	const auto to = int(input.u32());
	const auto source = SourceSelection(shown.text, { from, to });
	if (source.from < 0 || source.to > StripIsolates(shown.text).size() || source.to < source.from) {
		qFatal("fuzz_text: a source selection outside the text");
	}
	(void)IsolatedSelection(shown.text, { from, to });

	// Layout is the slow part: a share of the inputs only.
	if (flags & 0x10) {
		// Raw entities as well: the layout must not trust its caller either.
		auto string = String(st::defaultTextStyle, (flags & 0x20)
			? TextWithEntities{ text, entities }
			: shown);
		const auto width = 1 + int(input.below(600));
		const auto height = string.countHeight(width, int(input.byte() % 4));
		(void)string.countLines(width);
		(void)string.countMaxLineWidth(width);
		(void)string.countLastLine(width);
		(void)string.maxWidth();
		auto image = QImage(QSize(width, std::clamp(height, 1, 400)), QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::white);
		{
			auto p = QPainter(&image);
			auto context = PaintContext();
			context.availableWidth = width;
			context.elisionLines = int(input.byte() % 3);
			context.autoDirection = (flags & 0x40) != 0;
			context.selection = { from, to };
			context.hoveredLink = int(input.byte()) - 3;
			string.draw(p, context);
		}
		for (auto i = 0; i != string.linksCount(); ++i) {
			(void)LinkUrl(string.link(i));
		}
		(void)string.toString({ from, to });
		(void)string.toTextWithEntities();
		string.setSpoilersRevealed(true);
	}
	return 0;
}

FUZZ_SEEDS() {
	return {
		QByteArray("\x17\x00\x00\x00\x10\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
			"\x00\x00\x00\x05\x01\x02\x03\x00\x08\x40\x41\x00\x0c\x05\x06\x00", 37),
		QByteArray("\x37\x00\x00\x00\x30", 5) + QByteArray(48, char(14)) + QByteArray("\x00\x00\x00\x08", 4) + QByteArray(64, char(0x41)),
		QByteArray("\x77\x00\x00\x01\x00", 5) + QByteArray(300, char(2)) + QByteArray(100, char(0xC8)),
	};
}
