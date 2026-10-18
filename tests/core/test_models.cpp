// Parsing of wire JSON into the typed models, with fixtures captured from
// the mock API (tests/core/testdata) and hand-written rich messages.
#include "test_fakes.h"

#include "data/data_blurhash.h"
#include "data/data_json.h"
#include "data/data_social_types.h"
#include "data/data_stickers.h"
#include "data/data_types.h"
#include "rt/rt_proto.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>

using namespace Data;

class TestModels final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void jsonHelpers();
	void userAndProfile();
	void inboxEntriesAndChatInfo();
	void conversationChat();
	void historyMessages();
	void richMessageRoundTrip();
	void messageKindsVisibilityAndOrder();
	void resolveLocalAndStoredForm();
	void chatStoredRoundTrip();
	void outboxItemRoundTrip();
	void feedPosts();
	void socialLists();
	void stickerPacks();
	void modelsSurviveTheRealtimeCodec();
	void blurhash();

};

void TestModels::jsonHelpers() {
	const auto json = Test::Json(R"({"s":"x","n":5,"big":"9007199254740993","f":1.5,"t":true,"z":0,"nul":null,"list":["a",1,"b"],"ids":[1,"2"],"map":{"a":1,"b":"2"}})");
	QCOMPARE(Json::String(json, "s"_L1), u"x"_s);
	QCOMPARE(Json::String(json, "n"_L1), u"5"_s); // numbers as text where text is expected
	QCOMPARE(Json::Int64(json, "n"_L1), 5);
	QCOMPARE(Json::Int64(json, "big"_L1), qint64(9007199254740993)); // exact from a string
	QCOMPARE(Json::Int64(json, "missing"_L1, 7), 7);
	QCOMPARE(Json::Double(json, "f"_L1), 1.5);
	QVERIFY(Json::Bool(json, "t"_L1));
	QVERIFY(!Json::Bool(json, "z"_L1, true));
	QVERIFY(Json::Bool(json, "missing"_L1, true));
	QVERIFY(Json::Has(json, "z"_L1) && !Json::Has(json, "nul"_L1) && !Json::Has(json, "missing"_L1));
	QCOMPARE(Json::Strings(json.value("list"_L1)), (QStringList{ u"a"_s, u"b"_s }));
	QCOMPARE(Json::Int64s(json.value("ids"_L1)), (QVector<qint64>{ 1, 2 }));
	QCOMPARE(Json::CountMap(json.value("map"_L1)).value(u"b"_s), 2);
	QCOMPARE(Json::ToMs(1700000000), qint64(1700000000000));
	QCOMPARE(Json::ToMs(1700000000000), qint64(1700000000000));
	auto out = QJsonObject();
	Json::Put(out, "a"_L1, QString());
	Json::Put(out, "b"_L1, qint64(0));
	Json::Put(out, "c"_L1, false);
	Json::Put(out, "d"_L1, u"x"_s);
	Json::Put(out, "e"_L1, true);
	QCOMPARE(out, Test::Json(R"({"d":"x","e":true})"));

	// Numbers that do not fit saturate (a plain cast is undefined behaviour
	// and gave INT64_MIN / wrapped ints for sizes, counts and dimensions).
	const auto wild = Test::Json(R"({"huge":1e30,"low":-1e30,"edge":9223372036854775808,"text":"1e30","w":4294967297,"neg":-4294967297,"frac":2.9})");
	QCOMPARE(Json::Int64(wild, "huge"_L1), std::numeric_limits<qint64>::max());
	QCOMPARE(Json::Int64(wild, "low"_L1), std::numeric_limits<qint64>::min());
	QCOMPARE(Json::Int64(wild, "edge"_L1), std::numeric_limits<qint64>::max());
	QCOMPARE(Json::Int64(wild, "text"_L1), std::numeric_limits<qint64>::max());
	QCOMPARE(Json::Int(wild, "huge"_L1), std::numeric_limits<int>::max());
	QCOMPARE(Json::Int(wild, "w"_L1), std::numeric_limits<int>::max()); // not 1
	QCOMPARE(Json::Int(wild, "neg"_L1), std::numeric_limits<int>::min()); // not -1
	QCOMPARE(Json::Int(wild, "frac"_L1), 2);
	QCOMPARE(Json::String(wild, "huge"_L1), QString::number(std::numeric_limits<qint64>::max()));

	// Names lose direction overrides, isolates, invisible marks and control
	// characters; the joiners Persian and emoji need stay.
	const auto rlo = QChar(0x202E);
	QCOMPARE(Json::CleanName(u"Sara"_s), u"Sara"_s);
	QCOMPARE(Json::CleanName(rlo + u"troppuS AZELE"_s), u"troppuS AZELE"_s);
	QCOMPARE(
		Json::CleanName(QString(QChar(0x2069)) + u"a"_s + QChar(0x2066) + QChar(0x200B) + u"b\nc\td"_s + QChar(0) + QChar(0xFEFF)),
		u"ab c d"_s);
	const auto persian = u"می"_s + QChar(0x200C) + u"روم"_s;
	QCOMPARE(Json::CleanName(persian), persian);
	const auto family = QString::fromUcs4(U"\U0001F468\u200D\U0001F469");
	QCOMPARE(Json::CleanName(family), family);
	const auto user = User::FromJson(QJsonObject{
		{ u"id"_s, u"u1"_s },
		{ u"handle"_s, u"evil"_s + rlo + u"x"_s },
		{ u"display_name"_s, rlo + u"troppuS"_s },
	});
	QCOMPARE(user.handle, u"evilx"_s);
	QCOMPARE(user.displayName, u"troppuS"_s);
}

