#include "history/history_module.h"

#include "history/history_compose_controls.h"
#include "history/history_list_widget.h"
#include "history/history_widget.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"
#include "window/window_session_controller.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

#include <cstdio>

namespace History {
namespace {

constexpr auto kStressCount = 5000;
constexpr auto kStressSteps = 240;
constexpr auto kStressJumps = 60;

// A long, varied timeline that never touches the store: texts of every
// length in both scripts, links, replies, reactions, files, voice messages,
// locations, service lines, spread over two months.
[[nodiscard]] QVector<Data::Message> StressMessages(const QString &myId) {
	static const auto kTexts = QStringList{
		u"سلام! خوبی؟"_s,
		u"آره، همین الان رسیدم خانه. تو چطوری؟ امروز خیلی شلوغ بود و نتونستم زودتر جواب بدم."_s,
		u"Sure, let me check and get back to you in a minute."_s,
		u"باشه 👍"_s,
		u"این را ببین: https://eleza.me/eli/post/p1 به نظرم خیلی جالب بود و ارزش خواندن دارد."_s,
		u"فردا ساعت ۵ جلسه داریم، یادت نره. بعدش هم اگر وقت داشتی بریم یک قهوه بخوریم و دربارهٔ برنامهٔ سفر حرف بزنیم؛ هنوز بلیت نگرفته‌ام و باید زودتر تصمیم بگیریم."_s,
		u"😂😂"_s,
		u"The build is green again. I rebased on main, fixed the two flaky tests and pushed; can you take another look when you have a moment? Thanks!"_s,
		u"نه هنوز"_s,
		u"@sara را هم خبر کن #سفر"_s,
		u"خیلی ممنون از اینکه وقت گذاشتی. راستش فکر نمی‌کردم این‌قدر زود جواب بدهی. من همهٔ فایل‌ها را مرتب کردم و برایت می‌فرستم؛ اگر چیزی کم بود بگو تا دوباره نگاه کنم. فقط یک نکته: نسخهٔ آخر گزارش هنوز چند غلط تایپی دارد که باید قبل از ارسال درست شود."_s,
		u"ok"_s,
		u"❤️"_s,
		u"mixed متن with English وسط جمله and numbers 12345 to test bidi."_s,
	};
	static const auto kReactions = QStringList{ u"👍"_s, u"❤️"_s, u"😂"_s, u"🔥"_s };
	auto result = QVector<Data::Message>();
	result.reserve(kStressCount);
	const auto now = QDateTime::currentMSecsSinceEpoch();
	const auto span = qint64(60) * 24 * 3600 * 1000;
	auto state = quint32(0x2545F491);
	const auto next = [&] {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	};
	auto mine = false;
	for (auto i = 0; i != kStressCount; ++i) {
		auto message = Data::Message();
		message.chat = u"debug-stress"_s;
		message.seq = i + 1;
		message.id = u"m%1"_s.arg(i + 1);
		if (next() % 3 != 0) {
			mine = (next() % 2 == 0);
		}
		message.mine = mine;
		message.sender = mine ? myId : u"u_reza"_s;
		message.createdAt = now - span + (span * i) / kStressCount;
		message.status = (i + 40 > kStressCount)
			? Data::MessageStatus::Sent
			: Data::MessageStatus::Read;
		const auto roll = next() % 100;
		if (roll < 2) {
			message.kind = Data::MessageKind::System;
			message.system = u"message.pinned"_s;
		} else if (roll < 6) {
			message.kind = Data::MessageKind::File;
			message.text = u"report-%1.pdf"_s.arg(i);
			auto meta = Data::MediaMeta();
			meta.size = 120'000 + qint64(next() % 4'000'000);
			meta.mime = u"application/pdf"_s;
			message.media = meta;
		} else if (roll < 10) {
			message.kind = Data::MessageKind::Voice;
			auto meta = Data::MediaMeta();
			meta.durationMs = 3000 + qint64(next() % 60'000);
			meta.size = 40'000;
			message.media = meta;
		} else if (roll < 12) {
			message.kind = Data::MessageKind::Location;
			auto location = Data::Location();
			location.lat = 35.70 + (next() % 100) / 1000.;
			location.lng = 51.40 + (next() % 100) / 1000.;
			message.location = location;
		} else {
			message.kind = Data::MessageKind::Text;
			message.text = kTexts[next() % kTexts.size()];
			if (roll < 20 && message.text.size() > 20) {
				auto entity = Data::Entity();
				entity.type = u"spoiler"_s;
				entity.offset = 2;
				entity.length = 9;
				message.entities.push_back(entity);
			}
			if (roll > 80 && i > 10) {
				message.replyToSeq = i - qint64(next() % 8);
			}
			message.edited = (roll % 17 == 0);
		}
		if (next() % 9 == 0 && message.kind != Data::MessageKind::System) {
			message.reactions.insert(kReactions[next() % kReactions.size()], 1 + next() % 3);
			if (next() % 2) {
				message.myReaction = message.reactions.firstKey();
			}
		}
		result.push_back(std::move(message));
	}
	return result;
}

class StressSection final : public Window::SectionWidget {
public:
	StressSection(QWidget *parent, Window::SessionController *controller);

	[[nodiscard]] QString sectionKey() const override {
		return u"debug/history-stress"_s;
	}
	void showFinished() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void run();
	void step();
	void finish();

	Ui::ScrollArea *_scroll = nullptr;
	ListWidget *_list = nullptr;
	QTimer _timer;
	QStringList _report;
	int _step = 0;
	bool _started = false;
	qint64 _fillUs = 0;
	qint64 _stepsUs = 0;
	qint64 _stepsMaxUs = 0;
	qint64 _jumpsUs = 0;
	qint64 _jumpsMaxUs = 0;
	ListWidget::PaintStats _scrollStats;
	quint32 _random = 0x9E3779B9;

};

StressSection::StressSection(QWidget *parent, Window::SessionController *controller)
: SectionWidget(parent, controller)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this)) {
	_list = _scroll->setOwnedWidget(Ui::object_ptr<ListWidget>(
		_scroll,
		controller,
		u"debug-stress"_s,
		_scroll));
	_timer.setInterval(0);
	connect(&_timer, &QTimer::timeout, this, [this] { step(); });
}

void StressSection::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto top = st::historyTopBarHeight + 2 * st::historyBarHeight;
	_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
	_list->resizeToWidth(width());
}

