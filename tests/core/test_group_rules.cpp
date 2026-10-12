// The decisions of groups (data/data_group_rules.h): the pts rule, the answer
// of a send, the catch-up, unread, service notes, the rights table (the
// cases of the server's own acceptance tables), invite links and mentions.
#include "data/data_group_rules.h"

#include <QtCore/QMap>
#include <QtTest/QtTest>

#include <random>

using namespace Data;
using GroupRules::FrameAction;
using GroupRules::SendAction;
using GroupRules::SyncAction;
using GroupRules::Note;

namespace {

const auto kToken = u"j18P1DKWNqdem.6LSbibQJp9aVabUoyMbHMQ"_s;
const auto kToken2 = u"j18P1DKWNqdem.f1JPFW_Fy3eB7tjzx-2KnQ"_s;
const auto kBase = u"eleza.me/g/"_s;
const auto kGrin = QString::fromUcs4(U"\U0001F600");                       // 2 units
const auto kFamily = QString::fromUcs4(U"\U0001F468‍\U0001F469‍\U0001F467"); // 8 units

using Cache = QMap<qint64, QString>;

// A group on the server: messages by seq, each with the pts of its last change.
struct Server {
	qint64 pts = 0;
	Cache text;
	QMap<qint64, qint64> changedAt;

	void change(qint64 seq, const QString &now) {
		++pts;
		text.insert(seq, now);
		changedAt.insert(seq, pts);
	}
	// The messages whose last change is after `from`, as they are now.
	[[nodiscard]] QVector<QPair<qint64, QString>> difference(qint64 from) const {
		auto result = QVector<QPair<qint64, QString>>();
		for (auto i = changedAt.cbegin(); i != changedAt.cend(); ++i) {
			if (i.value() > from) {
				result.push_front({ i.key(), text.value(i.key()) });
			}
		}
		return result;
	}
};

// A client: its cache and the pts applied to it, following the rules.
struct Client {
	qint64 pts = 0;
	Cache cache;
	int differences = 0;

	void catchUp(const Server &server) {
		for (const auto &[seq, text] : server.difference(pts)) {
			cache.insert(seq, text);
		}
		pts = server.pts;
		++differences;
	}
	// `text` null: a frame without data.
	void frame(const Server &server, qint64 framePts, qint64 seq, const QString &text) {
		switch (GroupRules::FrameActionFor(pts, framePts, !text.isNull())) {
		case FrameAction::Apply:
			cache.insert(seq, text);
			pts = framePts;
			break;
		case FrameAction::CatchUp:
			catchUp(server);
			break;
		case FrameAction::Drop:
			break;
		}
	}
};

[[nodiscard]] GroupRights::Member Row(
		const QString &status,
		const QString &role,
		int granted = 0,
		int denied = 0,
		bool restricted = false,
		const QString &user = QString()) {
	auto result = GroupRights::Member();
	result.user = user;
	result.status = status;
	result.role = role;
	result.granted = granted;
	result.denied = denied;
	result.restricted = restricted;
	return result;
}

struct NamedAction {
	const char *name;
	GroupRights::Action action;
};

[[nodiscard]] QString Allowed(int defaults, const GroupRights::Member &member) {
	using namespace GroupRights;
	const NamedAction actions[] = {
		{ "member", kActMember },
		{ "send_text", ActSend(kSendMessages) },
		{ "send_media", ActSend(kSendMessages | kSendMedia) },
		{ "send_links", ActSend(kSendMessages | kSendLinks) },
		{ "add_members", kActAddMembers },
		{ "pin", kActPin },
		{ "change_info", kActChangeInfo },
		{ "delete_messages", kActDeleteMessages },
		{ "ban", kActBan },
		{ "invites", kActInvites },
		{ "add_admins", kActAddAdmins },
		{ "history", kActHistory },
		{ "owner", kActOwner },
	};
	auto result = QStringList();
	for (const auto &entry : actions) {
		if (Can(defaults, member, entry.action)) {
			result.push_back(QString::fromLatin1(entry.name));
		}
	}
	return result.join(u',');
}

} // namespace

class TestGroupRules final : public QObject {
	Q_OBJECT

private Q_SLOTS:
	void groupIds();
	void frameRule();
	void sendAnswer();
	void syncEntry();
	void unreadAndReadTarget();
	void serviceNotes();
	void listRowAndColors();
	void deleteAndComposer();
	void membersAndTitles();
	void framesInOrderNeedNoDifference();
	void lostDoubledAndReorderedFramesConverge();
	void ownSendAnswerAndFrameInEitherOrder();

	void rightsNamesAndMasks();
	void permissionsTable();
	void canTable();
	void canActOnTable();
	void rowActions();
	void messageNeeds();

	void inviteTokenShape();
	void inviteLinkForms();
	void inviteLinkTail();
	void inviteLinkServerPrefix();
	void inviteLinkNotAnInvite();

	void mentionQuery();
	void mentionLabelAndRoom();
	void mentionOffsetsAreUtf16();
	void mentionClipAndAddressing();

};

void TestGroupRules::groupIds() {
	QVERIFY(IsGroupId(u"j165ZW9qzo6KQ"));
	QVERIFY(!IsGroupId(u"c1"));
	QVERIFY(!IsGroupId(u"j"));
	QVERIFY(!IsGroupId(u""));
	QVERIFY(!IsGroupId(u"u1j"));
}

void TestGroupRules::frameRule() {
	// The next change with its data is applied.
	QCOMPARE(GroupRules::FrameActionFor(6, 7, true), FrameAction::Apply);
	QCOMPARE(GroupRules::FrameActionFor(0, 1, true), FrameAction::Apply);
	// At or below the stored pts: dropped, with or without data.
	QCOMPARE(GroupRules::FrameActionFor(6, 6, true), FrameAction::Drop);
	QCOMPARE(GroupRules::FrameActionFor(6, 2, true), FrameAction::Drop);
	QCOMPARE(GroupRules::FrameActionFor(6, 6, false), FrameAction::Drop);
	// A gap, or the very next change without data: the difference.
	QCOMPARE(GroupRules::FrameActionFor(6, 8, true), FrameAction::CatchUp);
	QCOMPARE(GroupRules::FrameActionFor(6, 100, true), FrameAction::CatchUp);
	QCOMPARE(GroupRules::FrameActionFor(6, 7, false), FrameAction::CatchUp);
	// 8 arrives before 7: a gap, the difference brings both; then 7 is old.
	QCOMPARE(GroupRules::FrameActionFor(8, 7, true), FrameAction::Drop);
	QCOMPARE(GroupRules::FrameActionFor(8, 9, true), FrameAction::Apply);
}

