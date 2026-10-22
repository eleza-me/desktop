// The widget-free rules of the chat list (dialogs/dialogs_logic.h): folders,
// order, search, previews and delivery marks.
#include "dialogs/dialogs_logic.h"
#include "support/chat_text.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"

#include <QtTest/QtTest>

class TestDialogsLogic : public QObject {
	Q_OBJECT

private:
	static QString U(const char *utf8) {
		return QString::fromUtf8(utf8);
	}
	static Data::Chat Chat(const char *id, qint64 activity, bool pinned = false) {
		auto result = Data::Chat();
		result.id = QString::fromLatin1(id);
		result.section = QStringLiteral("message");
		result.lastActivityAt = activity;
		result.pinned = pinned;
		result.peerUserId = QStringLiteral("u_") + result.id;
		return result;
	}
	static Data::Message Text(const char *utf8, qint64 seq = 5) {
		auto result = Data::Message();
		result.kind = Data::MessageKind::Text;
		result.text = U(utf8);
		result.seq = seq;
		return result;
	}
	static QStringList Ids(const QVector<Data::Chat> &chats) {
		auto result = QStringList();
		for (const auto &chat : chats) {
			result.push_back(chat.id);
		}
		return result;
	}

private Q_SLOTS:
	void init() {
		Lang::SetCurrent(Lang::Language::Persian);
	}

	void folders() {
		auto chat = Chat("a", 10);
		QVERIFY(Dialogs::InFolder(chat, Dialogs::Folder::Main));
		QVERIFY(!Dialogs::InFolder(chat, Dialogs::Folder::Archived));
		chat.archived = true;
		QVERIFY(!Dialogs::InFolder(chat, Dialogs::Folder::Main));
		QVERIFY(Dialogs::InFolder(chat, Dialogs::Folder::Archived));
		// A request is a request even when it is archived.
		chat.section = QStringLiteral("request");
		QVERIFY(Dialogs::InFolder(chat, Dialogs::Folder::Requests));
		QVERIFY(!Dialogs::InFolder(chat, Dialogs::Folder::Archived));
		auto incoming = Chat("b", 10);
		incoming.request = QStringLiteral("incoming");
		QVERIFY(Dialogs::InFolder(incoming, Dialogs::Folder::Requests));
		// My own outgoing request stays in the main list.
		auto outgoing = Chat("c", 10);
		outgoing.request = QStringLiteral("outgoing");
		QVERIFY(Dialogs::InFolder(outgoing, Dialogs::Folder::Main));
		// Conversations live in the main list too.
		auto conversation = Chat("d", 10);
		conversation.isConversation = true;
		conversation.section = QStringLiteral("conversation");
		QVERIFY(Dialogs::InFolder(conversation, Dialogs::Folder::Main));
	}

	void sortTimeUsesDraft() {
		auto chat = Chat("a", 1'700'000'000'000);
		QCOMPARE(Dialogs::SortTime(chat), qint64(1'700'000'000'000));
		auto draft = Data::Draft();
		draft.text = U("سلام");
		draft.updatedAt = 1'700'000'500'000; // milliseconds (a local draft)
		chat.draft = draft;
		QCOMPARE(Dialogs::SortTime(chat), qint64(1'700'000'500'000));
		chat.draft->updatedAt = 1'700'000'900; // seconds (from the server)
		QCOMPARE(Dialogs::SortTime(chat), qint64(1'700'000'900'000));
		chat.draft->updatedAt = 1'600'000'000; // older than the last message
		QCOMPARE(Dialogs::SortTime(chat), qint64(1'700'000'000'000));
		// An empty draft does not count.
		chat.draft->text = QString();
		chat.draft->updatedAt = 1'800'000'000'000;
		QCOMPARE(Dialogs::SortTime(chat), qint64(1'700'000'000'000));
	}