void TestModels::userAndProfile() {
	const auto users = Json::List<User>(Test::Fixture("users.json").value("users"_L1));
	QCOMPARE(users.size(), 3);
	QCOMPARE(users[0].id, u"u_sara"_s);
	QCOMPARE(users[0].handle, u"sara"_s);
	QCOMPARE(users[0].displayName, u"سارا احمدی"_s);
	QVERIFY(users[0].verified && !users[0].isBot && !users[0].deleted);
	QVERIFY(users[2].isBot);
	QCOMPARE(User::FromJson(users[0].toJson()), users[0]);
	QVERIFY(!User().valid());

	const auto me = Profile::FromJson(Test::Fixture("me.json"));
	QCOMPARE(me.id, u"u_me"_s);
	QCOMPARE(me.handle, u"hadi"_s);
	QCOMPARE(me.followers, 12);
	QCOMPARE(me.following, 30);
	QCOMPARE(me.posts, 4);
	QCOMPARE(me.province, u"IR-07"_s);
	QCOMPARE(me.visibility, u"full"_s);
	QVERIFY(me.birthday.has_value());
	QCOMPARE(me.birthday->calendar, u"jalali"_s);
	QCOMPARE(me.birthday->month, 7);
	QCOMPARE(me.birthday->day, 12);
	QCOMPARE(me.birthday->year, 0);

	const auto sara = Profile::FromJson(Test::Fixture("profile_sara.json"));
	QVERIFY(sara.followsYou);
	QCOMPARE(sara.followState, u"none"_s);
	QCOMPARE(sara.metaLine, u"عکاس"_s);
	QCOMPARE(sara.mutuals.size(), 1);
	QCOMPARE(sara.mutuals[0].handle, u"ali.r"_s);
	QCOMPARE(sara.mutualsTotal, 3);
	// The stored form keeps what the screens read.
	const auto again = Profile::FromJson(sara.toJson());
	QCOMPARE(again.card(), sara.card());
	QCOMPARE(again.bio, sara.bio);
	QCOMPARE(again.followers, sara.followers);
	QCOMPARE(again.mutuals.size(), 1);

	const auto suggestions = Json::List<Profile>(Test::Fixture("suggestions.json").value("people"_L1));
	QCOMPARE(suggestions.size(), 4);
	QCOMPARE(suggestions[0].reason, u"mutual"_s);
	QCOMPARE(suggestions[0].mutualCount, 3);
	QCOMPARE(suggestions[0].mutuals.size(), 2);
}

void TestModels::inboxEntriesAndChatInfo() {
	const auto list = Test::Fixture("inbox_message.json").value("chats"_L1).toArray();
	QCOMPARE(list.size(), 3);
	const auto first = InboxEntry::FromJson(list[0].toObject());
	QCOMPARE(first.chat, u"c1"_s);
	QCOMPARE(first.section, u"message"_s);
	QCOMPARE(first.unread, 2);
	QCOMPARE(first.pinned, std::make_optional(true));
	QCOMPARE(first.muted, std::make_optional(false));
	QVERIFY(!first.archived.has_value()); // not in the entry: the header decides
	QVERIFY(first.lastActivityAt > 1700000000000);
	QVERIFY(first.lastMessage.has_value());
	QCOMPARE(first.lastMessage->chat, u"c1"_s);
	QVERIFY(first.chatInfo.has_value());
	const auto &info = *first.chatInfo;
	QCOMPARE(info.chat, u"c1"_s);
	QCOMPARE(info.kind, u"direct"_s);
	QCOMPARE(info.state, u"active"_s);
	QCOMPARE(info.participants.size(), 2);
	QVERIFY(info.participants[0].me);
	QCOMPARE(info.peerReadSeq, 4);
	QCOMPARE(info.lastReadSeq, 5);
	QVERIFY(info.canSend);
	QVERIFY(!info.conversation.has_value());
	QVERIFY(info.peer());
	QCOMPARE(info.peer()->userId, u"u_sara"_s);
	QCOMPARE(info.myRefs(u"u_me"_s), QStringList{ u"u_me"_s });
	QCOMPARE(info.peopleIds(), (QStringList{ u"u_me"_s, u"u_sara"_s }));
	const auto third = InboxEntry::FromJson(list[2].toObject());
	QCOMPARE(third.muted, std::make_optional(true));
	QVERIFY(third.chatInfo->participants[1].bot);
	// toJson -> FromJson keeps the header.
	const auto again = ChatInfo::FromJson(info.toJson());
	QCOMPARE(again.peerReadSeq, info.peerReadSeq);
	QCOMPARE(again.participants.size(), 2);
	QCOMPARE(again.participants[1].userId, u"u_sara"_s);
	// `can_send` defaults to true when a server leaves it out, false stays false.
	QVERIFY(ChatInfo::FromJson(Test::Json(R"({"chat":"c"})")).canSend);
	QVERIFY(!ChatInfo::FromJson(Test::Json(R"({"chat":"c","can_send":false})")).canSend);
	QVERIFY(!ChatInfo::FromJson(QJsonObject()).valid());
}

