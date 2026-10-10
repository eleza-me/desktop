#include "boxes/about_box.h"

#include "lang/lang_format.h"
#include "lang_auto.h"
#include "support/brand_mark.h"
#include "support/links.h"
#include "ui/painter.h"
#include "core/core_application.h"
#include "window/window_main_window.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "styles/style_boxes.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>

namespace Boxes {

AboutBox::AboutBox(QWidget *parent) : BoxContent(parent) {
}

QString AboutBox::versionText() const {
	return tr::lng_boxes_about_version(
		tr::now,
		lt_version,
		Lang::Num(QCoreApplication::applicationVersion()));
}

void AboutBox::prepare() {
	setTitle(tr::lng_settings_about_eleza(tr::now));
	_text = new Ui::FlatLabel(
		this,
		tr::lng_boxes_about_text(tr::now),
		st::aboutText);
	const auto add = [&](const QString &text, const char *url) {
		const auto link = new Ui::LinkButton(this, text);
		const auto address = QString::fromLatin1(url);
		link->setClickedCallback([address] {
			Support::OpenLinkExternally(address);
		});
		_links.push_back(link);
	};
	add(QStringLiteral("eleza.me"), "https://eleza.me");
	add(tr::lng_boxes_about_terms(tr::now), "https://eleza.me/terms");
	add(tr::lng_boxes_about_privacy(tr::now), "https://eleza.me/privacy");
	add(tr::lng_boxes_about_guidelines(tr::now), "https://eleza.me/guidelines");
	addButton(tr::lng_close(tr::now), [this] { closeBox(); });

	_text->resizeToWidth(st::boxWidth - 2 * st::boxRowPadding.left());
	setDimensions(st::boxWidth, contentHeight());
}

int AboutBox::contentHeight() const {
	auto result = st::aboutTopSkip
		+ st::aboutMarkSize.height()
		+ st::aboutMarkSkip
		+ st::aboutNameFont->height
		+ st::aboutVersionSkip
		+ st::aboutVersionFont->height
		+ st::aboutTextSkip
		+ _text->height()
		+ st::aboutLinksSkip;
	for (const auto link : _links) {
		result += link->height() + st::aboutLinkSkip;
	}
	return result + st::aboutBottomSkip;
}

QRect AboutBox::versionRect() const {
	const auto top = st::aboutTopSkip
		+ st::aboutMarkSize.height()
		+ st::aboutMarkSkip
		+ st::aboutNameFont->height
		+ st::aboutVersionSkip;
	const auto width = st::aboutVersionFont->width(versionText());
	return QRect(
		(this->width() - width) / 2,
		top,
		width,
		st::aboutVersionFont->height);
}

void AboutBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	const auto padding = st::boxRowPadding.left();
	const auto available = width() - 2 * padding;
	_text->resizeToWidth(available);
	_textTop = versionRect().bottom() + 1 + st::aboutTextSkip;
	_text->move(padding, _textTop);
	auto top = _textTop + _text->height() + st::aboutLinksSkip;
	for (const auto link : _links) {
		link->resizeToNaturalWidth(available);
		link->move((width() - link->width()) / 2, top);
		top += link->height() + st::aboutLinkSkip;
	}
}

void AboutBox::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	auto top = int(st::aboutTopSkip);
	Support::PaintBrandMark(
		p,
		QRect(
			(width() - st::aboutMarkSize.width()) / 2,
			top,
			st::aboutMarkSize.width(),
			st::aboutMarkSize.height()),
		st::brandPrimary->c,
		st::boxBg->c,
		st::brandPrimary->c);
	top += st::aboutMarkSize.height() + st::aboutMarkSkip;
	p.setFont(st::aboutNameFont);
	p.setPen(st::aboutNameFg);
	p.drawTextCentered(
		QRect(0, top, width(), st::aboutNameFont->height),
		tr::lng_app_name(tr::now));
	p.setFont(st::aboutVersionFont);
	p.setPen(st::aboutVersionFg);
	p.drawTextCentered(versionRect(), versionText());
}

void AboutBox::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton
		&& versionRect().adjusted(-8, -4, 8, 4).contains(e->pos())) {
		QGuiApplication::clipboard()->setText(
			QStringLiteral("Eleza Desktop ")
				+ QCoreApplication::applicationVersion());
		Core::App().window()->showToast(tr::lng_copied(tr::now));
	}
}

} // namespace Boxes