	void order() {
		auto chats = QVector<Data::Chat>{
			Chat("old", 1'700'000'100'000),
			Chat("new", 1'700'000'300'000),
			Chat("pin_b", 1'700'000'050'000, true),
			Chat("pin_a", 1'700'000'200'000, true),
			Chat("mid", 1'700'000'200'000),
		};
		// A draft written after the last message lifts the chat.
		auto drafted = Chat("drafted", 1'700'000'010'000);
		auto draft = Data::Draft();
		draft.text = U("پیش‌نویس");
		draft.updatedAt = 1'700'000'250'000;
		drafted.draft = draft;
		chats.push_back(drafted);

		Dialogs::SortChats(chats, {});
		QCOMPARE(Ids(chats), QStringList({
			QStringLiteral("pin_a"),
			QStringLiteral("pin_b"),
			QStringLiteral("new"),
			QStringLiteral("drafted"),
			QStringLiteral("mid"),
			QStringLiteral("old"),
		}));

		// The user's own order of the pinned chats wins over the time; a
		// pinned chat the order does not know goes after the known ones.
		chats.push_back(Chat("pin_c", 1'700'000'900'000, true));
		Dialogs::SortChats(chats, {
			QStringLiteral("pin_b"),
			QStringLiteral("gone"),
			QStringLiteral("pin_a"),
		});
		QCOMPARE(Ids(chats).mid(0, 3), QStringList({
			QStringLiteral("pin_b"),
			QStringLiteral("pin_a"),
			QStringLiteral("pin_c"),
		}));
	}

	void movePinned() {
		const auto order = QStringList({
			QStringLiteral("a"),
			QStringLiteral("b"),
			QStringLiteral("c"),
		});
		QCOMPARE(Dialogs::MovePinned(order, 2, 0), QStringList({
			QStringLiteral("c"),
			QStringLiteral("a"),
			QStringLiteral("b"),
		}));
		QCOMPARE(Dialogs::MovePinned(order, 0, 9), QStringList({
			QStringLiteral("b"),
			QStringLiteral("c"),
			QStringLiteral("a"),
		}));
		QCOMPARE(Dialogs::MovePinned(order, 5, 0), order);
		QCOMPARE(Dialogs::MovePinned(order, 1, 1), order);
	}

	void normalize() {
		// Arabic yeh and kaf, a zero-width non-joiner, Persian digits.
		QCOMPARE(
			Dialogs::NormalizeQuery(U("  @Ali  ")),
			QStringLiteral("ali"));
		QCOMPARE(
			Dialogs::NormalizeQuery(U("\xD9\x83\xD8\xB1\xD9\x8A\xD9\x85\xD9\x8A")),
			U("\xDA\xA9\xD8\xB1\xDB\x8C\xD9\x85\xDB\x8C"));
		QCOMPARE(
			Dialogs::NormalizeQuery(U("می\xE2\x80\x8Cبینم ۱۲٣")),
			U("میبینم 123"));
	}

	void search() {
		auto chat = Chat("a", 10);
		chat.lastMessage = Text("فردا ساعت ۵ می‌بینمت");
		const auto title = U("علی رضایی");
		const auto handle = QStringLiteral("ali.r");
		const auto match = [&](const char *query) {
			return Dialogs::MatchesQuery(
				chat,
				title,
				handle,
				Dialogs::NormalizeQuery(U(query)));
		};
		QVERIFY(match(""));
		QVERIFY(match("رضا"));
		QVERIFY(match("@ALI"));
		QVERIFY(match("ساعت 5"));
		QVERIFY(match("میبینمت"));
		QVERIFY(!match("سارا"));
		auto draft = Data::Draft();
		draft.text = U("یادم نره نان بخرم");
		chat.draft = draft;
		QVERIFY(match("نان"));
	}