void TestGroupRules::sendAnswer() {
	QCOMPARE(GroupRules::SendActionFor(2, 3, false, false), SendAction::Advance);
	// The live frame of my own message came first.
	QCOMPARE(GroupRules::SendActionFor(3, 3, false, true), SendAction::Keep);
	QCOMPARE(GroupRules::SendActionFor(5, 3, false, true), SendAction::Keep);
	// Something else happened in between.
	QCOMPARE(GroupRules::SendActionFor(2, 5, false, false), SendAction::CatchUp);
	// A retry of a message the server already had: its pts says nothing.
	QCOMPARE(GroupRules::SendActionFor(9, 3, true, true), SendAction::Keep);
	QCOMPARE(GroupRules::SendActionFor(2, 3, true, true), SendAction::Keep);
	QCOMPARE(GroupRules::SendActionFor(2, 3, true, false), SendAction::CatchUp);
	QCOMPARE(GroupRules::SendActionFor(9, 3, true, false), SendAction::CatchUp);
}

void TestGroupRules::syncEntry() {
	QCOMPARE(GroupRules::SyncActionFor(-1, -1, 12, false), SyncAction::Take);
	QCOMPARE(GroupRules::SyncActionFor(6, 6, 9, false), SyncAction::Take);
	QCOMPARE(GroupRules::SyncActionFor(6, 6, 9, true), SyncAction::Behind);
	// The same pts (only my own state differed): the header is written.
	QCOMPARE(GroupRules::SyncActionFor(6, 6, 6, true), SyncAction::Behind);
	// Sent 6; frames 7..10 were applied meanwhile; the answer knows 8.
	QCOMPARE(GroupRules::SyncActionFor(6, 10, 8, true), SyncAction::Skip);
	QCOMPARE(GroupRules::SyncActionFor(6, 10, 8, false), SyncAction::Skip);
	QCOMPARE(GroupRules::SyncActionFor(6, 10, 10, true), SyncAction::Behind);
	QCOMPARE(GroupRules::SyncActionFor(6, 10, 12, false), SyncAction::Take);
	// A pts the group never had (a restored server, a corrupted cache).
	QCOMPARE(GroupRules::SyncActionFor(40, 40, 7, true), SyncAction::Rewind);
	QCOMPARE(GroupRules::SyncActionFor(40, 40, 7, false), SyncAction::Take);
}

void TestGroupRules::unreadAndReadTarget() {
	using GroupRules::Change;
	const auto changes = QVector<Change>{
		{ 4, true, false, false },  // new, above the head: counts
		{ 5, true, true, false },   // mine
		{ 6, true, false, true },   // deleted for everyone meanwhile
		{ 3, false, false, false }, // an edit of a cached message
		{ 2, true, false, false },  // below the head the row knew: counted already
	};
	QCOMPARE(GroupRules::UnreadFromDifference(changes, 3, 1), 1);
	const auto two = QVector<Change>{ { 4, true, false, false }, { 5, true, false, false } };
	QCOMPARE(GroupRules::UnreadFromDifference(two, 3, 4), 1);
	QCOMPARE(GroupRules::UnreadFromDifference(two, 3, 5), 0);
	QCOMPARE(GroupRules::UnreadFromDifference({}, 3, 0), 0);
	// After a catch-up set last_seq = 20 the difference that fills 16..20 adds nothing.
	auto filled = QVector<Change>();
	for (auto seq = qint64(16); seq <= 20; ++seq) {
		filled.push_back({ seq, true, false, false });
	}
	QCOMPARE(GroupRules::UnreadFromDifference(filled, 20, 15), 0);

	QCOMPARE(GroupRules::ReadTarget(7, 4, 9), qint64(7));
	QCOMPARE(GroupRules::ReadTarget(12, 4, 9), qint64(9));
	QCOMPARE(GroupRules::ReadTarget(4, 4, 9), qint64(0));
	QCOMPARE(GroupRules::ReadTarget(3, 4, 9), qint64(0));
	QCOMPARE(GroupRules::ReadTarget(0, 0, 0), qint64(0));
}

void TestGroupRules::serviceNotes() {
	QCOMPARE(GroupRules::NoteOf(u"group_created"_s), Note::Created);
	QCOMPARE(GroupRules::NoteOf(u"member_added:u1L8ksXclPN5j"_s), Note::Added);
	QCOMPARE(GroupRules::NoteOf(u"member_left"_s), Note::Left);
	QCOMPARE(GroupRules::NoteOf(u"member_removed:u1HYvJ4JMtOjW"_s), Note::Removed);
	QCOMPARE(GroupRules::NoteOf(u"title_changed"_s), Note::TitleChanged);
	QCOMPARE(GroupRules::NoteOf(u"photo_changed"_s), Note::PhotoChanged);
	QCOMPARE(GroupRules::NoteOf(u"member_joined"_s), Note::Joined);
	QCOMPARE(GroupRules::NoteOf(u"pinned:12"_s), Note::Pinned);
	// A code newer than this build, a broken pin, nothing at all.
	QCOMPARE(GroupRules::NoteOf(u"something_newer:x"_s), Note::None);
	QCOMPARE(GroupRules::NoteOf(u"pinned:x"_s), Note::None);
	QCOMPARE(GroupRules::NoteOf(u"pinned"_s), Note::None);
	QCOMPARE(GroupRules::NoteOf(QString()), Note::None);

	QCOMPARE(GroupRules::NoteUser(u"member_added:u1L8ksXclPN5j"_s), u"u1L8ksXclPN5j"_s);
	QCOMPARE(GroupRules::NoteUser(u"member_removed:u1HYvJ4JMtOjW"_s), u"u1HYvJ4JMtOjW"_s);
	QVERIFY(GroupRules::NoteUser(u"member_left"_s).isEmpty());
	QVERIFY(GroupRules::NoteUser(u"member_added:"_s).isEmpty());
	QVERIFY(GroupRules::NoteUser(u"title_changed:x"_s).isEmpty());

	QCOMPARE(GroupRules::NotePinnedSeq(u"pinned:12"_s), qint64(12));
	QCOMPARE(GroupRules::NotePinnedSeq(u"pinned:0"_s), qint64(0));
	QCOMPARE(GroupRules::NotePinnedSeq(u"pinned:-3"_s), qint64(0));
	QCOMPARE(GroupRules::NotePinnedSeq(u"member_added:12"_s), qint64(0));

	QCOMPARE(GroupRules::MemberDelta(u"member_added:u1"_s), 1);
	QCOMPARE(GroupRules::MemberDelta(u"member_joined"_s), 1);
	QCOMPARE(GroupRules::MemberDelta(u"member_left"_s), -1);
	QCOMPARE(GroupRules::MemberDelta(u"member_removed:u1"_s), -1);
	QCOMPARE(GroupRules::MemberDelta(u"group_created"_s), 0);
	QCOMPARE(GroupRules::MemberDelta(u"title_changed"_s), 0);
	QCOMPARE(GroupRules::MemberDelta(u"something_newer:x"_s), 0);

	QVERIFY(GroupRules::HeaderChanged(u"title_changed"_s));
	QVERIFY(GroupRules::HeaderChanged(u"photo_changed"_s));
	QVERIFY(!GroupRules::HeaderChanged(u"member_left"_s));
}

