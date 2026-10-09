#include "conversation/conversation_controls.h"

#include "conversation/conversation_card.h"
#include "conversation/conversation_time.h"
#include "data/data_session.h"
#include "data/data_users.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/media_image.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "styles/style_conversation.h"

#include <QtCore/QDateTime>

namespace Conversation {

// ---- ChipFlow

ChipFlow::ChipFlow(QWidget *parent)
: RpWidget(parent) {
}

void ChipFlow::setItems(const std::vector<Item> &items) {
	for (const auto &chip : _chips) {
		delete chip.button;
	}
	_chips.clear();
	for (const auto &item : items) {
		const auto button = new Ui::PillButton(
			this,
			item.text,
			st::conversationFlowChip);
		const auto id = item.id;
		button->setActive(id == _active, false);
		button->setClickedCallback([this, id] {
			if (_active != id) {
				setActive(id);
				if (_changed) {
					_changed(id);
				}
			}
		});
		button->show();
		_chips.push_back({ id, button });
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
}

void ChipFlow::setActive(const QString &id) {
	_active = id;
	for (const auto &chip : _chips) {
		chip.button->setActive(chip.id == id);
	}
}

int ChipFlow::resizeGetHeight(int newWidth) {
	const auto skip = int(st::conversationFlowSkip);
	if (_chips.empty()) {
		return 0;
	}
	auto left = 0;
	auto top = 0;
	auto rowHeight = 0;
	for (const auto &chip : _chips) {
		const auto button = chip.button;
		button->resizeToWidth(std::min(button->naturalWidth(), newWidth));
		if (left > 0 && left + button->width() > newWidth) {
			left = 0;
			top += rowHeight + skip;
		}
		button->moveToLeft(left, top, newWidth);
		left += button->width() + skip;
		rowHeight = button->height();
	}
	return top + rowHeight;
}

// ---- IdentityPicker

class IdentityPicker::Option final : public Ui::RippleButton {
public:
	Option(
		QWidget *parent,
		Main::Session *session,
		bool masked)
	: RippleButton(parent, st::conversationIdentityRipple)
	, _masked(masked) {
		const auto size = int(st::conversationIdentityUserpic);
		auto &users = session->data().users();
		_userpic = new Support::UserpicButton(this, session, users.myId(), size);
		if (masked) {
			_userpic->setSpecial(Ui::UserpicKind::Anonymous);
			_text = tr::lng_anonymous(tr::now);
		} else {
			const auto refresh = [this, session] {
				auto &users = session->data().users();
				const auto name = users.me().displayName.isEmpty()
					? users.peek(users.myId()).displayName
					: users.me().displayName;
				_text = name.isEmpty()
					? tr::lng_conv_as_myself(tr::now)
					: tr::lng_conv_as_name(
						tr::now,
						lt_name,
						QChar(0x2068) + name + QChar(0x2069));
				update();
			};
			QObject::connect(&users, &Data::Users::meChanged, this, refresh);
			refresh();
		}
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		resize(width(), st::conversationIdentityHeight);
	}

	void setSelected(bool selected) {
		if (_selected != selected) {
			_selected = selected;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto border = _selected ? 2. : 1.;
		const auto path = TailedRect(
			QRectF(rect()).marginsRemoved(QMarginsF(
				border / 2.,
				border / 2.,
				border / 2.,
				border / 2.)),
			st::conversationIdentityRadius,
			st::conversationCardTail);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(QPen(
				_selected
					? st::conversationIdentitySelected->c
					: isOver()
					? st::conversationIdentityBorderOver->c
					: st::conversationIdentityBorder->c,
				border));
			p.setBrush(_masked
				? st::conversationIdentityAnonBg
				: st::conversationIdentityBg);
			p.drawPath(path);
		}
		p.save();
		p.setClipPath(path);
		paintRipple(p, 0, 0);
		p.restore();
		const auto padding = int(st::conversationIdentityPadding);
		const auto left = padding
			+ int(st::conversationIdentityUserpic)
			+ int(st::conversationIdentitySkip);
		const auto available = width() - left - padding;
		p.setFont(st::conversationIdentityFont);
		p.setPen(_masked
			? st::conversationIdentityAnonFg
			: st::conversationIdentityFg);
		p.drawTextLeft(
			left,
			(height() - st::conversationIdentityFont->height) / 2,
			width(),
			st::conversationIdentityFont->elided(_text, std::max(available, 0)));
	}

	void resizeEvent(QResizeEvent *e) override {
		RippleButton::resizeEvent(e);
		const auto size = int(st::conversationIdentityUserpic);
		_userpic->moveToLeft(
			st::conversationIdentityPadding,
			(height() - size) / 2,
			width());
	}

	void onStateChanged(State was, StateChangeSource source) override {
		RippleButton::onStateChanged(was, source);
		update();
	}

private:
	Support::UserpicButton *_userpic = nullptr;
	QString _text;
	bool _masked = false;
	bool _selected = false;

};

IdentityPicker::IdentityPicker(QWidget *parent, Main::Session *session)
: RpWidget(parent)
, _named(new Option(this, session, false))
, _masked(new Option(this, session, true)) {
	_named->setClickedCallback([this] {
		if (_anonymous) {
			setAnonymous(false);
			if (_changed) {
				_changed(false);
			}
		}
	});
	_masked->setClickedCallback([this] {
		if (!_anonymous) {
			setAnonymous(true);
			if (_changed) {
				_changed(true);
			}
		}
	});
	_masked->setSelected(true);
}

void IdentityPicker::setAnonymous(bool anonymous) {
	_anonymous = anonymous;
	_named->setSelected(!anonymous);
	_masked->setSelected(anonymous);
}

int IdentityPicker::resizeGetHeight(int newWidth) {
	const auto skip = int(st::conversationIdentitySkip);
	const auto each = std::max((newWidth - skip) / 2, 1);
	const auto height = int(st::conversationIdentityHeight);
	_named->setGeometryToLeft(0, 0, each, height, newWidth);
	_masked->setGeometryToLeft(
		each + skip,
		0,
		newWidth - each - skip,
		height,
		newWidth);
	return height;
}

// ---- ExpiryHeader

ExpiryHeader::ExpiryHeader(
	QWidget *parent,
	qint64 expiresAt,
	const QString &category)
: RpWidget(parent)
, _expiresAt(expiresAt)
, _category(category) {
	resize(width(), st::conversationJoinTopHeight);
	scheduleTick();
}

void ExpiryHeader::scheduleTick() {
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (Expired(_expiresAt, now)) {
		return;
	}
	Ui::CallDelayed(NextTickDelay({ _expiresAt }, now), this, [this] {
		update();
		scheduleTick();
	});
}

void ExpiryHeader::setMoreCallback(std::function<void(QWidget *anchor)> callback) {
	if (!_more) {
		_more = new Ui::IconButton(this, st::smallIconButton);
		_more->show();
		_more->moveToRight(0, (height() - _more->height()) / 2, width());
	}
	const auto button = _more;
	_more->setClickedCallback([button, callback = std::move(callback)] {
		callback(button);
	});
	update();
}

int ExpiryHeader::resizeGetHeight(int newWidth) {
	if (_more) {
		_more->moveToRight(0, (height() - _more->height()) / 2, newWidth);
	}
	return st::conversationJoinTopHeight;
}

void ExpiryHeader::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto now = QDateTime::currentMSecsSinceEpoch();
	const auto ring = int(st::conversationJoinRing);
	const auto skip = int(st::conversationCardSkip);
	PaintExpiryRing(
		p,
		QRectF(myrtlrect(0, (height() - ring) / 2, ring, ring)),
		ExpiryFraction(_expiresAt, _expiresAt - kDefaultWindowSeconds, now),
		ExpiryFinal(_expiresAt, now),
		st::conversationRingStroke,
		st::conversationRingTrack->c);
	auto trailing = _more ? (_more->width() + skip) : 0;
	if (!_category.isEmpty()) {
		const auto tagHeight = int(st::conversationTagHeight);
		const auto tagWidth = st::conversationTagFont->width(_category)
			+ 2 * int(st::conversationTagPadding);
		const auto tag = myrtlrect(
			width() - trailing - tagWidth,
			(height() - tagHeight) / 2,
			tagWidth,
			tagHeight);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(QPen(st::conversationTagBorder->c, 1.));
			p.setBrush(Qt::NoBrush);
			p.drawPath(TailedRect(
				QRectF(tag).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
				st::conversationTagRadius,
				st::conversationCardTail));
		}
		p.setFont(st::conversationTagFont);
		p.setPen(st::conversationTagFg);
		p.drawTextCentered(tag, _category);
		trailing += tagWidth + skip;
	}
	p.setFont(st::conversationRemainingFont);
	p.setPen(st::conversationRemainingFg);
	p.drawTextLeft(
		ring + skip,
		(height() - st::conversationRemainingFont->height) / 2,
		width(),
		st::conversationRemainingFont->elided(
			RemainingText(_expiresAt, now),
			std::max(width() - ring - skip - trailing, 0)));
}

} // namespace Conversation
