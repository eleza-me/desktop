// Hostile media metadata and bytes: the blurhash decoder (any string, any
// requested size), the image decoder behind the media cache (format allow-list
// by magic bytes, dimension caps before allocation) and file names from
// messages (base::platform::SafeFileName).
#include "fuzz_driver.h"

#include "base/platform_file.h"
#include "data/data_blurhash.h"
#include "data/data_media.h"

#include <QtCore/QBuffer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

namespace {

void Start() {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;
	qputenv("QT_QPA_PLATFORM", "offscreen");
	static auto argc = 1;
	static char name[] = "fuzz_media";
	static char *argv[] = { name, nullptr };
	static auto application = new QGuiApplication(argc, argv);
	Q_UNUSED(application);
}

void CheckName(const QString &source) {
	const auto name = base::platform::SafeFileName(source);
	if (name.isEmpty() || name.size() > 181) {
		qFatal("fuzz_media: a bad length of a safe name");
	}
	if (name.startsWith(u'.') || name.endsWith(u'.') || name.endsWith(u' ') || name.startsWith(u' ')) {
		qFatal("fuzz_media: a safe name with a bad edge: '%s'", qPrintable(name));
	}
	for (const auto ch : name) {
		const auto code = ch.unicode();
		if (code < 0x20
			|| code == '/' || code == '\\' || code == ':' || code == '*' || code == '?'
			|| code == '"' || code == '<' || code == '>' || code == '|'
			|| ch.category() == QChar::Other_Format
			|| ch.category() == QChar::Other_Control) {
			qFatal("fuzz_media: a safe name with U+%04X", uint(code));
		}
	}
	if (base::platform::SafeFileName(name) != name) {
		qFatal("fuzz_media: SafeFileName is not stable on '%s'", qPrintable(name));
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	Start();
	auto input = Fuzz::Input(data, size);
	switch (input.byte() % 4) {
	case 0: {
		const auto width = int(input.u32());
		const auto height = int(input.u32());
		const auto image = Data::BlurhashImage(QString::fromUtf8(input.rest()), width, height);
		if (!image.isNull() && (image.width() > 256 || image.height() > 256)) {
			qFatal("fuzz_media: a blurhash image above its limit");
		}
	} break;
	case 1: {
		const auto fit = QSize(int(input.byte()) * 4, int(input.byte()) * 4);
		const auto image = Data::Media::Decode(QString(), input.rest(), fit);
		if (!image.isNull()
			&& (image.width() > 16384
				|| image.height() > 16384
				|| qint64(image.width()) * image.height() > (qint64(48) << 20))) {
			qFatal("fuzz_media: a decoded image above the limits");
		}
	} break;
	case 2:
		CheckName(QString::fromUtf8(input.rest()));
		break;
	default: {
		auto name = QString();
		while (!input.empty()) {
			name += QChar(char16_t((input.byte() << 8) | input.byte()));
		}
		CheckName(name);
	} break;
	}
	return 0;
}

FUZZ_SEEDS() {
	auto result = QList<QByteArray>();
	result.push_back(QByteArray("\x00\x00\x00\x00\x20\x00\x00\x00\x20", 9) + "LEHV6nWB2yk8pyo0adR*.7kCMdnj");
	result.push_back(QByteArray("\x00\x7f\xff\xff\xff\x7f\xff\xff\xff", 9) + "LEHV6nWB2yk8pyo0adR*.7kCMdnj");
	result.push_back(QByteArray("\x00\x00\x00\x00\x08\x00\x00\x00\x08", 9) + "~~~~~~");
	Start();
	for (const auto format : { "PNG", "JPG", "GIF", "WEBP" }) {
		auto image = QImage(48, 32, QImage::Format_ARGB32);
		image.fill(Qt::darkGreen);
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		if (image.save(&buffer, format)) {
			result.push_back(QByteArray("\x01\x10\x10", 3) + bytes);
		}
	}
	// A 20000 x 20000 PNG header, an SVG, a PNG that claims 2^31 - 1 pixels.
	result.push_back(QByteArray("\x01\x00\x00", 3) + QByteArray::fromHex(
		"89504e470d0a1a0a0000000d4948445200004e2000004e200806000000"));
	result.push_back(QByteArray("\x01\x00\x00", 3)
		+ "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><image href='file:///etc/passwd'/></svg>");
	result.push_back(QByteArray("\x01\x00\x00", 3) + QByteArray::fromHex(
		"89504e470d0a1a0a0000000d494844527fffffff7fffffff0806000000"));
	result.push_back(QByteArray("\x02") + "..\\..\\x\xe2\x80\xaegnp.exe. . ");
	result.push_back(QByteArray("\x02") + "CON.txt");
	result.push_back(QByteArray("\x02") + "a/b\\c:d*e?f\"g<h>i|j\n.desktop");
	return result;
}
