#include "profile/profile_boxes.h"

#include "boxes/report_box.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_session.h"
#include "lang/lang_format.h"
#include "lang/lang_instance.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "profile/profile_logic.h"
#include "profile/profile_qr.h"
#include "support/error_text.h"
#include "support/file_dialogs.h"
#include "support/media_image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/input_fields.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_profile.h"

#include <QtCore/QDate>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QWheelEvent>

#include <cmath>

namespace Profile {
namespace {

constexpr auto kWheelRows = 5;
// The code is always dark on white: scanners need the contrast in both themes.
constexpr auto kQrDark = QColor(0x16, 0x1A, 0x17);

class ShareCard final : public Ui::RpWidget {
public:
	ShareCard(
		QWidget *parent,
		Main::Session *session,
		const Data::User &user)
	: RpWidget(parent)
	, _user(user)
	, _link(ProfileLink(user.handle))
	, _code(Qr::Encode(_link.toUtf8()))
	, _name(st::profileShareNameStyle, user.displayName)
	, _userpic(Ui::CreateChild<Support::UserpicButton>(
		this,
		session,
		user.id,
		st::profileShareUserpic)) {
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	[[nodiscard]] QImage qrImage() const {
		return Qr::Render(_code, 12, 4, kQrDark, Qt::white);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto size = int(st::profileShareUserpic);
		_userpic->move((newWidth - size) / 2, st::profileSharePadding);
		auto y = int(st::profileSharePadding) + size + st::spaceSm;
		_nameTop = y;
		y += st::profileShareNameStyle.font->height + st::space2xs;
		_handleTop = y;
		y += st::profileShareHandleFont->height + st::spaceLg;
		_qrTop = y;
		y += st::profileShareQrSize + st::spaceMd;
		_linkTop = y;
		y += st::profileShareHandleFont->height;
		return y + st::profileSharePadding;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(QPen(st::borderSubtle->c, 1.));
			p.setBrush(st::bgSurface);
			p.drawRoundedRect(
				QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
				st::profileShareRadius,
				st::profileShareRadius);
		}
		const auto outer = width();
		const auto badge = _user.verified
			? (st::profileRowVerifiedIcon.width() + st::profileBadgeSkip)
			: 0;
		const auto available = std::max(
			outer - 2 * st::profileSharePadding - badge,
			0);
		const auto nameWidth = std::min(_name.maxWidth(), available);
		const auto nameLeft = (outer - nameWidth - badge) / 2;
		p.setPen(st::textPrimary);
		_name.drawLeftElided(p, nameLeft, _nameTop, nameWidth, outer);
		if (_user.verified) {
			const auto &icon = st::profileRowVerifiedIcon;
			icon.paint(
				p,
				nameLeft + nameWidth + st::profileBadgeSkip,
				_nameTop
					+ (st::profileShareNameStyle.font->height - icon.height()) / 2,
				outer);
		}
		const auto &font = st::profileShareHandleFont;
		// Handles and addresses are left-to-right in both languages.
		p.setLayoutDirection(Qt::LeftToRight);
		p.setFont(font->f);
		p.setPen(st::textTertiary);
		const auto handle = QLatin1Char('@') + _user.handle;
		p.drawText(
			(outer - font->width(handle)) / 2,
			_handleTop + font->ascent,
			handle);

		const auto size = int(st::profileShareQrSize);
		const auto frame = QRect((outer - size) / 2, _qrTop, size, size);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawRoundedRect(frame, st::radiusMd, st::radiusMd);
		}
		if (_code.valid()) {
			// Whole device pixels per module keep the code crisp.
			const auto ratio = devicePixelRatioF();
			const auto quiet = 2;
			const auto modules = _code.size + 2 * quiet;
			const auto pixels = std::max(int(size * ratio) / modules, 1);
			if (_qr.isNull() || _qrRatio != ratio) {
				_qr = Qr::Render(_code, pixels, quiet, kQrDark, Qt::white);
				_qr.setDevicePixelRatio(ratio);
				_qrRatio = ratio;
			}
			const auto side = int(std::floor(modules * pixels / ratio));
			p.drawImage(
				QPointF(
					frame.x() + (size - side) / 2.,
					frame.y() + (size - side) / 2.),
				_qr);
		}
		p.setPen(st::textSecondary);
		const auto link = font->elided(_link, outer - 2 * st::profileSharePadding);
		p.drawText(
			(outer - font->width(link)) / 2,
			_linkTop + font->ascent,
			link);
	}

private:
	Data::User _user;
	QString _link;
	Qr::Code _code;
	Ui::Text::String _name;
	Support::UserpicButton *_userpic = nullptr;
	QImage _qr;
	double _qrRatio = 0.;
	int _nameTop = 0;
	int _handleTop = 0;
	int _qrTop = 0;
	int _linkTop = 0;

};

} // namespace

