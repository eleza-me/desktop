#include "groups/groups_tags.h"

#include "api/api_error.h"
#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_users.h"
#include "history/history_themes.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/chat_text.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/image/userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rtl.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_groups.h"
#include "styles/style_history.h"

#include <QtCore/QTime>
#include <QtGui/QLinearGradient>
#include <QtGui/QPaintEvent>
#include <QtGui/QRegion>

#include <array>

namespace Groups {
namespace {

namespace Rights = Data::GroupRights;
namespace Tags = Data::MemberTags;

constexpr auto kPillAlpha = 0.15;
constexpr auto kTextLinesAlpha = 0.1;
constexpr auto kNameColors = 8;

[[nodiscard]] Data::Groups &Store(Window::SessionController *controller) {
	return controller->session().data().groups();
}

[[nodiscard]] QString GroupTitle(
		Window::SessionController *controller,
		const QString &groupId) {
	const auto header = Store(controller).group(groupId);
	return (header && !header->title.isEmpty())
		? header->title
		: tr::lng_groups_group(tr::now);
}

// The colour of a name above an incoming bubble (as the chat paints it).
[[nodiscard]] QColor NameColor(const QString &userId, const QColor &bubble) {
	auto color = Ui::UserpicColor(
		Data::GroupRules::NameColorIndex(userId, kNameColors))->c;
	return (bubble.lightness() > 127) ? color.darker(135) : color.lighter(120);
}

[[nodiscard]] QColor Faded(QColor color, double alpha) {
	color.setAlphaF(color.alphaF() * alpha);
	return color;
}

// `rect` mirrored inside `outer` in a right-to-left interface.
[[nodiscard]] QRect Place(const QRect &rect, int outer, bool rtl) {
	return rtl
		? QRect(outer - rect.x() - rect.width(), rect.y(), rect.width(), rect.height())
		: rect;
}

// The three faint lines of a mini bubble; the last ones end at `timeLeft`
// (where the time is) when they reach its row.
void PaintTextLines(
		QPainter &p,
		const QColor &color,
		int left,
		int top,
		int available,
		int lineHeight,
		int lineSkip,
		const std::array<double, 3> &fractions,
		int timeTop,
		int timeLeft,
		int outer,
		bool rtl) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	auto y = top;
	for (auto i = 0; i != int(fractions.size()); ++i) {
		auto width = int(available * fractions[i]);
		if (i > 0 && y + lineHeight > timeTop) {
			width = std::min(width, timeLeft - left);
		}
		if (width > 0) {
			const auto rect = Place(QRect(left, y, width, lineHeight), outer, rtl);
			p.drawRoundedRect(rect, lineHeight / 2., lineHeight / 2.);
		}
		y += lineHeight + lineSkip;
	}
}

// ---- the round logo of the info box: the role's colour, a person and a tag

class TagLogo final : public Ui::RpWidget {
public:
	TagLogo(QWidget *parent, TagRole role)
	: RpWidget(parent)
	, _role(role) {
		style::OnPaletteChanged(this, [this] { update(); });
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::groupsTagInfoLogo;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = int(st::groupsTagInfoLogo);
		const auto outer = QRect((width() - size) / 2, 0, size, size);
		p.setPen(Qt::NoPen);
		p.setBrush(TagColor(_role));
		p.drawEllipse(outer);
		st::groupsTagInfoIcon.paintInCenter(p, outer);
	}

private:
	TagRole _role = TagRole::Member;

};

// ---- «این تگ سبز [ادمین] تگ ادمینِ **سارا** در **تیم الزا** است.»

class TagSentence final : public Ui::RpWidget {
public:
	TagSentence(
		QWidget *parent,
		const Ui::Text::TextWithEntities &text,
		Ui::Text::Selection tag,
		TagRole role)
	: RpWidget(parent)
	, _tag(tag)
	, _role(role) {
		_text.setMarkedText(st::groupsTagInfoTextStyle, text);
		style::OnPaletteChanged(this, [this] { update(); });
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return _text.countHeight(std::max(newWidth, 1));
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto geometry = context();
		const auto rects = _text.rangeRects(_tag, geometry);
		auto tagRegion = QRegion();
		const auto color = TagColor(_role);
		if (_role != TagRole::Member) {
			auto hq = PainterHighQualityEnabler(p);
			const auto height = double(st::groupsTagInfoPillHeight);
			const auto padding = double(st::groupsTagInfoPillPadding);
			p.setPen(Qt::NoPen);
			p.setBrush(Faded(color, kPillAlpha));
			for (const auto &rect : rects) {
				const auto pill = QRectF(
					rect.x() - padding,
					rect.center().y() - height / 2.,
					rect.width() + 2. * padding,
					height);
				p.drawRoundedRect(pill, height / 2., height / 2.);
			}
		}
		for (const auto &rect : rects) {
			tagRegion += rect.toAlignedRect();
		}
		// The sentence, then the tag over it in the role's colour.
		p.setClipRegion(QRegion(rect()) - tagRegion);
		p.setPen(st::textPrimary);
		_text.draw(p, geometry);
		if (!tagRegion.isEmpty()) {
			p.setClipRegion(tagRegion);
			p.setPen(color);
			_text.draw(p, geometry);
		}
	}

private:
	[[nodiscard]] Ui::Text::PaintContext context() const {
		auto result = Ui::Text::PaintContext();
		result.position = QPoint(0, 0);
		result.availableWidth = std::max(width(), 1);
		result.align = style::al_center;
		return result;
	}

