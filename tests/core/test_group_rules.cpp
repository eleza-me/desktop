// The decisions of groups (data/data_group_rules.h): the pts rule, the answer
// of a send, the catch-up, unread, service notes, the rights table (the
// cases of the server's own acceptance tables), invite links, mentions and
// member tags.
#include "data/data_group_rules.h"
#include "data/data_types.h"

#include <QtCore/QJsonDocument>

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
		{ "edit_tag", kActEditOwnTag },
		{ "manage_tags", kActManageTags },
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

	void thirdReleaseShapes();
	void adminLogForms();
	void adminLogFilter();
	void slowModeSteps();

	void memberTagShapes();
	void memberTagText();
	void memberTagBadges();
	void memberTagSenders();
	void memberTagWhoMaySet();
	void memberTagRows();
	void memberTagAdminLog();

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
		u"manage_tags"_s,
	}));
	QCOMPARE(PermissionNames(), (QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s,
		u"add_members"_s, u"pin_messages"_s, u"change_info"_s,
		u"edit_tag"_s,
	}));
	// What the screens list, in Telegram's order.
	QCOMPARE(ShownRights(), (QStringList{
		u"change_info"_s, u"delete_messages"_s, u"ban_users"_s,
		u"invite_users"_s, u"pin_messages"_s, u"manage_tags"_s,
		u"add_admins"_s,
	}));
	QCOMPARE(ShownPermissions(), (QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s,
		u"add_members"_s, u"pin_messages"_s, u"edit_tag"_s,
		u"change_info"_s,
	}));
	QCOMPARE(RightBit(u"manage_tags"_s), int(kManageTags));
	QCOMPARE(RightBit(u"something_newer"_s), 0);
	QCOMPARE(PermissionBit(u"edit_tag"_s), int(kEditTag));
	QCOMPARE(PermissionBit(u"change_info"_s), int(kChangeInfoPermission));
	// New groups (and every existing one, by the server's migration) let
	// members set their own tag.
	QCOMPARE(int(kDefaultPermissions), 79);
	QCOMPARE(int(kRightsAll), 127);
	QCOMPARE(int(kPermissionsAll), 127);
	QCOMPARE(RightsMask({ u"ban_users"_s, u"add_admins"_s }), int(kBanUsers | kAddAdmins));
	QCOMPARE(RightsMask({ u"ban_users"_s, u"something_newer"_s }), int(kBanUsers));
	QCOMPARE(PermissionsMask({ u"send_messages"_s, u"pin_messages"_s }), int(kSendMessages | kPinMessages));
	QCOMPARE(RightsList(kChangeInfo | kPinMessagesRight), (QStringList{ u"change_info"_s, u"pin_messages"_s }));
	QCOMPARE(PermissionsList(kDefaultPermissions), (QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s,
		u"edit_tag"_s,
	}));
	// Unknown bits are never shown.
	QVERIFY(RightsList(1 << 20).isEmpty());
	QVERIFY(PermissionsList(1 << 20).isEmpty());
}