void ShowShareBox(
		Window::SessionController *controller,
		const Data::User &user) {
	if (user.handle.isEmpty()) {
		return;
	}
	const auto session = &controller->session();
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(tr::lng_profile_share_profile(tr::now));
		const auto card = box->addRow(
			Ui::object_ptr<ShareCard>(box, session, user),
			st::profileShareMargin);
		box->addRow(
			Ui::object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_profile_share_scan_hint(tr::now),
				st::profileShareHint),
			st::profileShareHintPadding);
		const auto link = ProfileLink(user.handle);
		box->addButton(tr::lng_copy_link(tr::now), [=] {
			QGuiApplication::clipboard()->setText(link);
			controller->showToast(tr::lng_link_copied(tr::now));
		}, st::defaultRoundButton);
		box->addButton(tr::lng_close(tr::now), [=] { box->closeBox(); });
		box->addLeftButton(tr::lng_profile_share_save_qr(tr::now), [=] {
			Support::SaveImageAs(
				controller->bodyWidget(),
				card->qrImage(),
				u"eleza-"_s + user.handle + u".png"_s);
		});
	}));
}

void ShowReportBox(
		Window::SessionController *controller,
		const Data::User &user,
		Fn<void()> blocked) {
	// The form is shared with the feed and the conversation topics.
	auto args = Boxes::ReportArgs();
	args.kind = u"account"_s;
	args.id = user.id;
	args.offerBlockUserId = user.id;
	args.blocked = std::move(blocked);
	Boxes::ShowReportBox(controller, std::move(args));
}

Wheel::Wheel(QWidget *parent) : RpWidget(parent) {
	setFocusPolicy(Qt::StrongFocus);
	setCursor(style::cur_pointer);
}

void Wheel::setItems(const QStringList &items, int selected) {
	_items = items;
	_animation.stop();
	_selected = items.isEmpty()
		? 0
		: std::clamp(selected, 0, int(items.size()) - 1);
	update();
}

double Wheel::offset() const {
	return _dragging ? _dragOffset : _animation.value(double(_selected));
}

void Wheel::setSelected(int index, bool animated) {
	choose(index, animated);
}

void Wheel::choose(int index, bool animated) {
	if (_items.isEmpty()) {
		return;
	}
	const auto from = offset();
	index = std::clamp(index, 0, int(_items.size()) - 1);
	const auto changed = (_selected != index);
	_selected = index;
	if (animated && std::abs(from - index) > 0.01) {
		_animation.start(
			[this] { update(); },
			from,
			double(index),
			st::profileWheelDuration,
			anim::easeOutCirc);
	} else {
		_animation.stop();
	}
	update();
	if (changed) {
		Q_EMIT selectedChanged(index);
	}
}

int Wheel::resizeGetHeight(int newWidth) {
	Q_UNUSED(newWidth);
	return kWheelRows * st::profileWheelRow;
}