void TestModels::conversationChat() {
	const auto entry = InboxEntry::FromJson(Test::Fixture("inbox_conversation.json").value("chats"_L1).toArray()[0].toObject());
	QCOMPARE(entry.section, u"conversation"_s);
	const auto &info = *entry.chatInfo;
	QVERIFY(info.conversation.has_value());
	const auto &conversation = *info.conversation;
	QCOMPARE(conversation.id, u"conv1"_s);
	QCOMPARE(conversation.topic, u"یه روز خوب تو تهران"_s);
	QCOMPARE(conversation.category, u"heart"_s);
	QCOMPARE(conversation.state, u"open"_s);
	QCOMPARE(conversation.myRole, u"speaker"_s);
	QVERIFY(conversation.speaker.me);
	QVERIFY(conversation.listener.has_value());
	QVERIFY(conversation.listener->masked);
	QVERIFY(conversation.expiresAt > 1700000000);
	// My refs in an anonymous conversation are my participant ref, not only my user id.
	QCOMPARE(info.myRefs(u"u_me"_s), (QStringList{ u"u_me"_s, u"p_a"_s }));
	// The peer is the conversation's other side.
	QVERIFY(info.peer());
	QCOMPARE(info.peer()->ref, u"p_b"_s);
	QVERIFY(info.peopleIds().isEmpty()); // nobody is unmasked
	auto message = *entry.lastMessage;
	message.resolveLocal(&info, u"u_me"_s);
	QVERIFY(!message.mine);
	message.sender = u"p_a"_s;
	message.resolveLocal(&info, u"u_me"_s);
	QVERIFY(message.mine);
	const auto again = Conversation::FromJson(conversation.toJson());
	QCOMPARE(again.topic, conversation.topic);
	QVERIFY(again.listener.has_value());
	// A conversation still waiting for its listener has none (also after the codec's always-keys).
	const auto waiting = Conversation::FromJson(Test::Json(R"({"id":"x","topic":"t","state":"open","speaker":{"ref":"p","me":true,"role":"speaker","masked":false},"listener":{}})"));
	QVERIFY(!waiting.listener.has_value());
	auto chat = Chat();
	chat.conversation = waiting;
	QVERIFY(chat.waitingForListener());
}

void TestModels::historyMessages() {
	const auto page = Test::Fixture("history_c1.json");
	const auto info = ChatInfo::FromJson(page.value("chat"_L1).toObject());
	auto messages = Json::List<Message>(page.value("messages"_L1));
	QVERIFY(messages.size() >= 7);
	// Newest first on the wire.
	QVERIFY(messages.front().seq > messages.back().seq);
	std::sort(messages.begin(), messages.end(), MessageLess);
	QCOMPARE(messages[0].seq, 1);
	QCOMPARE(messages[0].sender, u"u_sara"_s);
	QCOMPARE(messages[0].text, u"سلام! چطوری؟"_s);
	QVERIFY(messages[0].kind == MessageKind::Text);
	QCOMPARE(messages[0].key(), u"seq:1"_s);
	QCOMPARE(messages[4].reactions.value(u"❤️"_s), 1);
	const auto &photo = messages[5];
	QVERIFY(photo.kind == MessageKind::Photo);
	QCOMPARE(photo.attachment, u"media1"_s);
	QVERIFY(photo.media.has_value());
	QCOMPARE(photo.media->width, 1200);
	QCOMPARE(photo.media->height, 800);
	QCOMPARE(photo.media->blurhash, u"LKO2?U%2Tw=w]~RBVZRi};RPxuwH"_s);
	QCOMPARE(messages[6].replyToSeq, 6);
	for (auto &message : messages) {
		message.resolveLocal(&info, u"u_me"_s);
	}
	QVERIFY(!messages[0].mine);
	QVERIFY(messages[1].mine);
	QVERIFY(messages[1].status == MessageStatus::Read);  // seq 2 <= peer_read_seq 4
	QVERIFY(messages[3].status == MessageStatus::Read);  // seq 4
	const auto file = Json::List<Message>(Test::Fixture("history_c2.json").value("messages"_L1));
	const auto found = std::find_if(file.cbegin(), file.cend(), [](const Message &message) {
		return message.kind == MessageKind::File && message.seq == 2;
	});
	QVERIFY(found != file.cend());
	QCOMPARE(found->text, u"eleza-plan.pdf"_s);
	QCOMPARE(found->media->size, 1240000);
	QCOMPARE(found->media->mime, u"application/pdf"_s);
}

