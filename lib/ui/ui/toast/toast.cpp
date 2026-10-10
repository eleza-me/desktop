#include "ui/toast/toast.h"

#include "ui/effects/animations.h"
#include "ui/effects/shadow.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"

#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>

#include <algorithm>
#include <deque>

namespace Ui::Toast {
namespace {

constexpr auto kMinDuration = 2200;
constexpr auto kMaxDuration = 7000;
constexpr auto kPerCharacter = 55;

class Manager;

class Widget final : public RpWidget {
public:
	Widget(QWidget *parent, Config config, std::function<void()> finished)
	: RpWidget(parent)
	, _st(st::defaultToast)
	, _config(std::move(config))
	, _text(_st.style, _config.text)
	, _finished(std::move(finished)) {
		if (_config.clicked) {
			setCursor(style::cur_pointer);
		} else {
			setAttribute(Qt::WA_TransparentForMouseEvents);
		}
		_hideTimer.setSingleShot(true);
		QObject::connect(&_hideTimer, &QTimer::timeout, this, [this] {
			toggle(false);
		});
		place();
		show();
		raise();
		toggle(true);
		const auto duration = _config.duration
			? _config.duration
			: std::clamp(
				int(_config.text.size()) * kPerCharacter,
				kMinDuration,
				kMaxDuration);
		_hideTimer.start(duration);
	}

	void place() {
		const auto parent = parentWidget();
		if (!parent) {
			return;
		}
		const auto &extend = _st.shadow.extend;
		const auto iconWidth = _config.icon
			? (_config.icon->width() + _st.iconSkip)
			: 0;
		const auto maxBody = std::min(
			_st.maxWidth,
			parent->width() - 2 * _st.margin);
		const auto maxText = std::max(
			maxBody - _st.padding.left() - _st.padding.right() - iconWidth,
			1);
		_textWidth = std::min(_text.maxWidth(), maxText);
		const auto textHeight = _text.countHeight(_textWidth, _st.maxLines);
		const auto bodyWidth = _st.padding.left()
			+ iconWidth
			+ _textWidth
			+ _st.padding.right();
		const auto bodyHeight = std::max(
			_st.padding.top() + textHeight + _st.padding.bottom(),
			_st.minHeight);
		const auto body = QRect(
			(parent->width() - bodyWidth) / 2,
			parent->height() - _st.margin - _config.bottomSkip - bodyHeight,
			bodyWidth,
			bodyHeight);
		setGeometry(body.marginsAdded(extend));
	}

	void hideNow() {
		_hideTimer.stop();
		toggle(false);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto shown = _animation.value(_shown ? 1. : 0.);
		const auto body = rect().marginsRemoved(_st.shadow.extend).translated(
			0,
			int((1. - shown) * _st.slide));
		p.setOpacity(shown);
		Shadow::Paint(p, body, _st.radius, _st.shadow, shown);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_st.bg);
			const auto radius = std::min(_st.radius, body.height() / 2);
			p.drawRoundedRect(body, radius, radius);
		}
		auto left = _st.padding.left();
		if (_config.icon) {
			_config.icon->paint(
				p,
				(body.x() - 0) + left,
				body.y() + (body.height() - _config.icon->height()) / 2,
				width());
			left += _config.icon->width() + _st.iconSkip;
		}
		p.setPen(_st.fg);
		const auto textHeight = _text.countHeight(_textWidth, _st.maxLines);
		const auto x = style::RightToLeft()
			? (body.x() + body.width() - left - _textWidth)
			: (body.x() + left);
		_text.draw(p, Text::PaintContext{
			.position = QPoint(x, body.y() + (body.height() - textHeight) / 2),
			.availableWidth = _textWidth,
			.align = (_text.countLines(_textWidth) > 1)
				? style::al_left
				: style::al_top,
			.elisionLines = _st.maxLines,
		});
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton || !_shown) {
			return;
		}
		const auto body = rect().marginsRemoved(_st.shadow.extend);
		if (const auto callback = _config.clicked;
			callback && body.contains(e->pos())) {
			hideNow();
			callback();
		}
	}

private:
	void toggle(bool shown) {
		if (_shown == shown) {
			return;
		}
		_shown = shown;
		_animation.start([this] {
			update();
			if (!_animation.animating() && !_shown) {
				finish();
			}
		}, shown ? 0. : 1., shown ? 1. : 0., _st.duration, anim::easeOutCubic);
		update();
		if (!_animation.animating() && !shown) {
			finish();
		}
	}

	void finish() {
		hide();
		if (const auto callback = std::exchange(_finished, nullptr)) {
			PostponeCall(this, callback);
		}
	}

	const style::Toast &_st;
	Config _config;
	Text::String _text;
	int _textWidth = 0;
	bool _shown = false;
	QTimer _hideTimer;
	Animations::Simple _animation;
	std::function<void()> _finished;

};

class Manager final : public QObject {
public:
	explicit Manager(QWidget *parent) : QObject(parent), _parent(parent) {
		setObjectName(Name());
		parent->installEventFilter(this);
	}

	[[nodiscard]] static Manager *For(QWidget *parent, bool create) {
		// Found by name: the class has no meta object of its own.
		const auto existing = parent->findChild<QObject*>(
			Name(),
			Qt::FindDirectChildrenOnly);
		return existing
			? static_cast<Manager*>(existing)
			: create
			? new Manager(parent)
			: nullptr;
	}

	void show(Config config) {
		_queue.push_back(std::move(config));
		if (!_current) {
			showNext();
		} else if (_queue.size() > 2) {
			// Do not let a burst of toasts keep the screen busy for long.
			_current->hideNow();
		}
	}

	void hideAll() {
		_queue.clear();
		if (_current) {
			_current->hideNow();
		}
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (object == _parent && event->type() == QEvent::Resize && _current) {
			_current->place();
		}
		return QObject::eventFilter(object, event);
	}

private:
	[[nodiscard]] static QString Name() {
		return QStringLiteral("eleza_toast_manager");
	}

	void showNext() {
		if (_queue.empty()) {
			return;
		}
		auto config = std::move(_queue.front());
		_queue.pop_front();
		_current = new Widget(_parent, std::move(config), [this] {
			if (_current) {
				_current->deleteLater();
				_current = nullptr;
			}
			showNext();
		});
	}

	QWidget *_parent = nullptr;
	std::deque<Config> _queue;
	QPointer<Widget> _current;

};

} // namespace

void Show(QWidget *parent, const QString &text) {
	auto config = Config();
	config.text = text;
	Show(parent, std::move(config));
}

void Show(QWidget *parent, Config config) {
	if (!parent || config.text.isEmpty()) {
		return;
	}
	Manager::For(parent, true)->show(std::move(config));
}

void HideAll(QWidget *parent) {
	if (!parent) {
		return;
	}
	if (const auto manager = Manager::For(parent, false)) {
		manager->hideAll();
	}
}

} // namespace Ui::Toast