	Ui::Text::String _text;
	Ui::Text::Selection _tag;
	TagRole _role = TagRole::Member;

};

// The sentence with its pieces: the tag (a range the pill is drawn under)
// and the names in bold.
struct SentencePiece {
	QString key;
	QString value;
	bool tag = false;
	bool pill = false; // the tag is drawn on a pill: room for its sides
};

[[nodiscard]] Ui::Text::TextWithEntities FillSentence(
		const QString &pattern,
		const std::vector<SentencePiece> &pieces,
		Ui::Text::Selection *tagRange) {
	auto result = Ui::Text::TextWithEntities();
	auto from = 0;
	while (from < pattern.size()) {
		const auto open = int(pattern.indexOf(u'{', from));
		const auto close = (open >= 0) ? int(pattern.indexOf(u'}', open)) : -1;
		if (open < 0 || close < 0) {
			result.text += pattern.mid(from);
			break;
		}
		result.text += pattern.mid(from, open - from);
		const auto key = pattern.mid(open + 1, close - open - 1);
		const auto i = std::find_if(pieces.begin(), pieces.end(), [&](
				const SentencePiece &piece) {
			return piece.key == key;
		});
		if (i == pieces.end()) {
			result.text += pattern.mid(open, close - open + 1);
		} else if (i->tag) {
			// One unbreakable run in its own direction, with room for the
			// pill's sides around it.
			auto value = i->value;
			value.replace(u' ', QChar(0x00A0));
			if (i->pill) {
				result.text += QChar(0x2002);
			}
			result.text += QChar(0x2068);
			const auto start = int(result.text.size());
			result.text += value;
			if (tagRange) {
				*tagRange = { start, int(result.text.size()) };
			}
			result.text += QChar(0x2069);
			if (i->pill) {
				result.text += QChar(0x2002);
			}
		} else {
			const auto start = int(result.text.size());
			result.text += Lang::Isolated(i->value);
			result.entities.push_back({
				Ui::Text::EntityType::Bold,
				start,
				int(result.text.size()) - start,
				QString(),
			});
		}
		from = close + 1;
	}
	return result;
}

// ---- two examples side by side: a member's tag and an admin's (or the owner's)

class TagPreviews final : public Ui::RpWidget {
public:
	TagPreviews(QWidget *parent, TagRole role)
	: RpWidget(parent)
	, _second((role == TagRole::Owner) ? TagRole::Owner : TagRole::Admin) {
		style::OnPaletteChanged(this, [this] { update(); });
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::groupsTagInfoPreviewHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto gap = int(st::groupsTagInfoPreviewGap);
		const auto card = (width() - gap) / 2;
		if (card <= 0) {
			return;
		}
		const auto rtl = style::RightToLeft();
		// The member's card on the start side.
		const auto first = Place(QRect(0, 0, card, height()), width(), rtl);
		const auto second = Place(
			QRect(card + gap, 0, width() - card - gap, height()),
			width(),
			rtl);
		paintCard(p, first, TagRole::Member);
		paintCard(p, second, _second);
	}

private:
	void paintCard(QPainter &p, const QRect &card, TagRole role) const {
		const auto rtl = style::RightToLeft();
		const auto radius = double(st::groupsTagInfoPreviewRadius);
		p.save();
		auto clip = QPainterPath();
		clip.addRoundedRect(QRectF(card), radius, radius);
		p.setClipPath(clip);
		History::PaintWallpaper(p, card, card.topLeft(), QString());

		const auto ratio = devicePixelRatioF();
		auto image = QImage(
			card.size() * ratio,
			QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(ratio);
		image.fill(Qt::transparent);
		{
			auto q = Painter(&image);
			paintBubble(q, card.size(), role, rtl);
		}
		p.drawImage(card.topLeft(), image);
		p.restore();
	}

	// The bubble runs off the card on the start side and fades out there.
	void paintBubble(Painter &p, QSize size, TagRole role, bool rtl) const {
		const auto colors = History::ColorsForTheme(QString(), false);
		const auto outer = size.width();
		const auto radius = int(st::groupsTagInfoBubbleRadius);
		const auto padding = int(st::groupsTagInfoBubblePadding);
		const auto top = int(st::groupsTagInfoBubbleTop);
		const auto bottom = size.height() - int(st::groupsTagInfoBubbleBottom);
		const auto end = outer - int(st::groupsTagInfoBubbleEnd);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(colors.bubbleIn);
			p.drawRoundedRect(
				Place(QRect(-radius, top, end + radius, bottom - top), outer, rtl),
				radius,
				radius);
		}
		const auto innerLeft = padding;
		const auto innerRight = end - padding;
		const auto available = innerRight - innerLeft;

		const auto text = (role == TagRole::Owner)
			? tr::lng_groups_tag_info_preview_owner(tr::now)
			: (role == TagRole::Admin)
			? tr::lng_groups_tag_info_preview_admin(tr::now)
			: tr::lng_groups_tag_info_preview_member(tr::now);
		const auto &font = st::historyGroupTagFont;
		const auto pill = (role != TagRole::Member);
		const auto badgeHeight = int(st::historyGroupTagHeight);
		const auto badgeWidth = TagBadgeWidth(
			text,
			pill,
			font,
			badgeHeight,
			st::historyGroupTagPadding);
		const auto badgeTop = top + int(st::groupsTagInfoBadgeTop);
		PaintTagBadge(
			p,
			Place(
				QRect(innerRight - badgeWidth, badgeTop, badgeWidth, badgeHeight),
				outer,
				rtl),
			text,
			role,
			pill,
			font);

		const auto time = Lang::Clock(QTime(12, 0));
		const auto &timeFont = st::groupsTagInfoTimeFont;
		const auto timeWidth = timeFont->width(time);
		const auto timeTop = bottom - padding / 2 - timeFont->height;
		const auto timeLeft = innerRight - timeWidth;
		p.setFont(timeFont);
		p.setPen(colors.metaIn);
		p.drawText(
			Place(QRect(timeLeft, timeTop, timeWidth, timeFont->height), outer, rtl).x(),
			timeTop + timeFont->ascent,
			time);

		PaintTextLines(
			p,
			Faded(colors.textIn, kTextLinesAlpha),
			innerLeft,
			badgeTop + badgeHeight + int(st::groupsTagInfoLineSkip),
			available,
			st::groupsTagInfoLineHeight,
			st::groupsTagInfoLineSkip,
			{ 1., 0.65, 0.65 },
			timeTop,
			timeLeft - padding,
			outer,
			rtl);

		// The fade on the start edge: the wallpaper shows through.
		const auto fade = int(st::groupsTagInfoFade);
		auto gradient = QLinearGradient(
			rtl ? outer : 0,
			0,
			rtl ? (outer - fade) : fade,
			0);
		gradient.setStops({
			{ 0., QColor(255, 255, 255, 0) },
			{ 1., QColor(255, 255, 255, 255) },
		});
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.fillRect(
			Place(QRect(0, 0, fade, size.height()), outer, rtl),
			gradient);
	}

