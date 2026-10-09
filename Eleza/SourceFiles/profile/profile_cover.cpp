#include "profile/profile_cover.h"

#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "profile/profile_follow.h"
#include "support/links.h"
#include "ui/painter.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_profile.h"

#include <QtCore/QDateTime>

namespace Profile {
namespace {

constexpr auto kMutualFaces = 3;

} // namespace

StatButton::StatButton(QWidget *parent)
: RippleButton(parent, st::defaultRipple) {
	setClickable(false);
}

void StatButton::setValue(qint64 value, const QString &label) {
	const auto text = Lang::CompactCount(value);
	if (_value != text || _label != label) {
		_value = text;
		_label = label;
		update();
	}
}

void StatButton::setClickable(bool clickable) {
	_clickable = clickable;
	setAttribute(Qt::WA_TransparentForMouseEvents, !clickable);
	setPointerCursor(clickable);
	update();
}

QImage StatButton::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), st::profileStatRadius);
}

void StatButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	update();
}

void StatButton::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	if (_clickable && isOver()) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::stateHover);
		p.drawRoundedRect(rect(), st::profileStatRadius, st::profileStatRadius);
	}
	paintRipple(p, 0, 0);
	const auto &valueFont = st::profileStatValueFont;
	const auto &labelFont = st::profileStatLabelFont;
	const auto total = valueFont->height + labelFont->height;
	const auto top = (height() - total) / 2;
	p.setPen(st::textPrimary);
	p.setFont(valueFont->f);
	p.drawText(
		(width() - valueFont->width(_value)) / 2,
		top + valueFont->ascent,
		_value);
	p.setPen(st::textSecondary);
	p.setFont(labelFont->f);
	const auto label = labelFont->elided(_label, width());
	p.drawText(
		(width() - labelFont->width(label)) / 2,
		top + valueFont->height + labelFont->ascent,
		label);
}

Cover::Cover(QWidget *parent, Window::SessionController *controller)
: RpWidget(parent)
, _controller(controller)
, _userpic(Ui::CreateChild<Support::UserpicButton>(
	this,
	&controller->session(),
	QString(),
	st::profileUserpicSize))
, _posts(Ui::CreateChild<StatButton>(this))
, _followers(Ui::CreateChild<StatButton>(this))
, _following(Ui::CreateChild<StatButton>(this))
, _handle(Ui::CreateChild<Ui::LinkButton>(this, QString(), st::profileHandleLink))
, _link(Ui::CreateChild<Ui::LinkButton>(this, QString(), st::profileWebLink))
, _bio(Ui::CreateChild<Ui::FlatLabel>(this, st::profileBioLabel))
, _follow(Ui::CreateChild<FollowButton>(this))
, _message(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_profile_message(tr::now),
	st::profileActionButton))
, _more(Ui::CreateChild<Ui::IconButton>(this, st::profileMoreButton))
, _edit(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_profile_edit_profile(tr::now),
	st::profileActionButton))
, _share(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_profile_share_profile(tr::now),
	st::profileActionButton))
, _requests(Ui::CreateChild<Ui::SettingsButton>(
	this,
	tr::lng_eli_follow_requests(tr::now),
	st::profileRequestsButton))