void StressSection::showFinished() {
	if (_started) {
		return;
	}
	_started = true;
	QTimer::singleShot(100, this, [this] { run(); });
}

void StressSection::run() {
	const auto items = StressMessages(controller()->session().userId());
	auto timer = QElapsedTimer();
	timer.start();
	_list->setDebugItems(items);
	_list->resizeToWidth(_scroll->width());
	_scroll->scrollToY(_scroll->scrollTopMax());
	_list->repaint();
	_fillUs = timer.nsecsElapsed() / 1000;
	_list->resetPaintStats();
	_step = 0;
	_timer.start();
}

void StressSection::step() {
	const auto max = _scroll->scrollTopMax();
	auto timer = QElapsedTimer();
	timer.start();
	if (_step < kStressSteps) {
		// A steady scroll up from the end, a third of a screen per frame.
		const auto delta = std::max(_scroll->height() / 3, 80);
		_scroll->scrollToY(std::max(_scroll->scrollTop() - delta, 0));
		_list->repaint();
		const auto elapsed = timer.nsecsElapsed() / 1000;
		_stepsUs += elapsed;
		_stepsMaxUs = std::max(_stepsMaxUs, elapsed);
		if (_step + 1 == kStressSteps) {
			_scrollStats = _list->paintStats();
			_list->resetPaintStats();
		}
	} else if (_step < kStressSteps + kStressJumps) {
		// Jumps to far places: rows that were only estimated are measured.
		_random ^= _random << 13;
		_random ^= _random >> 17;
		_random ^= _random << 5;
		_scroll->scrollToY(max > 0 ? int(_random % quint32(max)) : 0);
		_list->repaint();
		const auto elapsed = timer.nsecsElapsed() / 1000;
		_jumpsUs += elapsed;
		_jumpsMaxUs = std::max(_jumpsMaxUs, elapsed);
	} else {
		_timer.stop();
		finish();
		return;
	}
	++_step;
}