void TestGroupRules::permissionsTable() {
	using namespace GroupRights;
	const auto all = PermissionNames();
	const auto def = QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s, u"edit_tag"_s };
	// An admin always sends and sets their own tag.
	const auto sending = QStringList{
		u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"edit_tag"_s };
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
			{ u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s,
				u"change_info"_s, u"edit_tag"_s }, {} },
		{ "restricted member loses what is denied",
			Row(u"member"_s, u"member"_s, 0, kSendMedia | kSendLinks, true), kDefaultPermissions,
			{ u"send_messages"_s, u"add_members"_s, u"edit_tag"_s }, {} },
		{ "a group without own tags",
			Row(u"member"_s, u"member"_s), kDefaultPermissions & ~kEditTag,
			{ u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s }, {} },
		{ "member restricted from the own tag",
			Row(u"member"_s, u"member"_s, 0, kEditTag, true), kDefaultPermissions,
			{ u"send_messages"_s, u"send_media"_s, u"send_links"_s, u"add_members"_s }, {} },
		{ "admin keeps the own tag where members have none",
			Row(u"member"_s, u"admin"_s), kDefaultPermissions & ~kEditTag, def, {} },
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
		{ "admin: manage_tags is a right, not a permission",
			Row(u"member"_s, u"admin"_s, kManageTags), 0, sending, { u"manage_tags"_s } },
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
		"delete_messages,ban,invites,add_admins,history,owner,edit_tag,manage_tags"_s;
	const auto sends = u"member,send_text,send_media,send_links"_s;
	const auto tag = u",edit_tag"_s;
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

		{ "member, new group", Row(u"member"_s, u"member"_s), kDefaultPermissions,
			sends + u",add_members"_s + tag },
		{ "member, locked group", Row(u"member"_s, u"member"_s), 0, u"member"_s },
		{ "member, text only", Row(u"member"_s, u"member"_s), kSendMessages, u"member,send_text"_s },
		{ "member, media without text allowed", Row(u"member"_s, u"member"_s), kSendMedia, u"member"_s },
		{ "member, defaults give pin and info", Row(u"member"_s, u"member"_s), kPermissionsAll,
			sends + u",add_members,pin,change_info"_s + tag },
		{ "member with rights on the row but no role",
			Row(u"member"_s, u"member"_s, kRightsAll), kDefaultPermissions,
			sends + u",add_members"_s + tag },
		{ "member restricted: no media, no links",
			Row(u"member"_s, u"member"_s, 0, kSendMedia | kSendLinks, true), kDefaultPermissions,
			u"member,send_text,add_members"_s + tag },
		{ "member restricted: read only",
			Row(u"member"_s, u"member"_s, 0, kPermissionsAll, true), kPermissionsAll, u"member"_s },
		{ "member whose restriction ended",
			Row(u"member"_s, u"member"_s, 0, kPermissionsAll, false), kDefaultPermissions,
			sends + u",add_members"_s + tag },
		{ "member restricted from adding",
			Row(u"member"_s, u"member"_s, 0, kAddMembers, true), kDefaultPermissions, sends + tag },
		{ "member restricted from the own tag",
			Row(u"member"_s, u"member"_s, 0, kEditTag, true), kDefaultPermissions,
			sends + u",add_members"_s },
		{ "member of a group without own tags",
			Row(u"member"_s, u"member"_s), kDefaultPermissions & ~kEditTag, sends + u",add_members"_s },

		{ "admin without rights", Row(u"member"_s, u"admin"_s), kDefaultPermissions,
			sends + u",add_members"_s + tag },
		{ "admin without rights, locked group", Row(u"member"_s, u"admin"_s), 0, sends + tag },
		{ "admin: change_info", Row(u"member"_s, u"admin"_s, kChangeInfo), 0,
			sends + u",change_info,history"_s + tag },
		{ "admin: delete_messages", Row(u"member"_s, u"admin"_s, kDeleteMessages), 0,
			sends + u",delete_messages"_s + tag },
		{ "admin: ban_users", Row(u"member"_s, u"admin"_s, kBanUsers), 0, sends + u",ban"_s + tag },
		{ "admin: invite_users", Row(u"member"_s, u"admin"_s, kInviteUsers), 0,
			sends + u",add_members,invites"_s + tag },
		{ "admin: pin_messages", Row(u"member"_s, u"admin"_s, kPinMessagesRight), 0,
			sends + u",pin"_s + tag },
		{ "admin: add_admins", Row(u"member"_s, u"admin"_s, kAddAdmins), 0,
			sends + u",add_admins"_s + tag },
		{ "admin: manage_tags", Row(u"member"_s, u"admin"_s, kManageTags), 0,
			sends + tag + u",manage_tags"_s },
		{ "admin: every right is still not the owner", Row(u"member"_s, u"admin"_s, kRightsAll), 0,
			sends + u",add_members,pin,change_info,delete_messages,ban,invites,add_admins,history"_s
				+ tag + u",manage_tags"_s },
		{ "admin: the defaults add to the rights",
			Row(u"member"_s, u"admin"_s, kBanUsers), kPinMessages | kChangeInfoPermission,
			sends + u",pin,change_info,ban"_s + tag },
		{ "admin with a restriction on the row",
			Row(u"member"_s, u"admin"_s, 0, kPermissionsAll, true), kDefaultPermissions,
			sends + u",add_members"_s + tag },

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
		// Other people's tags: the right, then the target rule.
		{ "owner tags a member", owner, member, kActManageTags, true },
		{ "owner tags an admin", owner, admin, kActManageTags, true },
		{ "an admin without the right tags a member", admin, member, kActManageTags, false },
		{ "admin tags a member", otherAdmin, member, kActManageTags, true },
		{ "admin tags another admin", otherAdmin, admin, kActManageTags, false },
		{ "admin tags the owner", otherAdmin, owner, kActManageTags, false },
		{ "admin tags themselves through the right", otherAdmin, otherAdmin, kActManageTags, false },
		{ "member tags a member", member, other, kActManageTags, false },
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


