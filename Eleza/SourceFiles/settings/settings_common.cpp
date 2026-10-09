#include "settings/settings_common.h"

#include "data/data_graph.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "settings/settings_logic.h"
#include "support/error_text.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/misc_widgets.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/top_bar.h"
#include "ui/ui_utility.h"
#include "window/window_routes.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

class DividerRow final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

protected:
	void paintEvent(QPaintEvent *e) override {
		Q_UNUSED(e);
		auto p = QPainter(this);
		p.fillRect(
			0,
			height() / 2,
			width(),
			st::lineWidth,
			st::settingsDividerFg);
	}
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return st::settingsDividerHeight;
	}

};

// Centers the column of rows and keeps it readable in a wide main column.
class ContentWrap final : public Ui::RpWidget {
public:
	ContentWrap(QWidget *parent, Ui::VerticalLayout *content)
	: RpWidget(parent)
	, _content(content) {
		_content->setParent(this);
		connect(_content, &Ui::RpWidget::heightChanged, this, [this] {
			if (!_inResize) {
				resizeToWidth(width());
			}
		});
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_inResize = true;
		const auto inner = std::min(newWidth, int(st::settingsContentWidth));
		_content->resizeToWidth(inner);
		_content->move((newWidth - inner) / 2, st::settingsPageTopSkip);
		_inResize = false;
		return st::settingsPageTopSkip
			+ _content->height()
			+ st::settingsPageBottomSkip;
	}
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		_content->setVisibleTopBottom(
			visibleTop - _content->y(),
			visibleBottom - _content->y());
	}

private:
	Ui::VerticalLayout *_content = nullptr;
	bool _inResize = false;

};

} // namespace

void AddSkip(Ui::VerticalLayout *container, int height) {
	container->add(Ui::object_ptr<Ui::FixedHeightWidget>(
		container,
		height ? height : int(st::settingsSideSkip)));
}

void AddDivider(Ui::VerticalLayout *container) {
	container->add(Ui::object_ptr<DividerRow>(container));
}

Ui::FlatLabel *AddSubsectionTitle(
		Ui::VerticalLayout *container,
		const QString &text) {
	return container->add(
		Ui::object_ptr<Ui::FlatLabel>(
			container,
			text,
			st::settingsSubsectionTitle),
		st::settingsSubsectionTitlePadding);
}

Ui::FlatLabel *AddFootnote(Ui::VerticalLayout *container, const QString &text) {
	return container->add(
		Ui::object_ptr<Ui::FlatLabel>(container, text, st::settingsFootnote),
		st::settingsFootnotePadding);
}

Ui::SettingsButton *AddButton(
		Ui::VerticalLayout *container,
		const QString &text,
		const style::icon *icon,
		std::function<void()> clicked) {
	const auto result = container->add(Ui::object_ptr<Ui::SettingsButton>(
		container,
		text,
		icon ? st::settingsButton : st::settingsButtonNoIcon));
	if (icon) {
		result->setIcon(icon);
	}
	if (clicked) {
		result->setClickedCallback(std::move(clicked));
	}
	return result;
}

Ui::SettingsButton *AddLinkButton(
		Ui::VerticalLayout *container,
		const QString &text,
		const style::icon *icon,
		std::function<void()> clicked) {
	const auto result = AddButton(container, text, icon, std::move(clicked));
	result->setChevron(true);
	return result;
}

Ui::SettingsButton *AddInfoRow(
		Ui::VerticalLayout *container,
		const QString &text,
		const QString &value,
		const style::icon *icon) {
	const auto result = AddButton(container, text, icon);
	result->setValueText(value);
	result->setAttribute(Qt::WA_TransparentForMouseEvents);
	result->setFocusPolicy(Qt::NoFocus);
	return result;
}

Ui::SettingsButton *AddToggle(
		Ui::VerticalLayout *container,
		const QString &text,
		bool checked,
		std::function<void(bool)> changed,
		const style::icon *icon) {
	const auto result = AddButton(container, text, icon);
	result->setToggle(checked);
	if (changed) {
		QObject::connect(
			result,
			&Ui::SettingsButton::toggledChanged,
			result,
			std::move(changed));
	}
	return result;
}