	TagRole _second = TagRole::Admin;

};

// ---- the bubble of the edit box: the person, their name and the tag as it
// will look for their role, updated as it is typed

class EditPreview final : public Ui::RpWidget {
public:
	EditPreview(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &userId,
		TagRole role,
		const QString &tag)
	: RpWidget(parent)
	, _session(&controller->session())
	, _userId(userId)
	, _role(role)
	, _tag(tag)
	, _userpics(_session, [this](const QString &) { update(); }) {
		connect(&_session->data().users(), &Data::Users::changed, this, [this] {
			update();
		});
		style::OnPaletteChanged(this, [this] {
			_userpics.clear();
			update();
		});
	}

	void setTag(const QString &tag) {
		if (_tag != tag) {
			_tag = tag;
			update();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		const auto &padding = st::groupsTagEditPreviewPadding;
		return padding.top() + bubbleHeight() + padding.bottom();
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		History::PaintWallpaper(p, rect(), QPoint(), QString());
		const auto colors = History::ColorsForTheme(QString(), false);
		const auto rtl = style::RightToLeft();
		const auto &padding = st::groupsTagEditPreviewPadding;
		const auto &inner = st::groupsTagEditBubblePadding;
		const auto userpic = int(st::historyGroupUserpic);
		const auto bubbleHeight = this->bubbleHeight();
		const auto top = padding.top();
		const auto bottom = top + bubbleHeight;
		// Incoming bubbles hug the left edge in both directions (as in a chat).
		const auto left = padding.left() + userpic + int(st::historyGroupUserpicSkip);
		const auto bubbleWidth = std::min(
			int(st::groupsTagEditBubbleWidth),
			width() - left - padding.right());
		if (bubbleWidth <= inner.left() + inner.right()) {
			return;
		}
		_userpics.paint(p, QRect(padding.left(), bottom - userpic, userpic, userpic), _userId);
		const auto bubble = QRect(left, top, bubbleWidth, bubbleHeight);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(colors.bubbleIn);
			const auto radius = double(st::groupsTagEditBubbleRadius);
			p.drawRoundedRect(bubble, radius, radius);
		}
		const auto x = bubble.x() + inner.left();
		const auto available = bubble.width() - inner.left() - inner.right();
		const auto nameTop = bubble.y() + inner.top();

		const auto badge = TagBadge(_role, _tag);
		const auto line = LayoutNameLine(
			Support::MemberName(_session, _userId),
			st::historyGroupNameFont,
			badge.text,
			badge.pill(),
			st::historyGroupTagFont,
			st::historyGroupTagHeight,
			st::historyGroupTagPadding,
			st::historyGroupTagSkip,
			available);
		const auto nameHeight = int(st::historyGroupNameHeight);
		const auto &nameFont = st::historyGroupNameFont;
		p.setFont(nameFont);
		p.setPen(NameColor(_userId, colors.bubbleIn));
		p.drawText(
			rtl ? (x + available - line.nameWidth) : x,
			nameTop + (nameHeight - nameFont->height) / 2 + nameFont->ascent,
			line.name);
		if (!line.badge.isEmpty()) {
			const auto height = int(st::historyGroupTagHeight);
			PaintTagBadge(
				p,
				QRect(
					rtl ? x : (x + available - line.badgeWidth),
					nameTop + (nameHeight - height) / 2,
					line.badgeWidth,
					height),
				line.badge,
				badge.role,
				badge.pill(),
				st::historyGroupTagFont);
		}

		const auto time = Lang::Clock(QTime(12, 0));
		const auto &timeFont = st::historyMetaFont;
		const auto timeWidth = timeFont->width(time);
		const auto timeTop = bottom - inner.bottom() - timeFont->height;
		p.setFont(timeFont);
		p.setPen(colors.metaIn);
		p.drawText(
			rtl ? x : (x + available - timeWidth),
			timeTop + timeFont->ascent,
			time);

		// The lines are laid out in the bubble's own coordinates, mirrored
		// in RTL around its middle.
		p.save();
		p.translate(x, 0);
		PaintTextLines(
			p,
			Faded(colors.textIn, kTextLinesAlpha),
			0,
			nameTop + nameHeight + int(st::groupsTagEditLineSkip),
			available,
			st::groupsTagEditLineHeight,
			st::groupsTagEditLineSkip,
			{ 1., 0.8, 0.55 },
			timeTop,
			available - timeWidth - int(st::historyMetaSkip),
			available,
			rtl);
		p.restore();
	}

private:
	[[nodiscard]] int bubbleHeight() const {
		const auto &inner = st::groupsTagEditBubblePadding;
		const auto lines = 3 * int(st::groupsTagEditLineHeight)
			+ 3 * int(st::groupsTagEditLineSkip);
		return inner.top()
			+ int(st::historyGroupNameHeight)
			+ lines
			+ inner.bottom();
	}

