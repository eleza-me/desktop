#include "conversation/conversation_radar.h"

#include "conversation/conversation_boxes.h"
#include "conversation/conversation_card.h"
#include "conversation/conversation_radar_model.h"
#include "conversation/conversation_time.h"
#include "data/data_conversations.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "support/error_text.h"
#include "support/media_image.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/userpic.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/top_bar.h"
#include "window/window_routes.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_conversation.h"

#include <QtCore/QDateTime>
#include <QtGui/QConicalGradient>
#include <QtGui/QKeyEvent>
#include <QtGui/QRadialGradient>

#include <array>
#include <cmath>

namespace Conversation {
namespace {

constexpr auto kSweepPeriod = 4000;
constexpr auto kSweepStaticAngle = 35.;
constexpr auto kSweepTail = 70.;
constexpr auto kDimOpacity = 0.45;
constexpr auto kFindChatTimeout = 7000;
// Without the connection the row cannot be announced: the inbox is read
// (once) sooner.
constexpr auto kFindChatOfflineTimeout = 2400;

struct FloatingPerson {
	double x = 0.;
	double y = 0.;
	double opacity = 1.;
};
// Decorative people around the eye, in a 360 x 360 stage.
constexpr auto kPeople = std::array<FloatingPerson, 6>{ {
	{ 210., 60., 1. },
	{ 52., 140., 1. },
	{ 292., 242., 0.9 },
	{ 110., 298., 0.8 },
	{ 330., 110., 0.45 },
	{ 30., 250., 0.45 },
} };

[[nodiscard]] qint64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

// A name inside a sentence of the other direction: isolated, so it neither
// reorders the words around it nor decides the direction of the line.
[[nodiscard]] QString Isolated(const QString &name) {
	return Lang::Isolated(name);
}

class Content final : public Ui::RpWidget {
public:
	Content(
		QWidget *parent,
		Window::SessionController *controller,
		RadarModel *model);

