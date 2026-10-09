// Ui::GenericBox — a box built in place from rows, without a subclass.
//
//   controller->show(Ui::Box([=](Ui::GenericBox *box) {
//       box->setTitle(tr::lng_report(tr::now));
//       const auto field = box->addRow(Ui::object_ptr<Ui::InputField>(box, st::defaultInputField, ...));
//       box->addRow(Ui::object_ptr<Ui::Checkbox>(box, text), st::boxRowPadding);
//       box->setFocusCallback([=] { field->setFocusFast(); });
//       box->addButton(tr::lng_send(tr::now), [=] { ...; box->closeBox(); }, st::defaultRoundButton);
//       box->addButton(tr::lng_cancel(tr::now), [=] { box->closeBox(); });
//   }));
//
// Rows are stacked in a VerticalLayout inside a scroll area; the box grows with them
// up to the window height.
//
// Ui::MakeConfirmBox / Ui::MakeInformBox build the usual question / notice boxes.
#pragma once

#include "ui/layers/box_content.h"
#include "ui/wrap/vertical_layout.h"

#include <optional>

namespace Ui {

class GenericBox : public BoxContent {
public:
	GenericBox(QWidget *parent, std::function<void(GenericBox*)> init);

	template <typename Widget>
	Widget *addRow(
			object_ptr<Widget> child,
			const style::margins &margins = st::boxRowPadding,
			std::optional<style::align> align = std::nullopt) {
		return _content->add(std::move(child), margins, align);
	}
	void addSkip(int height);
	void setWidth(int width) {
		_width = width;
	}
	void setFocusCallback(std::function<void()> callback) {
		_focus = std::move(callback);
	}
	void setShowFinishedCallback(std::function<void()> callback) {
		_showFinished = std::move(callback);
	}
	[[nodiscard]] VerticalLayout *verticalLayout() const {
		return _content;
	}

	void setInnerFocus() override;
	void showFinished() override;

protected:
	void prepare() override;

private:
	std::function<void(GenericBox*)> _init;
	std::function<void()> _focus;
	std::function<void()> _showFinished;
	object_ptr<VerticalLayout> _owned;
	VerticalLayout *_content = nullptr;
	int _width = 0;

};

[[nodiscard]] inline object_ptr<GenericBox> Box(
		std::function<void(GenericBox*)> init) {
	return Owned(new GenericBox(nullptr, std::move(init)));
}

struct ConfirmBoxArgs {
	QString text;
	QString title; // optional
	QString confirmText; // default: Ui::CurrentPhrases().ok
	QString cancelText; // default: Ui::CurrentPhrases().cancel
	// `close` closes the box; call it when the action is done (or right away).
	std::function<void(std::function<void()> close)> confirmed;
	std::function<void()> cancelled;
	bool destructive = false; // red confirm button
	bool inform = false; // a single button
	// An optional checkbox ("also block this user").
	QString checkboxText;
	bool checkboxChecked = false;
	std::function<void(bool checked)> checkboxChanged;
};

[[nodiscard]] object_ptr<GenericBox> MakeConfirmBox(ConfirmBoxArgs args);
[[nodiscard]] object_ptr<GenericBox> MakeInformBox(
	const QString &text,
	const QString &title = QString());

} // namespace Ui