	Main::Session *_session = nullptr;
	QString _userId;
	TagRole _role = TagRole::Member;
	QString _tag;
	mutable Support::UserpicPainter _userpics;

};

} // namespace

QColor TagColor(TagRole role) {
	switch (role) {
	case TagRole::Owner: return st::rankOwnerFg->c;
	case TagRole::Admin: return st::rankAdminFg->c;
	case TagRole::Member: break;
	}
	return st::rankUserFg->c;
}

Data::MemberTags::Badge TagBadge(TagRole role, const QString &tag) {
	return Tags::BadgeFor(
		role,
		tag,
		tr::lng_groups_tag_owner(tr::now),
		tr::lng_groups_tag_admin(tr::now));
}

int TagBadgeWidth(
		const QString &text,
		bool pill,
		const style::font &font,
		int pillHeight,
		int pillPadding) {
	if (text.isEmpty()) {
		return 0;
	}
	const auto width = font->width(text);
	return pill ? std::max(width + 2 * pillPadding, pillHeight) : width;
}

void PaintTagBadge(
		QPainter &p,
		const QRect &rect,
		const QString &text,
		TagRole role,
		bool pill,
		const style::font &font,
		double fillAlpha) {
	if (text.isEmpty() || rect.isEmpty()) {
		return;
	}
	const auto color = TagColor(role);
	if (pill) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(Faded(color, fillAlpha));
		const auto radius = rect.height() / 2.;
		p.drawRoundedRect(QRectF(rect), radius, radius);
	}
	const auto width = font->width(text);
	p.setFont(font);
	p.setPen(color);
	p.drawText(
		pill ? (rect.x() + (rect.width() - width) / 2) : rect.x(),
		rect.y() + (rect.height() - font->height) / 2 + font->ascent,
		text);
}