namespace {

[[nodiscard]] QJsonObject Parse(const char *json) {
	return QJsonDocument::fromJson(QByteArray(json)).object();
}

[[nodiscard]] GroupAdminLogEntry Entry(const char *json) {
	return GroupAdminLogEntry::FromJson(Parse(json));
}

} // namespace

void TestGroupRules::thirdReleaseShapes() {
	// GroupView: the new fields, and what an older server leaves out.
	const auto header = Group::FromJson(Parse(R"({"group":"j18P1DKWNqdem","title":"x",
		"member_count":4,"admin_count":2,"slow_mode_s":30,"slow_mode_wait_s":12,
		"hide_members":true,"members_hidden":true})"));
	QCOMPARE(header.adminCount, 2);
	QVERIFY(header.hideMembers);
	QVERIFY(header.membersHidden);
	QCOMPARE(header.slowModeS, 30);
	const auto again = Group::FromJson(header.toJson());
	QVERIFY(again.hideMembers && again.membersHidden);
	QCOMPARE(again.adminCount, 2);
	const auto old = Group::FromJson(Parse(R"({"group":"j1","title":"x"})"));
	QVERIFY(!old.hideMembers && !old.membersHidden);
	QCOMPARE(old.adminCount, 0);

	// An entry of the admin log with its detail.
	const auto entry = Entry(R"({"id":"a1Qm4d7xYk2Lp","at":1791747176011,
		"actor":"u1LrX5NIMrhqs","action":"restrict","target":"u1346hFY3UaS9",
		"detail":{"denied":["send_media","send_links"],"until":1791750775}})");
	QCOMPARE(entry.id, u"a1Qm4d7xYk2Lp"_s);
	QCOMPARE(entry.at, qint64(1791747176011));
	QCOMPARE(entry.actor, u"u1LrX5NIMrhqs"_s);
	QCOMPARE(entry.target, u"u1346hFY3UaS9"_s);
	QCOMPARE(entry.denied, (QStringList{ u"send_media"_s, u"send_links"_s }));
	QCOMPARE(entry.until, qint64(1791750775));
	// Over the connection int64 values come as strings.
	const auto wire = Entry(R"({"id":"a2","at":"1791747176090","actor":"u1",
		"action":"delete_messages","detail":{"seqs":["4","9"]}})");
	QCOMPARE(wire.at, qint64(1791747176090));
	QCOMPARE(wire.seqs, (QVector<qint64>{ 4, 9 }));
	QVERIFY(wire.target.isEmpty());

	const auto reaction = GroupReaction::FromJson(Parse(
		R"({"user":"u1346hFY3UaS9","emoji":"👍","at":1791747176100})"));
	QCOMPARE(reaction.user, u"u1346hFY3UaS9"_s);
	QCOMPARE(reaction.emoji, QString::fromUtf8("👍"));
	QCOMPARE(reaction.at, qint64(1791747176100));

	const auto failure = GroupDecideFailure::FromJson(Parse(
		R"({"user":"u1F0syPDjdfAz","reason":"not_found"})"));
	QCOMPARE(failure.reason, u"not_found"_s);

	const auto joined = GroupInviteMember::FromJson(Parse(
		R"({"user":"u1GEBUAJjIb2b","joined_at":1791747176047})"));
	QCOMPARE(joined.user, u"u1GEBUAJjIb2b"_s);
	QCOMPARE(joined.joinedAt, qint64(1791747176047));
}

