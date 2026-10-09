// The post lists of a profile. The real cards come from the feed module through
// the "eli.posts" embed (window/window_embeds.h); when nobody registered it the
// compact list below shows the same posts as text rows that open the post.
#pragma once

#include "data/data_social_types.h"
#include "ui/text/text.h"
#include "window/window_embeds.h"

#include <memory>
#include <vector>

class Painter;

namespace Ui {
class InfiniteRadialAnimation;
class PopupMenu;
} // namespace Ui

namespace Profile {

// source: "user" | "user_replies" | "user_media" | "bookmarks".
[[nodiscard]] Ui::object_ptr<Window::EmbeddedList> CreatePostsList(
	QWidget *parent,
	Window::SessionController *controller,
	const QString &source,
	const QString &userId);

// Whether a post belongs to the «رسانه» tab.
[[nodiscard]] bool IsMediaPost(const Data::Post &post);

class CompactPosts final : public Window::EmbeddedList {
public:
	CompactPosts(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &source,
		const QString &userId);
	~CompactPosts();

	void setVisibleRange(int top, int bottom) override;
	void loadMore() override;
	void reload() override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Row {
		QString id;
		Ui::Text::String text;
		QString time;
		QString likes;
		QString replies;
		const style::icon *kind = nullptr;
		bool pinned = false;
		int top = 0;
		int height = 0;
	};

	void load();
	void pageLoaded(const Data::PostsPage &page);
	void failed(const Api::Error &error);
	void appendPosts(const QStringList &ids);
	void fillRow(Row &row, const Data::Post &post);
	void removeRow(const QString &id);
	void recount();
	void recount(int forWidth);
	void refreshHeight();
	[[nodiscard]] int rowAt(QPoint position) const;
	void setOver(int index);
	void updateRow(int index);
	void paintRow(Painter &p, const Row &row, bool over, bool pressed);
	[[nodiscard]] int textWidth(int forWidth) const;

	Window::SessionController *_controller = nullptr;
	QString _source;
	QString _userId;
	std::vector<std::unique_ptr<Row>> _rows;
	QString _next;
	int _mediaPages = 0;
	int _generation = 0;
	bool _loading = false;
	bool _loadedOnce = false;
	bool _allLoaded = false;
	bool _empty = false;
	QString _error;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	int _over = -1;
	int _pressed = -1;
	int _countedWidth = 0;
	int _rowsHeight = 0;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _spinner;
	QPointer<Ui::PopupMenu> _menu;

};

} // namespace Profile
