// The country list of the phone step (and of «change number»): a searchable list
// of calling codes, like Telegram's.
//
//   window->show(Ui::Box<Boxes::CountrySelectBox>(currentIso, [=](const Lang::Country &country) {
//       phone->setCountryCode(country.code);
//   }));
//
// Typing filters by name (either language), ISO code or calling code; Up / Down /
// Enter choose with the keyboard.
#pragma once

#include "lang/lang_countries.h"
#include "ui/layers/box_content.h"
#include "ui/widgets/list_widget.h"

namespace Ui {
class SearchField;
} // namespace Ui

namespace Boxes {

class CountrySelectBox final
	: public Ui::BoxContent
	, private Ui::ListDelegate {
public:
	CountrySelectBox(
		QWidget *parent,
		const QString &currentIso,
		std::function<void(const Lang::Country &country)> chosen);

	void setInnerFocus() override;

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	int rowsCount() const override;
	int rowHeight(int index, int width) const override;
	void paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) override;

	void applyFilter(const QString &query);
	void choose(int index);

	QString _currentIso;
	std::function<void(const Lang::Country &country)> _chosen;
	Ui::SearchField *_search = nullptr;
	Ui::ListWidget *_list = nullptr;
	std::vector<const Lang::Country*> _rows;
	bool _flags = false;

};

} // namespace Boxes
