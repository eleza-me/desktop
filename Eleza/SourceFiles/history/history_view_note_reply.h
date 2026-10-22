// A reply to a note (Data::NoteReply), drawn the way the mobile apps draw it:
// above the bubble and on its side, «به نوتت جواب داد» / «به نوتش جواب دادی»
// in a small pill over the wallpaper, then the note in a small bubble of its
// author's colour; the message's own bubble holds only the answer.
//
//   _note = NoteReplyView::Create(message);         // null for any other message
//   const auto &shown = _note ? _note->message() : message; // the answer alone
//   top += _note->layout(edge, top, maxWidth, outgoing);   // physical coordinates
//   _note->paint(p, colors);
#pragma once

#include "data/data_note_types.h"
#include "data/data_types.h"
#include "ui/text/text.h"

#include <QtCore/QRect>

#include <memory>

class Painter;

namespace History {
struct ChatColors;
} // namespace History

namespace History::View {

class NoteReplyView final {
public:
	// A text message (not forwarded) whose text is a note reply.
	[[nodiscard]] static std::unique_ptr<NoteReplyView> Create(
		const Data::Message &message);

	// The message with only the answer as its text (entities moved).
	[[nodiscard]] const Data::Message &message() const {
		return _shown;
	}

	// Places the header above the bubble, against `edge` — the bubbles'
	// leading x for an incoming message, their trailing x (exclusive) for an
	// outgoing one — from `top`. Returns its height (the skip under it too).
	int layout(int edge, int top, int maxWidth, bool outgoing);
	void paint(Painter &p, const ChatColors &colors) const;
	void translate(int dy);
	[[nodiscard]] QRect rect() const {
		return _captionRect.united(_noteRect);
	}

private:
	NoteReplyView(const Data::Message &message, Data::NoteReply reply);

	Data::Message _shown;
	QString _caption;
	Ui::Text::String _note;
	bool _outgoing = false;
	int _noteLines = 1;
	QRect _captionRect;
	QRect _noteRect;

};

} // namespace History::View
