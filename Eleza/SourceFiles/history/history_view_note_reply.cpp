#include "history/history_view_note_reply.h"

#include "history/history_themes.h"
#include "lang_auto.h"
#include "ui/painter.h"
#include "styles/style_history.h"

#include <algorithm>

namespace History::View {

std::unique_ptr<NoteReplyView> NoteReplyView::Create(
		const Data::Message &message) {
	if (message.kind != Data::MessageKind::Text
		|| message.forwarded
		|| message.underReview) {
		return nullptr;
	}
	auto reply = Data::NoteReply::Parse(message.text);
	if (!reply) {
		return nullptr;
	}
	return std::unique_ptr<NoteReplyView>(
		new NoteReplyView(message, std::move(*reply)));
}

NoteReplyView::NoteReplyView(
	const Data::Message &message,
	Data::NoteReply reply)
: _shown(message)
, _caption(message.mine
	? tr::lng_notes_replied_mine(tr::now)
	: tr::lng_notes_replied(tr::now))
, _outgoing(message.mine) {
	_shown.entities = reply.replyEntities(message.entities);
	_shown.text = reply.reply;
	_note.setText(st::historyNoteReplyTextStyle, reply.note);
}

int NoteReplyView::layout(int edge, int top, int maxWidth, bool outgoing) {
	_outgoing = outgoing;
	const auto &font = st::historyNoteReplyCaptionFont;
	const auto &captionPadding = st::historyNoteReplyCaptionPadding;
	const auto captionWidth = std::min(
		font->width(_caption) + captionPadding.left() + captionPadding.right(),
		maxWidth);
	const auto captionHeight = font->height
		+ captionPadding.top()
		+ captionPadding.bottom();
	const auto &padding = st::historyNoteReplyPadding;
	const auto noteMax = std::min(maxWidth, int(st::historyNoteReplyMaxWidth));
	const auto inner = std::max(
		std::min(_note.maxWidth(), noteMax - padding.left() - padding.right()),
		1);
	const auto lineHeight = st::historyNoteReplyTextStyle.lineHeight;
	_noteLines = std::clamp(
		_note.countHeight(inner) / std::max(lineHeight, 1),
		1,
		int(st::historyNoteReplyLines));
	const auto noteWidth = inner + padding.left() + padding.right();
	const auto noteHeight = _noteLines * lineHeight
		+ padding.top()
		+ padding.bottom();
	const auto place = [&](int width) {
		return outgoing ? (edge - width) : edge;
	};
	_captionRect = QRect(place(captionWidth), top, captionWidth, captionHeight);
	const auto noteTop = top + captionHeight + st::historyNoteReplyCaptionSkip;
	_noteRect = QRect(place(noteWidth), noteTop, noteWidth, noteHeight);
	return _noteRect.bottom() + 1 + st::historyNoteReplySkip - top;
}

void NoteReplyView::translate(int dy) {
	_captionRect.translate(0, dy);
	_noteRect.translate(0, dy);
}

void NoteReplyView::paint(Painter &p, const ChatColors &colors) const {
	auto hq = PainterHighQualityEnabler(p);
	// The caption over the wallpaper, like the day pill.
	p.setPen(Qt::NoPen);
	p.setBrush(ServiceBg());
	const auto pill = double(_captionRect.height()) / 2.;
	p.drawRoundedRect(_captionRect, pill, pill);
	const auto &font = st::historyNoteReplyCaptionFont;
	const auto &captionPadding = st::historyNoteReplyCaptionPadding;
	p.setPen(st::historyServiceFg);
	p.setFont(font);
	p.drawText(
		_captionRect.marginsRemoved(captionPadding),
		Qt::AlignCenter,
		font->elided(
			_caption,
			_captionRect.width() - captionPadding.left() - captionPadding.right()));

	// The note in its author's colours: the other side's bubble, lighter.
	const auto fill = _outgoing ? colors.bubbleIn : colors.bubbleOut;
	const auto text = _outgoing ? colors.textIn : colors.textOut;
	const auto radius = double(st::historyNoteReplyRadius);
	p.setOpacity(st::historyNoteReplyOpacity);
	p.setBrush(fill);
	p.drawRoundedRect(_noteRect, radius, radius);
	p.setOpacity(1.);
	const auto &padding = st::historyNoteReplyPadding;
	auto geometry = Ui::Text::PaintContext();
	geometry.position = QPoint(
		_noteRect.x() + padding.left(),
		_noteRect.y() + padding.top());
	geometry.availableWidth = std::max(
		_noteRect.width() - padding.left() - padding.right(),
		1);
	geometry.elisionLines = _noteLines;
	geometry.autoDirection = true;
	p.setPen(text);
	_note.draw(p, geometry);
}

} // namespace History::View
