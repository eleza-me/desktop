// The strip of quick reactions shown over a message's context menu: a few
// emoji in a row and a chevron that opens the whole set.
#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"

namespace History {

class ReactionsStrip final : public Ui::RpWidget {
public:
	ReactionsStrip(
		QWidget *parent,
		const QStringList &emoji,
		const QString &selected,
		bool expanded);

	void setChosenCallback(Fn<void(const QString &emoji)> callback) {
		_chosen = std::move(callback);
	}
	void setExpandCallback(Fn<void()> callback) {
		_expand = std::move(callback);
	}

	[[nodiscard]] int naturalWidth() const override;

	static constexpr auto kQuickCount = 7;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	// -1: nothing, count(): the chevron.
	[[nodiscard]] int cellAt(QPoint position) const;
	[[nodiscard]] QRect cellRect(int index) const; // physical
	[[nodiscard]] int shownCount() const;
	[[nodiscard]] bool hasToggle() const;
	[[nodiscard]] int columns() const;

	QStringList _emoji;
	QString _selected;
	bool _expanded = false;
	int _over = -1;
	int _pressed = -1;
	Fn<void(const QString &emoji)> _chosen;
	Fn<void()> _expand;

};

} // namespace History
