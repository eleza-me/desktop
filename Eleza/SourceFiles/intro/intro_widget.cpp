#include "intro/intro_widget.h"

#include "core/core_application.h"
#include "intro/intro_steps.h"
#include "lang/lang_countries.h"
#include "lang_auto.h"
#include "main/main_account.h"
#include "ui/layers/box_content.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/tooltip.h"
#include "window/window_main_window.h"
#include "window/window_update.h"
#include "styles/style_intro.h"
#include "styles/style_window_update.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>

namespace Intro {
namespace {

constexpr auto kSlideShift = 64;

// The state survives the rebuild of the window content after a language
// switch (the direction of everything changes, the widgets are recreated).
struct Saved {
	State state;
	StepType type = StepType::Welcome;
};
std::optional<Saved> SavedForRebuild;

} // namespace

// Plays the change between two steps: the old column fades out moving away,
// the new one fades in from the other side, the background color follows.
class Widget::Transition final {
public:
	explicit Transition(Widget *owner) : _owner(owner) {
	}

	void start(
			QPixmap was,
			QPoint wasPosition,
			QPixmap now,
			QPoint nowPosition,
			bool wasNight,
			bool nowNight,
			bool back,
			std::function<void()> finished) {
		_was = std::move(was);
		_wasPosition = wasPosition;
		_now = std::move(now);
		_nowPosition = nowPosition;
		_wasNight = wasNight;
		_nowNight = nowNight;
		_back = back;
		_finished = std::move(finished);
		_animation.start([this] {
			_owner->update();
			if (!_animation.animating()) {
				finish();
			}
		}, 0., 1., st::introSlideDuration, anim::easeOutCubic);
		if (!_animation.animating()) {
			finish();
		}
	}

	void finish() {
		_animation.stop();
		_was = QPixmap();
		_now = QPixmap();
		if (const auto finished = std::exchange(_finished, nullptr)) {
			finished();
		}
	}

	[[nodiscard]] bool running() const {
		return !_now.isNull() || !_was.isNull();
	}

	[[nodiscard]] QColor background() const {
		const auto progress = _animation.value(1.);
		return anim::color(
			(_wasNight ? st::introNightBg : st::introBg)->c,
			(_nowNight ? st::introNightBg : st::introBg)->c,
			progress);
	}

	void paint(QPainter &p) const {
		const auto progress = _animation.value(1.);
		// Forward: the new step comes from the trailing side.
		const auto direction = (style::RightToLeft() ? -1 : 1) * (_back ? -1 : 1);
		if (!_was.isNull()) {
			p.setOpacity(std::max(1. - progress * 1.6, 0.));
			p.drawPixmap(
				_wasPosition - QPoint(int(direction * kSlideShift * progress), 0),
				_was);
		}
		if (!_now.isNull()) {
			p.setOpacity(progress);
			p.drawPixmap(
				_nowPosition
					+ QPoint(int(direction * kSlideShift * (1. - progress)), 0),
				_now);
		}
		p.setOpacity(1.);
	}

private:
	Widget *_owner = nullptr;
	QPixmap _was;
	QPixmap _now;
	QPoint _wasPosition;
	QPoint _nowPosition;
	bool _wasNight = false;
	bool _nowNight = false;
	bool _back = false;
	std::function<void()> _finished;
	Ui::Animations::Simple _animation;

};

Widget::Widget(QWidget *parent, Window::MainWindow *window)
: RpWidget(parent)
, _window(window)
, _back(new Ui::IconButton(this, st::topBarBackButton))
, _theme(new Ui::IconButton(this, st::introThemeButton))
, _language(new Ui::IconButton(this, st::introLanguageButton))
, _transition(new Transition(this)) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);

	_back->setClickedCallback([this] {
		if (_step && _step->hasBack()) {
			_step->goBack();
		}
	});
	_theme->setClickedCallback([this] {
		Core::App().toggleTheme();
		updateCorner();
	});
	_language->setClickedCallback([this] { switchLanguage(); });
	Ui::InstallTooltip(_back, [] { return tr::lng_back(tr::now); });
	Ui::InstallTooltip(_theme, [] {
		return style::IsDark()
			? tr::lng_theme_light(tr::now)
			: tr::lng_theme_dark(tr::now);
	});
	Ui::InstallTooltip(_language, [] {
		return tr::lng_intro_other_language(tr::now);
	});
	style::OnPaletteChanged(this, [this] {
		updateCorner();
		update();
	});
	_updatePill = Window::CreateUpdatePill(this, [this] { updateCorner(); });

	auto initial = StepType::Welcome;
	if (auto saved = std::exchange(SavedForRebuild, std::nullopt)) {
		_state = std::move(saved->state);
		initial = saved->type;
	}
	if (Core::App().session() && Core::App().introNeedsProfile()) {
		// Signed in before, the profile step is still to do.
		initial = StepType::Profile;
	}
	showStep(initial);
