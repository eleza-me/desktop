// Member tags (Telegram's "member tags"): the badge next to a sender's name,
// the box a tap on it opens, the box that sets a tag, and what decides who
// may set one. The rules themselves are Data::MemberTags (data_group_rules.h);
// the tags people have are kept by Data::Groups (senderTag, setTag).
//
//   const auto badge = Groups::TagBadge(role, tag);           // «مالک», «ادمین» or the tag
//   Groups::PaintTagBadge(p, rect, badge.text, badge.role, badge.pill(), font);
//   Groups::ShowTagInfoBox(controller, groupId, userId, badge.text, badge.role);
//   Groups::ShowEditTagBox(controller, groupId, userId, Groups::KnownTag(...));
//
// The behaviour follows Telegram Desktop's (TagInfoBox, EditTagControl,
// EditCustomRankBox, MemberListRow, the RightBadge of a message); the code is
// this application's own.
#pragma once

#include "base/basic_types.h"
#include "data/data_group_rules.h"
#include "ui/style/style_core.h"

#include <QtCore/QRect>
#include <QtGui/QColor>

class QPainter;

namespace Data {
struct Group;
struct GroupMember;
struct Message;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

namespace Groups {

using TagRole = Data::MemberTags::Role;
using TagSender = Data::MemberTags::Sender;

// Purple for the owner, green for an admin, grey for a member.
[[nodiscard]] QColor TagColor(TagRole role);
// The badge of a sender in the current language («مالک», «ادمین» or the tag).
[[nodiscard]] Data::MemberTags::Badge TagBadge(TagRole role, const QString &tag);

// The width of a badge: a pill at least as wide as it is high, or the text.
[[nodiscard]] int TagBadgeWidth(
	const QString &text,
	bool pill,
	const style::font &font,
	int pillHeight,
	int pillPadding);
// Paints a badge filling `rect` (as wide as TagBadgeWidth): a pill of the
// role's colour at `fillAlpha` with the text on it, or the text in grey,
// vertically centred. The text is already elided to fit.
void PaintTagBadge(
	QPainter &p,
	const QRect &rect,
	const QString &text,
	TagRole role,
	bool pill,
	const style::font &font,
	double fillAlpha = 0.15);

// The name line of a bubble: the name and the badge at its end. The name has
// the priority: the badge takes what the whole name leaves, but never less
// than a third of the line (then the name is elided).
struct NameLine {
	QString name;       // elided
	QString badge;      // elided ("" = none)
	int nameWidth = 0;
	int badgeWidth = 0; // the whole badge (a pill with its sides)
};
[[nodiscard]] NameLine LayoutNameLine(
	const QString &name,
	const style::font &nameFont,
	const QString &badge,
	bool pill,
	const style::font &badgeFont,
	int pillHeight,
	int pillPadding,
	int skip,
	int width);
// The width a name line wants (for the width of a bubble).
[[nodiscard]] int NameLineNaturalWidth(
	const QString &name,
	const style::font &nameFont,
	const QString &badge,
	bool pill,
	const style::font &badgeFont,
	int pillHeight,
	int pillPadding,
	int skip);

// ---- who may set a tag (from the header of the group I hold)
// Me as the rights table sees me.
[[nodiscard]] Data::GroupRights::Member TagActor(
	Window::SessionController *controller,
	const Data::Group &header);
// My own tag: a server that has tags, I am in the group, and I am an admin
// or the owner or hold `edit_tag`.
[[nodiscard]] bool MayEditOwnTag(
	Window::SessionController *controller,
	const QString &groupId);
// Somebody else's: `manage_tags` and the target rule (MemberTags::CanEditOther).
[[nodiscard]] bool MayEditTagOf(
	Window::SessionController *controller,
	const QString &groupId,
	const QString &userId,
	TagRole targetRole);
// What a person is known as in a group now: the map of Data::Groups, my own
// header for me, then what `message` said of its sender.
[[nodiscard]] TagSender KnownTag(
	Window::SessionController *controller,
	const QString &groupId,
	const QString &userId,
	const Data::Message *message = nullptr);

// ---- the boxes
// A tap on a badge: what this tag is, two examples, and my own tag.
void ShowTagInfoBox(
	Window::SessionController *controller,
	const QString &groupId,
	const QString &userId,
	const QString &tagText,
	TagRole role);
// Sets the tag of `userId` (mine too). `current`: the tag and the role now.
void ShowEditTagBox(
	Window::SessionController *controller,
	const QString &groupId,
	const QString &userId,
	const TagSender &current,
	Fn<void(const Data::GroupMember &)> saved = nullptr);

} // namespace Groups
