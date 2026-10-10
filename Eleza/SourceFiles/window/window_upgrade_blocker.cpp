#include "window/window_upgrade_blocker.h"

#include "core/core_application.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "support/brand_mark.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "updater/updater_checker.h"
#include "styles/style_window.h"
#include "styles/style_window_update.h"

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
, _status(st::updateBlockerStatusStyle, QString())
, _update(new Ui::RoundButton(
	this,
	tr::lng_app_update(tr::now),
	st::bigRoundButton))
, _quit(new Ui::LinkButton(this, tr::lng_tray_quit(tr::now))) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusPolicy(Qt::StrongFocus);
	_update->setClickedCallback([this] {
		const auto updater = Core::App().updater();
		if (updater && updater->state() == Updater::State::Ready) {
			Core::App().restartToUpdate();
		} else {
			openPage();
		}
	});
	_quit->setClickedCallback([] { Core::App().quit(); });

	if (const auto updater = Core::App().updater()) {
		QObject::connect(
			updater,
			&Updater::Checker::stateChanged,
			this,
			[this] { refresh(); });
		QObject::connect(
			updater,
			&Updater::Checker::progressChanged,
			this,
			[this] { refresh(); });
		// A screenshot run stays off the network.
		if (!Core::App().commandLine().ephemeral()) {
			updater->setRequired(true);
		}
	}
	refresh();
}

void UpgradeBlocker::openPage() {
	const auto url = QUrl(_url);
	// Only a web page may come from the configuration.
	if (url.scheme() == QStringLiteral("https")
		|| url.scheme() == QStringLiteral("http")) {
		QDesktopServices::openUrl(url);
	}
}

void UpgradeBlocker::refresh() {
	const auto updater = Core::App().updater();
	const auto state = updater ? updater->state() : Updater::State::Idle;
	const auto self = updater && updater->selfUpdate();
	auto status = QString();
	auto busy = false;
	switch (state) {
	case Updater::State::Checking:
		busy = self;
		status = self ? tr::lng_update_checking(tr::now) : QString();
		break;
	case Updater::State::Downloading: {
		busy = true;
		const auto total = updater->total();
		const auto percent = (total > 0)
			? int(std::clamp(updater->received() * 100 / total, qint64(0), qint64(100)))
			: 0;
		status = tr::lng_update_blocker_progress(
			tr::now,
			lt_percent,
			tr::lng_update_percent(tr::now, lt_value, Lang::Num(percent)));
	} break;
	case Updater::State::Preparing:
		busy = true;
		status = tr::lng_update_preparing(tr::now);
		break;
	case Updater::State::Ready:
		status = tr::lng_update_blocker_ready(tr::now);
		break;
	case Updater::State::Available:
		// Announced only: this copy is updated from somewhere else.
		status = self ? QString() : tr::lng_update_manual_about(tr::now);
		break;
	case Updater::State::Failed:
		status = tr::lng_update_blocker_manual(tr::now);
		break;
	case Updater::State::Idle:
	case Updater::State::UpToDate:
		break;
	}
	_update->setLoading(busy);
	_status.setText(st::updateBlockerStatusStyle, status);
	updateGeometries();
	update();
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
		+ st::updateBlockerStatusSkip
		+ st::updateBlockerStatusStyle.lineHeight * 2
		+ st::upgradeQuitSkip
		+ _quit->height();
}

void UpgradeBlocker::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	updateGeometries();
}

void UpgradeBlocker::updateGeometries() {
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
	// The status line has a place of two lines whatever it says: the
	// screen does not jump while the download goes on.
	_statusTop = top + _update->height() + st::updateBlockerStatusSkip;
	top = _statusTop
		+ st::updateBlockerStatusStyle.lineHeight * 2
		+ st::upgradeQuitSkip;
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
	if (!_status.isEmpty()) {
		p.setPen(st::updateBlockerStatusFg);
		_status.draw(p, left, _statusTop, available, style::al_top);
	}
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