#ifdef ELEZA_DEBUG_TOOLS
	applyDebugRoute();
#endif // ELEZA_DEBUG_TOOLS
}

Widget::~Widget() {
	delete std::exchange(_transition, nullptr);
}

Main::Account &Widget::account() const {
	return Core::App().account();
}

#ifdef ELEZA_DEBUG_TOOLS
void Widget::applyDebugRoute() {
	const auto route = Core::App().debugRoute();
	const auto prefix = QStringLiteral("intro/");
	if (!route.startsWith(prefix)) {
		return;
	}
	const auto name = route.mid(prefix.size());
	// Sample data, nothing is sent anywhere.
	const auto now = QDateTime::currentMSecsSinceEpoch();
	const auto persian = Lang::IsPersian();
	_state.countryIso = persian ? QStringLiteral("IR") : QStringLiteral("US");
	_state.countryCode = persian ? QStringLiteral("98") : QStringLiteral("1");
	_state.number = persian
		? QStringLiteral("9121234567")
		: QStringLiteral("2015550123");
	_state.phone = QLatin1Char('+') + _state.countryCode + _state.number;
	_state.adultConfirmed = true;
	_state.challenge.challengeId = QStringLiteral("debug");
	_state.challenge.codeLength = 6;
	_state.resendAt = now + 45 * 1000;
	_state.form.ticket = QStringLiteral("debug");
	_state.form.hint = persian
		? QStringLiteral("اسم اولین معلم")
		: QStringLiteral("first teacher’s name");
	_state.form.recoveryEmail = QStringLiteral("h•••@gmail.com");
	_state.form.expiresAt = now + 300 * 1000;
	_state.recovery.codeLength = 6;
	_state.recovery.email = _state.form.recoveryEmail;
	_state.recoveryResendAt = now + 60 * 1000;
	if (name == QStringLiteral("phone")) {
		showStep(StepType::Phone);
	} else if (name == QStringLiteral("country")) {
		showStep(StepType::Phone);
		QTimer::singleShot(0, this, [this] {
			ShowCountryBox(this, [](const QString &) {});
		});
	} else if (name == QStringLiteral("code")) {
		showStep(StepType::Code);
	} else if (name == QStringLiteral("password")) {
		showStep(StepType::Password);
	} else if (name == QStringLiteral("recovery")) {
		showStep(StepType::Recovery);
	} else if (name == QStringLiteral("profile")) {
		showStep(StepType::Profile);
	}
}
#endif // ELEZA_DEBUG_TOOLS

Step *Widget::createStep(StepType type) {
	return CreateStep(type, this, this);
}

QRect Widget::columnRect(int height) const {
	const auto available = std::max(width() - 2 * int(st::introSidePadding), 1);
	const auto columnWidth = std::min(int(st::introWidth), available);
	const auto centered = (this->height() - height) / 2 - st::introCenterShift;
	const auto maxTop = this->height() - st::introBottomMin - height;
	const auto top = std::max(
		std::min(centered, maxTop),
		std::min(int(st::introTopMin), std::max(maxTop, 8)));
	return QRect((width() - columnWidth) / 2, top, columnWidth, height);
}

void Widget::updateStepGeometry() {
	if (!_step) {
		return;
	}
	const auto column = columnRect(0);
	_step->resizeToWidth(column.width());
	const auto rect = columnRect(_step->height());
	_step->move(rect.topLeft());
}

