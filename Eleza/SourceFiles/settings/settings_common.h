// Building blocks of the settings pages.
//
//   class PrivacyPage : public Settings::SyncedPage {      // loads /v1/me/settings once
//       void build() override {
//           addToggle(tr::lng_…(tr::now), u"privacy.private_account"_s, false);
//           AddDivider(content());
//           AddSubsectionTitle(content(), tr::lng_…(tr::now));
//           addPicker(tr::lng_…(tr::now), u"privacy.messages"_s, options, u"everyone"_s);
//       }
//   };
//
// A Page is a section of the main column: a top bar and a scrolled column of rows
// (Ui::SettingsButton and friends) no wider than st::settingsContentWidth.
#pragma once

#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_section_widget.h"

#include <QtCore/QJsonValue>
#include <QtCore/QPointer>

#include <functional>
#include <vector>

namespace Api {
struct Error;
} // namespace Api

namespace Ui {
class EmptyPlaceholder;
class FlatLabel;
class ScrollArea;
class SkeletonList;
class TopBar;
} // namespace Ui

namespace Settings {

enum class Section;

struct Option {
	QString value;
	QString label;
};

// Wraps a callback so it does nothing once `object` is gone (a box that
// outlives the page that opened it).
template <typename Object, typename Callback>
[[nodiscard]] auto Guarded(Object *object, Callback callback) {
	return [weak = QPointer<Object>(object), callback = std::move(callback)](
			auto &&...args) {
		if (weak) {
			callback(std::forward<decltype(args)>(args)...);
		}
	};
}

// ---- rows
void AddSkip(Ui::VerticalLayout *container, int height = 0);
// A hairline between groups of rows.
void AddDivider(Ui::VerticalLayout *container);
Ui::FlatLabel *AddSubsectionTitle(
	Ui::VerticalLayout *container,
	const QString &text);
Ui::FlatLabel *AddFootnote(Ui::VerticalLayout *container, const QString &text);
Ui::SettingsButton *AddButton(
	Ui::VerticalLayout *container,
	const QString &text,
	const style::icon *icon = nullptr,
	std::function<void()> clicked = nullptr);
// A row that opens something: a chevron on the trailing side.
Ui::SettingsButton *AddLinkButton(
	Ui::VerticalLayout *container,
	const QString &text,
	const style::icon *icon,
	std::function<void()> clicked);
// A row with a value that does nothing when clicked.
Ui::SettingsButton *AddInfoRow(
	Ui::VerticalLayout *container,
	const QString &text,
	const QString &value,
	const style::icon *icon = nullptr);
Ui::SettingsButton *AddToggle(
	Ui::VerticalLayout *container,
	const QString &text,
	bool checked,
	std::function<void(bool)> changed,
	const style::icon *icon = nullptr);

// «یکی را انتخاب کن»: a box of radio buttons; `chosen` gets the new value.
void ShowPickerBox(
	Window::SessionController *controller,
	const QString &title,
	const std::vector<Option> &options,
	const QString &current,
	std::function<void(const QString &value)> chosen);

class Page : public Window::SectionWidget {
public:
	Page(
		QWidget *parent,
		Window::SessionController *controller,
		Section section);

	QString sectionKey() const override;
	void setInnerFocus() override;

protected:
	[[nodiscard]] Ui::VerticalLayout *content() const {
		return _content;
	}
	[[nodiscard]] Ui::TopBar *bar() const {
		return _bar;
	}
	// Replace the rows with a skeleton / an error with «دوباره» / the rows.
	void showLoading();
	void showError(const Api::Error &error, std::function<void()> retry);
	void showContent();
	// Removes every row (before building the page again).
	void clearContent();

	void resizeEvent(QResizeEvent *e) override;

private:
	void updateGeometries();

	Section _section;
	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::RpWidget *_wrap = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	QPointer<Ui::SkeletonList> _skeleton;
	QPointer<Ui::EmptyPlaceholder> _error;

};

// A page over the synced settings document (GET / PATCH /v1/me/settings): the
// rows read their values from Data::Graph and follow settingsChanged (also the
// rollback of a failed save).
class SyncedPage : public Page {
public:
	SyncedPage(
		QWidget *parent,
		Window::SessionController *controller,
		Section section);

protected:
	// Called once the document is here; add the rows to content().
	virtual void build() = 0;
	// Call at the end of the constructor of the final class.
	void load();
	// The same, but the rows that are there already stay on screen: the
	// state («در حال بارگذاری…», the error with «دوباره») is a row at the end.
	void loadInline();

	[[nodiscard]] QJsonValue value(const QString &path) const;
	[[nodiscard]] bool boolean(const QString &path, bool fallback) const;
	[[nodiscard]] QString string(
		const QString &path,
		const QString &fallback) const;
	void save(const QString &path, const QJsonValue &value);
	void savePatch(const QJsonObject &patch);

	Ui::SettingsButton *addToggle(
		const QString &text,
		const QString &path,
		bool fallback);
	Ui::SettingsButton *addPicker(
		const QString &text,
		const QString &path,
		std::vector<Option> options,
		const QString &fallback);
	// Runs now and on every change of the document.
	void onRefresh(std::function<void()> refresh);

private:
	void removeRow(QPointer<Ui::RpWidget> &row);

	std::vector<std::function<void()>> _refreshers;
	QPointer<Ui::RpWidget> _inlineState;
	QPointer<Ui::RpWidget> _inlineRetry;
	bool _built = false;

};

} // namespace Settings
