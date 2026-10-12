#include "history/history_mentions.h"

#include "data/data_json.h"
#include "lang/lang_format.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "styles/palette.h"
#include "styles/style_history.h"

#include <QtGui/QMouseEvent>

namespace History {

MentionsDropdown::MentionsDropdown(QWidget *parent, Main::Session *session)
: RpWidget(parent)
, _userpics(session, [this](const QString &) { update(); }) {
	setMouseTracking(true);
	setCursor(style::cur_pointer);
	// The caret stays in the field.
	setFocusPolicy(Qt::NoFocus);
	hide();
}

void MentionsDropdown::showFor(const QVector<Data::User> &users) {
	_users = users.mid(0, kMaxRows);
	_selected = 0;
	_pressed = -1;
	if (_users.isEmpty()) {
		hide();
		return;
	}
	placeAbove(_anchor);
	show();
	raise();
	update();
}

void MentionsDropdown::hideFast() {
	_users.clear();
	hide();
}

void MentionsDropdown::selectSkip(int direction) {
	const auto count = int(_users.size());
	if (count <= 0) {
		return;
	}
	_selected = ((_selected + direction) % count + count) % count;
	update();
}

std::optional<Data::User> MentionsDropdown::selected() const {
	return (_selected >= 0 && _selected < _users.size())
		? std::make_optional(_users[_selected])
		: std::nullopt;
}

void MentionsDropdown::placeAbove(const QRect &anchor) {
	_anchor = anchor;
	if (_users.isEmpty() || anchor.isEmpty()) {
		return;
	}
	const auto &padding = st::historyMentionsPadding;
	const auto height = padding.top()
		+ int(_users.size()) * st::historyMentionRowHeight
		+ padding.bottom();
	const auto width = std::min(anchor.width(), int(st::historyMentionsMaxWidth));
	// At the leading edge of the field, like the text it completes.
	const auto left = style::RightToLeft()
		? (anchor.x() + anchor.width() - width)
		: anchor.x();
	setGeometry(
		left,
		std::max(anchor.y() - height - st::historyMentionsSkip, 0),
		width,
		height);
}

int MentionsDropdown::rowAt(QPoint position) const {
	const auto top = position.y() - st::historyMentionsPadding.top();
	if (top < 0 || position.x() < 0 || position.x() >= width()) {
		return -1;
	}
	const auto index = top / st::historyMentionRowHeight;
	return (index < _users.size()) ? index : -1;
}

void MentionsDropdown::paintEvent(QPaintEvent *e) {
	Q_UNUSED(e);
	auto p = Painter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(QPen(st::historyComposeLineFg->c, 1.));
		p.setBrush(st::historyComposeBg);
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
			st::historyMentionsRadius,
			st::historyMentionsRadius);
	}
	const auto outer = width();
	const auto size = int(st::historyMentionUserpic);
	const auto rowHeight = int(st::historyMentionRowHeight);
	const auto &padding = st::historyMentionsPadding;
	auto top = padding.top();
	for (auto i = 0; i != int(_users.size()); ++i) {
		const auto &user = _users[i];
		if (i == _selected) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::historyMentionSelectedBg);
			p.drawRoundedRect(
				QRect(padding.left(), top, outer - padding.left() - padding.right(), rowHeight),
				st::historyMentionsRadius - 4,
				st::historyMentionsRadius - 4);
		}
		const auto left = padding.left() + st::historyMentionRowSkip;
		_userpics.paint(
			p,
			style::rtlrect(left, top + (rowHeight - size) / 2, size, size, outer),
			user);
		const auto textLeft = left + size + st::historyMentionRowSkip;
		auto available = outer - textLeft - padding.right() - st::historyMentionRowSkip;
		const auto handle = user.handle.isEmpty()
			? QString()
			: Lang::Handle(user.handle);
		const auto &handleFont = st::historyMentionHandleFont;
		const auto handleWidth = handle.isEmpty() ? 0 : handleFont->width(handle);
		const auto withHandle = (handleWidth > 0) && (handleWidth < available / 2);
		if (withHandle) {
			available -= handleWidth + st::historyMentionRowSkip;
		}
		const auto &nameFont = st::historyMentionNameFont;
		const auto name = nameFont->elided(
			user.displayName.trimmed().isEmpty()
				? (handle.isEmpty() ? tr::lng_groups_someone(tr::now) : handle)
				: Data::Json::CleanName(user.displayName.simplified()),
			std::max(available, 1));
		const auto nameWidth = nameFont->width(name);
		p.setFont(nameFont);
		p.setPen(st::textPrimary);
		p.drawTextLeft(
			textLeft,
			top + (rowHeight - nameFont->height) / 2,
			outer,
			name,
			nameWidth);
		if (withHandle && !user.displayName.trimmed().isEmpty()) {
			p.setFont(handleFont);
			p.setPen(st::textTertiary);
			p.drawTextLeft(
				textLeft + nameWidth + st::historyMentionRowSkip,
				top + (rowHeight - handleFont->height) / 2,
				outer,
				handle,
				handleWidth);
		}
		top += rowHeight;
	}
}

void MentionsDropdown::mouseMoveEvent(QMouseEvent *e) {
	const auto index = rowAt(e->pos());
	if (index >= 0 && index != _selected) {
		_selected = index;
		update();
	}
}

void MentionsDropdown::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) ? rowAt(e->pos()) : -1;
}

void MentionsDropdown::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	const auto index = rowAt(e->pos());
	if (e->button() == Qt::LeftButton
		&& index >= 0
		&& index == pressed
		&& _chosen) {
		// A copy: the callback hides the list.
		const auto user = _users[index];
		_chosen(user);
	}
}

void MentionsDropdown::leaveEvent(QEvent *e) {
	Q_UNUSED(e);
	_pressed = -1;
}

} // namespace History