void TestModels::richMessageRoundTrip() {
	const auto json = Test::Json(R"({
		"chat":"c1","seq":42,"id":"m42","sender":"u_bot","client_message_id":"AB12-CD","kind":"video",
		"text":"look at this https://eleza.me","reply_to_seq":40,"attachment":"med9",
		"media":{"w":1280,"h":720,"duration_ms":15000,"blurhash":"LKO2?U%2Tw=w]~RBVZRi};RPxuwH","mime":"video/mp4","size":3500000,"waveform":"AQID"},
		"forwarded":true,"created_at":1700000000123,"edited":true,"played":true,
		"reactions":{"👍":3,"🔥":0},"my_reaction":"👍",
		"entities":[{"type":"text_link","offset":13,"length":16,"url":"https://eleza.me"},{"type":"pre","offset":0,"length":4,"language":"cpp"}],
		"media_spoiler":true,
		"quote":{"text":"earlier words","offset":3},
		"link_preview":{"url":"https://eleza.me","site_name":"Eleza","title":"Eleza","description":"d","image_url":"https://eleza.me/i.png","image_width":600,"image_height":315},
		"reply_markup":{"inline_keyboard":[[{"text":"Yes","kind":"callback"},{"text":"Site","kind":"url","url":"https://eleza.me"}],[{"text":"Find","kind":"switch_inline","query":""}]],"resize":true},
		"via_bot":{"id":"u_bot","handle":"stickerbot","name":"Sticker bot"}
	})");
	const auto message = Message::FromJson(json);
	QCOMPARE(message.seq, 42);
	QCOMPARE(message.clientMessageId, u"ab12-cd"_s); // compared with our lowercase ids
	QVERIFY(message.kind == MessageKind::Video);
	QCOMPARE(message.replyToSeq, 40);
	QCOMPARE(message.media->durationMs, 15000);
	QCOMPARE(message.media->waveform, QByteArray("\x01\x02\x03", 3));
	QVERIFY(message.forwarded && message.edited && message.played && message.mediaSpoiler);
	QCOMPARE(message.reactions.size(), 1); // a zero count is no reaction
	QCOMPARE(message.reactions.value(u"👍"_s), 3);
	QCOMPARE(message.myReaction, u"👍"_s);
	QCOMPARE(message.entities.size(), 2);
	QCOMPARE(message.entities[0].type, u"text_link"_s);
	QCOMPARE(message.entities[0].offset, 13);
	QCOMPARE(message.entities[0].url, u"https://eleza.me"_s);
	QCOMPARE(message.entities[1].language, u"cpp"_s);
	QCOMPARE(message.quote->text, u"earlier words"_s);
	QCOMPARE(message.quote->offset, 3);
	QCOMPARE(message.linkPreview->siteName, u"Eleza"_s);
	QCOMPARE(message.linkPreview->imageWidth, 600);
	QVERIFY(message.replyMarkup.has_value());
	QCOMPARE(message.replyMarkup->inlineKeyboard.size(), 2);
	QCOMPARE(message.replyMarkup->inlineKeyboard[0][1].url, u"https://eleza.me"_s);
	QVERIFY(message.replyMarkup->inlineKeyboard[1][0].query.has_value()); // an explicit empty query
	QVERIFY(!message.replyMarkup->inlineKeyboard[0][0].query.has_value());
	QVERIFY(message.replyMarkup->resize);
	QCOMPARE(message.viaBot->handle, u"stickerbot"_s);
	// Everything survives toJson -> FromJson.
	const auto again = Message::FromJson(message.toJson());
	QCOMPARE(again.toJson(), message.toJson());
	QCOMPARE(again.entities.size(), 2);
	QCOMPARE(again.replyMarkup->inlineKeyboard[1][0].query, std::make_optional(QString()));
	QCOMPARE(again.media->waveform, message.media->waveform);

	const auto others = Test::Json(R"({"list":[
		{"chat":"c","seq":1,"id":"a","sender":"u","kind":"sticker","created_at":1,"sticker":{"id":"st1","pack":"p1","emoji":"😀","url":"https://cdn/s.webp","width":512,"height":512}},
		{"chat":"c","seq":2,"id":"b","sender":"u","kind":"location","created_at":2,"location":{"lat":35.6892,"lng":51.389,"accuracy_m":12.5,"label":"Azadi","address":"Tehran"}},
		{"chat":"c","seq":3,"id":"c","sender":"u","kind":"live_location","created_at":3,"location":{"lat":1,"lng":2},"live":{"period":900,"until":1700000900,"stopped":true}},
		{"chat":"c","seq":4,"id":"d","sender":"u","kind":"audio","created_at":4,"attachment":"m","audio":{"title":"Song","performer":"Band"}},
		{"chat":"c","seq":5,"id":"e","sender":"u","kind":"forwarded_post","created_at":5,"forwarded_post":"p1"},
		{"chat":"c","seq":6,"id":"f","sender":"u","kind":"system","created_at":6,"call":{"outcome":"missed","duration_s":0}},
		{"chat":"c","seq":7,"id":"g","sender":"u","kind":"voice","created_at":7,"attachment":"v","media":{"duration_ms":4200,"waveform":"CQg="}},
		{"chat":"c","seq":8,"id":"h","sender":"u","kind":"gif","created_at":8,"attachment":"g"},
		{"chat":"c","seq":9,"id":"i","sender":"u","kind":"video_note","created_at":9,"attachment":"n"},
		{"chat":"c","seq":10,"id":"j","sender":"u","kind":"hologram","created_at":10}
	]})");
	const auto list = Json::List<Message>(others.value("list"_L1));
	QCOMPARE(list.size(), 10);
	QVERIFY(list[0].kind == MessageKind::Sticker);
	QCOMPARE(list[0].sticker->id, u"st1"_s);
	QCOMPARE(list[0].sticker->width, 512);
	QVERIFY(list[1].kind == MessageKind::Location);
	QCOMPARE(list[1].location->lat, 35.6892);
	QCOMPARE(list[1].location->accuracyM, 12.5);
	QCOMPARE(list[1].location->label, u"Azadi"_s);
	QVERIFY(list[2].kind == MessageKind::LiveLocation);
	QCOMPARE(list[2].live->period, 900);
	QVERIFY(list[2].live->stopped);
	QVERIFY(list[3].kind == MessageKind::Audio);
	QCOMPARE(list[3].audio->performer, u"Band"_s);
	QVERIFY(list[4].kind == MessageKind::ForwardedPost);
	QCOMPARE(list[4].forwardedPost, u"p1"_s);
	QVERIFY(list[5].kind == MessageKind::System);
	QCOMPARE(list[5].call->outcome, u"missed"_s);
	QVERIFY(list[6].kind == MessageKind::Voice);
	QCOMPARE(list[6].media->waveform, QByteArray("\x09\x08", 2));
	QVERIFY(list[7].kind == MessageKind::Gif);
	QVERIFY(list[8].kind == MessageKind::VideoNote);
	QVERIFY(list[9].kind == MessageKind::Unknown);
	QCOMPARE(list[9].kindName, u"hologram"_s);
	for (const auto &item : list) {
		QCOMPARE(Message::FromJson(item.toJson()).toJson(), item.toJson());
		QCOMPARE(Message::FromJson(item.toJson()).kind, item.kind);
	}
	QVERIFY(MessageKindHasMedia(MessageKind::Voice) && !MessageKindHasMedia(MessageKind::Sticker));
	QCOMPARE(MessageKindToString(MessageKind::ForwardedPost), u"forwarded_post"_s);
	QVERIFY(MessageKindFromString(u"post"_s) == MessageKind::ForwardedPost);
}