void Widget::showStep(StepType type, bool back) {
	const auto animated = (_step != nullptr)
		&& isVisible()
		&& !Ui::AnimationsDisabled();
	if (_transition->running()) {
		_transition->finish();
	}
	auto was = QPixmap();
	auto wasPosition = QPoint();
	const auto wasNight = _night;
	if (const auto old = std::exchange(_step, nullptr)) {
		if (animated) {
			was = Ui::GrabWidget(old);
			wasPosition = old->pos();
		}
		old->hide();
		old->deleteLater();
	}
	_type = type;
	_step = createStep(type);
	_night = _step->night();
	QObject::connect(_step, &Ui::RpWidget::heightChanged, this, [this] {
		if (_step) {
			_step->move(columnRect(_step->height()).topLeft());
		}
	});
	updateStepGeometry();
	updateCorner();
	if (animated) {
		_step->show();
		Ui::SendPendingMoveResizeEvents(_step);
		auto now = Ui::GrabWidget(_step);
		const auto step = QPointer<Step>(_step);
		_step->hide();
		_transition->start(
			std::move(was),
			wasPosition,
			std::move(now),
			_step->pos(),
			wasNight,
			_night,
			back,
			[this, step] {
				if (step && step == _step) {
					step->show();
					step->setInnerFocus();
				}
				update();
			});
	} else {
		_step->show();
		_step->setInnerFocus();
	}
	update();
}

void Widget::updateCorner() {
	const auto padding = int(st::introCornerPadding);
	_back->setVisible(_step && _step->hasBack());
	_back->moveToLeft(padding, padding, width());
	_theme->moveToRight(padding, padding, width());
	_language->moveToRight(
		padding + _theme->width() + st::introCornerSkip,
		padding,
		width());
	if (_updatePill) {
		_updatePill->moveToLeft(
			st::updatePillMargin,
			height() - st::updatePillMargin - _updatePill->height(),
			width());
		_updatePill->raise();
	}
	_theme->setIconOverride(
		style::IsDark() ? &st::introThemeSunIcon : nullptr,
		style::IsDark() ? &st::introThemeSunIconOver : nullptr);
	// On the night background the icons take its text color.
	const auto tint = _night
		? std::optional<QColor>(st::introNightIconFg->c)
		: std::nullopt;
	_back->setIconColorOverride(tint);
	_theme->setIconColorOverride(tint);
	_language->setIconColorOverride(tint);
	_back->raise();
	_theme->raise();
	_language->raise();
}

void Widget::switchLanguage() {
	auto saved = Saved();
	saved.state = _state;
	saved.type = _type;
	// The whole content (this widget too) is rebuilt for the new language.
	QTimer::singleShot(0, &Core::App(), [saved = std::move(saved)]() mutable {
		SavedForRebuild = std::move(saved);
		Core::App().setLanguage(Lang::IsPersian()
			? Lang::Language::English
			: Lang::Language::Persian);
		// Nobody took it (the app left the intro meanwhile).
		SavedForRebuild = std::nullopt;
	});
}

void Widget::signedIn(const Api::VerifyOutcome &outcome) {
	_state.freshKey = false;
	if (outcome.needsProfile) {
		Core::App().setIntroNeedsProfile(true);
		showStep(StepType::Profile);
		return;
	}
	// Showing the session destroys this widget: not from inside its callback.
	QTimer::singleShot(0, &Core::App(), [] {
		Core::App().introFinished();
	});
}

void Widget::restartExpired() {
	_state.challenge = Api::OtpChallenge();
	_state.form = Api::TwoStepForm();
	_state.freshKey = true;
	showStep(StepType::Phone, true);
	showToast(tr::lng_intro_code_has_expired_request_new(tr::now));
}

void Widget::show(Ui::object_ptr<Ui::BoxContent> box) {
	_window->show(std::move(box));
}

void Widget::showToast(const QString &text) {
	_window->showToast(text);
}

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	if (_transition->running()) {
		_transition->finish();
	}
	updateStepGeometry();
	updateCorner();
}

void Widget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_transition->running()) {
		p.fillRect(e->rect(), _transition->background());
		_transition->paint(p);
	} else {
		p.fillRect(e->rect(), _night ? st::introNightBg : st::introBg);
	}
}

void Widget::focusInEvent(QFocusEvent *e) {
	Q_UNUSED(e);
	// A box closed, the window was activated: typing goes to the step.
	if (_step && _step->isVisible()) {
		_step->setInnerFocus();
	}
}

void Widget::keyPressEvent(QKeyEvent *e) {
	if (!_step) {
		return;
	} else if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) {
		if (_step->hasBack()) {
			_step->goBack();
		}
	} else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		_step->submit();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

} // namespace Intro