void ShowPickerBox(
		Window::SessionController *controller,
		const QString &title,
		const std::vector<Option> &options,
		const QString &current,
		std::function<void(const QString &value)> chosen) {
	controller->show(Ui::Box([=](Ui::GenericBox *box) {
		box->setTitle(title);
		auto selected = -1;
		for (auto i = 0, count = int(options.size()); i != count; ++i) {
			if (options[i].value == current) {
				selected = i;
			}
		}
		const auto group = (selected >= 0)
			? std::make_shared<Ui::RadiobuttonGroup>(selected)
			: std::make_shared<Ui::RadiobuttonGroup>();
		for (auto i = 0, count = int(options.size()); i != count; ++i) {
			box->addRow(
				Ui::object_ptr<Ui::Radiobutton>(
					box,
					group,
					i,
					options[i].label),
				st::boxCheckboxPadding);
		}
		group->setChangedCallback([=](int value) {
			if (value < 0 || value >= int(options.size())) {
				return;
			}
			const auto picked = options[value].value;
			// Let the radio animate, then close and save.
			Ui::CallDelayed(st::motionFast, box, [=] {
				box->closeBox();
				if (picked != current && chosen) {
					chosen(picked);
				}
			});
		});
		box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
	}));
}

// ---- Page

Page::Page(
	QWidget *parent,
	Window::SessionController *controller,
	Section section)
: SectionWidget(parent, controller)
, _section(section)
, _bar(new Ui::TopBar(this))
, _scroll(new Ui::ScrollArea(this)) {
	_bar->setTitle(SectionTitle(section));
	setTopBar(_bar);
	auto content = new Ui::VerticalLayout();
	_content = content;
	_wrap = _scroll->setOwnedWidget(
		Ui::object_ptr<ContentWrap>(_scroll, content));
	setFocusPolicy(Qt::StrongFocus);
}

QString Page::sectionKey() const {
	return Window::Routes::Settings(SectionId(_section));
}

void Page::setInnerFocus() {
	setFocus();
}

void Page::showLoading() {
	delete _error.data();
	if (!_skeleton) {
		_skeleton = new Ui::SkeletonList(this, Ui::SkeletonList::Kind::Lines);
		_skeleton->show();
	}
	_scroll->hide();
	updateGeometries();
}

void Page::showError(const Api::Error &error, std::function<void()> retry) {
	delete _skeleton.data();
	delete _error.data();
	auto descriptor = Ui::EmptyPlaceholderDescriptor();
	descriptor.icon = &st::emptyIconError;
	descriptor.title = tr::lng_error_load(tr::now);
	descriptor.text = Support::ErrorText(
		error,
		tr::lng_settings_check_connection_try_again(tr::now));
	descriptor.buttonText = tr::lng_retry(tr::now);
	descriptor.buttonCallback = std::move(retry);
	_error = new Ui::EmptyPlaceholder(this, std::move(descriptor));
	_error->show();
	_scroll->hide();
	updateGeometries();
}

void Page::showContent() {
	delete _skeleton.data();
	delete _error.data();
	_scroll->show();
	updateGeometries();
}

void Page::clearContent() {
	_content->clear();
}

void Page::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void Page::updateGeometries() {
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	const auto top = _bar->height();
	const auto rect = QRect(0, top, width(), std::max(height() - top, 0));
	_scroll->setGeometry(rect);
	if (_skeleton) {
		const auto inner = std::min(width(), int(st::settingsContentWidth));
		_skeleton->setGeometry(
			(width() - inner) / 2,
			top,
			inner,
			std::min(rect.height(), int(st::settingsLoadingHeight)));
	}
	if (_error) {
		_error->setGeometry(rect);
	}
}

// ---- SyncedPage

SyncedPage::SyncedPage(
	QWidget *parent,
	Window::SessionController *controller,
	Section section)
: Page(parent, controller, section) {
	connect(
		&controller->session().data().graph(),
		&Data::Graph::settingsChanged,
		this,
		[this] {
			if (_built) {
				for (const auto &refresh : _refreshers) {
					refresh();
				}
			}
		});
}