void TestGroupRules::adminLogForms() {
	using GroupAdminLog::Form;
	using GroupAdminLog::FormOf;
	using GroupAdminLog::NamesTarget;
	QCOMPARE(FormOf(Entry(R"({"action":"promote","target":"u2"})")), Form::Promote);
	QCOMPARE(FormOf(Entry(R"({"action":"restrict","target":"u2"})")), Form::Restrict);
	QCOMPARE(
		FormOf(Entry(R"({"action":"restrict","target":"u2","detail":{"until":5}})")),
		Form::RestrictUntil);
	QCOMPARE(FormOf(Entry(R"({"action":"ban","target":"u2"})")), Form::Ban);
	QCOMPARE(
		FormOf(Entry(R"({"action":"ban","target":"u2","detail":{"delete_messages":true}})")),
		Form::BanDelete);
	// No detail on slow_mode / hide_members: switched off.
	QCOMPARE(FormOf(Entry(R"({"action":"slow_mode"})")), Form::SlowModeOff);
	QCOMPARE(
		FormOf(Entry(R"({"action":"slow_mode","detail":{"slow_mode_s":30}})")),
		Form::SlowModeOn);
	QCOMPARE(FormOf(Entry(R"({"action":"hide_members"})")), Form::HideMembersOff);
	QCOMPARE(
		FormOf(Entry(R"({"action":"hide_members","detail":{"hide_members":true}})")),
		Form::HideMembersOn);
	QCOMPARE(FormOf(Entry(R"({"action":"invite_create"})")), Form::InviteCreate);
	QCOMPARE(
		FormOf(Entry(R"({"action":"invite_create","detail":{"link_name":"  "}})")),
		Form::InviteCreate);
	QCOMPARE(
		FormOf(Entry(R"({"action":"invite_revoke","detail":{"link_name":"x"}})")),
		Form::InviteRevokeNamed);
	QCOMPARE(FormOf(Entry(R"({"action":"delete_messages"})")), Form::DeleteMessages);
	QCOMPARE(
		FormOf(Entry(R"({"action":"delete_messages","target":"u2"})")),
		Form::DeleteMessagesOf);
	QCOMPARE(FormOf(Entry(R"({"action":"transfer","target":"u2"})")), Form::Transfer);
	QCOMPARE(FormOf(Entry(R"({"action":"something_new"})")), Form::Generic);
	QCOMPARE(FormOf(Entry(R"({})")), Form::Generic);

	QVERIFY(NamesTarget(Form::Promote));
	QVERIFY(NamesTarget(Form::DeleteMessagesOf));
	QVERIFY(!NamesTarget(Form::DeleteMessages));
	QVERIFY(!NamesTarget(Form::SlowModeOn));
	QVERIFY(!NamesTarget(Form::Generic));
}

void TestGroupRules::adminLogFilter() {
	using namespace GroupAdminLog;
	// Every action name of the server is in exactly one kind.
	const auto all = QStringList{
		u"promote"_s, u"admin_rights"_s, u"demote"_s, u"restrict"_s,
		u"unrestrict"_s, u"remove"_s, u"ban"_s, u"unban"_s,
		u"permissions"_s, u"slow_mode"_s, u"hide_members"_s, u"title"_s,
		u"photo"_s, u"about"_s, u"pin"_s, u"unpin"_s, u"invite_create"_s,
		u"invite_revoke"_s, u"join_approve"_s, u"join_decline"_s,
		u"delete_message"_s, u"delete_messages"_s,
		u"delete_all_messages"_s, u"transfer"_s, u"edit_tag"_s,
	};
	auto seen = QStringList();
	for (auto i = 0; i != kCategoryCount; ++i) {
		seen += Actions(i);
	}
	QCOMPARE(seen.size(), all.size());
	for (const auto &name : all) {
		QCOMPARE(seen.count(name), 1);
	}
	QVERIFY(Actions(-1).isEmpty());
	QVERIFY(Actions(kCategoryCount).isEmpty());

	QCOMPARE(ActionsParam(0), QString());
	QCOMPARE(ActionsParam((1 << kCategoryCount) - 1), QString());
	QCOMPARE(ActionsParam(1 << Links), u"invite_create,invite_revoke"_s);
	QCOMPARE(
		ActionsParam((1 << Admins) | (1 << Links)),
		u"promote,admin_rights,demote,transfer,invite_create,invite_revoke"_s);
	QCOMPARE(PickedCount(0), 0);
	QCOMPARE(PickedCount(1 << Settings), 1);
	QCOMPARE(PickedCount((1 << Settings) | (1 << Members)), 2);
	QCOMPARE(PickedCount((1 << kCategoryCount) - 1), 0);
}