void Wheel::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	const auto row = int(st::profileWheelRow);
	const auto center = (height() - row) / 2.;
	const auto current = offset();
	const auto first = std::max(int(std::floor(current)) - kWheelRows, 0);
	const auto last = std::min(
		int(std::ceil(current)) + kWheelRows,
		int(_items.size()) - 1);
	for (auto i = first; i <= last; ++i) {
		const auto distance = std::abs(i - current);
		const auto top = center + (i - current) * row;
		if (top + row < 0 || top > height()) {
			continue;
		}
		const auto &font = (distance < 0.5)
			? st::profileWheelFontActive
			: st::profileWheelFont;
		p.setFont(font->f);
		p.setOpacity(std::clamp(1. - distance * 0.32, 0.12, 1.));
		p.setPen((distance < 0.5) ? st::textPrimary : st::textSecondary);
		const auto text = font->elided(_items[i], width());
		p.drawText(
			QPointF(
				(width() - font->width(text)) / 2.,
				top + (row - font->height) / 2. + font->ascent),
			text);
	}
	p.setOpacity(1.);
	if (hasFocus()) {
		// A thin brand line marks the wheel the keyboard drives.
		const auto line = QRectF(
			width() / 2. - 10,
			center + row - 3,
			20,
			2);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::brandPrimary);
		p.drawRoundedRect(line, 1, 1);
	}
}

void Wheel::wheelEvent(QWheelEvent *e) {
	const auto pixels = e->pixelDelta().y();
	if (pixels) {
		_wheelDelta += pixels;
		const auto row = int(st::profileWheelRow);
		const auto steps = _wheelDelta / row;
		if (steps) {
			_wheelDelta -= steps * row;
			choose(_selected - steps, true);
		}
	} else {
		_wheelDelta += e->angleDelta().y();
		const auto steps = _wheelDelta / 120;
		if (steps) {
			_wheelDelta -= steps * 120;
			choose(_selected - steps, true);
		}
	}
	e->accept();
}

void Wheel::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	setFocus();
	_pressOffset = offset();
	_dragOffset = _pressOffset;
	_animation.stop();
	_dragging = true;
	_moved = false;
	_pressY = e->pos().y();
}

void Wheel::mouseMoveEvent(QMouseEvent *e) {
	if (!_dragging) {
		return;
	}
	const auto delta = e->pos().y() - _pressY;
	if (std::abs(delta) > 3) {
		_moved = true;
	}
	if (_moved) {
		_dragOffset = std::clamp(
			_pressOffset - delta / double(st::profileWheelRow),
			-0.4,
			std::max(double(_items.size()) - 0.6, 0.));
		update();
	}
}

void Wheel::mouseReleaseEvent(QMouseEvent *e) {
	if (!_dragging || e->button() != Qt::LeftButton) {
		return;
	}
	const auto from = _dragOffset;
	_dragging = false;
	if (_moved) {
		// Snaps to the nearest row from where the drag left it.
		const auto target = std::clamp(
			int(std::lround(from)),
			0,
			std::max(int(_items.size()) - 1, 0));
		const auto changed = (target != _selected);
		_selected = target;
		_animation.start(
			[this] { update(); },
			from,
			double(target),
			st::profileWheelDuration,
			anim::easeOutCirc);
		update();
		if (changed) {
			Q_EMIT selectedChanged(target);
		}
		return;
	}
	const auto row = int(st::profileWheelRow);
	const auto center = (height() - row) / 2;
	const auto shift = int(std::floor((e->pos().y() - center) / double(row)));
	choose(_selected + shift, true);
}

void Wheel::keyPressEvent(QKeyEvent *e) {
	switch (e->key()) {
	case Qt::Key_Up: choose(_selected - 1, true); break;
	case Qt::Key_Down: choose(_selected + 1, true); break;
	case Qt::Key_PageUp: choose(_selected - kWheelRows, true); break;
	case Qt::Key_PageDown: choose(_selected + kWheelRows, true); break;
	case Qt::Key_Home: choose(0, true); break;
	case Qt::Key_End: choose(int(_items.size()) - 1, true); break;
	default: RpWidget::keyPressEvent(e); break;
	}
}

void Wheel::focusInEvent(QFocusEvent *e) {
	Q_UNUSED(e);
	update();
}

void Wheel::focusOutEvent(QFocusEvent *e) {
	Q_UNUSED(e);
	update();
}

BirthdayBox::BirthdayBox(
	QWidget *parent,
	std::optional<Data::Birthday> current,
	Fn<void(std::optional<Data::Birthday>)> done)
: BoxContent(parent)
, _initial(std::move(current))
, _done(std::move(done)) {
}

