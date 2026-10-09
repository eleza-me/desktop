// Ui::BoxContent — the content of a modal box (or of a side sheet).
//
// Subclass it and build the box in prepare():
//
//   class EditNameBox : public Ui::BoxContent {
//   public:
//       EditNameBox(QWidget*, QString name);       // first argument is always QWidget*
//   protected:
//       void prepare() override {
//           setTitle(tr::lng_name(tr::now));
//           addButton(tr::lng_save(tr::now), [=] { save(); });
//           addButton(tr::lng_cancel(tr::now), [=] { closeBox(); }, st::defaultBoxButton);
//           setDimensions(st::boxWidth, _field->height() + ...);
//       }
//       void setInnerFocus() override { _field->setFocusFast(); }
//   };
//   controller->show(Ui::Box<EditNameBox>(name));
//
// The first added button is the default one (trailing side, triggered by Enter).
// For simple boxes use Ui::GenericBox (generic_box.h) and Ui::MakeConfirmBox.
#pragma once

#include "ui/rp_widget.h"
#include "ui/layers/layer_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_layers.h"

#include <functional>

namespace Ui {

class BoxContent;

class BoxContentDelegate {
public:
	virtual void setTitle(const QString &title) = 0;
	virtual RoundButton *addButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) = 0;
	virtual RoundButton *addLeftButton(
		const QString &text,
		std::function<void()> callback,
		const style::RoundButton &st) = 0;
	virtual void clearButtons() = 0;
	virtual void setDimensions(int width, int height) = 0;
	virtual void setCloseByOutsideClick(bool close) = 0;
	virtual void setCloseByEscape(bool close) = 0;
	virtual void showBox(object_ptr<BoxContent> box, LayerOption option) = 0;
	virtual void closeBox() = 0;
	virtual void triggerDefaultButton() = 0;

protected:
	~BoxContentDelegate() = default;

};

class BoxContent : public RpWidget {
	Q_OBJECT

public:
	BoxContent() : BoxContent(nullptr) {
	}
	explicit BoxContent(QWidget *parent) : RpWidget(parent) {
		setFocusPolicy(Qt::StrongFocus);
	}

	// Called by the hosting layer.
	void setDelegate(BoxContentDelegate *delegate) {
		_delegate = delegate;
		prepare();
		_prepared = true;
	}
	[[nodiscard]] bool isBoxShown() const {
		return _delegate != nullptr;
	}

	void setTitle(const QString &title) {
		_delegate->setTitle(title);
	}
	RoundButton *addButton(
			const QString &text,
			std::function<void()> callback = nullptr,
			const style::RoundButton &st = st::defaultBoxButton) {
		return _delegate->addButton(text, std::move(callback), st);
	}
	RoundButton *addLeftButton(
			const QString &text,
			std::function<void()> callback = nullptr,
			const style::RoundButton &st = st::defaultBoxButton) {
		return _delegate->addLeftButton(text, std::move(callback), st);
	}
	void clearButtons() {
		_delegate->clearButtons();
	}
	// The size of the content area (without title and buttons).
	void setDimensions(int width, int height) {
		_delegate->setDimensions(width, height);
	}
	// Follows the height of `content` (which is laid out for `width`).
	void setDimensionsToContent(int width, RpWidget *content);
	void setCloseByOutsideClick(bool close) {
		_delegate->setCloseByOutsideClick(close);
	}
	void setCloseByEscape(bool close) {
		_delegate->setCloseByEscape(close);
	}
	void showBox(
			object_ptr<BoxContent> box,
			LayerOption option = LayerOption::KeepOther) {
		_delegate->showBox(std::move(box), option);
	}
	void closeBox() {
		if (_delegate) {
			_delegate->closeBox();
		}
	}

	// Puts `inner` into a scroll area that fills the box (long content).
	template <typename Widget>
	Widget *setInnerWidget(
			object_ptr<Widget> inner,
			int topSkip = 0,
			int bottomSkip = 0) {
		const auto result = inner.data();
		setInner(Owned<RpWidget>(inner.release()), topSkip, bottomSkip);
		return result;
	}
	[[nodiscard]] ScrollArea *scrollArea() const {
		return _scroll;
	}

	// Where the keyboard focus goes when the box is shown.
	virtual void setInnerFocus() {
		setFocus();
	}
	// Called when the show animation finished.
	virtual void showFinished() {
	}

Q_SIGNALS:
	void boxClosing();

protected:
	virtual void prepare() = 0;

	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

	[[nodiscard]] BoxContentDelegate *delegate() const {
		return _delegate;
	}

private:
	void setInner(object_ptr<RpWidget> inner, int topSkip, int bottomSkip);
	void updateScrollGeometry();

	BoxContentDelegate *_delegate = nullptr;
	bool _prepared = false;
	ScrollArea *_scroll = nullptr;
	int _scrollTopSkip = 0;
	int _scrollBottomSkip = 0;

};

// Ui::Box<MyBox>(args...) -> object_ptr<MyBox> constructed as MyBox(nullptr, args...).
template <typename BoxType, typename ...Args>
[[nodiscard]] inline object_ptr<BoxType> Box(Args &&...args) {
	return Owned(new BoxType(nullptr, std::forward<Args>(args)...));
}

} // namespace Ui
