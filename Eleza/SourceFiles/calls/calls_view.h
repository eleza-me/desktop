// The expanded view of a call: it covers the body of the window while the
// bar is hidden.
//
//                        ( userpic with the voice ring )
//                                   name
//                     در حال زنگ زدن… / ۰۲:۱۵ / اتصال دوباره…
//
//                    [ devices ]   [ mute ]   [ end ]
//          «تماس‌ها از سرور الزا عبور می‌کنند و رمزنگاری سرتاسری ندارند.»
//
// The ring around the picture follows the other side's voice; a thinner one
// inside follows the microphone. «دستگاه‌ها» opens the microphone / speaker
// pickers; the choice is remembered (Core::Settings::calls()). After the end
// the buttons are «تماس دوباره» and «بستن». Esc and the chevron go back to
// the bar.
//
// Video (where the server has video calls): «دوربین» turns my camera on and
// off in a call of either kind. What is shown follows the cameras, not how
// the call started:
//
//   theirs on            their picture fills the view, the name and the state
//                        in a corner; mine (if on) small, mirrored, in the
//                        other corner
//   only mine on         my picture, mirrored, in place of the userpic
//   both off             the voice call above
#pragma once

#include "ui/effects/animations.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>

namespace Support {
class UserpicButton;
} // namespace Support

namespace Ui {
class IconButton;
class PopupMenu;
} // namespace Ui

class Painter;

namespace Calls {

class LabeledButton;
class Service;

// Fills the menu with the microphones and the speakers of the system, the
// chosen ones ticked; a choice goes to `service` (also used by the settings).
void FillDevicesMenu(Ui::PopupMenu *menu, Service *service);

class View final : public Ui::RpWidget {
public:
	View(QWidget *parent, Service *service);

	void refresh();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	void updateControls();
	void showDevicesMenu();
	[[nodiscard]] int contentTop() const;
	// What of the cameras there is to paint right now.
	[[nodiscard]] bool showsRemoteVideo() const;
	[[nodiscard]] bool showsLocalVideo() const;
	[[nodiscard]] QSize previewSize() const;
	[[nodiscard]] QRect pictureRect() const;
	void paintRemoteVideo(Painter &p);
	void paintLocalCorner(Painter &p);

	Service *_service = nullptr;
	Ui::IconButton *_collapse = nullptr;
	Support::UserpicButton *_userpic = nullptr;
	LabeledButton *_devices = nullptr;
	LabeledButton *_camera = nullptr;
	LabeledButton *_mute = nullptr;
	LabeledButton *_end = nullptr;
	LabeledButton *_again = nullptr;
	LabeledButton *_close = nullptr;
	QPointer<Ui::PopupMenu> _menu;
	Ui::Animations::Basic _levels;
	QTimer _clock;
	QString _peerId;
	QString _name;
	// Smoothed, 0..1.
	double _outputRing = 0.;
	double _inputRing = 0.;

};

} // namespace Calls