void StressSection::finish() {
	const auto jumps = _list->paintStats();
	const auto ms = [](qint64 us) {
		return QString::number(us / 1000., 'f', 2);
	};
	_report = QStringList{
		u"%1 messages: fill + first layout + first paint %2 ms"_s
			.arg(kStressCount)
			.arg(ms(_fillUs)),
		u"scroll x%1: frame avg %2 ms, max %3 ms (paint avg %4 ms, max %5 ms, %6 rows/frame)"_s
			.arg(kStressSteps)
			.arg(ms(_stepsUs / kStressSteps))
			.arg(ms(_stepsMaxUs))
			.arg(ms(_scrollStats.paints ? (_scrollStats.totalUs / _scrollStats.paints) : 0))
			.arg(ms(_scrollStats.maxUs))
			.arg(_scrollStats.rows),
		u"far jumps x%1: frame avg %2 ms, max %3 ms (paint avg %4 ms); layout total %5 ms"_s
			.arg(kStressJumps)
			.arg(ms(_jumpsUs / kStressJumps))
			.arg(ms(_jumpsMaxUs))
			.arg(ms(jumps.paints ? (jumps.totalUs / jumps.paints) : 0))
			.arg(ms(_scrollStats.layoutUs + jumps.layoutUs)),
	};
	for (const auto &line : std::as_const(_report)) {
		std::fprintf(stderr, "history-stress: %s\n", qPrintable(line));
	}
	std::fflush(stderr);
	_scroll->scrollToY(_scroll->scrollTopMax());
	update();
}

void StressSection::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::bgCanvas);
	p.setPen(st::textPrimary);
	p.setFont(st::historyTopBarStatusFont);
	const auto line = st::historyTopBarStatusFont->height + 6;
	auto top = 12;
	if (_report.isEmpty()) {
		p.drawText(16, top + st::historyTopBarStatusFont->ascent, u"history-stress: running…"_s);
		return;
	}
	for (const auto &text : std::as_const(_report)) {
		p.drawText(16, top + st::historyTopBarStatusFont->ascent, text);
		top += line;
	}
}

[[nodiscard]] Widget *CurrentChat(Window::SessionController *controller) {
	return qobject_cast<Widget*>(controller->mainSection());
}

} // namespace

void Register() {
	Window::RegisterRouteHandler(Window::RouteType::Chat, [](
			Window::SessionController *controller,
			const Window::Route &route) {
		if (route.id.isEmpty()) {
			return false;
		}
		const auto current = CurrentChat(controller);
		if (current
			&& current->chatId() == route.id
			&& controller->currentTab() == Window::Tab::Chats) {
			// Already on screen: only the message (and waiting forwards).
			current->showMessage(route.seq());
			current->setInnerFocus();
			return true;
		}
		controller->showMainSection(
			Factory(route.id, route.seq()),
			{ .tab = Window::Tab::Chats });
		return true;
	});
	// Developer routes: the stress test and states that need a real drag or
	// a system dialog to reach.
	Window::RegisterRouteHandler([](
			Window::SessionController *controller,
			const QString &route) {
		static const auto kPrefix = u"debug/history-"_s;
		if (!route.startsWith(kPrefix)) {
			return false;
		}
		const auto name = route.mid(kPrefix.size());
		if (name == u"stress") {
			controller->showMainSection([](
					QWidget *parent,
					Window::SessionController *controller) {
				return Ui::Owned<Window::SectionWidget>(
					new StressSection(parent, controller));
			}, { .tab = Window::Tab::Chats });
			return true;
		}
		const auto chat = CurrentChat(controller);
		if (!chat) {
			return false;
		} else if (name == u"drop") {
			chat->debugShowDropArea();
		} else if (name == u"emoji") {
			chat->compose()->debugShowPanel(false);
		} else if (name == u"stickers") {
			chat->compose()->debugShowPanel(true);
		} else if (name == u"theme") {
			chat->showThemeBox();
		} else if (name == u"search") {
			chat->showSearch();
		} else {
			return false;
		}
		return true;
	});
}

} // namespace History
