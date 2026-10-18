// What the chat screen shows for bots (the rules are history_bot_logic.h):
//
//   * BotSuggestPanel — the card right above the composer that lists either
//     the bot's commands («/command — description») or the results of an
//     inline query (thumbnail, title, description), with a header line for
//     the bot's placeholder / «searching» / «no results». The field keeps
//     the keyboard: the composer passes Up / Down / Enter / Tab / Esc here
//     while the card is shown. The inline list asks for its next page when
//     it is scrolled near its end.
//
//       _botPanel = Ui::CreateChild<BotSuggestPanel>(panelParent, session);
//       _botPanel->setCommandChosen([=](const Data::BotCommand &command, bool insert) { … });
//       _botPanel->setResultChosen([=](const Data::InlineResult &result) { … });
//       _botPanel->setNeedMore([=] { … });
//       _botPanel->showCommands(commands);         // or showInline(header, results)
//       _botPanel->placeAbove(fieldRect);          // in the parent's coordinates
//
//   * BotIntroCard — «What can this bot do?» and the bot's description in the
//     middle of an empty chat with a bot, above the «شروع» bar.
#pragma once

#include "data/data_bots.h"
#include "ui/image/image_source.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <QtCore/QHash>

#include <memory>
#include <optional>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class ScrollArea;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace History {

class BotSuggestPanel final : public Ui::RpWidget {
public:
	enum class Mode {
		None,
		Commands,
		Inline,
	};

	BotSuggestPanel(QWidget *parent, Main::Session *session);
	~BotSuggestPanel();

	void showCommands(const QVector<Data::BotCommand> &commands);
	// `header`: a line above the results (empty: none). `more`: a next page
	// was appended, the list stays where it is scrolled.
	void showInline(
		const QString &header,
		const QVector<Data::InlineResult> &results,
		bool more = false);
	void hideFast();
	[[nodiscard]] Mode mode() const {
		return isHidden() ? Mode::None : _mode;
	}

	void selectSkip(int direction);
	// Enter / Tab: the selected row is chosen; false when there is none.
	bool chooseSelected(bool insert);

	void setCommandChosen(Fn<void(const Data::BotCommand &, bool insert)> callback);
	void setResultChosen(Fn<void(const Data::InlineResult &)> callback);
	void setNeedMore(Fn<void()> callback);
	// Right above `anchor`, at most as wide as it (the parent's coordinates).
	void placeAbove(const QRect &anchor);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	class Inner;

	void refreshGeometry();

	Main::Session *_session = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Inner *_inner = nullptr;
	Mode _mode = Mode::None;
	QRect _anchor;
	Fn<void(const Data::BotCommand &, bool insert)> _commandChosen;
	Fn<void(const Data::InlineResult &)> _resultChosen;
	Fn<void()> _needMore;

};

class BotIntroCard final : public Ui::RpWidget {
public:
	BotIntroCard(QWidget *parent, Window::SessionController *controller);

	void setDescription(const QString &description);
	[[nodiscard]] bool empty() const {
		return _text.isEmpty();
	}
	// Centred in `area` (the parent's coordinates).
	void placeIn(const QRect &area);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	[[nodiscard]] Ui::Text::PaintContext textContext() const;
	[[nodiscard]] int linkAt(QPoint position) const;

	Window::SessionController *_controller = nullptr;
	QString _description;
	Ui::Text::String _text;
	int _textWidth = 0;
	int _hovered = -1;
	int _pressed = -1;

};

} // namespace History