, _mutualPics(&controller->session(), [this](const QString &) { update(); }) {
	_message->setIcon(&st::profileMessageIcon);
	_requests->setIcon(&st::profileRequestsIcon);
	_requests->setChevron(true);
	_bio->setSelectable(true);
	_bio->setLinkHandler(Support::LinkHandler(controller));

	_userpic->setClickedCallback([this] { Q_EMIT photoClicked(); });
	_followers->setClickedCallback([this] { Q_EMIT followersClicked(); });
	_following->setClickedCallback([this] { Q_EMIT followingClicked(); });
	_handle->setClickedCallback([this] { Q_EMIT handleClicked(); });
	_link->setClickedCallback([this] {
		Support::OpenLink(_controller, NormalizeLink(_profile.link));
	});
	_follow->setClickedCallback([this] { Q_EMIT followClicked(); });
	_message->setClickedCallback([this] { Q_EMIT messageClicked(); });
	_more->setClickedCallback([this] { Q_EMIT moreClicked(); });
	_edit->setClickedCallback([this] { Q_EMIT editClicked(); });
	_share->setClickedCallback([this] { Q_EMIT shareClicked(); });
	_requests->setClickedCallback([this] { Q_EMIT requestsClicked(); });

	for (const auto widget : std::initializer_list<QWidget*>{
		_handle, _link, _bio, _follow, _message, _more, _edit, _share,
		_requests,
	}) {
		widget->hide();
	}
}

QWidget *Cover::moreButton() const {
	return _more;
}