void TestGroupRules::slowModeSteps() {
	using GroupAdminLog::SlowModeSteps;
	const auto defaults = QVector<int>{ 0, 10, 30, 60, 300, 900, 3600 };
	QCOMPARE(
		SlowModeSteps({ u"0"_s, u"10s"_s, u"30s"_s, u"1m"_s, u"5m"_s, u"15m"_s, u"1h"_s }),
		defaults);
	QCOMPARE(SlowModeSteps({}), defaults);
	QCOMPARE(SlowModeSteps({ u"bogus"_s, u"-5"_s }), defaults);
	// Plain seconds, repeats, any order, no "0" given: off stays first.
	QCOMPARE(
		SlowModeSteps({ u"120"_s, u"2m"_s, u"30"_s, u"1h30m"_s }),
		(QVector<int>{ 0, 30, 120, 5400 }));
	// Nonsense mixed in is skipped.
	QCOMPARE(
		SlowModeSteps({ u"10x"_s, u"20s"_s, u"s"_s, u"99999999"_s }),
		(QVector<int>{ 0, 20 }));
}

// ---- member tags

void TestGroupRules::memberTagShapes() {
	// GroupMember: the tag of everybody; admin_title only for admins.
	const auto member = GroupMember::FromJson(Parse(
		R"({"user":"u1","role":"member","status":"member","tag":"طراح"})"));
	QCOMPARE(member.tag, u"طراح"_s);
	QCOMPARE(member.currentTag(), u"طراح"_s);
	QCOMPARE(GroupMember::FromJson(member.toJson()).tag, u"طراح"_s);
	const auto admin = GroupMember::FromJson(Parse(
		R"({"user":"u2","role":"admin","status":"member","admin_title":"ناظر","tag":"ناظر"})"));
	QCOMPARE(admin.currentTag(), u"ناظر"_s);
	// An older server: the admin title is the tag.
	const auto older = GroupMember::FromJson(Parse(
		R"({"user":"u3","role":"admin","admin_title":"ناظر"})"));
	QVERIFY(older.tag.isEmpty());
	QCOMPARE(older.currentTag(), u"ناظر"_s);
	// …but never for an ordinary member.
	const auto stray = GroupMember::FromJson(Parse(
		R"({"user":"u4","role":"member","admin_title":"x"})"));
	QVERIFY(stray.currentTag().isEmpty());

	// GroupView: my own tag, the new names in the lists (an old build keeps
	// them as strings and its masks drop them).
	const auto header = Group::FromJson(Parse(R"({"group":"j1","title":"x",
		"role":"member","my_tag":"طراح",
		"rights":[],"permissions":["send_messages","edit_tag"],
		"default_permissions":["send_messages","edit_tag"]})"));
	QCOMPARE(header.myTag, u"طراح"_s);
	QVERIFY(header.can(u"edit_tag"));
	QCOMPARE(Group::FromJson(header.toJson()).myTag, u"طراح"_s);
	QCOMPARE(GroupRights::PermissionsMask(header.permissions),
		int(GroupRights::kSendMessages | GroupRights::kEditTag));
	QCOMPARE(GroupRights::PermissionsMask({ u"send_messages"_s, u"edit_tag"_s })
			& ((1 << 6) - 1),
		int(GroupRights::kSendMessages)); // what a six-name build keeps
	QVERIFY(Group::FromJson(Parse(R"({"group":"j1","title":"x"})")).myTag.isEmpty());

	// MessageView: the sender's tag and role as they were read.
	const auto message = Message::FromJson(Parse(R"({"chat":"j1","seq":4,"id":"m4",
		"sender":"u2","kind":"text","text":"سلام","created_at":1,
		"sender_tag":"ناظر","sender_role":"admin"})"));
	QCOMPARE(message.senderTag, u"ناظر"_s);
	QCOMPARE(message.senderRole, u"admin"_s);
	const auto stored = Message::FromStored(message.toStored());
	QCOMPARE(stored.senderTag, u"ناظر"_s);
	QCOMPARE(stored.senderRole, u"admin"_s);
	const auto plain = Message::FromJson(Parse(R"({"chat":"j1","seq":5,"id":"m5",
		"sender":"u3","kind":"text","text":"x","created_at":2})"));
	QVERIFY(plain.senderTag.isEmpty() && plain.senderRole.isEmpty());
	QVERIFY(!plain.toJson().contains(u"sender_tag"_s));

	// An entry of the admin log: the tag now and before.
	const auto entry = Entry(R"({"id":"a9","at":1,"actor":"u1","action":"edit_tag",
		"target":"u2","detail":{"tag":"ناظر","prev_tag":"مدیر"}})");
	QCOMPARE(entry.tag, u"ناظر"_s);
	QCOMPARE(entry.prevTag, u"مدیر"_s);
}