	// The height the content may take without scrolling.
	void setViewportHeight(int height);
	void activateDefault();
	void setLeaveCallback(std::function<void()> callback) {
		_leave = std::move(callback);
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refresh();
	void refreshTexts();
	void updateAnimation();
	void scheduleTick();
	void goToChat();
	[[nodiscard]] QString participantName(const Data::Participant &participant) const;
	void paintStage(Painter &p, const QRect &stage);
	void paintPerson(Painter &p, const QRect &rect, int index);
	void paintParticipant(
		Painter &p,
		const QRect &rect,
		const Data::Participant &participant);
	void paintPanel(Painter &p);
	[[nodiscard]] int panelHeight(int width) const;
	[[nodiscard]] Data::Session &data() const {
		return _controller->session().data();
	}

	Window::SessionController *_controller = nullptr;
	RadarModel *_model = nullptr;
	Support::UserpicPainter _userpics;
	std::function<void()> _leave;

	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_matchedTitle = nullptr;
	Ui::FlatLabel *_mint = nullptr;
	Ui::FlatLabel *_mintBody = nullptr;
	Ui::FlatLabel *_dim = nullptr;
	Ui::RoundButton *_toChats = nullptr;
	Ui::RoundButton *_invite = nullptr;
	Ui::LinkButton *_end = nullptr;
	Ui::RoundButton *_go = nullptr;
	Ui::RoundButton *_retry = nullptr;
	Ui::LoadingSpinner *_spinner = nullptr;

	Ui::Text::String _topic;
	QString _topicSource;
	QString _category;
	RadarPhase _phase = RadarPhase::Loading;
	QRect _stage;
	QRect _panel;
	int _viewportHeight = 0;
	qint64 _sweepNow = 0;
	bool _waiting = false;
	bool _matched = false;
	bool _failed = false;
	bool _loading = true;
	bool _finding = false;

	Ui::Animations::Basic _sweep;

};

Content::Content(
	QWidget *parent,
	Window::SessionController *controller,
	RadarModel *model)
: RpWidget(parent)
, _controller(controller)
, _model(model)
, _userpics(&controller->session(), [this](const QString &) { update(); })
, _title(new Ui::FlatLabel(this, st::conversationRadarStatusTitle))
, _matchedTitle(new Ui::FlatLabel(this, st::conversationRadarMatchedTitle))
, _mint(new Ui::FlatLabel(this, st::conversationRadarMintLabel))
, _mintBody(new Ui::FlatLabel(this, st::conversationRadarMintBody))
, _dim(new Ui::FlatLabel(this, st::conversationRadarDimLabel))
, _toChats(new Ui::RoundButton(
	this,
	tr::lng_conv_go_chats(tr::now),
	st::conversationRadarNightButton))
, _invite(new Ui::RoundButton(
	this,
	tr::lng_conv_invite_eli(tr::now),
	st::conversationRadarMintButton))
, _end(new Ui::LinkButton(
	this,
	tr::lng_conv_close_conversation(tr::now),
	st::conversationRadarEnd))
, _go(new Ui::RoundButton(
	this,
	tr::lng_conv_go_chat(tr::now),
	st::conversationRadarBigButton))
, _retry(new Ui::RoundButton(
	this,
	tr::lng_retry(tr::now),
	st::conversationRadarRetry))
, _spinner(new Ui::LoadingSpinner(
	this,
	st::conversationRadarSpinner,
	st::conversationRadarMint)) {
	_invite->setIcon(&st::conversationRadarEliIcon);
	_go->setIcon(&st::conversationRadarChatIcon);
	_spinner->resize(st::conversationRadarSpinner, st::conversationRadarSpinner);

	_sweep.init([this](qint64 now) {
		_sweepNow = now;
		update(_stage);
		return true;
	});

	_toChats->setClickedCallback([this] {
		if (_leave) {
			_leave();
		}
	});
	_invite->setClickedCallback([this] {
		_controller->showToast(tr::lng_conversation_radar_will_notify(tr::now));
		ShowInviteBox(_controller, _model->id());
	});
	_end->setClickedCallback([this] {
		const auto weak = QPointer<Content>(this);
		ConfirmEnd(_controller, _model->id(), [weak] {
			if (weak && weak->_leave) {
				weak->_leave();
			}
		});
	});
	_go->setClickedCallback([this] { goToChat(); });
	_retry->setClickedCallback([this] { _model->reload(); });

	QObject::connect(_model, &RadarModel::changed, this, [this] {
		refresh();
	});
	QObject::connect(
		&data().users(),
		&Data::Users::changed,
		this,
		[this] {
			refreshTexts();
			update();
		});
	refresh();
}

void Content::setViewportHeight(int height) {
	if (_viewportHeight != height) {
		_viewportHeight = height;
		if (width() > 0) {
			resizeToWidth(width());
		}
	}
}

void Content::activateDefault() {
	if (_phase == RadarPhase::Matched) {
		goToChat();
	} else if (_phase == RadarPhase::Failed) {
		_model->reload();
	}
}

QString Content::participantName(const Data::Participant &participant) const {
	if (participant.masked || participant.userId.isEmpty()) {
		return participant.maskName.isEmpty()
			? tr::lng_conversation_passerby(tr::now)
			: participant.maskName;
	}
	const auto user = data().users().get(participant.userId);
	return user.deleted
		? tr::lng_deleted_account(tr::now)
		: user.displayName;
}

void Content::goToChat() {
	if (_finding) {
		return;
	}
	_finding = true;
	_go->setLoading(true);
	const auto controller = _controller;
	data().conversations().findChat(_model->id(), this, [=, this](
			const QString &chat) {
		_finding = false;
		_go->setLoading(false);
		if (chat.isEmpty()) {
			controller->showToast(tr::lng_conversation_chat_soon(tr::now));
			return;
		}
		// Replaces the radar: the section dies inside.
		Ui::PostponeCall(controller, [=] {
			controller->clearMainStack(Window::Tab::Chats);
			controller->openRoute(Window::Routes::Chat(chat));
		});
	}, data().transport().connected()
		? kFindChatTimeout
		: kFindChatOfflineTimeout);
}

void Content::refresh() {
	_phase = _model->phase();
	const auto &conversation = _model->conversation();
	if (conversation) {
		if (_topicSource != conversation->topic) {
			_topicSource = conversation->topic;
			_topic.setText(
				st::conversationRadarTopicStyle,
				tr::lng_conversation_quoted(
					tr::now,
					lt_text,
					conversation->topic));
		}
		_category = data().conversations().categoryTitle(conversation->category);
	}
	// A closed conversation keeps its last look until the section leaves.
	const auto known = (_phase != RadarPhase::Failed) && conversation;
	_failed = (_phase == RadarPhase::Failed);
	_matched = known && conversation->listener;
	_waiting = known && !conversation->listener;
	_loading = !_failed && !conversation;
	refreshTexts();
	_title->setVisible(_waiting || _failed);
	_mint->setVisible(_waiting);
	_dim->setVisible(_waiting || _failed);
	_toChats->setVisible(_waiting);
	_invite->setVisible(_waiting);
	_end->setVisible(_waiting);
	_matchedTitle->setVisible(_matched);
	_mintBody->setVisible(_matched);
	_go->setVisible(_matched);
	_retry->setVisible(_failed);
	_spinner->setVisible(_loading);
	updateAnimation();
	scheduleTick();
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void Content::refreshTexts() {
	const auto &conversation = _model->conversation();
	if (_failed) {
		_title->setText(tr::lng_error_load(tr::now));
		_dim->setText(_model->error()
			? Support::ErrorText(*_model->error())
			: QString());
		return;
	}
	_title->setText(tr::lng_conv_looking_listener(tr::now));
	_mint->setText(_category.isEmpty()
		? tr::lng_conversation_radar_shown_in_topics(tr::now)
		: tr::lng_conversation_radar_shown_to_category(
			tr::now,
			lt_category,
			Isolated(_category)));
	_dim->setText(tr::lng_conv_well_tell_right_here_when(tr::now));
	if (conversation && conversation->listener) {
		// The sentence may start with a name of the other direction: a
		// leading mark keeps the line in the interface's direction.
		_matchedTitle->setText(
			QString(QChar(style::RightToLeft() ? 0x200F : 0x200E))
			+ tr::lng_conv_joined(
				tr::now,
				lt_name,
				Isolated(participantName(*conversation->listener))));
		_mintBody->setText(tr::lng_conversation_started_until(
			tr::now,
			lt_duration,
			LongRemaining(conversation->expiresAt, NowMs())));
	}
}

void Content::scheduleTick() {
	// The clock in the panel and «تا … باز است»: a local timer.
	const auto &conversation = _model->conversation();
	if (!conversation
		|| isHidden()
		|| (_phase != RadarPhase::Waiting && _phase != RadarPhase::Matched)) {
		return;
	}
	const auto expires = conversation->expiresAt;
	const auto now = NowMs();
	if (Expired(expires, now)) {
		return;
	}
	const auto left = expires * 1000 - now;
	// The clock shows whole minutes left, rounded down.
	const auto delay = int(std::clamp<qint64>((left % 60'000) + 50, 250, 60'000));
	const auto phase = _phase;
	Ui::CallDelayed(delay, this, [this, phase, expires] {
		const auto &current = _model->conversation();
		if (_phase != phase || !current || current->expiresAt != expires) {
			return;
		}
		refreshTexts();
		update();
		scheduleTick();
	});
}

void Content::updateAnimation() {
	const auto wanted = !isHidden()
		&& !Ui::AnimationsDisabled()
		&& (_phase == RadarPhase::Waiting || _phase == RadarPhase::Loading);
	if (wanted && !_sweep.animating()) {
		_sweep.start();
	} else if (!wanted && _sweep.animating()) {
		_sweep.stop();
	}
}

void Content::showEvent(QShowEvent *e) {
	RpWidget::showEvent(e);
	updateAnimation();
	refreshTexts();
	scheduleTick();
}

void Content::hideEvent(QHideEvent *e) {
	RpWidget::hideEvent(e);
	updateAnimation();
}

int Content::panelHeight(int width) const {
	const auto &padding = st::conversationRadarPanelPadding;
	const auto inner = std::max(width - padding.left() - padding.right(), 1);
	return padding.top()
		+ int(st::conversationRadarCapsuleHeight)
		+ int(st::conversationRadarTopicSkip)
		+ _topic.countHeight(inner, 4)
		+ padding.bottom();
}

int Content::resizeGetHeight(int newWidth) {
	const auto padding = int(st::conversationRadarPadding);
	const auto skip = int(st::conversationRadarSkip);
	const auto column = std::max(
		std::min(newWidth - 2 * padding, int(st::conversationRadarWidth)),
		1);
	const auto left = (newWidth - column) / 2;
	const auto textWidth = std::max(column - 2 * padding, 1);
	const auto statusSkip = int(st::conversationRadarStatusSkip);
	const auto waiting = _waiting;
	const auto matched = _matched;
	const auto failed = _failed;
	const auto showPanel = waiting;

	// Everything under the stage first: the stage takes what is left.
	auto below = 0;
	const auto place = [&](Ui::FlatLabel *label) {
		if (!label->isHidden()) {
			label->resizeToWidth(textWidth);
			below += label->height() + statusSkip;
		}
	};
	place(_title);
	place(_matchedTitle);
	place(_mint);
	place(_mintBody);
	place(_dim);
	if (!_spinner->isHidden()) {
		below += _spinner->height() + statusSkip;
	}
	const auto panel = showPanel ? panelHeight(column) : 0;
	if (panel) {
		below += skip + panel;
	}
	if (waiting) {
		below += skip
			+ int(st::conversationRadarNightButton.height)
			+ int(st::conversationRadarEndSkip)
			+ _end->height();
	} else if (matched) {
		_go->resizeToWidth(column);
		below += skip + _go->height();
	} else if (failed) {
		_retry->resizeToNaturalWidth(column);
		below += skip + _retry->height();
	}
	below += int(st::conversationRadarBottomSkip);

	const auto stage = std::clamp(
		std::min(_viewportHeight - below - skip, column),
		int(st::conversationRadarStageMin),
		int(st::conversationRadarStage));
	// Spare room is shared above and below the block.
	const auto total = stage + skip + below;
	auto top = std::max((_viewportHeight - total) / 3, 0);
	_stage = QRect((newWidth - stage) / 2, top, stage, stage);
	top += stage + skip;

	const auto put = [&](Ui::FlatLabel *label) {
		if (!label->isHidden()) {
			label->move(left + padding, top);
			top += label->height() + statusSkip;
		}
	};
	put(_title);
	put(_matchedTitle);
	put(_mint);
	put(_mintBody);
	put(_dim);
	if (!_spinner->isHidden()) {
		_spinner->move((newWidth - _spinner->width()) / 2, top);
		top += _spinner->height() + statusSkip;
	}
	if (panel) {
		top += skip;
		_panel = QRect(left, top, column, panel);
		top += panel;
	} else {
		_panel = QRect();
	}
	if (waiting) {
		top += skip;
		const auto between = int(st::conversationRadarButtonsSkip);
		const auto each = (column - between) / 2;
		// Fixed sides, as in the design: «رفتن به چت‌ها» | «دعوت در الی».
		_toChats->setFullWidth(each);
		_toChats->move(left, top);
		_invite->setFullWidth(column - each - between);
		_invite->move(left + each + between, top);
		top += _toChats->height() + int(st::conversationRadarEndSkip);
		_end->resizeToNaturalWidth(column);
		_end->move((newWidth - _end->width()) / 2, top);
		top += _end->height();
	} else if (matched) {
		top += skip;
		_go->setFullWidth(column);
		_go->move(left, top);
		top += _go->height();
	} else if (failed) {
		top += skip;
		_retry->move((newWidth - _retry->width()) / 2, top);
		top += _retry->height();
	}
	return top + int(st::conversationRadarBottomSkip);
}

void Content::paintPerson(Painter &p, const QRect &rect, int index) {
	auto hq = PainterHighQualityEnabler(p);
	if (index % 2) {
		Ui::PaintUserpicPlaceholder(
			p,
			rect,
			QStringView(),
			QString(),
			Ui::UserpicKind::Anonymous);
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(Ui::UserpicColor(index + 3));
		p.drawEllipse(rect);
		st::conversationMenuProfile.paintInCenter(p, rect, st::staticWhite->c);
	}
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(st::conversationRadarPersonBorder->c, 2.));
	p.drawEllipse(QRectF(rect).marginsAdded(QMarginsF(1., 1., 1., 1.)));
}

void Content::paintParticipant(
		Painter &p,
		const QRect &rect,
		const Data::Participant &participant) {
	if (participant.masked || participant.userId.isEmpty()) {
		_userpics.paintSpecial(
			p,
			rect,
			Ui::UserpicKind::Anonymous,
			participant.ref,
			participantName(participant));
	} else {
		_userpics.paint(p, rect, participant.userId);
	}
}

void Content::paintStage(Painter &p, const QRect &stage) {
	const auto &conversation = _model->conversation();
	const auto matched = _matched;
	const auto dim = matched || _failed;
	const auto center = QPointF(stage.center()) + QPointF(0.5, 0.5);
	const auto size = double(stage.width());
	auto hq = PainterHighQualityEnabler(p);

	// ---- rings
	p.save();
	if (dim) {
		p.setOpacity(kDimOpacity);
	}
	p.setBrush(Qt::NoBrush);
	const auto ring = [&](double part, bool dashed) {
		auto pen = QPen(
			dashed
				? st::conversationRadarRingDashFg->c
				: st::conversationRadarRingFg->c,
			1.);
		if (dashed) {
			pen.setStyle(Qt::DashLine);
		}
		p.setPen(pen);
		const auto radius = size * part / 2.;
		p.drawEllipse(center, radius, radius);
	};
	ring(0.94, true);
	ring(0.69, false);
	ring(0.44, false);
	p.restore();
	if (matched) {
		const auto pair = int(st::conversationRadarPairSize);
		const auto between = int(st::conversationRadarPairSkip);
		const auto left = stage.x() + (stage.width() - 2 * pair - between) / 2;
		const auto top = stage.y() + (stage.height() - pair) / 2;
		// The listener on the leading side, with the «here» dot.
		const auto first = style::rtlrect(left, top, pair, pair, width());
		const auto second = style::rtlrect(
			left + pair + between,
			top,
			pair,
			pair,
			width());
		paintParticipant(p, first, *conversation->listener);
		paintParticipant(p, second, conversation->speaker);
		const auto dot = int(st::conversationRadarPairDot);
		const auto dotRect = QRect(
			style::RightToLeft() ? (first.x() + 4) : (first.right() - dot - 3),
			first.bottom() - dot - 3,
			dot,
			dot);
		auto glow = QRadialGradient(dotRect.center(), dot);
		glow.setColorAt(0., st::conversationRadarGlowFg->c);
		glow.setColorAt(1., st::conversationRadarSweepEnd->c);
		p.setPen(Qt::NoPen);
		p.setBrush(glow);
		p.drawEllipse(QRectF(dotRect).marginsAdded(QMarginsF(
			dot / 2.,
			dot / 2.,
			dot / 2.,
			dot / 2.)));
		p.setBrush(st::conversationRadarMint);
		p.drawEllipse(dotRect);
		return;
	}
	if (_failed) {
		return;
	}

	// ---- the sweep
	const auto pupil = double(st::conversationRadarPupil);
	const auto angle = _sweep.animating()
		? (double(std::max(_sweepNow - _sweep.started(), qint64(0)) % kSweepPeriod)
			* 360. / kSweepPeriod)
		: kSweepStaticAngle;
	{
		// Clockwise from the top; the tail fades behind the bright edge.
		auto gradient = QConicalGradient(center, 90. - angle);
		const auto tail = kSweepTail / 360.;
		gradient.setColorAt(0., st::conversationRadarSweepFg->c);
		gradient.setColorAt(tail, st::conversationRadarSweepEnd->c);
		gradient.setColorAt(1., st::conversationRadarSweepEnd->c);
		const auto outer = size * 0.94 / 2.;
		auto path = QPainterPath();
		path.addEllipse(center, outer, outer);
		auto hole = QPainterPath();
		hole.addEllipse(center, pupil / 2. + 8., pupil / 2. + 8.);
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawPath(path.subtracted(hole));
	}

	// ---- the eye
	{
		auto glow = QRadialGradient(center, pupil / 2. + 20.);
		glow.setColorAt(0., st::conversationRadarGlowFg->c);
		glow.setColorAt(pupil / (pupil + 40.), st::conversationRadarGlowFg->c);
		glow.setColorAt(1., st::conversationRadarSweepEnd->c);
		p.setPen(Qt::NoPen);
		p.setBrush(glow);
		p.drawEllipse(center, pupil / 2. + 20., pupil / 2. + 20.);
		p.setBrush(st::conversationRadarMint);
		p.drawEllipse(center, pupil / 2., pupil / 2.);
		const auto core = double(st::conversationRadarPupilCore);
		p.setBrush(st::conversationRadarBg);
		p.drawEllipse(center, core / 2., core / 2.);
		const auto glint = double(st::conversationRadarGlint);
		p.setBrush(st::staticWhite);
		p.drawEllipse(center + QPointF(6., -7.), glint / 2., glint / 2.);
	}

	// ---- people around
	const auto person = int(st::conversationRadarPersonSize);
	const auto scale = size / 360.;
	for (auto i = 0; i != int(kPeople.size()); ++i) {
		const auto &place = kPeople[i];
		p.setOpacity(place.opacity);
		paintPerson(
			p,
			QRect(
				stage.x() + int(std::lround(place.x * scale)) - person / 2,
				stage.y() + int(std::lround(place.y * scale)) - person / 2,
				person,
				person),
			i);
	}
	p.setOpacity(1.);
}

void Content::paintPanel(Painter &p) {
	const auto &conversation = _model->conversation();
	if (_panel.isEmpty() || !conversation) {
		return;
	}
	const auto &padding = st::conversationRadarPanelPadding;
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::conversationRadarPanelBorder->c, 1.));
		p.setBrush(st::conversationRadarPanelBg);
		p.drawPath(TailedRect(
			QRectF(_panel).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
			st::conversationRadarPanelRadius,
			st::conversationCardTail));
	}
	const auto inner = _panel.marginsRemoved(padding);
	const auto capsuleHeight = int(st::conversationRadarCapsuleHeight);
	const auto capsulePadding = int(st::conversationRadarCapsulePadding);
	const auto font = st::conversationRadarCapsuleFont;
	// Capsules flow from the leading side of the panel.
	auto used = 0;
	const auto capsule = [&](
			const QString &text,
			const style::icon *icon,
			style::color bg,
			style::color fg) {
		const auto iconWidth = icon ? (icon->width() + 4) : 0;
		const auto available = inner.width() - used - 2 * capsulePadding - iconWidth;
		if (available < font->elidew * 2) {
			return;
		}
		const auto shown = font->elided(text, available);
		const auto textWidth = font->width(shown);
		const auto width = textWidth + iconWidth + 2 * capsulePadding;
		const auto x = style::RightToLeft()
			? (inner.x() + inner.width() - used - width)
			: (inner.x() + used);
		const auto rect = QRect(x, inner.y(), width, capsuleHeight);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(bg);
			p.drawRoundedRect(rect, capsuleHeight / 2., capsuleHeight / 2.);
		}
		// Inside a capsule: the icon first in the reading direction.
		auto contentLeft = style::RightToLeft()
			? (rect.x() + rect.width() - capsulePadding)
			: (rect.x() + capsulePadding);
		if (icon) {
			const auto iconX = style::RightToLeft()
				? (contentLeft - icon->width())
				: contentLeft;
			p.drawPixmap(
				QPoint(iconX, rect.y() + (capsuleHeight - icon->height()) / 2),
				icon->pixmap(devicePixelRatioF()));
			contentLeft += style::RightToLeft() ? -iconWidth : iconWidth;
		}
		p.setFont(font);
		p.setPen(fg);
		p.drawText(
			style::RightToLeft() ? (contentLeft - textWidth) : contentLeft,
			rect.y() + (capsuleHeight - font->height) / 2 + font->ascent,
			shown);
		used += width + int(st::conversationRadarCapsuleSkip);
	};
	if (!_category.isEmpty()) {
		capsule(
			_category,
			nullptr,
			st::conversationRadarCapsuleBg,
			st::conversationRadarCapsuleFg);
	}
	capsule(
		ClockRemaining(conversation->expiresAt, NowMs()),
		&st::conversationRadarLampIcon,
		st::conversationRadarLampBg,
		st::conversationRadarLampFg);