void TestGroupRules::listRowAndColors() {
	QCOMPARE(GroupRules::FirstName(u"سارا احمدی"_s), u"سارا"_s);
	QCOMPARE(GroupRules::FirstName(u"  Sara  Ahmadi "_s), u"Sara"_s);
	QCOMPARE(GroupRules::FirstName(u"Sara"_s), u"Sara"_s);
	QVERIFY(GroupRules::FirstName(u"   "_s).isEmpty());
	QCOMPARE(int(GroupRules::FirstName(u"abcdefghijklmnopqrstuvwxyz0123456789"_s).size()), 24);
	// Never cut inside a surrogate pair.
	const auto cut = GroupRules::FirstName(kGrin.repeated(30));
	QCOMPARE(int(cut.size()), 48);
	QVERIFY(!cut.back().isHighSurrogate());

	// The same id takes the same colour, on every platform.
	QCOMPARE(GroupRules::NameColorIndex(u"u1G7Y5pXOKUSs", 8), GroupRules::NameColorIndex(u"u1G7Y5pXOKUSs", 8));
	QCOMPARE(GroupRules::NameColorIndex(u"", 8), 0);
	QCOMPARE(GroupRules::NameColorIndex(u"u1", 1), 0);
	QCOMPARE(GroupRules::NameColorIndex(u"a", 8), int(0xE40C292CU % 8U)); // FNV-1a of "a"
	for (const auto id : { u"u1", u"u2", u"u1G7Y5pXOKUSs", u"u1F0syPDjdfAz" }) {
		const auto index = GroupRules::NameColorIndex(id, 7);
		QVERIFY(index >= 0 && index < 7);
	}
}