void Cover::setProfile(const Data::Profile &profile, const ButtonModel &model) {
	_profile = profile;
	_model = model;
	_userpic->setUserId(profile.id);
	_posts->setValue(profile.posts, tr::lng_profile_posts(tr::now));
	_followers->setValue(profile.followers, tr::lng_profile_followers(tr::now));
	_following->setValue(profile.following, tr::lng_profile_following(tr::now));
	_followers->setClickable(model.statsClickable);
	_following->setClickable(model.statsClickable);
	_name.setText(st::profileNameStyle, profile.displayName);
	_handle->setText(profile.handle.isEmpty()
		? QString()
		: LtrText(QLatin1Char('@') + profile.handle));
	_link->setText(profile.link.isEmpty()
		? QString()
		: LtrText(DisplayLink(profile.link)));
	if (profile.bio.isEmpty()) {
		_bio->setText(QString());
	} else {
		_bio->setMarkedText(ParseBio(profile.bio));
	}
	refreshMeta();
	if (model.mutuals && !profile.mutuals.isEmpty()) {
		auto names = QStringList();
		for (const auto &user : profile.mutuals) {
			names.push_back(user.displayName);
		}
		_mutuals.setText(
			st::profileMetaStyle,
			MutualsText(names, profile.mutualsTotal));
	} else {
		_mutuals.clear();
	}
	_follow->setState(FollowUiOf(profile));
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void Cover::refreshMeta() {
	auto parts = QStringList();
	if (!_profile.metaLine.isEmpty()) {
		parts.push_back(_profile.metaLine);
	}
	if (!_profile.province.isEmpty()) {
		parts.push_back(ProvinceTitle(_profile.province));
	}
	if (_profile.birthday) {
		parts.push_back(tr::lng_profile_birthday_on(
			tr::now,
			lt_date,
			BirthdayText(*_profile.birthday)));
	}
	_meta.setText(st::profileMetaStyle, parts.join(u" · "_s));
}

void Cover::setFollow(FollowUi state, bool busy) {
	_follow->setState(state);
	_follow->setBusy(busy);
}

void Cover::setFollowers(qint64 count) {
	_profile.followers = count;
	_followers->setValue(count, tr::lng_profile_followers(tr::now));
}

void Cover::setPresence(const QString &text, bool online) {
	if (_presence == text && _online == online) {
		return;
	}
	const auto relayout = _presence.isEmpty() != text.isEmpty();
	_presence = text;
	_online = online;
	if (relayout && width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void Cover::setRequests(bool shown, int count) {
	_requests->setValueText((count > 0) ? Lang::Num(count) : QString());
	if (_requestsShown != shown) {
		_requestsShown = shown;
		if (width() > 0) {
			resizeToWidth(width());
		}
	}
}

int Cover::resizeGetHeight(int newWidth) {
	const auto &padding = st::profileCoverPadding;
	const auto left = padding.left();
	const auto inner = std::max(newWidth - left - padding.right(), 0);
	auto y = padding.top();

	const auto size = int(st::profileUserpicSize);
	_userpic->moveToLeft(left, y, newWidth);
	const auto statsLeft = left + size + st::profileStatsSkip;
	const auto statsWidth = std::max(inner - size - st::profileStatsSkip, 0);
	const auto cell = std::min(statsWidth / 3, int(st::profileStatMaxWidth));
	const auto statTop = y + (size - st::profileStatHeight) / 2;
	// Three equal cells spread over the rest of the row.
	const auto gap = (statsWidth - 3 * cell) / 3;
	auto statLeft = statsLeft + gap / 2;
	for (const auto stat : { _posts, _followers, _following }) {
		stat->setGeometryToLeft(
			statLeft,
			statTop,
			cell,
			st::profileStatHeight,
			newWidth);
		statLeft += cell + gap;
	}
	y += size + st::profileNameSkip;

	auto badges = 0;
	if (_profile.verified) {
		badges += st::profileVerifiedIcon.width() + st::profileBadgeSkip;
	}
	if (_profile.isPrivate) {
		badges += st::profileLockIcon.width() + st::profileBadgeSkip;
	}
	_nameTop = y;
	_nameWidth = std::min(_name.maxWidth(), std::max(inner - badges, 0));
	y += st::profileNameStyle.font->height;

	const auto hasHandle = !_profile.handle.isEmpty();
	_handle->setVisible(hasHandle);
	if (hasHandle || !_presence.isEmpty()) {
		y += st::profileHandleSkip;
		_presenceTop = y;
		_presenceLeft = left;
		if (hasHandle) {
			_handle->resizeToNaturalWidth(inner);
			_handle->moveToLeft(left, y, newWidth);
			_presenceLeft = left + _handle->width();
		}
		y += st::profileHandleLink.font->height;
	}

	_metaHeight = 0;
	if (!_meta.isEmpty()) {
		y += st::profileMetaSkip;
		_metaTop = y;
		_metaHeight = _meta.countHeight(inner, 2);
		y += _metaHeight;
	}

	const auto hasLink = !_profile.link.isEmpty();
	_link->setVisible(hasLink);
	if (hasLink) {
		y += st::profileLinkSkip;
		const auto iconWidth = st::profileLinkIcon.width() + st::profileBadgeSkip;
		_link->resizeToNaturalWidth(std::max(inner - iconWidth, 0));
		_link->moveToLeft(left + iconWidth, y, newWidth);
		_linkIconTop = y + (_link->height() - st::profileLinkIcon.height()) / 2;
		y += _link->height();
	}

	const auto hasBio = !_profile.bio.isEmpty();
	_bio->setVisible(hasBio);
	if (hasBio) {
		y += st::profileBioSkip;
		_bio->resizeToWidth(inner);
		_bio->moveToLeft(left, y, newWidth);
		y += _bio->height();
	}

	if (!_mutuals.isEmpty()) {
		y += st::profileMutualsSkip;
		_mutualsTop = y;
		const auto faces = std::min(int(_profile.mutuals.size()), kMutualFaces);
		const auto face = int(st::profileMutualSize);
		const auto pile = face + (faces - 1) * (face - st::profileMutualOverlap);
		_mutualsTextLeft = left + pile + st::profileMutualsTextSkip;
		_mutualsTextWidth = std::max(
			inner - pile - st::profileMutualsTextSkip,
			0);
		y += std::max(face, _mutuals.countHeight(_mutualsTextWidth, 2));
	}

	const auto buttonHeight = int(st::profileActionButton.height);
	const auto skip = int(st::profileActionsSkip);
	const auto stretch = (inner <= st::profileActionsStretchWidth);
	const auto own = _model.has(Action::EditProfile);
	const auto follow = _model.has(Action::Follow);
	const auto more = _model.has(Action::More);
	_edit->setVisible(own);
	_share->setVisible(own);
	_follow->setVisible(follow);
	_message->setVisible(_model.has(Action::Message));
	_more->setVisible(more);
	if (!_model.actions.isEmpty()) {
		y += st::profileActionsTop;
		if (own) {
			const auto half = (inner - skip) / 2;
			const auto editWidth = stretch
				? half
				: std::max(_edit->naturalWidth(), int(st::profileActionMinWidth));
			const auto shareWidth = stretch
				? (inner - skip - half)
				: std::max(_share->naturalWidth(), int(st::profileActionMinWidth));
			_edit->setFullWidth(editWidth);
			_share->setFullWidth(shareWidth);
			_edit->moveToLeft(left, y, newWidth);
			_share->moveToLeft(left + editWidth + skip, y, newWidth);
		} else {
			const auto moreWidth = more ? _more->width() : 0;
			if (follow) {
				const auto messageWidth = _message->naturalWidth();
				const auto followWidth = stretch
					? std::max(inner - messageWidth - moreWidth - 2 * skip, 0)
					: std::max(
						_follow->naturalWidth(),
						int(st::profileFollowMinWidth));
				_follow->resize(followWidth, buttonHeight);
				_follow->moveToLeft(left, y, newWidth);
				_message->setFullWidth(messageWidth);
				_message->moveToLeft(left + followWidth + skip, y, newWidth);
				_more->moveToLeft(
					left + followWidth + messageWidth + 2 * skip,
					y + (buttonHeight - _more->height()) / 2,
					newWidth);
			} else if (more) {
				// Blocked: only the menu.
				_more->moveToLeft(
					left - (_more->width() - st::profileMoreButton.rippleSize) / 2,
					y + (buttonHeight - _more->height()) / 2,
					newWidth);
			}
		}
		y += buttonHeight;
	}

	_requests->setVisible(_requestsShown);
	if (_requestsShown) {
		y += st::profileRequestsSkip;
		_requests->resizeToWidth(newWidth);
		_requests->moveToLeft(0, y, newWidth);
		y += _requests->height();
		return y + st::profileRequestsSkip;
	}
	return y + padding.bottom();
}

void Cover::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto outer = width();
	const auto &padding = st::profileCoverPadding;
	const auto left = padding.left();
	const auto inner = std::max(outer - left - padding.right(), 0);

	p.setPen(st::textPrimary);
	_name.drawLeftElided(p, left, _nameTop, _nameWidth, outer);
	auto badgeLeft = left + _nameWidth + st::profileBadgeSkip;
	const auto nameHeight = st::profileNameStyle.font->height;
	if (_profile.verified) {
		const auto &icon = st::profileVerifiedIcon;
		icon.paint(
			p,
			badgeLeft,
			_nameTop + (nameHeight - icon.height()) / 2,
			outer);
		badgeLeft += icon.width() + st::profileBadgeSkip;
	}
	if (_profile.isPrivate) {
		const auto &icon = st::profileLockIcon;
		icon.paint(
			p,
			badgeLeft,
			_nameTop + (nameHeight - icon.height()) / 2,
			outer);
	}

	if (!_presence.isEmpty()) {
		const auto &font = st::profileHandleLink.font;
		const auto separator = _profile.handle.isEmpty()
			? QString()
			: u" · "_s;
		p.setFont(font->f);
		auto x = _presenceLeft;
		if (!separator.isEmpty()) {
			p.setPen(st::textTertiary);
			p.drawTextLeft(x, _presenceTop, outer, separator);
			x += font->width(separator);
		}
		p.setPen(_online ? st::brandText : st::textTertiary);
		p.drawTextLeft(
			x,
			_presenceTop,
			outer,
			font->elided(_presence, std::max(left + inner - x, 0)));
	}

	if (_metaHeight > 0) {
		p.setPen(st::textTertiary);
		_meta.drawLeftElided(p, left, _metaTop, inner, outer, 2);
	}
	if (!_profile.link.isEmpty()) {
		st::profileLinkIcon.paint(p, left, _linkIconTop, outer);
	}
	if (!_mutuals.isEmpty()) {
		const auto faces = std::min(int(_profile.mutuals.size()), kMutualFaces);
		const auto face = int(st::profileMutualSize);
		const auto step = face - int(st::profileMutualOverlap);
		auto hq = PainterHighQualityEnabler(p);
		// The first face lies on top: painted last.
		for (auto i = faces - 1; i >= 0; --i) {
			const auto rect = style::rtlrect(
				left + i * step,
				_mutualsTop,
				face,
				face,
				outer);
			p.setPen(Qt::NoPen);
			p.setBrush(st::bgCanvas);
			p.drawEllipse(rect.adjusted(-2, -2, 2, 2));
			_mutualPics.paint(p, rect, _profile.mutuals[i]);
		}
		p.setPen(st::textSecondary);
		const auto textHeight = _mutuals.countHeight(_mutualsTextWidth, 2);
		_mutuals.drawLeftElided(
			p,
			_mutualsTextLeft,
			_mutualsTop + std::max((face - textHeight) / 2, 0),
			_mutualsTextWidth,
			outer,
			2);
	}
}

ConversationCard::ConversationCard(QWidget *parent)
: RippleButton(parent, st::profileCardRipple)
, _timer([this] { refreshTime(); }) {
}

void ConversationCard::setConversation(
		const Data::OpenConversation &conversation) {
	_id = conversation.id;
	_expiresAt = conversation.expiresAt;
	_topic.setText(st::profileCardTopicStyle, conversation.topic);
	refreshTime();
	// A local clock tick for the «… مانده» label, not a server poll.
	_timer.callEach(30'000);
	if (width() > 0) {
		resizeToWidth(width());
	}
}

bool ConversationCard::expired() const {
	return _expiresAt * 1000 <= QDateTime::currentMSecsSinceEpoch();
}

void ConversationCard::refreshTime() {
	const auto was = _time;
	const auto ended = expired();
	_time = ended
		? QString()
		: RemainingText(_expiresAt, QDateTime::currentMSecsSinceEpoch());
	setAttribute(Qt::WA_TransparentForMouseEvents, ended);
	if (ended) {
		_timer.cancel();
	}
	if (was != _time) {
		update();
		if (was.isEmpty() != _time.isEmpty() && width() > 0) {
			resizeToWidth(width());
		}
	}
}

int ConversationCard::resizeGetHeight(int newWidth) {
	const auto &padding = st::profileCardPadding;
	const auto inner = std::max(newWidth - padding.left() - padding.right(), 0);
	auto result = padding.top()
		+ st::profileCardPillHeight
		+ st::profileCardSkip
		+ _topic.countHeight(inner, 3);
	if (!expired()) {
		result += st::profileCardSkip + st::profileCardJoin.height;
	}
	return result + padding.bottom();
}

QImage ConversationCard::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), st::profileCardRadius);
}

