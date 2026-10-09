// The opened post (route eli/post/<id>) and the thread reader.
//
// PostSection resolves the root of whatever post was asked for (a reply opens
// its conversation with that reply highlighted), shows it as the large card
// with the date, the people counts and the follow button, the replies as a
// tree with one level of nesting (long branches fold behind «نمایش پاسخ‌های
// بیشتر», pages load as the end comes close) and a quick reply field docked at
// the bottom. It lives on the store afterwards: counts, edits, deletions and my
// own new replies show up without reloading.
//
// ThreadSection is the reading view of a thread: numbered parts in one
// column, a progress line under the top bar.
#pragma once

#include "data/data_social_types.h"
#include "window/window_section_widget.h"

#include <QtCore/QSet>

namespace Ui {
class EmptyPlaceholder;
class ScrollArea;
class SkeletonList;
class TopBar;
class VerticalLayout;
} // namespace Ui

namespace Eli {

class PostList;

class PostSection final : public Window::SectionWidget {
public:
	PostSection(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &postId);

	QString sectionKey() const override;
	void setInnerFocus() override;
	bool onBackRequest() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	class RepliesHeader;
	class RepliesTail;
	class ReplyDock;

	void load();
	void resolve(const Data::Post &post, int hop);
	void showRoot(const QString &rootId);
	void showError(const Api::Error &error);
	void loadReplies();
	void arrange();
	void refreshHeader();
	void refreshTail();
	void refreshDock();
	void setReplyTarget(const QString &postId);
	void sendReply();
	void toggleFollow();
	void applyHighlight();
	void updateLayout();

	QString _requestedId;
	QString _rootId;
	QString _highlightId;
	int _highlightTries = 0;

	Ui::TopBar *_bar = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	Ui::EmptyPlaceholder *_error = nullptr;
	PostList *_root = nullptr;
	RepliesHeader *_header = nullptr;
	PostList *_replies = nullptr;
	RepliesTail *_tail = nullptr;
	ReplyDock *_dock = nullptr;

	QStringList _replyIds;
	QString _cursor;
	bool _repliesLoading = false;
	bool _repliesDone = false;
	bool _repliesFailed = false;
	bool _repliesLoadedOnce = false;
	bool _newestFirst = false;
	QSet<QString> _expandedBranches;
	QString _replyTarget;
	bool _sending = false;
	int _generation = 0;

};

class ThreadSection final : public Window::SectionWidget {
public:
	ThreadSection(
		QWidget *parent,
		Window::SessionController *controller,
		const QString &postId);

	QString sectionKey() const override;
	void setInnerFocus() override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	class Content;
	class Progress;

	void load();
	void show(const Data::Post &post);

	QString _postId;
	Ui::TopBar *_bar = nullptr;
	Progress *_progress = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	Ui::EmptyPlaceholder *_error = nullptr;
	Content *_content = nullptr;

};

} // namespace Eli