QVector<Wheel*> BirthdayBox::order() const {
	// «۱۲ مهر ۱۳۷۰» reads day, month, year; "March 4, 1991" month first.
	return Lang::IsPersian()
		? QVector<Wheel*>{ _day, _month, _year }
		: QVector<Wheel*>{ _month, _day, _year };
}

void BirthdayBox::prepare() {
	setTitle(tr::lng_profile_birthday(tr::now));
	_jalali = Ui::CreateChild<Ui::PillButton>(
		this,
		tr::lng_profile_birthday_jalali(tr::now),
		st::smallPillButton);
	_gregorian = Ui::CreateChild<Ui::PillButton>(
		this,
		tr::lng_profile_birthday_gregorian(tr::now),
		st::smallPillButton);
	_day = Ui::CreateChild<Wheel>(this);
	_month = Ui::CreateChild<Wheel>(this);
	_year = Ui::CreateChild<Wheel>(this);
	const auto wheels = order();
	setTabOrder(wheels[0], wheels[1]);
	setTabOrder(wheels[1], wheels[2]);

	auto start = Data::Birthday();
	if (_initial) {
		start = ClampBirthday(*_initial);
		start.calendar = IsJalali(_initial->calendar)
			? u"jalali"_s
			: u"gregorian"_s;
	} else {
		// A neutral start: the first day of the year, no year.
		start.calendar = DefaultCalendar();
		start.month = 1;
		start.day = 1;
	}
	_calendar = start.calendar;
	fill(start);

	_jalali->setClickedCallback([this] { switchCalendar(u"jalali"_s); });
	_gregorian->setClickedCallback([this] { switchCalendar(u"gregorian"_s); });
	connect(_month, &Wheel::selectedChanged, this, [this] {
		refreshDays();
		update();
	});
	connect(_year, &Wheel::selectedChanged, this, [this] {
		refreshDays();
		update();
	});
	connect(_day, &Wheel::selectedChanged, this, [this] { update(); });

	addButton(tr::lng_profile_birthday_set(tr::now), [this] {
		const auto result = value();
		if (!IsValidBirthday(result, QDate::currentDate())) {
			return;
		}
		const auto done = _done;
		closeBox();
		if (done) {
			done(result);
		}
	}, st::defaultRoundButton);
	addButton(tr::lng_cancel(tr::now), [this] { closeBox(); });
	if (_initial) {
		addLeftButton(tr::lng_profile_birthday_remove(tr::now), [this] {
			const auto done = _done;
			closeBox();
			if (done) {
				done(std::nullopt);
			}
		}, st::dangerBoxButton);
	}
	setDimensions(
		st::boxWidth,
		st::profileBirthdayTop
			+ _jalali->height()
			+ st::spaceLg
			+ st::smallFont->height
			+ st::spaceXs
			+ kWheelRows * st::profileWheelRow
			+ st::spaceMd
			+ st::profileBirthdayPreviewFont->height
			+ st::spaceLg);
}

void BirthdayBox::setInnerFocus() {
	order().front()->setFocus();
}

void BirthdayBox::fill(const Data::Birthday &birthday) {
	const auto today = QDate::currentDate();
	_jalali->setActive(IsJalali(_calendar), false);
	_gregorian->setActive(!IsJalali(_calendar), false);

	auto months = QStringList();
	for (auto month = 1; month <= 12; ++month) {
		months.push_back(MonthName(_calendar, month));
	}
	_month->setItems(months, birthday.month - 1);

	const auto range = BirthYears(_calendar, today);
	_yearFrom = range.from;
	auto years = QStringList{ tr::lng_profile_birthday_no_year(tr::now) };
	for (auto year = range.from; year >= range.till; --year) {
		years.push_back(Lang::Num(year));
	}
	const auto yearIndex = (birthday.year >= range.till
		&& birthday.year <= range.from)
		? (range.from - birthday.year + 1)
		: 0;
	_year->setItems(years, yearIndex);

	auto days = QStringList();
	const auto year = yearIndex ? (range.from - yearIndex + 1) : 0;
	const auto count = MonthDays(_calendar, year, birthday.month);
	for (auto day = 1; day <= count; ++day) {
		days.push_back(Lang::Num(day));
	}
	_day->setItems(days, birthday.day - 1);
	update();
}

