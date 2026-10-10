#include "window/window_upgrade_blocker.h"

#include "core/core_application.h"
#include "lang_auto.h"
#include "support/brand_mark.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "styles/style_window.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeyEvent>

namespace Window {
namespace {

const auto kDefaultUpdateUrl = QStringLiteral("https://eleza.me");

} // namespace

UpgradeBlocker::UpgradeBlocker(QWidget *parent, const QString &updateUrl)
: RpWidget(parent)
, _url(updateUrl.isEmpty() ? kDefaultUpdateUrl : updateUrl)
, _title(st::upgradeTitleStyle, tr::lng_window_upgrade_title(tr::now))
, _text(st::upgradeTextStyle, tr::lng_window_upgrade_text(tr::now))
, _update(new Ui::RoundButton(
	this,
	tr::lng_app_update(tr::now),
	st::bigRoundButton))
, _quit(new Ui::LinkButton(this, tr::lng_tray_quit(tr::now))) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);
	_update->setClickedCallback([this] {
		const auto url = QUrl(_url);
		// Only a web page may come from the configuration.
		if (url.scheme() == QStringLiteral("https")
			|| url.scheme() == QStringLiteral("http")) {
			QDesktopServices::openUrl(url);
		}
	});
	_quit->setClickedCallback([] { Core::App().quit(); });
}

int UpgradeBlocker::textWidth() const {
	return std::max(
		std::min(width() - 2 * int(st::upgradePadding), int(st::upgradeWidth)),
		1);
}

int UpgradeBlocker::contentHeight(int textWidth) const {
	return st::upgradeMarkSize.height()
		+ st::upgradeMarkSkip
		+ _title.countHeight(textWidth)
		+ st::upgradeTitleSkip
		+ _text.countHeight(textWidth)
		+ st::upgradeButtonSkip
		+ _update->height()
		+ st::upgradeQuitSkip
		+ _quit->height();
}

void UpgradeBlocker::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const auto available = textWidth();
	_update->resizeToNaturalWidth(available);
	_quit->resizeToNaturalWidth(available);
	_contentTop = std::max((height() - contentHeight(available)) / 2, 0);
	auto top = _contentTop
		+ st::upgradeMarkSize.height()
		+ st::upgradeMarkSkip
		+ _title.countHeight(available)
		+ st::upgradeTitleSkip
		+ _text.countHeight(available)
		+ st::upgradeButtonSkip;
	_update->move((width() - _update->width()) / 2, top);
	top += _update->height() + st::upgradeQuitSkip;
	_quit->move((width() - _quit->width()) / 2, top);
}

void UpgradeBlocker::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	p.fillRect(rect(), st::bgCanvas);

	const auto available = textWidth();
	const auto left = (width() - available) / 2;
	auto top = _contentTop;
	Support::PaintBrandMark(
		p,
		QRect(
			(width() - st::upgradeMarkSize.width()) / 2,
			top,
			st::upgradeMarkSize.width(),
			st::upgradeMarkSize.height()),
		st::brandPrimary->c,
		st::bgCanvas->c,
		st::brandPrimary->c);
	top += st::upgradeMarkSize.height() + st::upgradeMarkSkip;
	p.setPen(st::textPrimary);
	_title.draw(p, left, top, available, style::al_top);
	top += _title.countHeight(available) + st::upgradeTitleSkip;
	p.setPen(st::textSecondary);
	_text.draw(p, left, top, available, style::al_top);
}

void UpgradeBlocker::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
		_update->click();
	}
	// Everything else stops here: nothing under the blocker may react.
	e->accept();
}

void UpgradeBlocker::mousePressEvent(QMouseEvent *e) {
	e->accept();
}

} // namespace Window