void TestGroupRules::memberTagText() {
	using namespace MemberTags;
	QCOMPARE(RoleOf(u"owner"), Role::Owner);
	QCOMPARE(RoleOf(u"admin"), Role::Admin);
	QCOMPARE(RoleOf(u"member"), Role::Member);
	QCOMPARE(RoleOf(u""), Role::Member);
	QCOMPARE(RoleOf(u"left"), Role::Member);
	QCOMPARE(WireRole(Role::Owner), u"owner"_s);
	QCOMPARE(WireRole(Role::Member), u"member"_s);

	// One line, blanks collapsed, emoji out (Telegram: RemoveEmoji(SingleLine())).
	QCOMPARE(Shown(u"  طراح \n ارشد  "_s), u"طراح ارشد"_s);
	QCOMPARE(Shown(u"ناظر "_s + QString::fromUtf8("🌱")), u"ناظر"_s);
	QCOMPARE(Shown(QString::fromUtf8("🇮🇷 تیم")), u"تیم"_s);              // a flag
	QCOMPARE(Shown(QString::fromUtf8("1️⃣ نفر")), u"نفر"_s);               // a keycap
	QCOMPARE(Shown(kFamily + u" خانواده"_s), u"خانواده"_s);                // a joined sequence
	QCOMPARE(Shown(QString::fromUtf8("👍🏽ok")), u"ok"_s);                  // a skin tone
	QCOMPARE(Shown(QString::fromUtf8("❤️ x ♥")), u"x"_s);
	QCOMPARE(Shown(QString::fromUtf8("©️ کپی")), u"کپی"_s);                // a symbol asked to be emoji
	QCOMPARE(Shown(QString::fromUtf8("© 2026")), QString::fromUtf8("© 2026")); // …and as text
	// The joiners of Persian words stay.
	QCOMPARE(Shown(u"می‌خواهم"_s), u"می‌خواهم"_s);
	QCOMPARE(Shown(u"#1 fan"_s), u"#1 fan"_s);
	QCOMPARE(Typed(u"  طراح  "_s), u"طراح"_s);
	QVERIFY(Typed(kGrin + kGrin).isEmpty());

	QVERIFY(TypedOk(QString(16, u'x'), 16));
	QVERIFY(!TypedOk(QString(17, u'x'), 16));
	QVERIFY(TypedOk(QString(), 16)); // "" removes the tag
	QVERIFY(TypedOk(u"  "_s + QString(16, u'x') + u"\n"_s, 16));
	QVERIFY(TypedOk(QString(16, u'x') + kGrin, 16));
	QVERIFY(TypedOk(QString::fromUcs4(U"\U00010348").repeated(16), 16)); // code points, not units
}

