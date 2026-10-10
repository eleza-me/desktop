#include "gallery/gallery_widget.h"

#include "core/core_application.h"
#include "gallery/gallery_pages.h"
#include "lang_auto.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tab_bar.h"
#include "ui/widgets/tooltip.h"
#include "ui/widgets/top_bar.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_main_window.h"
#include "styles/style_gallery.h"

#include <QtCore/QTimer>

namespace Gallery {
namespace {

[[nodiscard]] QString T(const char *fa, const char *en) {
	return QString::fromUtf8(Lang::IsPersian() ? fa : en);
}

} // namespace

QStringList Widget::PageIds() {
	return {
		QStringLiteral("buttons"),
		QStringLiteral("inputs"),
		QStringLiteral("text"),
		QStringLiteral("lists"),
		QStringLiteral("layers"),
		QStringLiteral("misc"),
	};
}

Widget::Widget(
	QWidget *parent,
	Window::MainWindow *window,
	const QString &page,
	const QString &overlay)
: RpWidget(parent)
, _window(window)
, _bar(new Ui::TopBar(this))
, _tabs(new Ui::TabBar(this))
, _scroll(new Ui::ScrollArea(this))
, _overlay(overlay) {
	_bar->setTitle(T("نمونهٔ رابط کاربری الزا", "Eleza UI gallery"));
	_bar->setSubtitle(T(
		"همهٔ ابزارک‌ها · فارسی / انگلیسی · روشن / تاریک",
		"Every widget · Persian / English · light / dark"));
	_language = _bar->addAction(st::galleryLanguageButton);
	_theme = _bar->addAction(st::galleryThemeButton);
	updateThemeButton();
	Ui::InstallTooltip(_language, [] {
		return Lang::NativeName(Lang::IsPersian()
			? Lang::Language::English
			: Lang::Language::Persian);
	});
	Ui::InstallTooltip(_theme, [] {
		return style::IsDark()
			? tr::lng_theme_light(tr::now)
			: tr::lng_theme_dark(tr::now);
	});
	_theme->setClickedCallback([this] {
		Core::App().toggleTheme();
		updateThemeButton();
	});
	_language->setClickedCallback([] {
		// The whole content (this widget too) is rebuilt for the new language.
		QTimer::singleShot(0, &Core::App(), [] {
			Core::App().setLanguage(Lang::IsPersian()
				? Lang::Language::English
				: Lang::Language::Persian);
		});
	});

	_tabs->setTabs({
		T("دکمه‌ها", "Buttons"),
		T("ورودی‌ها", "Inputs"),
		T("متن", "Text"),
		T("فهرست‌ها", "Lists"),
		T("لایه‌ها", "Layers"),
		T("متفرقه", "Misc"),
	});
	QObject::connect(_tabs, &Ui::TabBar::activeChanged, this, [this](int index) {
		_overlay.clear();
		showPage(index);
	});

	const auto index = std::max(int(PageIds().indexOf(page)), 0);
	_tabs->setActive(index, anim::type::instant);
	showPage(index);
}

void Widget::updateThemeButton() {
	_theme->setIconOverride(style::IsDark() ? &st::galleryThemeSunIcon : nullptr);
}

void Widget::showPage(int index) {
	_page = index;
	auto wrap = Ui::object_ptr<Ui::VerticalLayout>(_scroll);
	const auto content = wrap.data();
	const auto context = Context{ _window, _overlay };
	// The page is a centered column inside the scroll area.
	const auto column = content->add(
		Ui::object_ptr<Ui::VerticalLayout>(content),
		st::galleryPadding);
	switch (index) {
	case 0: FillButtonsPage(column, context); break;
	case 1: FillInputsPage(column, context); break;
	case 2: FillTextPage(column, context); break;
	case 3: FillListsPage(column, context); break;
	case 4: FillLayersPage(column, context); break;
	default: FillMiscPage(column, context); break;
	}
	_scroll->setOwnedWidget(std::move(wrap));
	_scroll->scrollToY(0);
	emit pageChanged(PageIds().value(index));
}

void Widget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	_bar->resizeToWidth(width());
	_bar->move(0, 0);
	_tabs->resizeToWidth(width());
	_tabs->move(0, _bar->height());
	const auto top = _bar->height() + _tabs->height();
	// Keep the content readable in wide windows.
	const auto contentWidth = std::min(width(), int(st::galleryMaxWidth));
	_scroll->setGeometry(
		(width() - contentWidth) / 2,
		top,
		contentWidth,
		std::max(height() - top, 0));
}

void Widget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.fillRect(rect(), st::bgCanvas);
}

} // namespace Gallery
