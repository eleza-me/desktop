#include "boxes/country_select_box.h"

#include "lang/lang_format.h"
#include "lang_auto.h"
#include "ui/painter.h"
#include "ui/widgets/input_fields.h"
#include "styles/style_boxes.h"

#include <QtGui/QKeyEvent>

namespace Boxes {

CountrySelectBox::CountrySelectBox(
	QWidget *parent,
	const QString &currentIso,
	std::function<void(const Lang::Country &country)> chosen)
: BoxContent(parent)
, _currentIso(currentIso)
, _chosen(std::move(chosen))
, _flags(Lang::FlagEmojiSupported()) {
}

void CountrySelectBox::prepare() {
	setTitle(tr::lng_intro_country(tr::now));
	_search = new Ui::SearchField(this, tr::lng_intro_country_search(tr::now));
	auto list = Ui::object_ptr<Ui::ListWidget>(
		this,
		static_cast<Ui::ListDelegate*>(this));
	_list = setInnerWidget(std::move(list), 0, 0);
	applyFilter(QString());

	QObject::connect(_search, &Ui::SearchField::changed, this, [this](
			const QString &text) {
		applyFilter(text);
	});
	QObject::connect(_search, &Ui::SearchField::submitted, this, [this] {
		if (_list->selectedRow() >= 0) {
			choose(_list->selectedRow());
		} else if (!_rows.empty()) {
			choose(0);
		}
	});
	QObject::connect(_search, &Ui::SearchField::cancelled, this, [this] {
		closeBox();
	});
	QObject::connect(_list, &Ui::ListWidget::rowClicked, this, [this](int index) {
		choose(index);
	});
	QObject::connect(
		_list,
		&Ui::ListWidget::scrollToRequested,
		this,
		[this](int top, int bottom) {
			scrollArea()->scrollToY(top, bottom);
		});
	// Arrows typed into the field move the selection in the list.
	_search->field()->rawTextEdit()->installEventFilter(this);

	addButton(tr::lng_close(tr::now), [this] { closeBox(); });
	setDimensions(st::boxWidth, st::boxListHeight);
}

void CountrySelectBox::setInnerFocus() {
	_search->setFocusFast();
}

bool CountrySelectBox::eventFilter(QObject *object, QEvent *event) {
	if (event->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(event)->key();
		if (key == Qt::Key_Down) {
			_list->selectSkip(1);
			return true;
		} else if (key == Qt::Key_Up) {
			_list->selectSkip(-1);
			return true;
		} else if (key == Qt::Key_PageDown) {
			_list->selectSkipPage(scrollArea()->height(), 1);
			return true;
		} else if (key == Qt::Key_PageUp) {
			_list->selectSkipPage(scrollArea()->height(), -1);
			return true;
		}
	}
	return BoxContent::eventFilter(object, event);
}

void CountrySelectBox::applyFilter(const QString &query) {
	const auto trimmed = Lang::Latinize(query.trimmed());
	if (trimmed.isEmpty()) {
		_rows.clear();
		for (const auto &country : Lang::Countries()) {
			_rows.push_back(&country);
		}
	} else {
		_rows = Lang::SearchCountries(trimmed);
	}
	_list->clearSelection();
	_list->refresh();
	auto active = -1;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i]->iso == _currentIso) {
			active = i;
			break;
		}
	}
	_list->setActiveRow(active);
	if (trimmed.isEmpty() && active >= 0) {
		// Open on the current country.
		const auto rect = _list->rowRect(active);
		scrollArea()->scrollToY(std::max(rect.top() - 2 * rect.height(), 0));
	} else {
		scrollArea()->scrollToY(0);
	}
	update();
}

void CountrySelectBox::choose(int index) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto country = *_rows[index];
	const auto chosen = _chosen;
	closeBox();
	if (chosen) {
		chosen(country);
	}
}

void CountrySelectBox::resizeEvent(QResizeEvent *e) {
	const auto &padding = st::boxSearchPadding;
	_search->resizeToWidth(width() - padding.left() - padding.right());
	_search->moveToLeft(padding.left(), padding.top());
	const auto top = padding.top() + _search->height() + padding.bottom();
	BoxContent::resizeEvent(e);
	scrollArea()->setGeometry(0, top, width(), std::max(height() - top, 0));
}

void CountrySelectBox::paintEvent(QPaintEvent *e) {
	BoxContent::paintEvent(e);
	if (!_rows.empty()) {
		return;
	}
	auto p = Painter(this);
	p.setFont(st::normalFont);
	p.setPen(st::boxListEmptyFg);
	const auto top = scrollArea()->y() + st::boxListEmptyTop;
	p.drawTextCentered(
		QRect(0, top, width(), st::normalFont->height),
		tr::lng_intro_country_not_found(tr::now));
}

int CountrySelectBox::rowsCount() const {
	return int(_rows.size());
}

int CountrySelectBox::rowHeight(int index, int width) const {
	Q_UNUSED(index);
	Q_UNUSED(width);
	return st::countryRowHeight;
}

void CountrySelectBox::paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) {
	Q_UNUSED(state);
	const auto &country = *_rows[index];
	const auto width = rect.width();
	const auto height = rect.height();
	const auto &padding = st::countryPadding;
	p.translate(rect.topLeft());

	// The flag (or the ISO code where the system has no flag glyphs).
	const auto flagRect = style::rtlrect(
		padding.left(),
		0,
		st::countryFlagWidth,
		height,
		width);
	if (_flags) {
		p.setFont(st::countryFlagFont);
		p.setPen(st::countryNameFg);
		p.drawText(flagRect, Qt::AlignCenter, Lang::FlagEmoji(country.iso));
	} else {
		const auto &font = st::countryIsoFont;
		const auto pill = QRect(
			flagRect.x(),
			(height - font->height - 6) / 2,
			flagRect.width(),
			font->height + 6);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::countryIsoBg);
			p.drawRoundedRect(pill, st::countryIsoRadius, st::countryIsoRadius);
		}
		p.setFont(font);
		p.setPen(st::countryIsoFg);
		p.drawText(pill, Qt::AlignCenter, country.iso);
	}

	// "+98" on the trailing side, always left-to-right.
	const auto code = QStringLiteral("+") + country.code;
	const auto codeWidth = st::countryCodeFont->width(code);
	p.setFont(st::countryCodeFont);
	p.setPen(st::countryCodeFg);
	p.setLayoutDirection(Qt::LeftToRight);
	p.drawTextRight(
		padding.right(),
		(height - st::countryCodeFont->height) / 2,
		width,
		code,
		codeWidth);
	p.setLayoutDirection(Ui::LayoutDirection());

	const auto nameLeft = padding.left() + st::countryFlagWidth + st::countrySkip;
	const auto nameWidth = width
		- nameLeft
		- padding.right()
		- codeWidth
		- st::countrySkip;
	const auto name = st::countryNameFont->elided(country.name(), nameWidth);
	p.setFont(st::countryNameFont);
	p.setPen(st::countryNameFg);
	p.drawTextLeft(
		nameLeft,
		(height - st::countryNameFont->height) / 2,
		width,
		name);
}

} // namespace Boxes
