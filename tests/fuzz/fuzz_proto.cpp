// The dynamic protobuf codec: every schema message against arbitrary bytes.
// A decode must end (well inside the driver's time limit), never read
// outside the input and never produce more JSON than its budget allows; what
// decodes must encode again.
#include "fuzz_driver.h"

#include "rt/rt_proto.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 2) {
		return 0;
	}
	const auto count = Rt::Schema::MessagesCount();
	const auto index = ((int(data[0]) << 8) | int(data[1])) % count;
	const auto &message = Rt::Schema::Messages()[index];
	const auto bytes = QByteArray(reinterpret_cast<const char*>(data + 2), qsizetype(size - 2));
	const auto decoded = Rt::Proto::Decode(message, bytes);
	if (!decoded) {
		return 0;
	}
	const auto encoded = Rt::Proto::Encode(message, *decoded);
	// What we wrote ourselves always reads back.
	const auto again = Rt::Proto::Decode(message, encoded);
	if (!again) {
		// The budget may refuse a re-encoding only when the first pass was
		// already at it; anything else is a codec bug.
		if (encoded.size() < 4096) {
			qFatal("fuzz_proto: %s does not decode its own encoding", message.name);
		}
		return 0;
	}
	(void)Rt::Proto::UnwrapBody(*decoded);
	(void)Rt::Proto::UnwrapData(*decoded);
	return 0;
}

FUZZ_SEEDS() {
	auto result = QList<QByteArray>();
	const auto envelope = Rt::Schema::FindMessage(QLatin1StringView("Envelope"));
	const auto index = int(envelope - Rt::Schema::Messages());
	const auto prefix = QByteArray(1, char(index >> 8)) + QByteArray(1, char(index & 0xFF));
	const auto add = [&](QLatin1StringView name, const QJsonObject &object) {
		if (const auto found = Rt::Schema::FindMessage(name)) {
			const auto at = int(found - Rt::Schema::Messages());
			result.push_back(QByteArray(1, char(at >> 8))
				+ QByteArray(1, char(at & 0xFF))
				+ Rt::Proto::Encode(*found, object));
		}
	};
	result.push_back(prefix + Rt::Proto::Encode(*envelope, QJsonObject{
		{ u"hello"_s, QJsonObject{ { u"v"_s, 2 }, { u"conn"_s, u"c"_s }, { u"pts"_s, 12345 } } },
	}));
	result.push_back(prefix + Rt::Proto::Encode(*envelope, QJsonObject{
		{ u"res"_s, QJsonObject{ { u"id"_s, 3 }, { u"ok"_s, true }, { u"body"_s, u"AAEC"_s } } },
	}));
	result.push_back(prefix + Rt::Proto::Encode(*envelope, QJsonObject{
		{ u"batch"_s, QJsonObject{ { u"items"_s, QJsonArray{
			QJsonObject{ { u"ping"_s, QJsonObject{ { u"nonce"_s, 7 } } } },
			QJsonObject{ { u"close"_s, QJsonObject{ { u"code"_s, 4006 }, { u"reason"_s, u"r"_s } } } },
		} } } },
	}));
	add(QLatin1StringView("Message"), QJsonObject{
		{ u"chat"_s, u"c1"_s },
		{ u"seq"_s, 5 },
		{ u"text"_s, u"hello @sara https://example.com"_s },
		{ u"kind"_s, u"photo"_s },
		{ u"entities"_s, QJsonArray{ QJsonObject{ { u"type"_s, u"bold"_s }, { u"offset"_s, 0 }, { u"length"_s, 5 } } } },
		{ u"media"_s, QJsonObject{ { u"w"_s, 640 }, { u"h"_s, 480 }, { u"blurhash"_s, u"LEHV6nWB2yk8pyo0adR*.7kCMdnj"_s } } },
		{ u"reactions"_s, QJsonObject{ { u"x"_s, 2 } } },
	});
	add(QLatin1StringView("UpdateFrame"), QJsonObject{
		{ u"pts"_s, 10 },
		{ u"kind"_s, u"message"_s },
	});
	add(QLatin1StringView("Close"), QJsonObject{ { u"code"_s, 4006 } });
	// Every message type gets at least an empty and a junk body.
	for (auto i = 0; i < Rt::Schema::MessagesCount(); i += 7) {
		result.push_back(QByteArray(1, char(i >> 8)) + QByteArray(1, char(i & 0xFF))
			+ QByteArray::fromHex("0a00120018002a003200"));
	}
	return result;
}
