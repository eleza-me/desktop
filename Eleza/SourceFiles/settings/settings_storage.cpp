// «داده و حافظه»: how much the media cache holds, its limit and «پاک کردن».
#include "settings/settings_pages.h"

#include "core/core_application.h"
#include "data/data_media.h"
#include "data/data_session.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>

namespace Settings {
namespace {

// «۱۲۳ مگابایت از ۵۱۲ مگابایت» with a bar.
class UsageRow final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setValues(qint64 used, qint64 limit) {
		_used = used;
		_limit = limit;
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::settingsUsageHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		const auto &padding = st::settingsUsagePadding;
		const auto value = Lang::Bytes(_used);
		const auto valueWidth = st::settingsUsageValueFont->width(value);
		p.setFont(st::settingsUsageValueFont);
		p.setPen(st::settingsUsageValueFg);
		p.drawTextLeft(padding.left(), padding.top(), width(), value, valueWidth);
		p.setFont(st::settingsUsageLabelFont);
		p.setPen(st::settingsUsageLabelFg);
		p.drawTextLeft(
			padding.left() + valueWidth + st::spaceSm,
			padding.top()
				+ st::settingsUsageValueFont->ascent
				- st::settingsUsageLabelFont->ascent,
			width(),
			tr::lng_settings_storage_of(
				tr::now,
				lt_size,
				Lang::Bytes(_limit)));

		const auto bar = QRect(
			padding.left(),
			height() - padding.bottom() - st::settingsUsageBarHeight,
			width() - padding.left() - padding.right(),
			st::settingsUsageBarHeight);
		const auto radius = bar.height() / 2.;
		const auto part = (_limit > 0)
			? std::clamp(double(_used) / double(_limit), 0., 1.)
			: 0.;
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::settingsUsageBarBg);
		p.drawRoundedRect(bar, radius, radius);
		if (part > 0.) {
			const auto filled = std::max(
				int(std::round(bar.width() * part)),
				bar.height());
			p.setBrush(st::settingsUsageBarFg);
			p.drawRoundedRect(
				style::rtlrect(bar.x(), bar.y(), filled, bar.height(), width()),
				radius,
				radius);
		}
	}

private:
	qint64 _used = 0;
	qint64 _limit = 0;

};

// A slider over a few fixed steps. Sizes grow to the right in both languages.
class StepSlider final : public Ui::RpWidget {
public:
	StepSlider(
		QWidget *parent,
		QStringList labels,
		int index,
		std::function<void(int)> changed)
	: RpWidget(parent)
	, _labels(std::move(labels))
	, _index(std::clamp(index, 0, std::max(int(_labels.size()) - 1, 0)))
	, _changed(std::move(changed)) {
		setFocusPolicy(Qt::StrongFocus);
		setCursor(Qt::PointingHandCursor);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::settingsSliderHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto line = lineRect();
		const auto radius = line.height() / 2.;
		const auto x = knobX(_index);
		p.setPen(Qt::NoPen);
		p.setBrush(st::settingsSliderBg);
		p.drawRoundedRect(line, radius, radius);
		p.setBrush(st::settingsSliderFg);
		p.drawRoundedRect(
			QRectF(line.x(), line.y(), x - line.x(), line.height()),
			radius,
			radius);
		const auto knob = double(st::settingsSliderKnob);
		p.drawEllipse(QRectF(
			x - knob / 2.,
			line.center().y() + 0.5 - knob / 2.,
			knob,
			knob));
		p.setFont(st::settingsSliderTickFont);
		const auto count = int(_labels.size());
		for (auto i = 0; i != count; ++i) {
			const auto width = st::settingsSliderTickFont->width(_labels[i]);
			const auto left = std::clamp(
				knobX(i) - width / 2,
				0,
				this->width() - width);
			p.setPen((i == _index)
				? st::settingsSliderFg
				: st::settingsSliderTickFg);
			p.drawText(
				left,
				line.bottom()
					+ st::spaceMd
					+ st::settingsSliderTickFont->ascent,
				_labels[i]);
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_pressed = true;
			setFromX(e->pos().x());
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (_pressed) {
			setFromX(e->pos().x());
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_pressed = false;
		}
	}
	void keyPressEvent(QKeyEvent *e) override {
		if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Down) {
			setIndex(_index - 1);
		} else if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Up) {
			setIndex(_index + 1);
		} else {
			RpWidget::keyPressEvent(e);
		}
	}

