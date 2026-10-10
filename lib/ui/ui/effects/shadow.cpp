#include "ui/effects/shadow.h"

#include "ui/image/image_prepare.h"

#include <QtCore/QHash>
#include <QtGui/QPainter>

#include <algorithm>
#include <array>
#include <cmath>

namespace Ui {
namespace Shadow {
namespace {

struct Patch {
	QImage image; // nine-patch source
	int corner = 0; // logical size of a corner cell
	int version = 0;
};

[[nodiscard]] QHash<quint64, Patch> &Cache() {
	static auto result = QHash<quint64, Patch>();
	return result;
}

[[nodiscard]] const Patch &Prepare(
		int radius,
		const style::Shadow &st,
		double ratio) {
	const auto key = (quint64(st.color->c.rgba()) << 32)
		| (quint64(radius & 0xFF) << 24)
		| (quint64(st.blur & 0xFF) << 16)
		| (quint64(qRound(ratio * 16.) & 0xFF) << 8);
	auto &cache = Cache();
	const auto i = cache.find(key);
	if (i != cache.end() && i->version == style::PaletteVersion()) {
		return *i;
	}
	// A cell must contain the whole blur falloff plus the corner arc.
	const auto corner = radius + st.blur * 2 + 2;
	const auto side = corner * 2 + 1;
	const auto pad = st.blur * 2 + 2;
	auto image = QImage(
		QSize(side + pad * 2, side + pad * 2) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(st.color->c);
		p.drawRoundedRect(QRectF(pad, pad, side, side), radius, radius);
	}
	image = Images::Blur(std::move(image), int(std::lround(st.blur * ratio)));
	image.setDevicePixelRatio(ratio);

	auto patch = Patch();
	patch.image = std::move(image);
	patch.corner = corner + pad;
	patch.version = style::PaletteVersion();
	return *cache.insert(key, patch);
}

} // namespace

void Paint(
		QPainter &p,
		const QRect &body,
		int radius,
		const style::Shadow &st,
		double opacity) {
	if (body.isEmpty() || opacity <= 0.) {
		return;
	}
	const auto ratio = p.device()->devicePixelRatioF();
	const auto &patch = Prepare(radius, st, ratio);
	const auto pad = st.blur * 2 + 2;
	const auto cell = patch.corner; // logical
	const auto total = int(patch.image.width() / ratio);
	const auto middle = total - cell * 2; // 1 logical px

	// The outer rect the nine-patch covers.
	const auto outer = body.marginsAdded(QMargins(pad, pad, pad, pad)).translated(
		st.offset);
	// If the body is smaller than two corner cells, shrink the cells.
	const auto cw = std::min(cell, outer.width() / 2);
	const auto ch = std::min(cell, outer.height() / 2);

	const auto source = [&](int x, int y, int w, int h) {
		return QRectF(x * ratio, y * ratio, w * ratio, h * ratio);
	};
	const auto xs = std::array<int, 3>{ 0, cell, cell + middle };
	const auto ws = std::array<int, 3>{ cell, middle, cell };
	const auto dxs = std::array<int, 3>{
		outer.x(),
		outer.x() + cw,
		outer.x() + outer.width() - cw,
	};
	const auto dws = std::array<int, 3>{ cw, outer.width() - 2 * cw, cw };
	const auto dys = std::array<int, 3>{
		outer.y(),
		outer.y() + ch,
		outer.y() + outer.height() - ch,
	};
	const auto dhs = std::array<int, 3>{ ch, outer.height() - 2 * ch, ch };

	const auto was = p.opacity();
	p.setOpacity(was * opacity);
	p.save();
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	for (auto row = 0; row != 3; ++row) {
		for (auto column = 0; column != 3; ++column) {
			if (row == 1 && column == 1) {
				continue; // hidden behind the body
			} else if (dws[column] <= 0 || dhs[row] <= 0) {
				continue;
			}
			auto from = source(xs[column], xs[row], ws[column], ws[row]);
			if (column != 1 && cw < cell) {
				// Keep the outer part of a shrunk corner.
				const auto cut = (cell - cw) * ratio;
				if (column == 0) {
					from.setWidth(from.width() - cut);
				} else {
					from.setLeft(from.left() + cut);
				}
			}
			if (row != 1 && ch < cell) {
				const auto cut = (cell - ch) * ratio;
				if (row == 0) {
					from.setHeight(from.height() - cut);
				} else {
					from.setTop(from.top() + cut);
				}
			}
			p.drawImage(
				QRectF(dxs[column], dys[row], dws[column], dhs[row]),
				patch.image,
				from);
		}
	}
	p.restore();
	p.setOpacity(was);
}

void ClearCache() {
	Cache().clear();
}

} // namespace Shadow

PlainShadow::PlainShadow(QWidget *parent)
: PlainShadow(parent, st::chromeHairline) {
}

PlainShadow::PlainShadow(QWidget *parent, style::color color)
: RpWidget(parent)
, _color(color) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	resize(st::lineWidth, st::lineWidth);
}

void PlainShadow::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = QPainter(this);
	p.fillRect(rect(), _color);
}

} // namespace Ui