void TestGroupRules::memberTagBadges() {
	using namespace MemberTags;
	const auto owner = u"مالک"_s;
	const auto admin = u"ادمین"_s;
	struct Case {
		const char *name;
		Role role;
		QString tag;
		QString text;
		bool pill;
	};
	const Case cases[] = {
		{ "owner without a tag", Role::Owner, QString(), owner, true },
		{ "owner with a tag", Role::Owner, u"بنیان‌گذار"_s, u"بنیان‌گذار"_s, true },
		{ "admin without a tag", Role::Admin, QString(), admin, true },
		{ "admin with a tag", Role::Admin, u"ناظر"_s, u"ناظر"_s, true },
		{ "admin whose tag is only emoji", Role::Admin, kGrin, admin, true },
		{ "member with a tag", Role::Member, u"طراح"_s, u"طراح"_s, false },
		{ "member without a tag", Role::Member, QString(), QString(), false },
		{ "member whose tag is only emoji", Role::Member, kGrin, QString(), false },
		{ "a tag of two lines", Role::Member, u"a\nb"_s, u"a b"_s, false },
	};
	for (const auto &entry : cases) {
		const auto badge = BadgeFor(entry.role, entry.tag, owner, admin);
		QVERIFY2(badge.text == entry.text, entry.name);
		QVERIFY2(badge.pill() == entry.pill, entry.name);
		QVERIFY2(badge.empty() == entry.text.isEmpty(), entry.name);
	}
	// Only incoming messages of a group that are not service notes.
	QVERIFY(HasBadge(true, false, false));
	QVERIFY(!HasBadge(true, true, false));
	QVERIFY(!HasBadge(true, false, true));
	QVERIFY(!HasBadge(false, false, false));
}

void TestGroupRules::memberTagSenders() {
	using namespace MemberTags;
	// A message says who its sender is now…
	QCOMPARE(*FromMessage(u"admin"_s, QString(), false), (Sender{ QString(), Role::Admin }));
	QCOMPARE(*FromMessage(u"owner"_s, u"x"_s, false), (Sender{ u"x"_s, Role::Owner }));
	QCOMPARE(*FromMessage(QString(), u"طراح"_s, false), (Sender{ u"طراح"_s, Role::Member }));
	// …and with neither field «an ordinary member without a tag» — only
	// from a server that has tags.
	QVERIFY(!FromMessage(QString(), QString(), false));
	QCOMPARE(*FromMessage(QString(), QString(), true), Sender());

	QCOMPARE(
		FromMember(u"member"_s, u"admin"_s, QString(), u"ناظر"_s),
		(Sender{ u"ناظر"_s, Role::Admin }));
	QCOMPARE(
		FromMember(QString(), u"member"_s, u"طراح"_s, QString()),
		(Sender{ u"طراح"_s, Role::Member }));
	QCOMPARE(FromMember(u"member"_s, u"member"_s, QString(), u"x"_s), Sender());
	// Nobody who is not in the group has a role or a tag.
	QCOMPARE(FromMember(u"banned"_s, u"admin"_s, u"x"_s, u"x"_s), Sender());
	QCOMPARE(FromMember(u"left"_s, u"owner"_s, u"x"_s, QString()), Sender());
}

void TestGroupRules::memberTagWhoMaySet() {
	using namespace MemberTags;
	// My own tag.
	QVERIFY(CanEditOwn(u"owner"_s, false, true));
	QVERIFY(CanEditOwn(u"admin"_s, false, true));
	QVERIFY(CanEditOwn(u"member"_s, true, true));
	QVERIFY(!CanEditOwn(u"member"_s, false, true));
	QVERIFY(!CanEditOwn(u"admin"_s, true, false)); // not in the group
	QVERIFY(!CanEditOwn(QString(), false, true));

	// Somebody else's.
	using GroupRights::Member;
	const auto owner = Member::Of(u"u1"_s, u"owner"_s);
	const auto tagger = Member::Of(u"u2"_s, u"admin"_s, GroupRights::kManageTags);
	const auto plain = Member::Of(u"u3"_s, u"admin"_s, GroupRights::kBanUsers);
	const auto member = Member::Of(u"u4"_s, u"member"_s);
	const auto other = Member::Of(u"u5"_s, u"member"_s);
	auto gone = Member::Of(u"u6"_s, u"member"_s);
	gone.status = u"left"_s;
	QVERIFY(CanEditOther(owner, member));
	QVERIFY(CanEditOther(owner, tagger));    // an admin's: the owner's to set
	QVERIFY(CanEditOther(tagger, member));
	QVERIFY(!CanEditOther(tagger, plain));   // another admin's
	QVERIFY(!CanEditOther(tagger, owner));   // the owner's: nobody's
	QVERIFY(!CanEditOther(plain, member));   // no right
	QVERIFY(!CanEditOther(member, other));
	QVERIFY(!CanEditOther(owner, owner));    // the own tag is CanEditOwn's
	QVERIFY(!CanEditOther(owner, gone));     // 409 group.target_not_member
}