	p.setPen(st::conversationRadarFg);
	auto context = Ui::Text::PaintContext();
	context.position = QPoint(
		inner.x(),
		inner.y() + capsuleHeight + int(st::conversationRadarTopicSkip));
	context.availableWidth = inner.width();
	context.elisionLines = 4;
	context.autoDirection = true;
	_topic.draw(p, context);
}

void Content::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (e->rect().intersects(_stage.marginsAdded(QMargins(4, 4, 4, 4)))) {
		paintStage(p, _stage);
	}
	if (e->rect().intersects(_panel)) {
		paintPanel(p);
	}
}

class Section final : public Window::SectionWidget {
public:
	Section(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &id,
		const std::optional<Data::Conversation> &initial);

	QString sectionKey() const override {
		return Window::Routes::Conversation(_model->id());
	}
	void showFinished() override;
	void setInnerFocus() override {
		setFocus();
	}

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	void closed(const QString &state);
	void leave();

	RadarModel *_model = nullptr;
	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Content *_content = nullptr;
	bool _left = false;
	bool _leavePending = false;

};

Section::Section(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &id,
	const std::optional<Data::Conversation> &initial)
: SectionWidget(parent, controller)
, _model(new RadarModel(&controller->session().data(), id, this))
, _bar(new Ui::TopBar(this, st::conversationRadarBar))
, _scroll(new Ui::ScrollArea(this)) {
	setBackground(st::conversationRadarBg);
	setFocusPolicy(Qt::StrongFocus);
	_bar->setCentered(true);
	_bar->setTitle(tr::lng_conv_conversation_open(tr::now));
	setTopBar(_bar);
	_scroll->setTopBottomShadows(false, false);
	if (initial) {
		_model->setInitial(*initial);
	}
	_content = _scroll->setOwnedWidget(
		Ui::object_ptr<Content>(_scroll, controller, _model));
	_content->setLeaveCallback([this] { leave(); });

	const auto refreshTitle = [this] {
		const auto &conversation = _model->conversation();
		_bar->setTitle((conversation && conversation->listener)
			? QString()
			: tr::lng_conv_conversation_open(tr::now));
	};
	QObject::connect(_model, &RadarModel::changed, this, refreshTitle);
	QObject::connect(_model, &RadarModel::closed, this, [this](
			const QString &state) {
		closed(state);
	});
	// Closed from somewhere else on this device (the topics list's menu).
	QObject::connect(Events(), &Notifier::topicHidden, this, [this](
			const QString &conversationId) {
		if (conversationId == _model->id() && !_left) {
			_model->reload();
		}
	});
	refreshTitle();
	_model->start();
	if (_model->phase() == RadarPhase::Closed) {
		closed(_model->closedState());
	}
}