void SyncedPage::load() {
	auto &graph = controller()->session().data().graph();
	if (!graph.settingsLoaded()) {
		showLoading();
	}
	// The cached document shows at once; a fresh one follows.
	const auto finish = [this] {
		if (!_built) {
			_built = true;
			build();
			showContent();
		}
	};
	if (graph.settingsLoaded()) {
		finish();
	}
	graph.loadSettings(true, this, [=](const QJsonObject &) {
		finish();
	}, [=, this](const Api::Error &error) {
		if (!_built) {
			showError(error, [this] { load(); });
		}
	});
}

void SyncedPage::removeRow(QPointer<Ui::RpWidget> &row) {
	if (const auto widget = row.data()) {
		content()->removeChild(widget);
	}
}

void SyncedPage::loadInline() {
	auto &graph = controller()->session().data().graph();
	const auto finish = [this] {
		removeRow(_inlineState);
		removeRow(_inlineRetry);
		if (!_built) {
			_built = true;
			build();
		}
	};
	if (graph.settingsLoaded()) {
		finish();
	} else if (!_inlineState) {
		_inlineState = AddFootnote(content(), tr::lng_loading(tr::now));
	}
	graph.loadSettings(true, this, [=](const QJsonObject &) {
		finish();
	}, [=, this](const Api::Error &error) {
		if (_built) {
			return;
		}
		removeRow(_inlineState);
		removeRow(_inlineRetry);
		_inlineState = AddFootnote(content(), Support::ErrorText(
			error,
			tr::lng_settings_check_connection_try_again(tr::now)));
		_inlineRetry = AddButton(
			content(),
			tr::lng_try_again(tr::now),
			nullptr,
			[this] {
				removeRow(_inlineState);
				removeRow(_inlineRetry);
				loadInline();
			});
	});
}

QJsonValue SyncedPage::value(const QString &path) const {
	return controller()->session().data().graph().settingValue(path);
}

bool SyncedPage::boolean(const QString &path, bool fallback) const {
	const auto result = value(path);
	return result.isBool() ? result.toBool() : fallback;
}

QString SyncedPage::string(
		const QString &path,
		const QString &fallback) const {
	const auto result = value(path);
	return (result.isString() && !result.toString().isEmpty())
		? result.toString()
		: fallback;
}

void SyncedPage::save(const QString &path, const QJsonValue &value) {
	savePatch(Data::Graph::SettingPatch(path, value));
}

void SyncedPage::savePatch(const QJsonObject &patch) {
	const auto window = controller();
	controller()->session().data().graph().patchSettings(
		patch,
		this,
		nullptr,
		[=](const Api::Error &error) {
			Support::ShowError(
				window,
				error,
				tr::lng_settings_couldnt_save_try_again(tr::now));
		});
}

void SyncedPage::onRefresh(std::function<void()> refresh) {
	refresh();
	_refreshers.push_back(std::move(refresh));
}

Ui::SettingsButton *SyncedPage::addToggle(
		const QString &text,
		const QString &path,
		bool fallback) {
	const auto button = AddToggle(
		content(),
		text,
		boolean(path, fallback),
		[=, this](bool checked) { save(path, checked); });
	_refreshers.push_back([=, this] {
		const auto now = boolean(path, fallback);
		if (button->toggled() != now) {
			button->setToggle(now, true);
		}
	});
	return button;
}

Ui::SettingsButton *SyncedPage::addPicker(
		const QString &text,
		const QString &path,
		std::vector<Option> options,
		const QString &fallback) {
	const auto button = AddButton(content(), text);
	const auto labelOf = [=](const QString &value) {
		for (const auto &option : options) {
			if (option.value == value) {
				return option.label;
			}
		}
		return QString();
	};
	button->setClickedCallback([=, this] {
		ShowPickerBox(
			controller(),
			text,
			options,
			string(path, fallback),
			Guarded(this, [=, this](const QString &value) {
				save(path, value);
			}));
	});
	onRefresh([=, this] {
		button->setValueText(labelOf(string(path, fallback)));
	});
	return button;
}

} // namespace Settings
