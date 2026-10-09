// The frames that host a Ui::BoxContent: a centered box and a side sheet.
// Created by Ui::LayerStackWidget::showBox / showSheet.
#pragma once

#include "ui/layers/box_content.h"
#include "ui/text/text.h"

namespace Ui {

class IconButton;

class BoxLayerWidget : public LayerWidget, public BoxContentDelegate {
public:
	BoxLayerWidget(LayerStackWidget *stack, object_ptr<BoxContent> content);
	~BoxLayerWidget();

	void parentResized() override;
	void showFinished() override;
	void setInnerFocus() override;
	bool closeByOutsideClick() const override {
		return _closeByOutsideClick;
	}
	bool closeByEscape() const override {
		return _closeByEscape;
	}
	QRect bodyRect() const override;

	void setTitle(const QString &title) override;
	RoundButton *addButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) override;
	RoundButton *addLeftButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) override;
	void clearButtons() override;
	void setDimensions(int width, int height) override;
	void setCloseByOutsideClick(bool close) override {
		_closeByOutsideClick = close;
	}
	void setCloseByEscape(bool close) override {
		_closeByEscape = close;
	}
	void showBox(object_ptr<BoxContent> box, LayerOption option) override;
	void closeBox() override;
	void triggerDefaultButton() override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	[[nodiscard]] int titleHeight() const;
	[[nodiscard]] int buttonsHeight() const;
	void updateGeometryInParent();
	void updateChildren();

	LayerStackWidget *_stack = nullptr;
	object_ptr<BoxContent> _content;
	Text::String _title;
	std::vector<RoundButton*> _buttons; // the default one first
	RoundButton *_leftButton = nullptr;
	int _contentWidth = 0;
	int _contentHeight = 0;
	bool _closeByOutsideClick = true;
	bool _closeByEscape = true;
	bool _inGeometryUpdate = false;

};

// A panel attached to the trailing edge of the window, full height.
class SheetLayerWidget : public LayerWidget, public BoxContentDelegate {
public:
	SheetLayerWidget(LayerStackWidget *stack, object_ptr<BoxContent> content);
	~SheetLayerWidget();

	void parentResized() override;
	void showFinished() override;
	void setInnerFocus() override;
	bool closeByOutsideClick() const override {
		return _closeByOutsideClick;
	}
	bool closeByEscape() const override {
		return _closeByEscape;
	}
	Transition transition() const override {
		return Transition::SlideFromEnd;
	}

	void setTitle(const QString &title) override;
	RoundButton *addButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) override;
	RoundButton *addLeftButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) override;
	void clearButtons() override;
	void setDimensions(int width, int height) override;
	void setCloseByOutsideClick(bool close) override {
		_closeByOutsideClick = close;
	}
	void setCloseByEscape(bool close) override {
		_closeByEscape = close;
	}
	void showBox(object_ptr<BoxContent> box, LayerOption option) override;
	void closeBox() override;
	void triggerDefaultButton() override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	[[nodiscard]] int buttonsHeight() const;
	void updateChildren();

	LayerStackWidget *_stack = nullptr;
	object_ptr<BoxContent> _content;
	IconButton *_close = nullptr;
	Text::String _title;
	std::vector<RoundButton*> _buttons;
	RoundButton *_leftButton = nullptr;
	int _wantedWidth = 0;
	bool _closeByOutsideClick = true;
	bool _closeByEscape = true;

};

} // namespace Ui