private:
	[[nodiscard]] QRect lineRect() const {
		const auto &padding = st::settingsSliderPadding;
		const auto knob = int(st::settingsSliderKnob);
		return QRect(
			padding.left() + knob / 2,
			st::spaceMd + (knob - st::settingsSliderLine) / 2,
			width() - padding.left() - padding.right() - knob,
			st::settingsSliderLine);
	}
	[[nodiscard]] int knobX(int index) const {
		const auto line = lineRect();
		const auto steps = std::max(int(_labels.size()) - 1, 1);
		return line.x() + (line.width() * index) / steps;
	}
	void setFromX(int x) {
		const auto line = lineRect();
		const auto steps = std::max(int(_labels.size()) - 1, 1);
		const auto ratio = std::clamp(
			double(x - line.x()) / std::max(line.width(), 1),
			0.,
			1.);
		setIndex(int(std::round(ratio * steps)));
	}
	void setIndex(int index) {
		index = std::clamp(index, 0, std::max(int(_labels.size()) - 1, 0));
		if (_index != index) {
			_index = index;
			update();
			if (_changed) {
				_changed(index);
			}
		}
	}

	QStringList _labels;
	int _index = 0;
	bool _pressed = false;
	std::function<void(int)> _changed;

};

class StoragePage final : public Page {
public:
	StoragePage(QWidget *parent, Window::SessionController *controller)
	: Page(parent, controller, Section::Storage) {
		const auto window = controller;
		const auto media = &controller->session().data().media();
		const auto steps = CacheLimitSteps();

		AddSubsectionTitle(content(), tr::lng_settings_storage_cache(tr::now));
		const auto usage = content()->add(
			Ui::object_ptr<UsageRow>(content()));
		const auto refresh = [=] {
			usage->setValues(media->diskUsage(), media->diskLimit());
		};
		refresh();
		connect(media, &Data::Media::diskUsageChanged, usage, refresh);

		AddDivider(content());
		AddSubsectionTitle(content(), tr::lng_settings_storage_limit(tr::now));
		auto labels = QStringList();
		for (const auto step : steps) {
			labels.push_back(Lang::Bytes(step));
		}
		content()->add(Ui::object_ptr<StepSlider>(
			content(),
			labels,
			CacheLimitIndex(media->diskLimit()),
			[=](int index) {
				const auto bytes = steps[index];
				Core::App().settings().setMediaCacheLimit(bytes);
				media->setDiskLimit(bytes);
				refresh();
			}));
		AddFootnote(content(), tr::lng_settings_storage_limit_about(tr::now));

		AddDivider(content());
		const auto clear = AddButton(
			content(),
			tr::lng_settings_storage_clear(tr::now),
			&st::settingsIconDelete,
			[=] {
				auto args = Ui::ConfirmBoxArgs();
				args.title = tr::lng_settings_storage_clear(tr::now);
				args.text = tr::lng_settings_storage_clear_sure(tr::now);
				args.confirmText = tr::lng_settings_storage_clear_confirm(tr::now);
				args.destructive = true;
				args.confirmed = [=](std::function<void()> close) {
					close();
					window->session().data().media().clearDisk();
					window->showToast(
						tr::lng_settings_storage_cleared(tr::now));
				};
				window->show(Ui::MakeConfirmBox(std::move(args)));
			});
		clear->setDestructive(true);
		AddFootnote(content(), tr::lng_settings_storage_clear_about(tr::now));
		showContent();
	}

};

} // namespace

Window::SectionFactory StoragePageFactory() {
	return [](QWidget *parent, Window::SessionController *controller) {
		return Ui::Owned<Window::SectionWidget>(
			new StoragePage(parent, controller));
	};
}

} // namespace Settings
