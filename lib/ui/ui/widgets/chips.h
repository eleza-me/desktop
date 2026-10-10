// Ui::ChipsRow — a horizontally scrollable row of chips (categories, filters).
//
//   const auto chips = Ui::CreateChild<Ui::ChipsRow>(this);
//   chips->addChip(u"all"_s, tr::lng_all(tr::now));
//   chips->addChip(u"people"_s, tr::lng_people(tr::now), &st::chipIconPeople);
//   chips->setActive(u"all"_s);
//   connect(chips, &Ui::ChipsRow::activated, this, [=](const QString &id) { ... });
//   chips->resizeToWidth(width());
//
// Scrolls with the wheel (vertical wheel too), by dragging, and keeps the active
// chip visible. Edges fade when there is more content.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "styles/style_widgets.h"

#include <vector>

namespace Ui {

class PillButton;

class ChipsRow : public RpWidget {
	Q_OBJECT

public:
	explicit ChipsRow(
		QWidget *parent,
		const style::ChipsRow &st = st::defaultChipsRow);

	void addChip(
		const QString &id,
		const QString &text,
		const style::icon *icon = nullptr);
	void clearChips();
	[[nodiscard]] int count() const {
		return int(_chips.size());
	}
	// Empty id = none active.
	void setActive(const QString &id, anim::type animated = anim::type::normal);
	[[nodiscard]] QString active() const {
		return _active;
	}
	// Clicking the active chip again deactivates it (filters).
	void setToggleable(bool toggleable) {
		_toggleable = toggleable;
	}

Q_SIGNALS:
	void activated(QString id);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Chip {
		QString id;
		PillButton *button = nullptr;
	};

	void layoutChips();
	void setScroll(int scroll, bool animated = false);
	[[nodiscard]] int scrollMax() const;
	void ensureVisible(PillButton *button);
	void updateFades();

	class Fade;

	const style::ChipsRow &_st;
	RpWidget *_inner = nullptr;
	Fade *_fadeStart = nullptr;
	Fade *_fadeEnd = nullptr;
	std::vector<Chip> _chips;
	QString _active;
	bool _toggleable = false;
	int _contentWidth = 0;
	int _scroll = 0; // from the leading side
	int _scrollTarget = 0;
	Animations::Simple _scrollAnimation;

	bool _dragging = false;
	bool _dragMoved = false;
	int _dragStartX = 0;
	int _dragStartScroll = 0;

};

} // namespace Ui