void TestModels::messageKindsVisibilityAndOrder() {
	auto message = Message::FromJson(Test::Json(R"({"chat":"c","seq":1,"id":"a","sender":"u","kind":"text","text":"x","created_at":1})"));
	QVERIFY(message.visible());
	message.deleted = true;
	QVERIFY(!message.visible());
	message.deleted = false;
	message.underReview = true;
	QVERIFY(!message.visible());

	const auto service = [](const char *system, const char *text = "") {
		auto result = Message();
		result.kind = MessageKind::System;
		result.system = QString::fromLatin1(system);
		result.text = QString::fromUtf8(text);
		return result;
	};
	QVERIFY(service("conversation.joined:p_b").serviceEvent() == ServiceEvent::Joined);
	QVERIFY(service("conversation_ended").serviceEvent() == ServiceEvent::Ended);
	QVERIFY(service("conversation.expired").serviceEvent() == ServiceEvent::Ended);
	QVERIFY(service("message-pinned:12").serviceEvent() == ServiceEvent::Pinned);
	QVERIFY(service("chat.theme:blue").serviceEvent() == ServiceEvent::ThemeChanged);
	QVERIFY(service("conversation.continued").serviceEvent() == ServiceEvent::Continued);
	QVERIFY(service("something.new", "worded by the server").serviceEvent() == ServiceEvent::Text);
	QVERIFY(service("something.new").serviceEvent() == ServiceEvent::None);
	QVERIFY(service("conversation.joined").visible());
	QVERIFY(!service("something.new").visible()); // nobody can word it
	QVERIFY(!service("").visible());
	auto call = service("");
	call.call = CallRecord{ u"missed"_s, 0 };
	QVERIFY(call.visible());

	// Server messages by seq, then local pending ones by creation time.
	auto a = Message();
	a.seq = 5;
	auto b = Message();
	b.seq = 9;
	auto local1 = Message();
	local1.clientMessageId = u"x"_s;
	local1.createdAt = 100;
	auto local2 = Message();
	local2.clientMessageId = u"y"_s;
	local2.createdAt = 50;
	auto list = QVector<Message>{ local1, b, local2, a };
	std::sort(list.begin(), list.end(), MessageLess);
	QCOMPARE(list[0].seq, 5);
	QCOMPARE(list[1].seq, 9);
	QCOMPARE(list[2].clientMessageId, u"y"_s);
	QCOMPARE(list[3].clientMessageId, u"x"_s);
	QCOMPARE(list[3].key(), u"local:x"_s);
	QVERIFY(list[3].local() && !list[0].local());

	auto pinned = Chat();
	pinned.id = u"p"_s;
	pinned.pinned = true;
	pinned.lastActivityAt = 1;
	auto recent = Chat();
	recent.id = u"r"_s;
	recent.lastActivityAt = 100;
	auto old = Chat();
	old.id = u"o"_s;
	old.lastActivityAt = 50;
	auto chats = QVector<Chat>{ old, recent, pinned };
	std::sort(chats.begin(), chats.end(), ChatLess);
	QCOMPARE(chats[0].id, u"p"_s);
	QCOMPARE(chats[1].id, u"r"_s);
	QCOMPARE(chats[2].id, u"o"_s);
}

void TestModels::resolveLocalAndStoredForm() {
	auto info = ChatInfo();
	info.chat = u"c"_s;
	info.peerReadSeq = 5;
	auto self = Participant();
	self.ref = self.userId = u"u_me"_s;
	self.me = true;
	auto peer = Participant();
	peer.ref = peer.userId = u"u_x"_s;
	info.participants = { self, peer };
	auto message = Message();
	message.chat = u"c"_s;
	message.seq = 6;
	message.sender = u"u_me"_s;
	message.resolveLocal(&info, u"u_me"_s);
	QVERIFY(message.mine);
	QVERIFY(message.status == MessageStatus::Sent);
	message.seq = 5;
	message.resolveLocal(&info, u"u_me"_s);
	QVERIFY(message.status == MessageStatus::Read);
	message.sender = u"u_x"_s;
	message.resolveLocal(&info, u"u_me"_s);
	QVERIFY(!message.mine);
	QVERIFY(message.status == MessageStatus::Sent);
	// Without a header my user id is all there is.
	message.sender = u"u_me"_s;
	message.resolveLocal(nullptr, u"u_me"_s);
	QVERIFY(message.mine);

	message.status = MessageStatus::Read;
	message.text = u"hi"_s;
	const auto stored = Message::FromStored(message.toStored());
	QVERIFY(stored.mine);
	QVERIFY(stored.status == MessageStatus::Read);
	QCOMPARE(stored.text, u"hi"_s);
	// The wire form carries no local state.
	QVERIFY(!message.toJson().contains(u"_mine"_s));
	QVERIFY(!Message::FromJson(message.toJson()).mine);
}

