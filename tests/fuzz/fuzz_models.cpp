// The JSON model parsers (Data::*::FromJson) against JSON of any shape:
// wrong types, missing fields, huge numbers, deep nesting, enormous counts.
// Half of the inputs are parsed as JSON text; the other half build a tree
// from the input bytes with keys taken from the realtime schema (so the
// fields the parsers actually read are hit) and hostile values.
#include "fuzz_driver.h"

#include "data/data_social_types.h"
#include "data/data_stickers.h"
#include "data/data_types.h"
#include "main/main_account.h"
#include "rt/rt_schema.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <cmath>

namespace {

[[nodiscard]] const QStringList &Keys() {
	static const auto result = [] {
		auto list = QStringList();
		for (auto i = 0; i != Rt::Schema::MessagesCount(); ++i) {
			const auto &message = Rt::Schema::Messages()[i];
			for (auto j = 0; j != message.count; ++j) {
				const auto key = QString::fromLatin1(message.fields[j].json);
				if (!list.contains(key)) {
					list.push_back(key);
				}
			}
		}
		return list;
	}();
	return result;
}

[[nodiscard]] QJsonValue Value(Fuzz::Input &input, int depth);

[[nodiscard]] QString Text(Fuzz::Input &input) {
	switch (input.byte() % 12) {
	case 0: return QString();
	case 1: return QString::number(qint64(input.u64()));
	case 2: return QString::fromUtf8(input.bytes(input.byte()));
	case 3: return QString(int(input.below(5000)), QChar(0x202E));
	case 4: return u"..\\..\\..\\x/../../etc/passwd"_s;
	case 5: return u"\U0001F600\U0001F600"_s + QChar(0xD83D);
	case 6: return u"javascript:alert(1)"_s;
	case 7: return u"file:///etc/passwd"_s;
	case 8: return Keys()[int(input.below(uint32_t(Keys().size())))];
	case 9: return u"1e308"_s;
	case 10: return u"-9223372036854775808"_s;
	default: return QString::fromLatin1(input.bytes(input.byte() % 16).toHex());
	}
}

[[nodiscard]] QJsonValue Number(Fuzz::Input &input) {
	static const double kValues[] = {
		0., -1., 1., 2147483647., 2147483648., -2147483649., 4294967296.,
		9007199254740993., 9.3e18, -9.3e18, 1.8e19, 1e300, -1e300, 0.5,
		1e-320, 65536., 16777216., 20000., -20000., 1e12, 1.7e12, 1.7e9,
	};
	const auto pick = input.byte();
	if (pick < 200) {
		return kValues[pick % (sizeof(kValues) / sizeof(kValues[0]))];
	}
	auto bits = input.u64();
	auto value = 0.;
	std::memcpy(&value, &bits, sizeof(value));
	return std::isfinite(value) ? value : 0.;
}

[[nodiscard]] QJsonValue Value(Fuzz::Input &input, int depth) {
	const auto kind = input.byte() % (depth > 6 ? 5 : 8);
	switch (kind) {
	case 0: return QJsonValue(QJsonValue::Null);
	case 1: return (input.byte() & 1) != 0;
	case 2: return Number(input);
	case 3:
	case 4: return Text(input);
	case 5: {
		auto list = QJsonArray();
		const auto count = input.byte() % 6;
		for (auto i = 0; i != count && !input.empty(); ++i) {
			list.push_back(Value(input, depth + 1));
		}
		return list;
	}
	default: {
		auto object = QJsonObject();
		const auto count = 1 + input.byte() % 24;
		for (auto i = 0; i != count && !input.empty(); ++i) {
			object.insert(
				Keys()[int(input.below(uint32_t(Keys().size())))],
				Value(input, depth + 1));
		}
		return object;
	}
	}
}

template <typename Type>
void RoundTrip(const QJsonObject &json) {
	const auto parsed = Type::FromJson(json);
	const auto written = parsed.toJson();
	// What we store ourselves (the SQLite cache) is read with the same parser.
	const auto again = Type::FromJson(written);
	(void)again.toJson();
}

template <typename Type>
void ParseOnly(const QJsonObject &json) {
	(void)Type::FromJson(json);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1) {
		return 0;
	}
	auto input = Fuzz::Input(data, size);
	auto json = QJsonObject();
	if (input.byte() & 1) {
		const auto document = QJsonDocument::fromJson(input.rest());
		if (document.isObject()) {
			json = document.object();
		} else if (document.isArray()) {
			json = QJsonObject{ { u"items"_s, document.array() } };
		} else {
			return 0;
		}
	} else {
		const auto value = Value(input, 0);
		json = value.isObject() ? value.toObject() : QJsonObject{ { u"message"_s, value } };
	}
	using namespace Data;
	RoundTrip<User>(json);
	RoundTrip<Profile>(json);
	RoundTrip<Birthday>(json);
	RoundTrip<Participant>(json);
	RoundTrip<Conversation>(json);
	RoundTrip<Entity>(json);
	RoundTrip<Draft>(json);
	RoundTrip<ChatInfo>(json);
	RoundTrip<MediaMeta>(json);
	RoundTrip<Sticker>(json);
	RoundTrip<LinkPreview>(json);
	RoundTrip<Location>(json);
	RoundTrip<LiveInfo>(json);
	RoundTrip<AudioMeta>(json);
	RoundTrip<CallRecord>(json);
	RoundTrip<Quote>(json);
	RoundTrip<ViaBot>(json);
	RoundTrip<MarkupButton>(json);
	RoundTrip<ReplyMarkup>(json);
	RoundTrip<Message>(json);
	ParseOnly<InboxEntry>(json);
	ParseOnly<Presence>(json);
	ParseOnly<PinnedMessage>(json);
	RoundTrip<OutboxItem>(json);
	ParseOnly<PostCounts>(json);
	ParseOnly<PostMy>(json);
	ParseOnly<PostMedia>(json);
	ParseOnly<PostPoll>(json);
	ParseOnly<PostClip>(json);
	ParseOnly<PostConversation>(json);
	ParseOnly<PostSound>(json);
	ParseOnly<Post>(json);
	ParseOnly<NotificationEntry>(json);
	ParseOnly<GraphState>(json);
	ParseOnly<TopicInfo>(json);
	ParseOnly<Trend>(json);
	ParseOnly<OpenConversation>(json);
	ParseOnly<Filters>(json);
	ParseOnly<DeviceSession>(json);
	ParseOnly<StickerPack>(json);
	ParseOnly<Main::Tokens>(json);
	for (auto i = json.begin(); i != json.end(); ++i) {
		if (i.value().isObject()) {
			const auto inner = i.value().toObject();
			RoundTrip<Message>(inner);
			RoundTrip<ChatInfo>(inner);
			ParseOnly<Post>(inner);
			RoundTrip<Profile>(inner);
		}
	}
	return 0;
}

