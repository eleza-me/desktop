#include "ui/image/userpic.h"

#include "ui/image/image_prepare.h"
#include "ui/painter.h"

#include <QtCore/QTextBoundaryFinder>

#include <algorithm>

namespace Ui {

int UserpicColorIndex(QStringView id) {
	// FNV-1a over the UTF-16 units: the same everywhere, unlike qHash.
	auto hash = quint32(2166136261U);
	for (const auto ch : id) {
		hash ^= ch.unicode();
		hash *= 16777619U;
	}
	return int(hash % 8U);
}

style::color UserpicColor(int index) {
	switch (((index % 8) + 8) % 8) {
	case 0: return st::userpicBg1;
	case 1: return st::userpicBg2;
	case 2: return st::userpicBg3;
	case 3: return st::userpicBg4;
	case 4: return st::userpicBg5;
	case 5: return st::userpicBg6;
	case 6: return st::userpicBg7;
	}
	return st::userpicBg8;
}

QString UserpicInitial(const QString &name) {
	const auto trimmed = name.trimmed();
	if (trimmed.isEmpty()) {
		return QString(QChar(0x061F)); // Arabic question mark
	}
	auto finder = QTextBoundaryFinder(
		QTextBoundaryFinder::Grapheme,
		trimmed);
	const auto next = finder.toNextBoundary();
	const auto first = trimmed.left((next > 0) ? next : 1);
	return first.toUpper();
}

QImage PrepareUserpic(const QImage &image, int size, double ratio) {
	if (image.isNull()) {
		return QImage();
	}
	return Images::Circle(
		Images::PrepareCover(image, QSize(size, size), ratio));
}

void PaintUserpic(QPainter &p, const QRect &rect, const QImage &image) {
	if (image.isNull()) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	const auto wanted = QSize(
		int(rect.width() * ratio),
		int(rect.height() * ratio));
	auto hq = PainterHighQualityEnabler(p);
	if (image.size() == wanted) {
		// Already prepared with PrepareUserpic.
		p.drawImage(rect, image);
		return;
	}
	auto path = QPainterPath();
	path.addEllipse(rect);
	p.save();
	p.setClipPath(path);
	const auto scaled = image.size().scaled(
		rect.size(),
		Qt::KeepAspectRatioByExpanding);
	p.drawImage(
		QRect(
			rect.x() + (rect.width() - scaled.width()) / 2,
			rect.y() + (rect.height() - scaled.height()) / 2,
			scaled.width(),
			scaled.height()),
		image);
	p.restore();
}

void PaintUserpicPlaceholder(
		QPainter &p,
		const QRect &rect,
		QStringView id,
		const QString &name,
		UserpicKind kind) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	switch (kind) {
	case UserpicKind::User: p.setBrush(UserpicColor(UserpicColorIndex(id))); break;
	case UserpicKind::Anonymous: p.setBrush(st::anonSurface); break;
	case UserpicKind::Saved: p.setBrush(st::brandPrimary); break;
	case UserpicKind::Deleted: p.setBrush(st::borderStrong); break;
	}
	p.drawEllipse(rect);
	const auto iconSize = std::max(rect.width() / 2, 12);
	const auto iconRect = QRect(
		rect.x() + (rect.width() - iconSize) / 2,
		rect.y() + (rect.height() - iconSize) / 2,
		iconSize,
		iconSize);
	const auto ratio = p.device()->devicePixelRatioF();
	if (kind == UserpicKind::Anonymous) {
		p.drawPixmap(
			iconRect.topLeft(),
			style::IconPixmap(
				QStringLiteral("anonymous"),
				st::anonContent->c,
				iconSize,
				ratio));
		return;
	} else if (kind == UserpicKind::Saved) {
		p.drawPixmap(
			iconRect.topLeft(),
			style::IconPixmap(
				QStringLiteral("bookmark"),
				st::textOnBrand->c,
				iconSize,
				ratio));
		return;
	} else if (kind == UserpicKind::Deleted) {
		p.drawPixmap(
			iconRect.topLeft(),
			style::IconPixmap(
				QStringLiteral("me"),
				st::textSecondary->c,
				iconSize,
				ratio));
		return;
	}
	const auto initial = UserpicInitial(name);
	// The letter is ~40% of the diameter, like the design's avatar sizes.
	const auto font = style::font(
		std::max(int(rect.height() * 0.4), 9),
		style::internal::FontBold,
		QString());
	p.setFont(font);
	p.setPen(st::staticWhite);
	const auto width = font->width(initial);
	p.drawText(
		rect.x() + (rect.width() - width) / 2,
		rect.y() + (rect.height() - font->height) / 2 + font->ascent,
		initial);
}