void TestGroupRules::memberTagRows() {
	using namespace MemberTags;
	const auto owner = u"مالک"_s;
	const auto admin = u"ادمین"_s;
	const auto add = u"افزودن تگ"_s;
	const auto row = [&](Role role, const QString &tag, bool self, bool canEdit) {
		return RowTagFor(role, tag, self, canEdit, owner, admin, add);
	};
	QCOMPARE(row(Role::Owner, QString(), false, false),
		(RowTag{ RowMode::Pill, owner, Role::Owner, false }));
	QCOMPARE(row(Role::Admin, u"ناظر"_s, false, true),
		(RowTag{ RowMode::Pill, u"ناظر"_s, Role::Admin, true }));
	QCOMPARE(row(Role::Admin, QString(), true, true),
		(RowTag{ RowMode::Pill, admin, Role::Admin, true }));
	QCOMPARE(row(Role::Member, u"طراح"_s, false, false),
		(RowTag{ RowMode::Text, u"طراح"_s, Role::Member, false }));
	QCOMPARE(row(Role::Member, u"طراح"_s, false, true),
		(RowTag{ RowMode::Text, u"طراح"_s, Role::Member, true }));
	// My own row without a tag: the button, when I may set one.
	QCOMPARE(row(Role::Member, QString(), true, true),
		(RowTag{ RowMode::Add, add, Role::Member, true }));
	QCOMPARE(row(Role::Member, QString(), true, false), RowTag());
	// Somebody else without a tag: nothing (the row's menu offers it).
	QCOMPARE(row(Role::Member, QString(), false, true), RowTag());
	QCOMPARE(row(Role::Member, kGrin, false, false), RowTag());
}

void TestGroupRules::memberTagAdminLog() {
	using GroupAdminLog::Form;
	using GroupAdminLog::FormOf;
	using GroupAdminLog::NamesTarget;
	using namespace GroupAdminLog;
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","target":"u2","detail":{"tag":"b","prev_tag":"a"}})")),
		Form::TagChanged);
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","target":"u2","detail":{"tag":"b"}})")),
		Form::TagSet);
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","target":"u2","detail":{"prev_tag":"a"}})")),
		Form::TagRemoved);
	// The actor's own tag (the target is the actor, or left out).
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","target":"u1","detail":{"tag":"b","prev_tag":"a"}})")),
		Form::OwnTagChanged);
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","detail":{"tag":"b"}})")),
		Form::OwnTagSet);
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","target":"u1","detail":{"prev_tag":"a"}})")),
		Form::OwnTagRemoved);
	// Nothing before, nothing after: nothing to say but «a change».
	QCOMPARE(
		FormOf(Entry(R"({"action":"edit_tag","actor":"u1","target":"u2","detail":{"tag":"  "}})")),
		Form::Generic);
	QVERIFY(NamesTarget(Form::TagChanged));
	QVERIFY(NamesTarget(Form::TagSet));
	QVERIFY(NamesTarget(Form::TagRemoved));
	QVERIFY(!NamesTarget(Form::OwnTagChanged));
	QVERIFY(!NamesTarget(Form::OwnTagSet));
	QVERIFY(!NamesTarget(Form::OwnTagRemoved));

	// «تغییر تگ‌ها» in the filter.
	QCOMPARE(Actions(Tags), QStringList{ u"edit_tag"_s });
	QCOMPARE(ActionsParam(1 << Tags), u"edit_tag"_s);
	QCOMPARE(
		ActionsParam((1 << Members) | (1 << Tags)),
		u"restrict,unrestrict,remove,ban,unban,join_approve,join_decline,edit_tag"_s);
}

QTEST_GUILESS_MAIN(TestGroupRules)

#include "test_group_rules.moc"
