#include "ui/layers/generic_box.h"

#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/ui_phrases.h"

#include <QtCore/QPointer>

namespace Ui {

GenericBox::GenericBox(QWidget *parent, std::function<void(GenericBox*)> init)
: BoxContent(parent)
, _init(std::move(init))
, _owned(this)
, _content(_owned.data())
, _width(st::boxWidth) {
}

void GenericBox::addSkip(int height) {
	_content->add(object_ptr<FixedHeightWidget>(_content, height));
}

void GenericBox::prepare() {
	if (const auto init = std::exchange(_init, nullptr)) {
		init(this);
	}
	const auto content = _content;
	const auto width = _width;
	content->resizeToWidth(width);
	setDimensions(width, content->height());
	QObject::connect(
		content,
		&RpWidget::heightChanged,
		this,
		[this, width](int height) { setDimensions(width, height); });
	setInnerWidget(std::move(_owned));
}

void GenericBox::setInnerFocus() {
	if (_focus) {
		_focus();
	} else {
		BoxContent::setInnerFocus();
	}
}

void GenericBox::showFinished() {
	if (_showFinished) {
		_showFinished();
	}
}

object_ptr<GenericBox> MakeConfirmBox(ConfirmBoxArgs args) {
	return Box([args = std::move(args)](GenericBox *box) {
		const auto &phrases = CurrentPhrases();
		if (!args.title.isEmpty()) {
			box->setTitle(args.title);
		}
		box->addRow(
			object_ptr<FlatLabel>(box, args.text, st::boxLabel),
			args.title.isEmpty()
				? st::boxRowPaddingNoTitle
				: st::boxRowPadding);
		if (!args.checkboxText.isEmpty()) {
			const auto check = box->addRow(
				object_ptr<Checkbox>(
					box,
					args.checkboxText,
					args.checkboxChecked),
				st::boxCheckboxPadding);
			if (args.checkboxChanged) {
				QObject::connect(
					check,
					&Checkbox::checkedChanged,
					box,
					args.checkboxChanged);
			}
		}
		const auto weak = QPointer<GenericBox>(box);
		const auto close = [weak] {
			if (weak) {
				weak->closeBox();
			}
		};
		const auto confirmed = args.confirmed;
		box->addButton(
			args.confirmText.isEmpty() ? phrases.ok : args.confirmText,
			[confirmed, close] {
				if (confirmed) {
					confirmed(close);
				} else {
					close();
				}
			},
			args.destructive ? st::dangerRoundButton : st::defaultRoundButton);
		if (!args.inform) {
			const auto cancelled = args.cancelled;
			box->addButton(
				args.cancelText.isEmpty() ? phrases.cancel : args.cancelText,
				[cancelled, close] {
					if (cancelled) {
						cancelled();
					}
					close();
				});
		}
	});
}

object_ptr<GenericBox> MakeInformBox(const QString &text, const QString &title) {
	auto args = ConfirmBoxArgs();
	args.text = text;
	args.title = title;
	args.inform = true;
	return MakeConfirmBox(std::move(args));
}

} // namespace Ui