void ConversationCard::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto outer = width();
	const auto &padding = st::profileCardPadding;
	const auto inner = std::max(outer - padding.left() - padding.right(), 0);
	const auto ended = expired();
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::bgNight);
		p.drawRoundedRect(rect(), st::profileCardRadius, st::profileCardRadius);
	}
	paintRipple(p, 0, 0);

	// The status pill.
	const auto &pillFont = st::profileCardPillFont;
	const auto pillText = ended
		? tr::lng_ended(tr::now)
		: tr::lng_chat_waiting_listener(tr::now);
	const auto dot = int(st::profileCardDot);
	const auto pillPadding = int(st::profileCardPillPadding);
	const auto pillWidth = pillFont->width(pillText) + dot + 6 + 2 * pillPadding;
	const auto pillHeight = int(st::profileCardPillHeight);
	const auto pill = style::rtlrect(
		padding.left(),
		padding.top(),
		pillWidth,
		pillHeight,
		outer);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(ended ? st::onNightTint : st::accentLampSoft);
		p.drawRoundedRect(pill, pillHeight / 2., pillHeight / 2.);
		p.setBrush(ended ? st::textOnNight : st::accentLamp);
		const auto dotRect = style::rtlrect(
			padding.left() + pillPadding,
			padding.top() + (pillHeight - dot) / 2,
			dot,
			dot,
			outer);
		p.drawEllipse(dotRect);
	}
	p.setFont(pillFont->f);
	p.setPen(ended ? st::textOnNight : st::textPrimary);
	p.drawTextLeft(
		padding.left() + pillPadding + dot + 6,
		padding.top() + (pillHeight - pillFont->height) / 2,
		outer,
		pillText);
	if (!_time.isEmpty()) {
		p.setPen(st::brandMint);
		p.drawTextLeft(
			padding.left() + pillWidth + st::profileCardSkip,
			padding.top() + (pillHeight - pillFont->height) / 2,
			outer,
			_time);
	}

	const auto topicTop = padding.top() + pillHeight + st::profileCardSkip;
	p.setPen(st::textOnNight);
	_topic.drawLeftElided(p, padding.left(), topicTop, inner, outer, 3);

	if (!ended) {
		const auto &st = st::profileCardJoin;
		const auto text = tr::lng_eli_join_conversation(tr::now);
		const auto buttonWidth = st.font->width(text) + 2 * st.padding;
		const auto button = style::rtlrect(
			padding.left(),
			topicTop + _topic.countHeight(inner, 3) + st::profileCardSkip,
			buttonWidth,
			st.height,
			outer);
		PaintPill(p, button, st, text, isOver(), false);
	}
}