void TestGroupRules::deleteAndComposer() {
	using namespace GroupRules;
	const auto now = qint64(1'800'000'000'000);
	QVERIFY(CanDeleteForEveryone(true, false, 5, now - 1000, now, 0, false));
	QVERIFY(!CanDeleteForEveryone(false, false, 5, now - 1000, now, 0, false));
	QVERIFY(CanDeleteForEveryone(false, false, 5, now - 1000, now, 0, true));
	QVERIFY(!CanDeleteForEveryone(true, true, 5, now, now, 0, true));  // a service note
	QVERIFY(!CanDeleteForEveryone(true, false, 0, now, now, 0, true)); // not sent yet
	// The server sets a window for one's own messages.
	QVERIFY(CanDeleteForEveryone(true, false, 5, now - 59'000, now, 60'000, false));
	QVERIFY(!CanDeleteForEveryone(true, false, 5, now - 60'000, now, 60'000, false));
	QVERIFY(CanDeleteForEveryone(true, false, 5, 0, now, 60'000, false)); // an unknown time: the server decides
	QVERIFY(CanDeleteForEveryone(false, false, 5, now - 600'000, now, 60'000, true));

	QCOMPARE(ComposerFor(true, QString(), 0, now), Composer::Compose);
	QCOMPARE(ComposerFor(false, u"restricted"_s, 0, now), Composer::Restricted);
	QCOMPARE(ComposerFor(false, QString(), 0, now), Composer::Restricted);
	QCOMPARE(ComposerFor(false, u"slow_mode"_s, now + 5000, now), Composer::Wait);
	QCOMPARE(ComposerFor(false, u"slow_mode"_s, now, now), Composer::Compose);
	QCOMPARE(WaitSeconds(now + 12'000, now), qint64(12));
	QCOMPARE(WaitSeconds(now + 11'001, now), qint64(12));
	QCOMPARE(WaitSeconds(now - 1, now), qint64(0));

	QVERIFY(!RestrictionLeft(0, now));
	QVERIFY(!RestrictionLeft(now / 1000 - 1, now));
	auto left = RestrictionLeft(now / 1000 + 30, now);
	QVERIFY(left && left->amount == 1 && left->unit == TimeUnit::Minutes);
	left = RestrictionLeft(now / 1000 + 59 * 60, now);
	QVERIFY(left && left->amount == 59 && left->unit == TimeUnit::Minutes);
	left = RestrictionLeft(now / 1000 + 60 * 60, now);
	QVERIFY(left && left->amount == 1 && left->unit == TimeUnit::Hours);
	left = RestrictionLeft(now / 1000 + 47 * 3600, now);
	QVERIFY(left && left->amount == 47 && left->unit == TimeUnit::Hours);
	left = RestrictionLeft(now / 1000 + 48 * 3600, now);
	QVERIFY(left && left->amount == 2 && left->unit == TimeUnit::Days);
}

void TestGroupRules::membersAndTitles() {
	using namespace GroupRules;
	QVERIFY(CanAdd(true, 10, 5000));
	QVERIFY(!CanAdd(false, 10, 5000));
	QVERIFY(!CanAdd(true, 5000, 5000));
	QCOMPARE(AddRoom(10, 5000, 100), 100);
	QCOMPARE(AddRoom(4950, 5000, 100), 50);
	QCOMPARE(AddRoom(5000, 5000, 100), 0);
	QCOMPARE(AddRoom(5001, 5000, 100), 0);

	QVERIFY(MatchesMember(u"Sara Ahmadi"_s, u"sara"_s, QString()));
	QVERIFY(MatchesMember(u"Sara Ahmadi"_s, u"sara"_s, u"AHM"_s));
	QVERIFY(MatchesMember(u"سارا"_s, u"sara_a"_s, u"@Sara"_s));
	QVERIFY(MatchesMember(u"سارا احمدی"_s, QString(), u"احم"_s));
	QVERIFY(!MatchesMember(u"Sara"_s, u"sara"_s, u"ali"_s));

	QCOMPARE(CleanTitle(u"  هم‌کلاسی‌ها \n  ۱۴۰۵ "_s), u"هم‌کلاسی‌ها ۱۴۰۵"_s);
	QVERIFY(TitleOk(u"a"_s, 128));
	QVERIFY(!TitleOk(u"   "_s, 128));
	QVERIFY(TitleOk(QString(128, u'x'), 128));
	QVERIFY(!TitleOk(QString(129, u'x'), 128));
	QVERIFY(TitleOk(kGrin.repeated(128), 128)); // code points, not units
}

void TestGroupRules::framesInOrderNeedNoDifference() {
	auto server = Server();
	auto client = Client();
	for (auto i = 1; i <= 20; ++i) {
		server.change(i, u"v1"_s);
		client.frame(server, server.pts, i, u"v1"_s);
	}
	server.change(7, u"edited"_s);
	client.frame(server, server.pts, 7, u"edited"_s);
	QCOMPARE(client.differences, 0);
	QCOMPARE(client.pts, server.pts);
	QCOMPARE(client.cache, server.text);
}

void TestGroupRules::lostDoubledAndReorderedFramesConverge() {
	// A fixed pseudo-random run: new messages, edits and deletes; frames
	// dropped, doubled, delayed past later ones or delivered without data.
	struct Wire {
		qint64 pts = 0;
		qint64 seq = 0;
		QString text;
	};
	auto random = std::mt19937(20261011U);
	const auto pick = [&](int below) {
		return int(random() % quint32(below));
	};
	for (auto round = 0; round != 200; ++round) {
		auto server = Server();
		auto client = Client();
		auto wire = QVector<Wire>();
		auto nextSeq = qint64(1);
		const auto deliver = [&] {
			for (const auto &frame : std::as_const(wire)) {
				client.frame(server, frame.pts, frame.seq, frame.text);
			}
			wire.clear();
		};
		for (auto step = 0; step != 60; ++step) {
			const auto what = pick(10);
			auto seq = qint64(0);
			auto text = QString();
			if (what < 6 || nextSeq == 1) {
				seq = nextSeq++;
				text = u"m%1"_s.arg(seq);
			} else if (what < 9) {
				seq = 1 + pick(int(nextSeq - 1));
				text = u"edit%1"_s.arg(step);
			} else {
				seq = 1 + pick(int(nextSeq - 1));
				text = u"(deleted)"_s;
			}
			server.change(seq, text);
			const auto fate = pick(10);
			if (fate == 0) {
				continue; // lost
			}
			const auto frame = Wire{ server.pts, seq, (fate == 1) ? QString() : text };
			if (fate == 2 && !wire.isEmpty()) {
				wire.insert(pick(int(wire.size())), frame); // overtaken by later ones
			} else {
				wire.push_back(frame);
			}
			if (fate == 3) {
				wire.push_back(frame); // delivered twice
			}
			if (pick(3) == 0) {
				deliver();
				QVERIFY2(client.pts <= server.pts, "pts never ahead of the server");
			}
		}
		deliver();
		// The catch-up after a reconnect: whatever the frames left open.
		if (client.pts != server.pts) {
			client.catchUp(server);
		}
		QCOMPARE(client.pts, server.pts);
		QCOMPARE(client.cache, server.text);
	}
}

void TestGroupRules::ownSendAnswerAndFrameInEitherOrder() {
	for (const auto frameFirst : { false, true }) {
		auto server = Server();
		auto client = Client();
		server.change(1, u"created"_s);
		client.frame(server, 1, 1, u"created"_s);
		server.change(2, u"mine"_s); // my send: the answer says seq 2, pts 2; so does the frame
		const auto answer = [&] {
			const auto cached = client.cache.contains(2);
			switch (GroupRules::SendActionFor(client.pts, 2, false, cached)) {
			case SendAction::Advance:
				client.cache.insert(2, u"mine"_s);
				client.pts = 2;
				break;
			case SendAction::Keep:
				client.cache.insert(2, u"mine"_s);
				break;
			case SendAction::CatchUp:
				client.catchUp(server);
				break;
			}
		};
		if (frameFirst) {
			client.frame(server, 2, 2, u"mine"_s);
			answer();
		} else {
			answer();
			client.frame(server, 2, 2, u"mine"_s);
		}
		QCOMPARE(client.differences, 0);
		QCOMPARE(client.pts, qint64(2));
		QCOMPARE(client.cache, server.text);
	}
}

// ---- the rights table

void TestGroupRules::rightsNamesAndMasks() {
	using namespace GroupRights;
	// The bit values are stored on the server: they never move.
	QCOMPARE(RightNames(), (QStringList{
		u"change_info"_s, u"delete_messages"_s, u"ban_users"_s,
		u"invite_users"_s, u"pin_messages"_s, u"add_admins"_s,
	}));
	QCOMPARE(PermissionNames(), (QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s,
		u"add_members"_s, u"pin_messages"_s, u"change_info"_s,
	}));
	QCOMPARE(int(kDefaultPermissions), 15);
	QCOMPARE(int(kRightsAll), 63);
	QCOMPARE(int(kPermissionsAll), 63);
	QCOMPARE(RightsMask({ u"ban_users"_s, u"add_admins"_s }), int(kBanUsers | kAddAdmins));
	QCOMPARE(RightsMask({ u"ban_users"_s, u"something_newer"_s }), int(kBanUsers));
	QCOMPARE(PermissionsMask({ u"send_messages"_s, u"pin_messages"_s }), int(kSendMessages | kPinMessages));
	QCOMPARE(RightsList(kChangeInfo | kPinMessagesRight), (QStringList{ u"change_info"_s, u"pin_messages"_s }));
	QCOMPARE(PermissionsList(kDefaultPermissions), (QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s,
	}));
	// Unknown bits are never shown.
	QVERIFY(RightsList(1 << 20).isEmpty());
	QVERIFY(PermissionsList(1 << 20).isEmpty());
}

void TestGroupRules::permissionsTable() {
	using namespace GroupRights;
	const auto all = PermissionNames();
	const auto def = QStringList{ u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s };
	const auto sending = QStringList{ u"send_messages"_s, u"send_media"_s, u"send_links"_s };
	const auto everyRight = RightNames();
	struct Case {
		const char *name;
		Member member;
		int defaults;
		QStringList permissions;
		QStringList rights;
	};
	const Case cases[] = {
		{ "owner has everything", Row(u"member"_s, u"owner"_s), kDefaultPermissions, all, everyRight },
		{ "owner is not bound by the defaults", Row(u"member"_s, u"owner"_s), 0, all, everyRight },
		{ "owner is not bound by a restriction",
			Row(u"member"_s, u"owner"_s, 0, kPermissionsAll, true), kDefaultPermissions, all, everyRight },
		{ "member gets the defaults", Row(u"member"_s, u"member"_s), kDefaultPermissions, def, {} },
		{ "member of a locked group", Row(u"member"_s, u"member"_s), 0, {}, {} },
		{ "member: rights without the role mean nothing",
			Row(u"member"_s, u"member"_s, kRightsAll), kDefaultPermissions, def, {} },
		{ "member may change info when the defaults say so",
			Row(u"member"_s, u"member"_s), kDefaultPermissions | kChangeInfoPermission,
			def + QStringList{ u"change_info"_s }, {} },
		{ "restricted member loses what is denied",
			Row(u"member"_s, u"member"_s, 0, kSendMedia | kSendLinks, true), kDefaultPermissions,
			{ u"send_messages"_s, u"add_members"_s }, {} },
		{ "an expired restriction denies nothing",
			Row(u"member"_s, u"member"_s, 0, kPermissionsAll, false), kDefaultPermissions, def, {} },
		{ "fully restricted member",
			Row(u"member"_s, u"member"_s, 0, kPermissionsAll, true), kDefaultPermissions, {}, {} },
		{ "admin without rights: defaults plus sending",
			Row(u"member"_s, u"admin"_s), kAddMembers, def, {} },
		{ "admin in a locked group still sends", Row(u"member"_s, u"admin"_s), 0, sending, {} },
		{ "admin rights imply the matching permissions",
			Row(u"member"_s, u"admin"_s, kChangeInfo | kPinMessagesRight | kInviteUsers), 0,
			all, { u"change_info"_s, u"invite_users"_s, u"pin_messages"_s } },
		{ "admin: delete and ban imply no member permission",
			Row(u"member"_s, u"admin"_s, kDeleteMessages | kBanUsers), 0,
			sending, { u"delete_messages"_s, u"ban_users"_s } },
		{ "admin is not bound by a restriction",
			Row(u"member"_s, u"admin"_s, 0, kPermissionsAll, true), kDefaultPermissions, def, {} },
		{ "someone who left has nothing",
			Row(u"left"_s, u"owner"_s, kRightsAll), kDefaultPermissions, {}, {} },
		{ "someone removed has nothing", Row(u"kicked"_s, u"member"_s), kDefaultPermissions, {}, {} },
		{ "unknown bits are never shown", Row(u"member"_s, u"admin"_s, 1 << 20), 1 << 20, sending, {} },
	};
	for (const auto &entry : cases) {
		QVERIFY2(
			PermissionsList(Permissions(entry.member, entry.defaults)) == entry.permissions,
			entry.name);
		QVERIFY2(RightsList(Rights(entry.member)) == entry.rights, entry.name);
	}
}

void TestGroupRules::canTable() {
	using namespace GroupRights;
	const auto everything = u"member,send_text,send_media,send_links,add_members,pin,change_info,"
		"delete_messages,ban,invites,add_admins,history,owner"_s;
	const auto sends = u"member,send_text,send_media,send_links"_s;
	struct Case {
		const char *name;
		Member member;
		int defaults;
		QString want;
	};
	const Case cases[] = {
		{ "owner", Row(u"member"_s, u"owner"_s), kDefaultPermissions, everything },
		{ "owner of a locked group", Row(u"member"_s, u"owner"_s), 0, everything },
		{ "owner with a restriction on the row",
			Row(u"member"_s, u"owner"_s, 0, kPermissionsAll, true), 0, everything },

		{ "member, new group", Row(u"member"_s, u"member"_s), kDefaultPermissions, sends + u",add_members"_s },
		{ "member, locked group", Row(u"member"_s, u"member"_s), 0, u"member"_s },
		{ "member, text only", Row(u"member"_s, u"member"_s), kSendMessages, u"member,send_text"_s },
		{ "member, media without text allowed", Row(u"member"_s, u"member"_s), kSendMedia, u"member"_s },
		{ "member, defaults give pin and info", Row(u"member"_s, u"member"_s), kPermissionsAll,
			sends + u",add_members,pin,change_info"_s },
		{ "member with rights on the row but no role",
			Row(u"member"_s, u"member"_s, kRightsAll), kDefaultPermissions, sends + u",add_members"_s },
		{ "member restricted: no media, no links",
			Row(u"member"_s, u"member"_s, 0, kSendMedia | kSendLinks, true), kDefaultPermissions,
			u"member,send_text,add_members"_s },
		{ "member restricted: read only",
			Row(u"member"_s, u"member"_s, 0, kPermissionsAll, true), kPermissionsAll, u"member"_s },
		{ "member whose restriction ended",
			Row(u"member"_s, u"member"_s, 0, kPermissionsAll, false), kDefaultPermissions,
			sends + u",add_members"_s },
		{ "member restricted from adding",
			Row(u"member"_s, u"member"_s, 0, kAddMembers, true), kDefaultPermissions, sends },

		{ "admin without rights", Row(u"member"_s, u"admin"_s), kDefaultPermissions, sends + u",add_members"_s },
		{ "admin without rights, locked group", Row(u"member"_s, u"admin"_s), 0, sends },
		{ "admin: change_info", Row(u"member"_s, u"admin"_s, kChangeInfo), 0, sends + u",change_info,history"_s },
		{ "admin: delete_messages", Row(u"member"_s, u"admin"_s, kDeleteMessages), 0, sends + u",delete_messages"_s },
		{ "admin: ban_users", Row(u"member"_s, u"admin"_s, kBanUsers), 0, sends + u",ban"_s },
		{ "admin: invite_users", Row(u"member"_s, u"admin"_s, kInviteUsers), 0, sends + u",add_members,invites"_s },
		{ "admin: pin_messages", Row(u"member"_s, u"admin"_s, kPinMessagesRight), 0, sends + u",pin"_s },
		{ "admin: add_admins", Row(u"member"_s, u"admin"_s, kAddAdmins), 0, sends + u",add_admins"_s },
		{ "admin: every right is still not the owner", Row(u"member"_s, u"admin"_s, kRightsAll), 0,
			sends + u",add_members,pin,change_info,delete_messages,ban,invites,add_admins,history"_s },
		{ "admin: the defaults add to the rights",
			Row(u"member"_s, u"admin"_s, kBanUsers), kPinMessages | kChangeInfoPermission,
			sends + u",pin,change_info,ban"_s },
		{ "admin with a restriction on the row",
			Row(u"member"_s, u"admin"_s, 0, kPermissionsAll, true), kDefaultPermissions,
			sends + u",add_members"_s },

		{ "left", Row(u"left"_s, u"member"_s), kPermissionsAll, QString() },
		{ "left, with an owner's row", Row(u"left"_s, u"owner"_s, kRightsAll), kPermissionsAll, QString() },
		{ "removed", Row(u"kicked"_s, u"admin"_s, kRightsAll), kPermissionsAll, QString() },
		{ "banned", Row(u"banned"_s, u"member"_s), kPermissionsAll, QString() },
	};
	for (const auto &entry : cases) {
		const auto got = Allowed(entry.defaults, entry.member);
		QVERIFY2(got == entry.want, qPrintable(u"%1: got %2"_s.arg(QString::fromLatin1(entry.name), got)));
	}
}

void TestGroupRules::canActOnTable() {
	using namespace GroupRights;
	const auto defaults = int(kDefaultPermissions);
	const auto owner = Row(u"member"_s, u"owner"_s, 0, 0, false, u"u1"_s);
	const auto admin = Row(u"member"_s, u"admin"_s, kBanUsers | kAddAdmins, 0, false, u"u2"_s);
	const auto weak = Row(u"member"_s, u"admin"_s, kChangeInfo, 0, false, u"u3"_s);
	const auto member = Row(u"member"_s, u"member"_s, 0, 0, false, u"u4"_s);
	const auto other = Row(u"member"_s, u"member"_s, 0, 0, false, u"u5"_s);
	const auto otherAdmin = Row(u"member"_s, u"admin"_s, kRightsAll, 0, false, u"u6"_s);
	const auto gone = Row(u"left"_s, u"member"_s, 0, 0, false, u"u7"_s);
	const auto goneAdminRow = Row(u"kicked"_s, u"admin"_s, 0, 0, false, u"u8"_s);
	const auto stranger = Row(QString(), QString(), 0, 0, false, u"u9"_s);
	struct Case {
		const char *name;
		Member actor;
		Member target;
		Action action;
		bool want;
	};
	const Case cases[] = {
		{ "owner bans a member", owner, member, kActBan, true },
		{ "owner bans an admin", owner, admin, kActBan, true },
		{ "owner demotes an admin", owner, admin, kActAddAdmins, true },
		{ "owner on themselves", owner, owner, kActBan, false },
		{ "admin bans a member", admin, member, kActBan, true },
		{ "admin promotes a member", admin, member, kActAddAdmins, true },
		{ "admin bans the owner", admin, owner, kActBan, false },
		{ "admin bans another admin", admin, otherAdmin, kActBan, false },
		{ "admin changes another admin", admin, otherAdmin, kActAddAdmins, false },
		{ "admin on themselves", admin, admin, kActBan, false },
		{ "admin without the right", weak, member, kActBan, false },
		{ "admin without the right promotes", weak, member, kActAddAdmins, false },
		{ "member bans a member", member, other, kActBan, false },
		{ "admin bans someone who left", admin, gone, kActBan, true },
		{ "admin bans someone removed whose row says admin", admin, goneAdminRow, kActBan, true },
		{ "admin bans someone who never was a member", admin, stranger, kActBan, true },
		{ "someone who left acts", gone, member, kActBan, false },
	};
	for (const auto &entry : cases) {
		QVERIFY2(CanActOn(defaults, entry.actor, entry.target, entry.action) == entry.want, entry.name);
	}
	// What a member may grant is what they hold.
	QCOMPARE(Grantable(owner), int(kRightsAll));
	QCOMPARE(Grantable(admin), int(kBanUsers | kAddAdmins));
	QCOMPARE(Grantable(member), 0);
}

void TestGroupRules::rowActions() {
	using namespace GroupRights;
	const auto owner = Member::Of(u"u1"_s, u"owner"_s);
	const auto admin = Member::Of(u"u2"_s, u"admin"_s, kBanUsers | kAddAdmins);
	const auto otherAdmin = Member::Of(u"u6"_s, u"admin"_s, kRightsAll);
	const auto member = Member::Of(u"u4"_s, u"member"_s);
	const auto other = Member::Of(u"u5"_s, u"member"_s);

	QVERIFY(CanPromote(owner, member, 3, 50));
	QVERIFY(CanPromote(admin, member, 3, 50));
	QVERIFY(!CanPromote(admin, member, 50, 50));   // the group has its admins
	QVERIFY(!CanPromote(admin, otherAdmin, 3, 50)); // already one
	QVERIFY(!CanPromote(member, other, 3, 50));
	QVERIFY(!CanPromote(admin, admin, 3, 50));

	QVERIFY(CanEditAdmin(owner, admin));
	QVERIFY(!CanEditAdmin(admin, otherAdmin));
	QVERIFY(!CanEditAdmin(owner, member));

	QVERIFY(CanRestrict(owner, member));
	QVERIFY(CanRestrict(admin, member));
	QVERIFY(!CanRestrict(owner, admin)); // an admin is made a member first
	QVERIFY(!CanRestrict(member, other));

	QVERIFY(CanBan(owner, admin));
	QVERIFY(CanBan(admin, member));
	QVERIFY(!CanBan(admin, otherAdmin));
	QVERIFY(!CanBan(admin, owner));
	QVERIFY(!CanBan(member, other));
	QVERIFY(!CanBan(owner, owner));
}

void TestGroupRules::messageNeeds() {
	using namespace GroupRights;
	QCOMPARE(Needs(u"text"_s, false, false), int(kSendMessages));
	QCOMPARE(Needs(u"forwarded_post"_s, false, false), int(kSendMessages));
	QCOMPARE(Needs(u"photo"_s, false, false), int(kSendMessages | kSendMedia));
	QCOMPARE(Needs(u"sticker"_s, false, false), int(kSendMessages | kSendMedia));
	QCOMPARE(Needs(u"location"_s, false, false), int(kSendMessages | kSendMedia));
	QCOMPARE(Needs(u"text"_s, true, false), int(kSendMessages | kSendLinks));
	QCOMPARE(Needs(u"text"_s, false, true), int(kSendMessages | kSendLinks));
}

// ---- invite links

void TestGroupRules::inviteTokenShape() {
	using namespace GroupInviteLinks;
	QVERIFY(IsToken(kToken));
	QVERIFY(IsToken(kToken2)); // «_» and «-» belong to the secret's alphabet
	QVERIFY(!IsToken(QString()));
	QVERIFY(!IsToken(u"j18P1DKWNqdem"_s));
	QVERIFY(!IsToken(u"j18P1DKWNqdem.short"_s));
	QVERIFY(!IsToken(kToken + u'x')); // 23 characters
	QVERIFY(!IsToken(u"u18P1DKWNqdem.6LSbibQJp9aVabUoyMbHMQ"_s)); // not a group id
	QVERIFY(!IsToken(u"j.6LSbibQJp9aVabUoyMbHMQ"_s));
	QVERIFY(!IsToken(u"j18P1DKWNqdem.6LSbibQJp9aVabUoyMbHM="_s)); // padding is not part of it
	QVERIFY(!IsToken(u"j18P1/KWNqdem.6LSbibQJp9aVabUoyMbHMQ"_s));
	QVERIFY(!IsToken(u"j18P1DKWNqdem.6LSbibQJp9aVabUoy bHMQ"_s));
	QCOMPARE(GroupOf(kToken), u"j18P1DKWNqdem"_s);
	QVERIFY(GroupOf(u"nope"_s).isEmpty());
}

void TestGroupRules::inviteLinkForms() {
	using namespace GroupInviteLinks;
	QCOMPARE(Token(u"https://eleza.me/g/"_s + kToken, kBase), kToken);
	QCOMPARE(Token(u"http://eleza.me/g/"_s + kToken, kBase), kToken);
	QCOMPARE(Token(u"eleza.me/g/"_s + kToken, kBase), kToken);
	QCOMPARE(Token(u"https://www.eleza.me/g/"_s + kToken, kBase), kToken);
	QCOMPARE(Token(u"HTTPS://Eleza.Me/g/"_s + kToken, kBase), kToken);
	QCOMPARE(Token(u"eleza://g/"_s + kToken, kBase), kToken);
	QCOMPARE(Token(kToken, kBase), kToken);
	QCOMPARE(Token(u"  https://eleza.me/g/"_s + kToken + u'\n', kBase), kToken);
	QCOMPARE(Token(u"https://eleza.me/g/"_s + kToken2, kBase), kToken2);
}

void TestGroupRules::inviteLinkTail() {
	using namespace GroupInviteLinks;
	const auto link = u"https://eleza.me/g/"_s + kToken;
	QCOMPARE(Token(link + u"?utm=x"_s, kBase), kToken);
	QCOMPARE(Token(link + u"#top"_s, kBase), kToken);
	QCOMPARE(Token(link + u'/', kBase), kToken);
	QCOMPARE(Token(link + u'.', kBase), kToken);
	QCOMPARE(Token(link + QChar(0x060C), kBase), kToken); // the Persian comma
	QCOMPARE(Token(link + u')' + QChar(0x00BB), kBase), kToken);
}

void TestGroupRules::inviteLinkServerPrefix() {
	using namespace GroupInviteLinks;
	// Another prefix from client-config, with or without a scheme and the last slash.
	QCOMPARE(Token(u"https://join.example.org/in/"_s + kToken, u"join.example.org/in/"_s), kToken);
	QCOMPARE(Token(u"join.example.org/in/"_s + kToken, u"https://join.example.org/in"_s), kToken);
	// The prefix every build knows still works beside it.
	QCOMPARE(Token(u"https://eleza.me/g/"_s + kToken, u"join.example.org/in/"_s), kToken);
	// No prefix known at all (client-config not read yet).
	QCOMPARE(Token(u"https://eleza.me/g/"_s + kToken), kToken);
	QCOMPARE(Url(kToken), u"https://eleza.me/g/"_s + kToken);
	QCOMPARE(Url(kToken, u"join.example.org/in"_s), u"https://join.example.org/in/"_s + kToken);
}

void TestGroupRules::inviteLinkNotAnInvite() {
	using namespace GroupInviteLinks;
	QVERIFY(Token(QString(), kBase).isEmpty());
	QVERIFY(Token(u"https://eleza.me/@sara"_s, kBase).isEmpty());
	QVERIFY(Token(u"https://eleza.me/g/"_s, kBase).isEmpty());
	QVERIFY(Token(u"https://eleza.me/g/not-a-token"_s, kBase).isEmpty());
	QVERIFY(Token(u"https://eleza.me/group/"_s + kToken, kBase).isEmpty());
	QVERIFY(Token(u"https://evil.example/eleza.me/g/"_s + kToken, kBase).isEmpty());
	QVERIFY(Token(u"https://eleza.me.evil.example/g/"_s + kToken, kBase).isEmpty());
	QVERIFY(Token(u"https://eleza.me/p/123"_s, kBase).isEmpty());
	QVERIFY(Token(u"eleza://p/"_s + kToken, kBase).isEmpty());
}

// ---- mentions

void TestGroupRules::mentionQuery() {
	using namespace MentionRules;
	auto query = QueryAt(u"@sa"_s, 3);
	QVERIFY(query && query->at == 0 && query->text == u"sa");
	query = QueryAt(u"hello @"_s, 7);
	QVERIFY(query && query->at == 6 && query->text.isEmpty());
	query = QueryAt(u"line\n@x"_s, 7);
	QVERIFY(query && query->at == 5);
	// Persian text; a half-space inside the word is part of it.
	const auto persian = u"سلام @سا"_s;
	query = QueryAt(persian, int(persian.size()));
	QVERIFY(query && query->at == 5 && query->text == u"سا");
	const auto halfSpace = u"@می‌خو"_s;
	query = QueryAt(halfSpace, int(halfSpace.size()));
	QVERIFY(query && query->text == u"می‌خو");
	// After an emoji or an opening mark.
	const auto afterEmoji = kGrin + u"@a"_s;
	query = QueryAt(afterEmoji, int(afterEmoji.size()));
	QVERIFY(query && query->at == 2);
	QVERIFY(QueryAt(u"(@a"_s, 3));
	QVERIFY(QueryAt(u"«@a"_s, 3));
	// Not inside a word, an address or an e-mail.
	QVERIFY(!QueryAt(u"mail@ex"_s, 7));
	QVERIFY(!QueryAt(u"eleza.me/@sara"_s, 14));
	QVERIFY(!QueryAt(u"a.@b"_s, 4));
	QVERIFY(!QueryAt(u"@@b"_s, 3));
	QVERIFY(!QueryAt(u"سارا@ب"_s, 6));
	// Ends at a blank and at the limit.
	QVERIFY(!QueryAt(u"@sara "_s, 6));
	QVERIFY(!QueryAt(u"@sara j"_s, 7));
	QVERIFY(!QueryAt(u"no at here"_s, 5));
	QVERIFY(!QueryAt(QString(), 0));
	QVERIFY(!QueryAt(u"@a"_s, 0));
	QVERIFY(!QueryAt(u"@a"_s, 9));
	auto longWord = u"@"_s + QString(kQueryMax, u'x');
	QVERIFY(QueryAt(longWord, int(longWord.size())));
	longWord += u'x';
	QVERIFY(!QueryAt(longWord, int(longWord.size())));
	// The cursor in the middle of the text.
	query = QueryAt(u"hi @sa and more"_s, 6);
	QVERIFY(query && query->at == 3 && query->text == u"sa");
}

void TestGroupRules::mentionLabelAndRoom() {
	using namespace MentionRules;
	QCOMPARE(Label(u"  Sara \n Ahmadi "_s, u"sara"_s, u"A member"_s), u"Sara Ahmadi"_s);
	QCOMPARE(Label(u"   "_s, u"sara"_s, u"A member"_s), u"sara"_s);
	QCOMPARE(Label(QString(), QString(), u"A member"_s), u"A member"_s);
	QVERIFY(Label(QString(), QString(), QString()).isEmpty());
	// Cut between code points.
	const auto label = Label(kGrin.repeated(kLabelMax + 5), QString(), u"x"_s);
	QCOMPARE(int(label.size()), kLabelMax * 2);
	QVERIFY(!label.back().isHighSurrogate());

	QVERIFY(Room({}, u"u1"_s, 2));
	QVERIFY(Room({ u"u1"_s }, u"u2"_s, 2));
	QVERIFY(!Room({ u"u1"_s, u"u2"_s }, u"u3"_s, 2));
	QVERIFY(Room({ u"u1"_s, u"u2"_s, u"u1"_s }, u"u2"_s, 2)); // someone already named again
	QVERIFY(!Room({}, u"u1"_s, 0));
}

void TestGroupRules::mentionOffsetsAreUtf16() {
	using namespace MentionRules;
	const auto same = [](const std::optional<Range> &range, int offset, int length) {
		return range && range->offset == offset && range->length == length;
	};
	// The contract's example: «سلام سارا», the mention over «سارا».
	QVERIFY(same(ToWire(5, 9, u"سلام سارا"_s), 5, 4));
	// The offsets move with the trim.
	const auto padded = u"  \n سلام سارا  "_s;
	const auto at = int(padded.indexOf(u"سارا"));
	QVERIFY(same(ToWire(at, at + 4, padded), 5, 4));
	// A mention that ends in blanks the trim removes keeps what is left.
	QVERIFY(same(ToWire(0, 8, u"@Sara   "_s), 0, 5));
	// UTF-16 units after emoji.
	const auto emoji = kFamily + u' ' + kGrin + u" Sara"_s;
	const auto name = int(emoji.indexOf(u"Sara"));
	QCOMPARE(name, 12);
	QVERIFY(same(ToWire(name, name + 4, emoji), 12, 4));
	// A name that itself holds an emoji: both of its units are inside.
	QVERIFY(same(ToWire(3, 11, u"hi "_s + kGrin + u"Sara"_s + kGrin + u" ok"_s), 3, 8));
	// Mixed directions: logical order, as typed.
	const auto mixed = u"سلام Sara جان"_s;
	const auto latin = int(mixed.indexOf(u"Sara"));
	QVERIFY(same(ToWire(latin, latin + 4, mixed), latin, 4));
	QVERIFY(same(ToWire(6, 10, u"hello سارا!"_s), 6, 4));
	// Direction marks around the name are units like any other.
	QVERIFY(same(ToWire(1, 5, u"‏سارا‏ x"_s), 1, 4));
	// A range that starts or ends inside a pair grows to hold it.
	const auto pair = u"a"_s + kGrin + u"b"_s;
	QVERIFY(same(ToWire(2, 3, pair), 1, 2));
	QVERIFY(same(ToWire(0, 2, pair), 0, 3));
	// Nothing left.
	QVERIFY(!ToWire(0, 2, u"   "_s));
	QVERIFY(!ToWire(3, 3, u"abc def"_s));
	QVERIFY(!ToWire(-1, 2, u"abc"_s));
	QVERIFY(!ToWire(1, 9, u"abc"_s));
	QVERIFY(!ToWire(0, 1, QString()));
}

void TestGroupRules::mentionClipAndAddressing() {
	using namespace MentionRules;
	auto range = Clip(5, 4, 9);
	QVERIFY(range && range->offset == 5 && range->length == 4);
	range = Clip(5, 40, 9);
	QVERIFY(range && range->offset == 5 && range->length == 4);
	range = Clip(0, std::numeric_limits<int>::max(), 3);
	QVERIFY(range && range->offset == 0 && range->length == 3);
	QVERIFY(!Clip(9, 1, 9));
	QVERIFY(!Clip(-1, 3, 9));
	QVERIFY(!Clip(2, 0, 9));

	QVERIFY(AddressesMe(u"me"_s, u"u1"_s, { u"u2"_s, u"me"_s }, QString()));
	QVERIFY(AddressesMe(u"me"_s, u"u1"_s, {}, u"me"_s)); // a reply to my message
	QVERIFY(!AddressesMe(u"me"_s, u"u1"_s, { u"u2"_s }, u"u3"_s));
	QVERIFY(!AddressesMe(u"me"_s, u"me"_s, { u"me"_s }, u"me"_s)); // my own message never does
	QVERIFY(!AddressesMe(QString(), u"u1"_s, { QString() }, QString()));

	QCOMPARE(NextMention({ 2, 9, 14 }, 0), qint64(2));
	QCOMPARE(NextMention({ 2, 9, 14 }, 2), qint64(9));
	QCOMPARE(NextMention({ 2, 9, 14 }, 14), qint64(0));
	QCOMPARE(NextMention({}, 0), qint64(0));
}

QTEST_GUILESS_MAIN(TestGroupRules)

#include "test_group_rules.moc"
