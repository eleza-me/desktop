// A bot's reply keyboard: rows of buttons under the message field. Pressing
// one sends its text as a message.
//
//   _keyboard = Ui::CreateChild<BotKeyboard>(this);
//   _keyboard->setRows(markup.keyboard);
//   _keyboard->setPressedCallback([=](const QString &text) { send(text); });
//   _keyboard->resizeToWidth(width);   // the height follows the rows
#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"

#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <vector>

namespace History {

class BotKeyboard final : public Ui::RpWidget {
public:
	explicit BotKeyboard(QWidget *parent);

	void setRows(const QVector<QStringList> &rows);
	[[nodiscard]] bool empty() const {
		return _buttons.empty();
	}
	void setPressedCallback(Fn<void(const QString &text)> callback) {
		_pressedCallback = std::move(callback);
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	struct Button {
		QString text;
		int row = 0;
		int column = 0;
		int columns = 1;
		QRect rect;
	};

	[[nodiscard]] int buttonAt(QPoint position) const;
	void setHovered(int index);

	QVector<QStringList> _rows;
	std::vector<Button> _buttons;
	Fn<void(const QString &text)> _pressedCallback;
	int _hovered = -1;
	int _pressed = -1;

};

} // namespace History
