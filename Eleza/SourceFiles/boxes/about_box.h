// Boxes::AboutBox — «دربارهٔ الزا»: the mark, the version (click copies it) and
// the links to the site, the terms, the privacy policy and the guidelines.
//
//   controller->show(Ui::Box<Boxes::AboutBox>());
#pragma once

#include "ui/layers/box_content.h"

namespace Ui {
class FlatLabel;
class LinkButton;
} // namespace Ui

namespace Boxes {

class AboutBox final : public Ui::BoxContent {
public:
	explicit AboutBox(QWidget *parent);

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] QString versionText() const;
	[[nodiscard]] QRect versionRect() const;
	[[nodiscard]] int contentHeight() const;

	Ui::FlatLabel *_text = nullptr;
	std::vector<Ui::LinkButton*> _links;
	int _textTop = 0;

};

} // namespace Boxes