NameLine LayoutNameLine(
		const QString &name,
		const style::font &nameFont,
		const QString &badge,
		bool pill,
		const style::font &badgeFont,
		int pillHeight,
		int pillPadding,
		int skip,
		int width) {
	auto result = NameLine();
	const auto available = std::max(width, 1);
	const auto nameFull = nameFont->width(name);
	if (!badge.isEmpty()) {
		const auto natural = TagBadgeWidth(
			badge,
			pill,
			badgeFont,
			pillHeight,
			pillPadding);
		const auto sides = pill ? (2 * pillPadding) : 0;
		const auto room = std::min(
			natural,
			std::max(available - nameFull - skip, available / 3));
		if (room >= natural) {
			result.badge = badge;
		} else if (room - sides >= 2 * badgeFont->elidew) {
			result.badge = badgeFont->elided(badge, room - sides);
		}
		result.badgeWidth = TagBadgeWidth(
			result.badge,
			pill,
			badgeFont,
			pillHeight,
			pillPadding);
	}
	const auto nameRoom = result.badge.isEmpty()
		? available
		: std::max(available - result.badgeWidth - skip, 1);
	result.name = (nameFull <= nameRoom)
		? name
		: nameFont->elided(name, nameRoom);
	result.nameWidth = nameFont->width(result.name);
	return result;
}

int NameLineNaturalWidth(
		const QString &name,
		const style::font &nameFont,
		const QString &badge,
		bool pill,
		const style::font &badgeFont,
		int pillHeight,
		int pillPadding,
		int skip) {
	const auto badgeWidth = TagBadgeWidth(
		badge,
		pill,
		badgeFont,
		pillHeight,
		pillPadding);
	return nameFont->width(name) + (badgeWidth ? (skip + badgeWidth) : 0);
}