void TestModels::chatStoredRoundTrip() {
	auto chat = Chat();
	chat.id = u"c1"_s;
	chat.section = u"message"_s;
	chat.title = u"سارا"_s;
	chat.peerUserId = u"u_sara"_s;
	chat.peerRef = u"u_sara"_s;
	chat.verified = true;
	chat.lastMessage = Message::FromJson(Test::Json(R"({"chat":"c1","seq":7,"id":"m7","sender":"u_sara","kind":"text","text":"hi","created_at":1700000000000})"));
	chat.lastMine = false;
	chat.lastActivityAt = 1700000000000;
	chat.unread = 2;
	chat.pinned = true;
	chat.muted = true;
	chat.mutedUntil = 1700009999;
	chat.canSend = false;
	chat.draft = Draft{ u"typing…"_s, 5, {}, 1700000001 };
	chat.lastSeq = 7;
	chat.peerReadSeq = 4;
	chat.lastReadSeq = 5;
	chat.theme = u"blue"_s;
	chat.info.chat = u"c1"_s;
	chat.info.kind = u"direct"_s;
	chat.info.pins = { 3, 6 };
	const auto again = Chat::FromStored(chat.toStored());
	QCOMPARE(again.id, chat.id);
	QCOMPARE(again.title, chat.title);
	QCOMPARE(again.peerUserId, chat.peerUserId);
	QVERIFY(again.verified && again.pinned && again.muted && !again.canSend);
	QCOMPARE(again.lastMessage->text, u"hi"_s);
	QCOMPARE(again.lastActivityAt, chat.lastActivityAt);
	QCOMPARE(again.unread, 2);
	QCOMPARE(again.mutedUntil, chat.mutedUntil);
	QCOMPARE(again.draft->text, u"typing…"_s);
	QCOMPARE(again.draft->replyToSeq, 5);
	QCOMPARE(again.lastSeq, 7);
	QCOMPARE(again.peerReadSeq, 4);
	QCOMPARE(again.lastReadSeq, 5);
	QCOMPARE(again.theme, u"blue"_s);
	QCOMPARE(again.info.pins, (QVector<qint64>{ 3, 6 }));
	QVERIFY(!again.isRequest() && !again.isConversation);
	// A queued (local) last message is not written to the cache.
	chat.lastMessage->seq = 0;
	QVERIFY(!Chat::FromStored(chat.toStored()).lastMessage.has_value());
}

void TestModels::outboxItemRoundTrip() {
	auto item = OutboxItem();
	item.clientMessageId = u"3f2c1e6a-0000-4000-8000-000000000001"_s;
	item.chat = u"c1"_s;
	item.kind = u"photo"_s;
	item.body = Test::Json(R"({"kind":"photo","text":"caption","reply_to_seq":3})");
	item.createdAt = 1700000000000;
	item.attempts = 2;
	item.filePath = u"/tmp/a.jpg"_s;
	item.fileName = u"a.jpg"_s;
	item.mime = u"image/jpeg"_s;
	item.fileSize = 1234;
	item.width = 640;
	item.height = 480;
	item.uploadId = u"up1"_s;
	item.failed = true;
	item.errorCode = u"media.rejected"_s;
	const auto again = OutboxItem::FromJson(item.toJson());
	QCOMPARE(again.toJson(), item.toJson());
	QCOMPARE(again.body, item.body);
	QCOMPARE(again.attempts, 2);
	QVERIFY(again.failed);
	QCOMPARE(again.uploadId, u"up1"_s);
	QVERIFY(again.mediaId.isEmpty());
}

void TestModels::feedPosts() {
	const auto feed = Test::Fixture("feed_for_you.json");
	const auto items = feed.value("items"_L1).toArray();
	QCOMPARE(items.size(), 8);
	auto posts = QHash<QString, Post>();
	for (const auto &item : items) {
		const auto post = Post::FromJson(item.toObject().value("post"_L1).toObject());
		QVERIFY(post.valid());
		posts.insert(post.id, post);
	}
	const auto &text = posts[u"p1"_s];
	QCOMPARE(text.author, u"u_sara"_s);
	QCOMPARE(text.type, u"text"_s);
	QCOMPARE(text.replyPolicy, u"everyone"_s);
	QVERIFY(text.canReply && text.canRepost);
	QCOMPARE(text.counts.likes, 42);
	QCOMPARE(text.counts.replies, 3);
	QCOMPARE(text.counts.reposts, 5);
	QCOMPARE(text.counts.views, 1200);
	QVERIFY(text.counts.hasDislikes);
	QVERIFY(text.my.reaction.isEmpty());
	const auto &media = posts[u"p2"_s];
	QCOMPARE(media.media.size(), 1);
	QCOMPARE(media.media[0].kind, u"image"_s);
	QCOMPARE(media.media[0].width, 1200);
	QCOMPARE(media.media[0].alt, u"غروب خورشید پشت کوه"_s);
	QCOMPARE(media.my.reaction, u"like"_s);
	const auto &poll = posts[u"p3"_s];
	QVERIFY(poll.poll.has_value());
	QCOMPARE(poll.poll->options.size(), 3);
	QCOMPARE(poll.poll->votes, (QVector<qint64>{ 12, 30, 8 }));
	QCOMPARE(poll.poll->total, 50);
	QCOMPARE(poll.poll->myVote, -1);
	QVERIFY(!poll.poll->closed);
	const auto &quote = posts[u"p4"_s];
	QVERIFY(quote.quote != nullptr);
	QCOMPARE(quote.quote->id, u"p1"_s);
	QCOMPARE(quote.quote->counts.likes, 42);
	const auto &invite = posts[u"p5"_s];
	QVERIFY(invite.conversation.has_value());
	QCOMPARE(invite.conversation->id, u"conv9"_s);
	QCOMPARE(invite.conversation->state, u"open"_s);
	QVERIFY(posts[u"p6"_s].pinned);
	QCOMPARE(posts[u"p7"_s].media.size(), 5);
	QCOMPARE(posts[u"p8"_s].threadParts.size(), 2);
	QCOMPARE(posts[u"p8"_s].threadParts[1].text, u"دوم: صبح‌ها بنویس، قبل از این‌که گوشی رو باز کنی."_s);
	QCOMPARE(items[2].toObject().value("reason"_L1).toString(), u"repost"_s);
	// A voted poll.
	const auto voted = PostPoll::FromJson(Test::Json(R"({"question":"q","options":["a","b"],"ends_at":1,"closed":true,"my_vote":0})"));
	QCOMPARE(voted.myVote, 0);
	QVERIFY(voted.closed);
	QVERIFY(voted.votes.isEmpty());
	QCOMPARE(PostMy::FromJson(Test::Json(R"({"reaction":"none"})")).reaction, QString());
	QVERIFY(!PostCounts::FromJson(Test::Json(R"({"likes":1})")).hasDislikes);

	const auto replies = Json::List<Post>(Test::Fixture("replies_p1.json").value("replies"_L1));
	QCOMPARE(replies.size(), 3);
	QCOMPARE(replies[1].replyTo, u"r1"_s);
	QCOMPARE(replies[1].threadRoot, u"p1"_s);
}

