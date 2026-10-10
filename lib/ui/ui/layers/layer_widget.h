// Modal layers shown over the window content.
//
//   Ui::LayerStackWidget  covers its parent, dims it and hosts a stack of layers:
//                         boxes (centered dialogs), sheets (a panel on the trailing
//                         side) or any custom Ui::LayerWidget.
//
// Feature code normally goes through Window::SessionController::show(...):
//
//   controller->show(Ui::Box<MyBox>(args...));                       // a BoxContent subclass
//   controller->show(Ui::Box([=](Ui::GenericBox *box) { ... }));     // built in place
//   controller->show(Ui::MakeConfirmBox({ .text = ..., .confirmed = ... }));
//   controller->showSheet(Ui::Box<MySheetContent>(...));             // side sheet
//
// Esc and a click on the dimmed background close the top layer (unless it forbids
// that); a box shown over another one hides it until closed (LayerOption::KeepOther).
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "styles/style_layers.h"

#include <QtGui/QPixmap>

#include <functional>
#include <memory>
#include <vector>

namespace Ui {

class BoxContent;

enum class LayerOption {
	CloseOther, // replace everything that is shown
	KeepOther, // show over, return to the previous layer when closed
};

class LayerWidget : public RpWidget {
public:
	using RpWidget::RpWidget;

	enum class Transition {
		Scale, // fade + slight zoom (boxes)
		SlideFromEnd, // slides in from the trailing side (sheets)
		Fade,
	};

	// Place yourself inside the parent (the stack) after it was resized.
	virtual void parentResized() = 0;
	virtual void showFinished() {
	}
	virtual void setInnerFocus() {
		setFocus();
	}
	[[nodiscard]] virtual bool closeByOutsideClick() const {
		return true;
	}
	[[nodiscard]] virtual bool closeByEscape() const {
		return true;
	}
	[[nodiscard]] virtual Transition transition() const {
		return Transition::Scale;
	}
	// The part of the widget that takes clicks (the rest is shadow).
	[[nodiscard]] virtual QRect bodyRect() const {
		return rect();
	}

	void setClosedCallback(std::function<void()> callback) {
		_closedCallback = std::move(callback);
	}
	// Asks the stack to close this layer.
	void closeLayer() {
		if (const auto callback = _closedCallback) {
			callback();
		}
	}

private:
	std::function<void()> _closedCallback;

};

class LayerStackWidget : public RpWidget {
	Q_OBJECT

public:
	explicit LayerStackWidget(QWidget *parent);
	~LayerStackWidget();

	void showBox(
		object_ptr<BoxContent> box,
		LayerOption option = LayerOption::KeepOther,
		anim::type animated = anim::type::normal);
	void showSheet(
		object_ptr<BoxContent> content,
		LayerOption option = LayerOption::KeepOther,
		anim::type animated = anim::type::normal);
	void showLayer(
		object_ptr<LayerWidget> layer,
		LayerOption option = LayerOption::KeepOther,
		anim::type animated = anim::type::normal);

	void hideTopLayer(anim::type animated = anim::type::normal);
	void hideLayers(anim::type animated = anim::type::normal);
	[[nodiscard]] bool hasLayers() const {
		return !_layers.empty();
	}
	[[nodiscard]] LayerWidget *topLayer() const;

Q_SIGNALS:
	// The last layer finished hiding / the first one started showing.
	void layersShownChanged(bool shown);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	struct Entry {
		LayerWidget *widget = nullptr;
		QPixmap cache;
		Animations::Simple animation;
		bool shown = true; // target state
	};

	void startTransition(Entry &entry, bool shown, anim::type animated);
	void transitionCallback(LayerWidget *widget);
	void finishTransition(Entry &entry);
	void closeLayer(LayerWidget *widget, anim::type animated);
	void destroyHidden();
	void updateDim(anim::type animated);
	void updateVisibility();
	void paintEntry(QPainter &p, Entry &entry);
	[[nodiscard]] Entry *find(LayerWidget *widget);

	std::vector<std::unique_ptr<Entry>> _layers; // bottom to top
	std::vector<std::unique_ptr<Entry>> _hiding;
	Animations::Simple _dimAnimation;
	bool _dimShown = false;

};

} // namespace Ui