FUZZ_SEEDS() {
	return {
		QByteArray("\x01") + R"json({"chat":"c1","seq":12,"sender":"u1","kind":"photo","text":"hi","sent_at":1700000000000,
			"entities":[{"type":"bold","offset":-5,"length":2147483647},{"type":"text_link","offset":1,"length":1,"url":"javascript:alert(1)"}],
			"media":{"w":20000,"h":-1,"blurhash":"LEHV6nWB2yk8pyo0adR*.7kCMdnj","size":1e30,"waveform":"AAECAw=="},
			"reply_to":{"seq":1,"text":"q","quote":{"text":"x","offset":99999999999}},
			"reactions":{"a":1e400,"b":-3},"link_preview":{"url":"file:///etc/passwd","image_url":"javascript:1","image_width":1e99},
			"reply_markup":{"inline_keyboard":[[{"text":"a","kind":"url","url":"file:///x"}],[],"x",[null]]}})json",
		QByteArray("\x01") + R"json({"chat":"c1","kind":"direct","participants":[{"ref":"a","me":true,"user_id":7},null,3],
			"last_seq":"9223372036854775808","pins":[1,"2",null,{},1e40],"draft":{"text":7,"entities":"x"},
			"conversation":{"id":"k","speaker":null,"listener":[]}})json",
		QByteArray("\x01") + R"json({"id":"p1","author":{"id":"u","handle":"a b\u)json" "202e" R"json("},"text":"t","media":[{"id":"m","w":0,"h":0},7],
			"poll":{"options":[{"text":"a","votes":-1}],"total":1e19},"counts":{"likes":"x"},"created_at":"never"})json",
		QByteArray("\x01") + R"json([1,2,3])json",
		QByteArray("\x00\x07\x10\x20\x30\x40\x50\x60\x70\x80\x90\xa0\xb0\xc0\xd0\xe0\xf0", 17),
	};
}
