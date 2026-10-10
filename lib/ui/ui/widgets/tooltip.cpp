#include "ui/widgets/tooltip.h"

#include "ui/painter.h"
#include "ui/ui_utility.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

#include <algorithm>

namespace Ui {
namespace {

constexpr auto kShowDelay = 600;

QPointer<Tooltip> Instance;
bool HoverSuppressed = false;

[[nodiscard]] QStringList WrapLines(const QString &text, int maxWidth) {
	auto result = QStringList();
	const auto &font = st::defaultTooltip.font;
	const auto paragraphs = text.split(QLatin1Char('\n'));
	for (const auto &paragraph : paragraphs) {
		auto line = QString();
		const auto words = paragraph.split(QLatin1Char(' '));
		for (const auto &word : words) {
			const auto candidate = line.isEmpty()
				? word
				: (line + QLatin1Char(' ') + word);
			if (!line.isEmpty() && font->width(candidate) > maxWidth) {
				result.push_back(line);
				line = word;
			} else {
				line = candidate;
			}
		}
		result.push_back(line);
	}
	return result;
}

void PaintBubble(QPainter &p, const QRect &rect, const QStringList &lines) {
	const auto &st = st::defaultTooltip;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st.bg);
	p.drawRoundedRect(rect, st.radius, st.radius);
	p.setFont(st.font);
	p.setPen(st.fg);
	p.setLayoutDirection(LayoutDirection());
	auto top = rect.y() + st.padding.top();
	for (const auto &line : lines) {
		const auto width = st.font->width(line);
		const auto left = style::RightToLeft()
			? (rect.x() + rect.width() - st.padding.right() - width)
			: (rect.x() + st.padding.left());
		p.drawText(left, top + st.font->ascent, line);
		top += st.font->height;
	}
}

[[nodiscard]] QSize BubbleSize(const QStringList &lines) {
	const auto &st = st::defaultTooltip;
	auto width = 0;
	for (const auto &line : lines) {
		width = std::max(width, st.font->width(line));
	}
	return QSize(
		st.padding.left() + width + st.padding.right(),
		st.padding.top()
			+ int(lines.size()) * st.font->height
			+ st.padding.bottom());
}

class Watcher final : public QObject {
public:
	Watcher(QWidget *widget, std::function<QString()> text)
	: QObject(widget)
	, _widget(widget)
	, _text(std::move(text)) {
		_timer.setSingleShot(true);
		_timer.setInterval(kShowDelay);
		QObject::connect(&_timer, &QTimer::timeout, this, [this] {
			if (HoverSuppressed) {
				return;
			}
			if (_widget && _widget->underMouse() && _text) {
				const auto text = _text();
				if (!text.isEmpty()) {
					Tooltip::Show(_widget, text);
					_shown = true;
				}
			}
		});
		widget->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		switch (event->type()) {
		case QEvent::Enter:
			_timer.start();
			break;
		case QEvent::Leave:
		case QEvent::MouseButtonPress:
		case QEvent::Wheel:
		case QEvent::Hide:
			_timer.stop();
			if (std::exchange(_shown, false)) {
				Tooltip::Hide();
			}
			break;
		default: break;
		}
		return QObject::eventFilter(object, event);
	}

private:
	QPointer<QWidget> _widget;
	std::function<QString()> _text;
	QTimer _timer;
	bool _shown = false;

};

} // namespace

Tooltip::Tooltip() : RpWidget(nullptr) {
	setWindowFlags(Qt::ToolTip
		| Qt::FramelessWindowHint
		| Qt::NoDropShadowWindowHint
		| Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void Tooltip::setTextAndResize(const QString &text) {
	_text = text;
	_lines = WrapLines(text, st::defaultTooltip.maxWidth);
	resize(BubbleSize(_lines));
	update();
}

void Tooltip::place(const QRect &globalAnchor) {
	auto screen = QGuiApplication::screenAt(globalAnchor.center());
	if (!screen) {
		screen = QGuiApplication::primaryScreen();
	}
	const auto available = screen
		? screen->availableGeometry()
		: QRect(0, 0, 1920, 1080);
	const auto skip = st::defaultTooltip.skip;
	auto left = globalAnchor.center().x() - width() / 2;
	auto top = globalAnchor.bottom() + 1 + skip;
	if (top + height() > available.bottom()) {
		top = globalAnchor.top() - skip - height();
	}
	left = std::clamp(
		left,
		available.left() + skip,
		std::max(available.right() - width() - skip, available.left() + skip));
	move(left, top);
}

void Tooltip::Show(QWidget *anchor, const QString &text) {
	if (!anchor || text.isEmpty()) {
		Hide();
		return;
	}
	if (!Instance) {
		Instance = new Tooltip();
	}
	Instance->setTextAndResize(text);
	Instance->place(QRect(anchor->mapToGlobal(QPoint()), anchor->size()));
	Instance->show();
}

void Tooltip::ShowAt(const QPoint &globalPosition, const QString &text) {
	if (text.isEmpty()) {
		Hide();
		return;
	}
	if (!Instance) {
		Instance = new Tooltip();
	}
	Instance->setTextAndResize(text);
	Instance->place(QRect(globalPosition, QSize(1, 16)));
	Instance->show();
}

void Tooltip::Hide() {
	if (Instance) {
		Instance->hide();
	}
}

bool Tooltip::IsShown() {
	return Instance && !Instance->isHidden();
}

QPixmap Tooltip::Render(const QString &text, double ratio) {
	const auto lines = WrapLines(text, st::defaultTooltip.maxWidth);
	const auto size = BubbleSize(lines);
	auto result = QPixmap(size * ratio);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	PaintBubble(p, QRect(QPoint(), size), lines);
	return result;
}

void Tooltip::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.setCompositionMode(QPainter::CompositionMode_Source);
	p.fillRect(rect(), Qt::transparent);
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	PaintBubble(p, rect(), _lines);
}

void InstallTooltip(QWidget *widget, std::function<QString()> text) {
	new Watcher(widget, std::move(text));
}

void SetHoverTooltipsSuppressed(bool suppressed) {
	HoverSuppressed = suppressed;
	if (suppressed) {
		Tooltip::Hide();
	}
}

bool HoverTooltipsSuppressed() {
	return HoverSuppressed;
}

} // namespace Ui
