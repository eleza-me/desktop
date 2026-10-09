// «جستجو» — the side column of the search tab: the field, the browse state
// (recent searches, hot hashtags, new videos, people you may know) and the
// results by scope (people, topics, posts, open conversations). Clicking a row
// opens it through its route: in the main column on a wide window, over the
// list in the single column mode.
#pragma once

#include "conversation/conversation_card.h"
#include "data/data_media.h"
#include "search/search_debounce.h"
#include "search/search_people.h"
#include "search/search_recent.h"
#include "ui/widgets/action_list.h"
#include "window/window_section_widget.h"

#include <QtCore/QHash>
#include <QtCore/QSet>

#include <memory>
#include <optional>
#include <vector>

namespace Ui {
class ChipsRow;
class InfiniteRadialAnimation;
class ScrollArea;
class SearchField;
class SkeletonList;
class VerticalLayout;
} // namespace Ui

namespace Window {
class EmbeddedList;
} // namespace Window

namespace Data {
class Session;
} // namespace Data

namespace Search {

class Widget final
	: public Window::SectionWidget
	, private Ui::ActionListDelegate {
public:
	Widget(QWidget *parent, Window::SessionController *controller);
	~Widget();

	// The route `search?q=…`.
	void setQuery(const QString &query);

	QString sectionKey() const override;
	void setInnerFocus() override;
	bool onBackRequest() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	struct Row;
	struct Browse;
	struct Results;
	struct Poster;

	// Ui::ActionListDelegate
	int rowsCount() const override;
	int rowHeight(int index, int width) const override;
	void paintRow(
		Painter &p,
		int index,
		const QRect &rect,
		const Ui::ListRowState &state) override;
	bool rowClickable(int index) const override;
	int rowActionAt(int index, QPoint point, int width) const override;

	[[nodiscard]] Data::Session &data() const;

	// ---- state
	void queryFired(const QString &query);
	void setScope(const QString &scope);
	void loadBrowse();
	void browsePartDone();
	void fillSuggestions();
	void runSearch();
	void searchPartDone(int generation);
	void loadMoreResults();
	void rebuild();
	void buildBrowse();
	void buildResults();
	void refreshList();
	void updateSkeleton();
	void resetEmbed();
	void ensureEmbed();
	void updateEmbedRange();

	// ---- rows
	Row &addRow(int kind);
	void addHeader(const QString &title, const QString &link = QString(), int action = 0);
	void addPerson(const Data::Profile &profile, bool suggestion);
	void addState(
		const style::icon *icon,
		const QString &title,
		const QString &text,
		const QString &button = QString(),
		int action = 0);
	void paintHeader(Painter &p, const Row &row, int width, int index);
	void paintRecent(Painter &p, const Row &row, int width, int index);
	void paintTrend(Painter &p, const Row &row, int width);
	void paintVideos(Painter &p, const Row &row, int width, int index);
	void paintPerson(Painter &p, const Row &row, int width, int index);
	void paintPost(Painter &p, const Row &row, int width, int height);
	void paintState(Painter &p, const Row &row, int width, int index);
	[[nodiscard]] QRect followRect(const Row &row, int width) const;
	[[nodiscard]] QRect dismissRect(const Row &row, int width) const;
	[[nodiscard]] QRect headerLinkRect(const Row &row, int width) const;
	[[nodiscard]] QRect stateButtonRect(const Row &row, int width) const;
	[[nodiscard]] QRect videoRect(int cell, int width) const;
	[[nodiscard]] int stateHeight(const Row &row, int width) const;

	// ---- actions
	void rowActivated(int index);
	void actionTriggered(int index, int action);
	void showRowMenu(int index, QPoint globalPosition);
	void toggleFollow(const QString &userId);
	void applyFollow(const QString &userId, bool follow);
	void setFollowState(const QString &userId, FollowState state, bool busy);
	void dismissSuggestion(const QString &userId);
	void rememberQuery();
	void saveRecent();
	void runRecent(const QString &query);
	void openNewConversation();
	void requestPoster(const QString &postId);
	void requestTopicInfo(const QStringList &slugs);

	Ui::SearchField *_field = nullptr;
	Ui::ChipsRow *_scopes = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_inner = nullptr;
	Ui::ActionList *_list = nullptr;
	Ui::SkeletonList *_skeleton = nullptr;
	Window::EmbeddedList *_embed = nullptr;
	QString _embedQuery;
	bool _embedEmpty = false;

	Conversation::CardPainter _cards;
	Support::UserpicPainter _userpics;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _loader;
	Debouncer _debounce;
	RecentList _recent;
	QSet<QString> _hiddenPeople;
	QStringList _hiddenPeopleOrder;

	std::vector<std::unique_ptr<Row>> _rows;
	std::unique_ptr<Browse> _browse;
	std::unique_ptr<Results> _results;
	QHash<QString, Poster> _posters;
	QHash<QString, Data::TopicInfo> _topicInfo;
	QSet<QString> _topicInfoRequested;
	QSet<QString> _followBusy;
	QString _query;
	QString _scope;

};

} // namespace Search