void Section::showFinished() {
	if (_leavePending) {
		leave();
	}
}

void Section::closed(const QString &state) {
	if (_left) {
		return;
	}
	const auto &conversation = _model->conversation();
	const auto window = controller();
	if (state == u"continued") {
		if (conversation && !conversation->continuedInto.isEmpty()) {
			const auto chat = conversation->continuedInto;
			_left = true;
			Ui::PostponeCall(window, [=] {
				window->clearMainStack(Window::Tab::Chats);
				window->openRoute(Window::Routes::Chat(chat));
			});
			return;
		}
		window->showToast(tr::lng_chat_conversation_continued(tr::now));
	} else if (state == u"expired") {
		window->showToast(tr::lng_conversation_expired_unheard(tr::now));
	} else {
		window->showToast(tr::lng_chat_conversation_has_ended(tr::now));
	}
	leave();
}

void Section::leave() {
	if (_left) {
		return;
	}
	const auto window = controller();
	if (window->mainSection() != this) {
		// Covered by another section: leave when it comes back.
		_leavePending = true;
		return;
	}
	_left = true;
	// The section dies inside.
	Ui::PostponeCall(window, [window] {
		if (!window->showBack()) {
			window->clearMainStack();
		}
	});
}

void Section::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto top = _bar->height();
	const auto available = std::max(height() - top, 0);
	_content->setViewportHeight(available);
	_scroll->setGeometry(0, top, width(), available);
}

void Section::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		_content->activateDefault();
		return;
	}
	SectionWidget::keyPressEvent(e);
}

void Section::showEvent(QShowEvent *e) {
	SectionWidget::showEvent(e);
	_model->setActive(true);
}

void Section::hideEvent(QHideEvent *e) {
	SectionWidget::hideEvent(e);
	_model->setActive(false);
}

} // namespace

Window::SectionFactory RadarFactory(
		const QString &conversationId,
		std::optional<Data::Conversation> initial) {
	return [=](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new Section(parent, controller, conversationId, initial));
	};
}

} // namespace Conversation