Rights::Member TagActor(
		Window::SessionController *controller,
		const Data::Group &header) {
	auto result = Rights::Member::Of(
		controller->session().userId(),
		header.role.isEmpty() ? u"member"_s : header.role,
		Rights::RightsMask(header.rights));
	if (!header.isMember()) {
		result.status = header.status;
	}
	return result;
}

bool MayEditOwnTag(
		Window::SessionController *controller,
		const QString &groupId) {
	auto &store = Store(controller);
	const auto header = store.group(groupId);
	return header
		&& store.memberTagsIn(groupId)
		&& Tags::CanEditOwn(
			header->role,
			header->can(u"edit_tag"),
			header->isMember());
}

bool MayEditTagOf(
		Window::SessionController *controller,
		const QString &groupId,
		const QString &userId,
		TagRole targetRole) {
	auto &store = Store(controller);
	const auto header = store.group(groupId);
	if (!header || !store.memberTagsIn(groupId) || userId.isEmpty()) {
		return false;
	} else if (userId == controller->session().userId()) {
		return false;
	}
	return Tags::CanEditOther(
		TagActor(controller, *header),
		Rights::Member::Of(userId, Tags::WireRole(targetRole)));
}

TagSender KnownTag(
		Window::SessionController *controller,
		const QString &groupId,
		const QString &userId,
		const Data::Message *message) {
	auto &store = Store(controller);
	if (const auto known = store.senderTag(groupId, userId)) {
		return *known;
	}
	if (userId == controller->session().userId()) {
		if (const auto header = store.group(groupId)) {
			return TagSender{ header->myTag, Tags::RoleOf(header->role) };
		}
	}
	if (message) {
		if (const auto said = Tags::FromMessage(
				message->senderRole,
				message->senderTag,
				true)) {
			return *said;
		}
	}
	return TagSender();
}

void ShowTagInfoBox(
		Window::SessionController *controller,
		const QString &groupId,
		const QString &userId,
		const QString &tagText,
		TagRole role) {
	const auto session = &controller->session();
	const auto me = session->userId();
	const auto title = (role == TagRole::Owner)
		? tr::lng_groups_tag_info_title_owner(tr::now)
		: (role == TagRole::Admin)
		? tr::lng_groups_tag_info_title_admin(tr::now)
		: tr::lng_groups_tag_info_title_member(tr::now);
	const auto placeholders = [](auto phrase) {
		return phrase(
			tr::now,
			lt_tag,
			u"{tag}"_s,
			lt_author,
			u"{author}"_s,
			lt_group,
			u"{group}"_s);
	};
	const auto pattern = (role == TagRole::Owner)
		? placeholders(tr::lng_groups_tag_info_text_owner)
		: (role == TagRole::Admin)
		? placeholders(tr::lng_groups_tag_info_text_admin)
		: placeholders(tr::lng_groups_tag_info_text_member);
	auto tagRange = Ui::Text::Selection();
	const auto sentence = FillSentence(pattern, {
		{ u"tag"_s, tagText, true, (role != TagRole::Member) },
		{ u"author"_s, Data::GroupRules::FirstName(Support::MemberName(session, userId)) },
		{ u"group"_s, GroupTitle(controller, groupId) },
	}, &tagRange);
	const auto mayEdit = MayEditOwnTag(controller, groupId);
	const auto mine = KnownTag(controller, groupId, me);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setWidth(st::boxWideWidth);
		const auto close = Ui::CreateChild<Ui::IconButton>(
			box,
			st::groupsTagInfoClose);
		close->setClickedCallback([=] { box->closeBox(); });
		const auto place = [=](QSize size) {
			close->moveToRight(
				st::groupsTagInfoCloseTop,
				st::groupsTagInfoCloseTop,
				size.width());
			close->raise();
		};
		QObject::connect(box, &Ui::RpWidget::sizeChanged, close, place);
		place(box->size());

		box->addRow(
			Ui::object_ptr<TagLogo>(box, role),
			st::groupsTagInfoLogoPadding);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(box, title, st::groupsTagInfoTitle),
			st::groupsTagInfoTitlePadding);
		box->addRow(
			Ui::object_ptr<TagSentence>(box, sentence, tagRange, role),
			st::groupsTagInfoTextPadding);
		box->addRow(
			Ui::object_ptr<TagPreviews>(box, role),
			st::groupsTagInfoPreviewPadding);
		if (mayEdit) {
			box->addButton(
				mine.tag.isEmpty()
					? tr::lng_groups_tag_info_add_mine(tr::now)
					: tr::lng_groups_tag_info_edit_mine(tr::now),
				[=] {
					box->closeBox();
					ShowEditTagBox(controller, groupId, me, mine);
				},
				st::defaultRoundButton);
		} else {
			box->addRow(
				Ui::object_ptr<Ui::FlatLabel>(
					box,
					tr::lng_groups_tag_info_admins_only(tr::now),
					st::groupsTagInfoAdminsOnly),
				st::groupsTagInfoAdminsOnlyPadding);
			box->addButton(
				tr::lng_groups_tag_info_understood(tr::now),
				[=] { box->closeBox(); },
				st::defaultRoundButton);
		}
	}));
}

