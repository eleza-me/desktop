// Boxes of the profile screens: share (link + QR code), report an account,
// the birthday picker.
#pragma once

#include "data/data_types.h"
#include "ui/effects/animations.h"
#include "ui/layers/box_content.h"

#include <optional>

namespace Ui {
class PillButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Profile {

// «اشتراک‌گذاری نمایه»: the card, the link as a QR code, copy.
void ShowShareBox(
	Window::SessionController *controller,
	const Data::User &user);

// «گزارش حساب»: the reasons of the server, a note, «این کاربر را هم مسدود کن».
// `blocked` is called when the account was blocked along the way.
void ShowReportBox(
	Window::SessionController *controller,
	const Data::User &user,
	Fn<void()> blocked);

// One column of the date picker: rows that snap to the middle; the wheel, a
// drag, a click on a row or Up / Down choose.
class Wheel final : public Ui::RpWidget {
	Q_OBJECT

public:
	explicit Wheel(QWidget *parent);

	void setItems(const QStringList &items, int selected);
	[[nodiscard]] int selected() const {
		return _selected;
	}
	void setSelected(int index, bool animated = true);

Q_SIGNALS:
	void selectedChanged(int index);

protected:
	void paintEvent(QPaintEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] double offset() const;
	void choose(int index, bool animated);

	QStringList _items;
	int _selected = 0;
	double _from = 0.;
	Ui::Animations::Simple _animation;
	bool _dragging = false;
	bool _moved = false;
	int _pressY = 0;
	double _pressOffset = 0.;
	double _dragOffset = 0.;
	int _wheelDelta = 0;

};

// Day / month / year wheels: Jalali in Persian, Gregorian in English, with a
// switch between the two; the year is optional («بدون سال»).
class BirthdayBox final : public Ui::BoxContent {
public:
	BirthdayBox(
		QWidget *parent,
		std::optional<Data::Birthday> current,
		Fn<void(std::optional<Data::Birthday>)> done);

	void setInnerFocus() override;

protected:
	void prepare() override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	[[nodiscard]] Data::Birthday value() const;
	void fill(const Data::Birthday &birthday);
	void refreshDays();
	void switchCalendar(const QString &calendar);
	[[nodiscard]] QVector<Wheel*> order() const;

	std::optional<Data::Birthday> _initial;
	Fn<void(std::optional<Data::Birthday>)> _done;
	QString _calendar;
	Ui::PillButton *_jalali = nullptr;
	Ui::PillButton *_gregorian = nullptr;
	Wheel *_day = nullptr;
	Wheel *_month = nullptr;
	Wheel *_year = nullptr;
	int _yearFrom = 0;
	QRect _band;
	int _captionTop = 0;
	int _previewTop = 0;

};

} // namespace Profile