void TestModels::socialLists() {
	const auto notifications = Json::List<NotificationEntry>(Test::Fixture("notifications.json").value("notifications"_L1));
	QCOMPARE(notifications.size(), 5);
	QCOMPARE(notifications[0].kind, u"like"_s);
	QCOMPARE(notifications[0].actors, (QStringList{ u"u_sara"_s, u"u_ali"_s }));
	QCOMPARE(notifications[0].actorCount, 2);
	QCOMPARE(notifications[0].refs.value(u"post"_s), u"p6"_s);
	QVERIFY(!notifications[0].read && notifications[2].read);
	QCOMPARE(notifications[4].refs.value(u"chat"_s), u"c4"_s);
	const auto unread = Test::Fixture("notifications_unread.json");
	QCOMPARE(Json::Int64(unread, "news"_L1), 2);
	QCOMPARE(Json::CountMap(unread.value("kinds"_L1)).value(u"follow_request"_s), 1);

	const auto open = Json::List<OpenConversation>(Test::Fixture("conversations_open.json").value("conversations"_L1));
	QCOMPARE(open.size(), 4);
	QCOMPARE(open[0].speakerId, u"u_me"_s);
	QVERIFY(!open[0].speakerMasked);
	QVERIFY(open[1].speakerMasked);
	QCOMPARE(open[1].maskName, u"رهگذر"_s);
	QCOMPARE(open[1].maskAvatar, 3);
	QCOMPARE(open[1].category, u"cinema"_s);

	const auto trends = Json::List<Trend>(Test::Fixture("trending.json").value("topics"_L1));
	QCOMPARE(trends.size(), 3);
	QVERIFY(trends[0].rising);
	QCOMPARE(trends[0].posts, 1240);
	QCOMPARE(trends[0].openConversations, 4);

	const auto filters = Filters::FromJson(Test::Fixture("filters.json"));
	QCOMPARE(filters.muted, QStringList{ u"u_bot"_s });
	QCOMPARE(filters.mutedWords, QStringList{ u"اسپویل"_s });
	QCOMPARE(filters.hiddenTopics.size(), 1);
	QCOMPARE(filters.hiddenTopics[0].id, u"t3"_s); // a bare id
	QCOMPARE(Filters::FromJson(Test::Json(R"({"hidden_topics":[{"id":"t9","title":"T","slug":"s"}]})")).hiddenTopics[0].title, u"T"_s);

	const auto sessions = Json::List<DeviceSession>(Test::Fixture("sessions.json").value("sessions"_L1));
	QCOMPARE(sessions.size(), 2);
	QCOMPARE(sessions[0].id, u"s1"_s);
	QVERIFY(sessions[0].current);
	QCOMPARE(sessions[1].platform, u"ios"_s);

	// Contact bans: a Go time (RFC 3339) and the mask name only.
	const auto bans = Json::List<ContactBan>(Test::Json(R"({"bans":[
		{"token":"b1.x","label":"روباه","created_at":"2026-03-01T10:20:30.5Z"},
		{"token":"b2.y","label":"","created_at":"bogus"}]})").value("bans"_L1));
	QCOMPARE(bans.size(), 2);
	QCOMPARE(bans[0].token, u"b1.x"_s);
	QCOMPARE(bans[0].label, u"روباه"_s);
	QCOMPARE(
		bans[0].createdAt,
		QDateTime(QDate(2026, 3, 1), QTime(10, 20, 30, 500), QTimeZone::utc()).toMSecsSinceEpoch());
	QVERIFY(bans[1].label.isEmpty());
	QCOMPARE(bans[1].createdAt, qint64(0));

	// Last-seen exceptions: the cards inside `items`, a card without an id skipped.
	const auto exceptions = LastSeenExceptionsPage::FromJson(Test::Json(R"({"items":[
		{"user":{"id":"u1","handle":"sara","display_name":"سارا"},"kind":"always"},
		{"user":{"display_name":"x"},"kind":"always"},
		{"kind":"never"}],"next_cursor":"c2"})"));
	QCOMPARE(exceptions.users.size(), 1);
	QCOMPARE(exceptions.users[0].id, u"u1"_s);
	QCOMPARE(exceptions.users[0].handle, u"sara"_s);
	QCOMPARE(exceptions.nextCursor, u"c2"_s);
	QVERIFY(LastSeenExceptionsPage::FromJson(QJsonObject()).users.isEmpty());

	const auto state = GraphState::FromJson(Test::Json(R"({"follow":"requested","follows_you":true})"));
	QCOMPARE(state.follow, u"requested"_s);
	QVERIFY(state.followsYou && !state.blockedByYou);
	QCOMPARE(GraphState::FromJson(QJsonObject()).follow, u"none"_s);

	const auto users = Json::List<Profile>(Test::Fixture("search_users.json").value("users"_L1));
	QVERIFY(!users.isEmpty());
	QCOMPARE(users[0].followState.isEmpty(), false);
	QCOMPARE(PathSegment(u"a b/ج"_s), u"a%20b%2F%D8%AC"_s);
}