Data::Birthday BirthdayBox::value() const {
	auto result = Data::Birthday();
	result.calendar = _calendar;
	result.month = _month->selected() + 1;
	result.day = _day->selected() + 1;
	const auto yearIndex = _year->selected();
	result.year = yearIndex ? (_yearFrom - yearIndex + 1) : 0;
	return result;
}

void BirthdayBox::refreshDays() {
	const auto current = value();
	const auto count = MonthDays(_calendar, current.year, current.month);
	auto days = QStringList();
	for (auto day = 1; day <= count; ++day) {
		days.push_back(Lang::Num(day));
	}
	// Esfand 30 -> 29 when the chosen year is not a leap one.
	_day->setItems(days, std::min(current.day, count) - 1);
}

void BirthdayBox::switchCalendar(const QString &calendar) {
	if (IsJalali(calendar) == IsJalali(_calendar)) {
		return;
	}
	const auto converted = ConvertBirthday(
		value(),
		calendar,
		QDate::currentDate());
	_calendar = calendar;
	fill(ClampBirthday(converted));
}

void BirthdayBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	const auto skip = int(st::spaceSm);
	const auto chips = _jalali->naturalWidth() + skip + _gregorian->naturalWidth();
	auto y = int(st::profileBirthdayTop);
	_jalali->resizeToNaturalWidth(width());
	_gregorian->resizeToNaturalWidth(width());
	_jalali->moveToLeft((width() - chips) / 2, y, width());
	_gregorian->moveToLeft(
		(width() - chips) / 2 + _jalali->width() + skip,
		y,
		width());
	y += _jalali->height() + st::spaceLg;
	_captionTop = y;
	y += st::smallFont->height + st::spaceXs;

	const auto padding = int(st::profileBirthdayPadding);
	const auto inner = width() - 2 * padding;
	// The month names need more room than the numbers.
	const auto monthWidth = inner * 2 / 5;
	const auto other = (inner - monthWidth) / 2;
	auto left = padding;
	for (const auto wheel : order()) {
		const auto wheelWidth = (wheel == _month) ? monthWidth : other;
		wheel->resizeToWidth(wheelWidth);
		wheel->moveToLeft(left, y, width());
		left += wheelWidth;
	}
	const auto row = int(st::profileWheelRow);
	_band = QRect(padding, y + (_day->height() - row) / 2, inner, row);
	y += _day->height() + st::spaceMd;
	_previewTop = y;
}

void BirthdayBox::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::bgSunken);
		p.drawRoundedRect(_band, st::radiusMd, st::radiusMd);
	}
	p.setFont(st::smallFont->f);
	p.setPen(st::textTertiary);
	const auto caption = [&](Wheel *wheel, const QString &text) {
		p.drawText(
			wheel->x() + (wheel->width() - st::smallFont->width(text)) / 2,
			_captionTop + st::smallFont->ascent,
			text);
	};
	caption(_day, tr::lng_profile_birthday_day(tr::now));
	caption(_month, tr::lng_profile_birthday_month(tr::now));
	caption(_year, tr::lng_profile_birthday_year(tr::now));

	const auto &font = st::profileBirthdayPreviewFont;
	const auto text = BirthdayText(value());
	p.setFont(font->f);
	p.setPen(st::brandText);
	p.drawText(
		(width() - font->width(text)) / 2,
		_previewTop + font->ascent,
		text);
}

void BirthdayBox::keyPressEvent(QKeyEvent *e) {
	const auto wheels = order();
	const auto focused = int(wheels.indexOf(
		qobject_cast<Wheel*>(focusWidget())));
	// Left / Right walk between the wheels in reading order.
	const auto next = style::RightToLeft() ? Qt::Key_Left : Qt::Key_Right;
	const auto previous = style::RightToLeft() ? Qt::Key_Right : Qt::Key_Left;
	if (e->key() == next || e->key() == previous) {
		const auto index = (focused < 0)
			? 0
			: std::clamp(focused + ((e->key() == next) ? 1 : -1), 0, 2);
		wheels[index]->setFocus();
		return;
	}
	BoxContent::keyPressEvent(e);
}

} // namespace Profile