	void ticks() {
		auto chat = Chat("a", 10);
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::None);
		chat.lastMessage = Text("سلام", 7);
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::None); // not mine
		chat.lastMine = true;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::Sent);
		chat.peerReadSeq = 7;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::Read);
		chat.lastMessage->status = Data::MessageStatus::Failed;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::Failed);
		chat.lastMessage->status = Data::MessageStatus::Sending;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::Pending);
		// Still in the outbox: no seq yet.
		chat.lastMessage->status = Data::MessageStatus::Sent;
		chat.lastMessage->seq = 0;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::Pending);
		// A draft hides the mark; «Saved messages» never has one.
		chat.lastMessage->seq = 7;
		auto draft = Data::Draft();
		draft.text = U("…");
		chat.draft = draft;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::None);
		chat.draft.reset();
		chat.saved = true;
		QCOMPARE(Dialogs::TickFor(chat), Dialogs::Tick::None);
	}

	void previews() {
		auto chat = Chat("a", 10);
		QCOMPARE(
			Dialogs::PreviewFor(chat).kind,
			Dialogs::Preview::Kind::Empty);

		chat.lastMessage = Text("سلام\nچطوری؟");
		auto preview = Dialogs::PreviewFor(chat);
		QCOMPARE(preview.kind, Dialogs::Preview::Kind::Message);
		QVERIFY(preview.prefix.isEmpty());
		QCOMPARE(preview.text, U("سلام چطوری؟")); // one line

		chat.lastMine = true;
		preview = Dialogs::PreviewFor(chat);
		QCOMPARE(preview.prefix, U("تو:"));

		// A photo without a caption: the kind label, with a thumbnail.
		auto photo = Data::Message();
		photo.kind = Data::MessageKind::Photo;
		photo.attachment = QStringLiteral("media1");
		photo.seq = 6;
		chat.lastMessage = photo;
		chat.lastMine = false;
		preview = Dialogs::PreviewFor(chat);
		QCOMPARE(preview.kind, Dialogs::Preview::Kind::Media);
		QVERIFY(preview.thumbnail);
		QCOMPARE(preview.text, tr::lng_media_photo(tr::now));
		// With a caption the thumbnail stands for the kind.
		chat.lastMessage->text = U("غروب امروز");
		preview = Dialogs::PreviewFor(chat);
		QCOMPARE(preview.kind, Dialogs::Preview::Kind::Message);
		QCOMPARE(preview.text, U("غروب امروز"));
		// A hidden (spoiler) photo shows no thumbnail.
		chat.lastMessage->mediaSpoiler = true;
		QVERIFY(!Dialogs::PreviewFor(chat).thumbnail);

		// A file shows its name after the label.
		auto file = Data::Message();
		file.kind = Data::MessageKind::File;
		file.attachment = QStringLiteral("media2");
		file.text = QStringLiteral("plan.pdf");
		chat.lastMessage = file;
		preview = Dialogs::PreviewFor(chat);
		QVERIFY(!preview.thumbnail);
		QVERIFY(preview.text.contains(QStringLiteral("plan.pdf")));
		QVERIFY(preview.text.startsWith(tr::lng_media_file(tr::now)));
		// The sender's direction override does not disguise the extension
		// ("invoice<RLO>fdp.exe" would read "invoiceexe.pdf"), nor reorder
		// the words of ours around a text.
		chat.lastMessage->text = QStringLiteral("invoice") + QChar(0x202E) + QStringLiteral("fdp.exe");
		preview = Dialogs::PreviewFor(chat);
		QVERIFY(preview.text.endsWith(QStringLiteral("invoicefdp.exe")));
		QVERIFY(!preview.text.contains(QChar(0x202E)));
		chat.lastMessage = Text("hello");
		chat.lastMessage->text = QChar(0x202E) + QStringLiteral("olleh") + QChar(0x2069) + QChar(0x2066);
		preview = Dialogs::PreviewFor(chat);
		QCOMPARE(preview.text, QStringLiteral("olleh"));

		// A draft wins over the last message, typing wins over both.
		auto draft = Data::Draft();
		draft.text = U("  فردا\nمی‌آیم ");
		chat.draft = draft;
		preview = Dialogs::PreviewFor(chat);
		QCOMPARE(preview.kind, Dialogs::Preview::Kind::Draft);
		QCOMPARE(preview.prefix, tr::lng_dialogs_draft(tr::now));
		QCOMPARE(preview.text, U("فردا می‌آیم"));
		preview = Dialogs::PreviewFor(chat, QStringLiteral("recording_voice"));
		QCOMPARE(preview.kind, Dialogs::Preview::Kind::Activity);
		QCOMPARE(preview.text, tr::lng_action_recording_voice(tr::now));
		// "cancel" is not an activity.
		QCOMPARE(
			Dialogs::PreviewFor(chat, QStringLiteral("cancel")).kind,
			Dialogs::Preview::Kind::Draft);
		// A deleted last message leaves nothing to show.
		chat.draft.reset();
		chat.lastMessage->deleted = true;
		QCOMPARE(
			Dialogs::PreviewFor(chat).kind,
			Dialogs::Preview::Kind::Empty);
	}

	void noteNames() {
		auto note = Data::Message();
		note.chat = QStringLiteral("j1group");
		note.kind = Data::MessageKind::System;
		note.system = QStringLiteral("member_added:u_ali");
		note.sender = QStringLiteral("u_sara");
		note.seq = 7;
		const auto person = [](const QString &name, const char *id, bool me = false) {
			auto result = Support::NotePerson();
			result.name = name;
			result.userId = QString::fromLatin1(id);
			result.me = me;
			result.linkable = !me;
			return result;
		};
		const auto check = [&](const Support::NoteLine &line, const QMap<QString, QString> &expected) {
			QCOMPARE(line.names.size(), expected.size());
			for (const auto &name : line.names) {
				QVERIFY(expected.contains(name.userId));
				QCOMPARE(line.text.mid(name.offset, name.length), expected.value(name.userId));
			}
		};
		for (const auto language : { Lang::Language::Persian, Lang::Language::English }) {
			Lang::SetCurrent(language);
			// Who did it and whom it names: both open a profile, wherever
			// the language puts them.
			auto line = Support::GroupNoteLine(note, person(U("سارا"), "u_sara"), person(QStringLiteral("Ali"), "u_ali"));
			check(line, { { QStringLiteral("u_sara"), U("سارا") }, { QStringLiteral("u_ali"), QStringLiteral("Ali") } });
			// Two people of the same name are told apart by where they are.
			line = Support::GroupNoteLine(note, person(U("رضا"), "u_sara"), person(U("رضا"), "u_ali"));
			QCOMPARE(line.names.size(), 2);
			QVERIFY(line.names[0].offset != line.names[1].offset);
			check(line, { { QStringLiteral("u_sara"), U("رضا") }, { QStringLiteral("u_ali"), U("رضا") } });
			// I am never a link: a note about me speaks to me.
			line = Support::GroupNoteLine(note, person(U("من"), "u_me", true), person(U("علی"), "u_ali"));
			check(line, { { QStringLiteral("u_ali"), U("علی") } });
			auto toMe = note;
			toMe.system = QStringLiteral("member_added:u_me");
			line = Support::GroupNoteLine(toMe, person(U("سارا"), "u_sara"), person(U("من"), "u_me", true));
			check(line, { { QStringLiteral("u_sara"), U("سارا") } });
			// A deleted account is named, without a link.
			auto deleted = person(tr::lng_deleted_account(tr::now), "u_gone");
			deleted.linkable = false;
			auto left = note;
			left.system = QStringLiteral("member_left");
			line = Support::GroupNoteLine(left, deleted, Support::NotePerson());
			QVERIFY(line.names.isEmpty());
			QVERIFY(line.text.contains(tr::lng_deleted_account(tr::now)));
		}
		Lang::SetCurrent(Lang::Language::Persian);
		// Not a note of a group: nothing.
		auto text = Text("hello");
		QVERIFY(Support::GroupNoteLine(text, person(U("سارا"), "u_sara"), Support::NotePerson()).text.isEmpty());
	}

	void activityWording() {
		QVERIFY(Dialogs::ActivityText(QString()).isEmpty());
		QCOMPARE(
			Dialogs::ActivityText(QStringLiteral("typing")),
			tr::lng_action_typing(tr::now));
		QCOMPARE(
			Dialogs::ActivityText(QStringLiteral("uploading_photo")),
			tr::lng_action_sending_photo(tr::now));
		// A kind this build does not know still shows that something happens.
		QCOMPARE(
			Dialogs::ActivityText(QStringLiteral("choosing_sticker")),
			tr::lng_action_typing(tr::now));
	}

	void deleteAndPinRules() {
		auto chat = Chat("a", 10);
		QVERIFY(Dialogs::CanDeleteForBoth(chat));
		QVERIFY(Dialogs::CanPin(chat));
		auto saved = chat;
		saved.saved = true;
		QVERIFY(!Dialogs::CanDeleteForBoth(saved));
		auto bot = chat;
		bot.isBot = true;
		QVERIFY(!Dialogs::CanDeleteForBoth(bot));
		auto conversation = chat;
		conversation.isConversation = true;
		QVERIFY(!Dialogs::CanDeleteForBoth(conversation));
		QVERIFY(!Dialogs::CanPin(conversation));
		auto request = chat;
		request.section = QStringLiteral("request");
		QVERIFY(!Dialogs::CanPin(request));
	}

	void muteDurations() {
		QCOMPARE(
			Dialogs::MuteDurationText(QStringLiteral("forever")),
			tr::lng_dialogs_mute_forever(tr::now));
		Lang::SetCurrent(Lang::Language::English);
		QCOMPARE(
			Dialogs::MuteDurationText(QStringLiteral("8h")),
			QStringLiteral("For 8 hours"));
		QCOMPARE(
			Dialogs::MuteDurationText(QStringLiteral("3w")),
			QStringLiteral("3w"));
	}

};

QTEST_GUILESS_MAIN(TestDialogsLogic)
#include "test_dialogs_logic.moc"
