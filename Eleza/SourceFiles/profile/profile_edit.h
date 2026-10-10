// «ویرایش نمایه»: photo (cropped, uploaded in the background), name, handle
// with a live availability check, link, bio, birthday, «نمایش استان روی نمایه».
// A section of the main column: Save is enabled while something changed and
// everything is valid; leaving with changes asks first.
#pragma once

#include "data/data_graph.h"
#include "data/data_upload.h"
#include "profile/profile_logic.h"
#include "window/window_registry.h"
#include "window/window_section_widget.h"

namespace Ui {
class FlatLabel;
class InputField;
class LinkButton;
class PopupMenu;
class RoundButton;
class ScrollArea;
class SettingsButton;
class TopBar;
class VerticalLayout;
} // namespace Ui

namespace Profile {

class CenteredWrap;

[[nodiscard]] Window::SectionFactory EditFactory();

class EditSection final : public Window::SectionWidget {
public:
	EditSection(QWidget *parent, Window::SessionController *controller);
	~EditSection();

	QString sectionKey() const override;
	void setInnerFocus() override;
	bool onBackRequest() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;

private:
	class Photo;
	enum class Field {
		None,
		Photo,
		Name,
		Handle,
		Link,
		Bio,
		Birthday,
	};

	void build();
	void fill(const Data::Profile &profile);
	void loadSettings();
	[[nodiscard]] bool dirty() const;
	[[nodiscard]] bool valid() const;
	void refreshState();
	void refreshHandleHint();
	void refreshLinkHint();
	void refreshBioCounter();
	void refreshBirthday();
	void refreshPhoto();
	void setHint(Ui::FlatLabel *label, const QString &text, bool error, bool ok = false);

	void choosePhoto();
	// A picture dropped on the section: the same crop box as a chosen one.
	void cropPhoto(const QImage &image);
	void cropPhotoFile(const QString &path);
	void showPhotoMenu();
	void startUpload(const QImage &square);
	void cancelUpload();
	void removePhoto();
	void toggleProvince(bool on);

	void save();
	void saveProfile();
	void saveHandle();
	void saveBirthday();
	void finishSave();
	void saveFailed(const Api::Error &error, Field fallback);
	[[nodiscard]] static Field FieldOf(const QString &code, Field fallback);
	void leave();

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	CenteredWrap *_wrap = nullptr;
	Ui::VerticalLayout *_layout = nullptr;
	Photo *_photo = nullptr;
	Ui::LinkButton *_changePhoto = nullptr;
	Ui::FlatLabel *_photoHint = nullptr;
	Ui::InputField *_name = nullptr;
	Ui::FlatLabel *_nameHint = nullptr;
	Ui::InputField *_handle = nullptr;
	Ui::FlatLabel *_handleHint = nullptr;
	Ui::InputField *_link = nullptr;
	Ui::FlatLabel *_linkHint = nullptr;
	Ui::InputField *_bio = nullptr;
	Ui::FlatLabel *_bioCounter = nullptr;
	Ui::SettingsButton *_birthdayRow = nullptr;
	Ui::FlatLabel *_birthdayHint = nullptr;
	Ui::SettingsButton *_province = nullptr;
	Ui::FlatLabel *_provinceHint = nullptr;
	Ui::RoundButton *_save = nullptr;
	Ui::RoundButton *_cancel = nullptr;
	QPointer<Ui::PopupMenu> _menu;

	Data::Profile _base;
	bool _filled = false;
	HandleChecker _checker;
	std::optional<Data::Birthday> _birthday;
	QString _birthdayError;
	QString _handleError;
	int _nameMax = 0;
	int _bioMax = 0;

	// The photo: a fresh upload, or the removal of the current one.
	bool _hasNewPhoto = false;
	bool _clearPhoto = false;
	bool _uploading = false;
	Data::Upload::Id _uploadId = 0;
	QString _uploadedMediaId;
	int _photoGeneration = 0;

	bool _saving = false;
	bool _saveAfterUpload = false;
	bool _leaving = false;
	bool _provinceBusy = false;

};

} // namespace Profile