void PaintOnlineDot(
		QPainter &p,
		const QRect &userpicRect,
		style::color border) {
	auto hq = PainterHighQualityEnabler(p);
	const auto size = std::clamp(userpicRect.width() / 4, 8, 14);
	const auto stroke = 2.;
	const auto x = style::RightToLeft()
		? userpicRect.x()
		: (userpicRect.x() + userpicRect.width() - size);
	const auto y = userpicRect.y() + userpicRect.height() - size;
	p.setPen(QPen(border->c, stroke));
	p.setBrush(st::brandPrimary);
	p.drawEllipse(QRectF(x, y, size, size).marginsRemoved(
		QMarginsF(-stroke / 2., -stroke / 2., -stroke / 2., -stroke / 2.)
	).translated(style::RightToLeft() ? -1. : 1., 1.));
}

UserpicWidget::UserpicWidget(QWidget *parent, int size)
: AbstractButton(parent)
, _size(size)
, _border(st::bgCanvas) {
	resize(size, size);
	setPointerCursor(false);
}

void UserpicWidget::setPlaceholder(
		const QString &id,
		const QString &name,
		UserpicKind kind) {
	_id = id;
	_name = name;
	_kind = kind;
	update();
}

void UserpicWidget::setImage(const QImage &image) {
	_original = image;
	_prepared = QImage();
	update();
}

void UserpicWidget::setSource(std::shared_ptr<ImageSource> source, bool keep) {
	if (_sourceConnection) {
		QObject::disconnect(_sourceConnection);
	}
	_source = std::move(source);
	if (!keep || !_source) {
		_original = QImage();
		_prepared = QImage();
	}
	if (_source) {
		_sourceConnection = QObject::connect(
			_source.get(),
			&ImageSource::updated,
			this,
			[this] { refreshFromSource(); });
		refreshFromSource();
	}
	update();
}

void UserpicWidget::refreshFromSource() {
	if (!_source) {
		return;
	}
	const auto ratio = devicePixelRatioF();
	const auto image = _source->request(QSize(_size, _size) * ratio);
	if (!image.isNull()) {
		_original = image;
		_prepared = QImage();
		update();
	}
}

void UserpicWidget::setOnline(bool online) {
	if (_online != online) {
		_online = online;
		update();
	}
}

void UserpicWidget::setBorderColor(style::color color) {
	_border = color;
	update();
}

void UserpicWidget::setSize(int size) {
	_size = size;
	_prepared = QImage();
	resize(size, size);
	if (_source) {
		refreshFromSource();
	}
	update();
}

void UserpicWidget::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	const auto rect = QRect(0, 0, _size, _size);
	if (!_original.isNull()) {
		const auto ratio = devicePixelRatioF();
		if (_prepared.isNull() || _preparedRatio != ratio) {
			_prepared = PrepareUserpic(_original, _size, ratio);
			_preparedRatio = ratio;
		}
		p.drawImage(rect, _prepared);
	} else {
		PaintUserpicPlaceholder(p, rect, _id, _name, _kind);
	}
	if (_online) {
		PaintOnlineDot(p, rect, _border);
	}
}

} // namespace Ui