void TestModels::stickerPacks() {
	const auto pack = StickerPack::FromJson(Test::Json(R"({
		"id":"pk1","slug":"cats","title":"Cats","cover_url":"https://cdn/c.webp","default":true,"installed":false,
		"stickers":[{"id":"s1","url":"https://cdn/1.webp","width":512,"height":512,"emojis":["😺","😸"]},{"id":"s2","url":"https://cdn/2.webp","width":512,"height":400,"emoji":"😾"}]})"));
	QCOMPARE(pack.slug, u"cats"_s);
	QVERIFY(pack.isDefault && !pack.installed);
	QCOMPARE(pack.stickers.size(), 2);
	QCOMPARE(pack.stickers[0].pack, u"pk1"_s);     // filled from the pack
	QCOMPARE(pack.stickers[0].emoji, u"😺"_s);      // the first of `emojis`
	QCOMPARE(pack.stickers[1].emoji, u"😾"_s);
	const auto again = StickerPack::FromJson(pack.toJson());
	QCOMPARE(again.stickers.size(), 2);
	QCOMPARE(again.stickers[1].height, 400);
	QCOMPARE(again.coverUrl, pack.coverUrl);
}

void TestModels::modelsSurviveTheRealtimeCodec() {
	// The same views arrive over the connection as protobuf: encode the
	// HTTP fixtures, decode them, and the models must come out the same.
	const auto page = Test::Fixture("history_c1.json");
	const auto bytes = Rt::Proto::Encode("ChatHistoryResponse"_L1, QJsonObject{ { u"body"_s, page } });
	QVERIFY(bytes.has_value());
	const auto decoded = Rt::Proto::Decode("ChatHistoryResponse"_L1, *bytes);
	QVERIFY(decoded.has_value());
	const auto back = Rt::Proto::UnwrapBody(*decoded).toObject();
	const auto direct = Json::List<Message>(page.value("messages"_L1));
	const auto viaWire = Json::List<Message>(back.value("messages"_L1));
	QCOMPARE(viaWire.size(), direct.size());
	for (auto i = 0; i != direct.size(); ++i) {
		QCOMPARE(viaWire[i].toJson(), direct[i].toJson());
	}
	const auto infoDirect = ChatInfo::FromJson(page.value("chat"_L1).toObject());
	const auto infoWire = ChatInfo::FromJson(back.value("chat"_L1).toObject());
	QCOMPARE(infoWire.toJson(), infoDirect.toJson());

	const auto inbox = Test::Fixture("inbox_conversation.json");
	const auto inboxBytes = Rt::Proto::Encode("ChatInboxResponse"_L1, inbox);
	const auto inboxBack = Rt::Proto::Decode("ChatInboxResponse"_L1, inboxBytes.value()).value();
	const auto entryDirect = InboxEntry::FromJson(inbox.value("chats"_L1).toArray()[0].toObject());
	const auto entryWire = InboxEntry::FromJson(inboxBack.value("chats"_L1).toArray()[0].toObject());
	QCOMPARE(entryWire.chatInfo->toJson(), entryDirect.chatInfo->toJson());
	QCOMPARE(entryWire.lastMessage->toJson(), entryDirect.lastMessage->toJson());
	QCOMPARE(entryWire.unread, entryDirect.unread);
	QCOMPARE(entryWire.section, entryDirect.section);
	QVERIFY(entryWire.chatInfo->conversation->listener.has_value());

	// A conversation without a listener: the codec restores `speaker` (an
	// always-key) but must not invent the other side.
	const auto waiting = Test::Json(R"({"body":{"id":"x","topic":"t","category":"heart","state":"open","expires_at":1700000000,"speaker":{"ref":"p","role":"speaker","masked":true,"me":true}}})");
	const auto conv = Conversation::FromJson(Rt::Proto::UnwrapBody(
		Rt::Proto::Decode("ConvGetResponse"_L1, Rt::Proto::Encode("ConvGetResponse"_L1, waiting).value()).value()).toObject());
	QVERIFY(!conv.listener.has_value());
	QVERIFY(conv.speaker.masked);

	const auto users = Test::Fixture("users.json");
	const auto usersBack = Rt::Proto::Decode("UsersCardsResponse"_L1, Rt::Proto::Encode("UsersCardsResponse"_L1, users).value()).value();
	const auto cardsDirect = Json::List<User>(users.value("users"_L1));
	const auto cardsWire = Json::List<User>(usersBack.value("users"_L1));
	QCOMPARE(cardsWire, cardsDirect);
}

void TestModels::blurhash() {
	const auto image = BlurhashImage(u"LKO2?U%2Tw=w]~RBVZRi};RPxuwH"_s);
	QVERIFY(!image.isNull());
	QCOMPARE(image.size(), QSize(32, 32));
	// The DC component is the average colour: every pixel is close to it for a 1x1 hash.
	const auto flat = BlurhashImage(u"00OZZy"_s, 4, 4);
	QVERIFY(!flat.isNull());
	QCOMPARE(flat.pixel(0, 0), flat.pixel(3, 3));
	QVERIFY(BlurhashImage(u"short"_s).isNull());
	QVERIFY(BlurhashImage(u"LKO2?U%2Tw=w]~RBVZRi};RPxuw"_s).isNull());  // wrong length
	QVERIFY(BlurhashImage(u"LKO2?U%2Tw=w]~RBVZRi};RPxuéH"_s).isNull()); // not base-83
	QVERIFY(BlurhashImage(QString()).isNull());
}

QTEST_GUILESS_MAIN(TestModels)

#include "test_models.moc"