void ShowEditTagBox(
		Window::SessionController *controller,
		const QString &groupId,
		const QString &userId,
		const TagSender &current,
		Fn<void(const Data::GroupMember &)> saved) {
	const auto session = &controller->session();
	const auto self = (userId == session->userId());
	const auto max = std::max(Store(controller).adminTitleMaxChars(), 1);
	const auto name = Data::GroupRules::FirstName(
		Support::MemberName(session, userId));
	const auto initial = Tags::Typed(current.tag);
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_groups_tag_edit(tr::now));
		const auto preview = box->addRow(
			Ui::object_ptr<EditPreview>(
				box,
				controller,
				userId,
				current.role,
				initial),
			style::margins());
		const auto field = box->addRow(
			Ui::object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				tr::lng_groups_tag_placeholder(tr::now),
				initial),
			st::groupsTagEditFieldPadding);
		// (Code points are counted below; units are a loose bound.)
		field->setMaxLength(max * 2);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				self
					? tr::lng_groups_tag_about_self(tr::now)
					: tr::lng_groups_tag_about_other(
						tr::now,
						lt_name,
						Lang::Isolated(name)),
				st::groupsBoxCaption),
			st::groupsTagEditAboutPadding);
		QObject::connect(field, &Ui::InputField::changed, box, [=] {
			// Like Telegram: emoji never get into a tag, a line never breaks.
			const auto text = field->text();
			auto clean = Tags::RemoveEmoji(text);
			clean.replace(u'\n', u' ');
			const auto codes = clean.toUcs4();
			if (codes.size() > max) {
				clean = QString::fromUcs4(
					reinterpret_cast<const char32_t*>(codes.constData()),
					max);
			}
			if (clean != text) {
				field->setText(clean);
				field->setCursorToEnd();
			}
			preview->setTag(field->text());
		});
		box->setFocusCallback([=] {
			field->setFocusFast();
			field->setCursorToEnd();
		});
		const auto saving = std::make_shared<bool>(false);
		const auto submit = box->addButton(
			tr::lng_groups_save(tr::now),
			nullptr,
			st::defaultRoundButton);
		const auto save = [=] {
			if (*saving) {
				return;
			}
			const auto tag = Tags::Typed(field->text());
			if (!Tags::TypedOk(tag, max)) {
				field->showError();
				controller->showToast(tr::lng_groups_tag_invalid(tr::now));
				return;
			} else if (tag == initial) {
				box->closeBox();
				return;
			}
			*saving = true;
			submit->setLoading(true);
			Store(controller).setTag(
				groupId,
				userId,
				tag,
				base::Guard(box),
				[=](const Data::GroupMember &member) {
					if (saved) {
						saved(member);
					}
					box->closeBox();
				},
				[=](const Api::Error &error) {
					*saving = false;
					submit->setLoading(false);
					if (error.code == u"group.tag_invalid") {
						field->showError();
						controller->showToast(tr::lng_groups_tag_invalid(tr::now));
					} else {
						Support::ShowError(controller, error);
					}
				});
		};
		submit->setClickedCallback(save);
		QObject::connect(field, &Ui::InputField::submitted, box, save);
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

} // namespace Groups