StateBlock::StateBlock(QWidget *parent, Descriptor descriptor)
: RpWidget(parent)
, _icon(descriptor.icon)
, _title(st::profileStateTitleStyle, descriptor.title)
, _text(st::profileStateTextStyle, descriptor.text) {
	if (!descriptor.buttonText.isEmpty()) {
		_button = Ui::CreateChild<Ui::RoundButton>(
			this,
			descriptor.buttonText,
			descriptor.buttonLight
				? st::lightRoundButton
				: st::outlineRoundButton);
		_button->setClickedCallback(std::move(descriptor.buttonCallback));
	}
}

int StateBlock::resizeGetHeight(int newWidth) {
	const auto inner = std::min(
		std::max(newWidth - 2 * st::profileStatePadding, 0),
		int(st::profileStateMaxWidth));
	auto y = int(st::profileStateTop) + st::profileStateGlyph + st::spaceLg;
	_titleTop = y;
	y += _title.countHeight(inner);
	if (!_text.isEmpty()) {
		y += st::spaceSm;
		_textTop = y;
		y += _text.countHeight(inner);
	}
	if (_button) {
		y += st::spaceLg;
		_button->resizeToNaturalWidth(inner);
		_button->move((newWidth - _button->width()) / 2, y);
		y += _button->height();
	}
	return y + st::profileStateTop;
}

void StateBlock::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto inner = std::min(
		std::max(width() - 2 * st::profileStatePadding, 0),
		int(st::profileStateMaxWidth));
	const auto left = (width() - inner) / 2;
	const auto glyph = int(st::profileStateGlyph);
	const auto circle = QRect(
		(width() - glyph) / 2,
		st::profileStateTop,
		glyph,
		glyph);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::bgSunken);
		p.drawEllipse(circle);
	}
	if (_icon) {
		_icon->paintInCenter(p, circle);
	}
	p.setPen(st::textPrimary);
	_title.draw(p, left, _titleTop, inner, style::al_top);
	if (!_text.isEmpty()) {
		p.setPen(st::textSecondary);
		_text.draw(p, left, _textTop, inner, style::al_top);
	}
}

} // namespace Profile
