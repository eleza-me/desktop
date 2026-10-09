// Ui::VerticalLayout — stacks RpWidgets vertically; each row gets the full width
// (minus its margins) through resizeToWidth() and reports its own height.
//
//   const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
//   const auto title = content->add(
//       Ui::object_ptr<Ui::FlatLabel>(content, text, st::titleLabel),
//       st::settingsTitlePadding);                    // margins: left = leading side
//   content->add(Ui::object_ptr<Ui::RoundButton>(...), margins, style::al_top);  // natural width, centered
//   content->resizeToWidth(width());                  // lays everything out
//
// Rows re-layout automatically when a row changes its height or is shown / hidden.
#pragma once

#include "ui/rp_widget.h"

#include <optional>
#include <vector>

namespace Ui {

class VerticalLayout : public RpWidget {
public:
	explicit VerticalLayout(QWidget *parent = nullptr);

	// `align` set: the row keeps its natural width and is placed at the leading
	// side (al_left), centered (al_top / al_center) or trailing (al_right).
	template <typename Widget>
	Widget *add(
			object_ptr<Widget> child,
			const style::margins &margin = style::margins(),
			std::optional<style::align> align = std::nullopt) {
		return insert(count(), std::move(child), margin, align);
	}
	template <typename Widget>
	Widget *insert(
			int atPosition,
			object_ptr<Widget> child,
			const style::margins &margin = style::margins(),
			std::optional<style::align> align = std::nullopt) {
		const auto result = child.data();
		insertChild(atPosition, child.release(), margin, align);
		return result;
	}

	[[nodiscard]] int count() const {
		return int(_rows.size());
	}
	[[nodiscard]] RpWidget *widgetAt(int index) const {
		return _rows[index].widget;
	}
	// Destroys the row.
	void removeChild(RpWidget *child);
	void clear();
	void reorderRows(int oldIndex, int newIndex);
	void setVerticalShift(int index, int shift);

protected:
	int resizeGetHeight(int newWidth) override;
	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override;

private:
	struct Row {
		RpWidget *widget = nullptr;
		style::margins margin;
		std::optional<style::align> align;
		int verticalShift = 0;
	};

	void insertChild(
		int atPosition,
		RpWidget *child,
		const style::margins &margin,
		std::optional<style::align> align);
	void relayout();

	std::vector<Row> _rows;
	bool _inResize = false;
	bool _relayoutPending = false;
	int _visibleTop = 0;
	int _visibleBottom = 0;

};

// A fixed-height empty row.
class FixedHeightWidget : public RpWidget {
public:
	explicit FixedHeightWidget(QWidget *parent, int height = 0)
	: RpWidget(parent) {
		resize(width(), height);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		Q_UNUSED(newWidth);
		return height();
	}

};

} // namespace Ui
