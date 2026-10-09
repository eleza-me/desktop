// Profile::PeopleList — a virtualised list of people rows with a follow button
// per row (followers / following) or «پذیرفتن / رد کردن» (follow requests).
// Lives inside a Ui::ScrollArea; only the visible rows are painted, texts are
// laid out once per row.
#pragma once

#include "data/data_types.h"
#include "profile/profile_logic.h"
#include "support/media_image.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"

#include <memory>
#include <vector>

class Painter;

namespace Ui {
class InfiniteRadialAnimation;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Profile {

class PeopleList final : public Ui::RpWidget {
	Q_OBJECT

public:
	enum class Mode {
		Follow,
		Requests,
	};

	PeopleList(
		QWidget *parent,
		Window::SessionController *controller,
		Mode mode);
	~PeopleList();

	// My own followers: rows offer «حذف از دنبال‌کننده‌ها».
	void setRemovable(bool removable);
	void setPeople(const QVector<Data::Profile> &people);
	void appendPeople(const QVector<Data::Profile> &people);
	void removePerson(const QString &userId);
	// A lower-case filter over names and handles ("" shows everybody).
	void setFilter(const QString &query);
	// A spinner row under the last one while the next page loads.
	void setLoadingMore(bool loading);

	[[nodiscard]] int totalCount() const {
		return int(_rows.size());
	}
	[[nodiscard]] int shownCount() const {
		return int(_shown.size());
	}

	// Keyboard: Up / Down move the selection, Enter opens the profile.
	void selectSkip(int direction);
	bool chooseSelected();

Q_SIGNALS:
	void removeFollowerRequested(const QString &userId, const QString &name);
	void requestAnswered(const QString &userId, bool accepted);
	void scrollToRequested(int top, int bottom);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	enum class Part {
		None,
		Row,
		Primary, // follow / accept
		Secondary, // decline
		More,
	};
	struct Hit {
		int index = -1;
		Part part = Part::None;
		friend bool operator==(const Hit &a, const Hit &b) {
			return (a.index == b.index) && (a.part == b.part);
		}
	};
	struct Row {
		Data::Profile profile;
		FollowUi follow = FollowUi::None;
		bool busy = false;
		bool mine = false;
		QString search; // lower-case name + handle
		QString subtitle;
		Ui::Text::String name;
		bool nameReady = false;
	};
	struct Layout {
		QRect userpic; // all physical, relative to the row
		QRect primary;
		QRect secondary;
		QRect more;
		int textLeft = 0; // logical
		int textWidth = 0;
	};

	[[nodiscard]] Row *rowById(const QString &userId);
	[[nodiscard]] Row &shownRow(int index) {
		return *_rows[_shown[index]];
	}
	void addRow(const Data::Profile &profile);
	void refilter();
	void refreshHeight();
	[[nodiscard]] Layout layoutFor(const Row &row) const;
	[[nodiscard]] Hit hitTest(QPoint position) const;
	void setOver(Hit hit);
	void updateRow(int index);
	void paintRow(Painter &p, int index, int top);
	void activate(const Hit &hit, QPoint globalPosition);
	void toggleFollow(const QString &userId);
	void answerRequest(const QString &userId, bool accept);
	void showMenu(int index, QPoint globalPosition);
	void openProfile(const Row &row);
	[[nodiscard]] QString subtitleFor(const Row &row) const;

	Window::SessionController *_controller = nullptr;
	Mode _mode = Mode::Follow;
	bool _removable = false;
	bool _loadingMore = false;
	QString _filter;
	std::vector<std::unique_ptr<Row>> _rows;
	std::vector<int> _shown;
	Hit _over;
	Hit _pressed;
	int _selected = -1;
	Support::UserpicPainter _userpics;
	QPointer<Ui::PopupMenu> _menu;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _spinner;

};

} // namespace Profile
