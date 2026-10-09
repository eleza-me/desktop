// «موضوع‌ها» — the open topics of the «گفتگو» tab: a «new topic» field, category
// chips, topic cards with their expiry rings, paging, live refresh on the
// server's hints. Lives as a side sub-tab of the chats column (the widget
// scrolls itself); TopicsSectionFactory() shows the same list as a section.
#pragma once

#include "conversation/conversation_card.h"
#include "base/timer.h"
#include "ui/rp_widget.h"
#include "ui/widgets/action_list.h"
#include "ui/widgets/misc_widgets.h"
#include "window/window_registry.h"

namespace Ui {
class ChipsRow;
class EmptyPlaceholder;
class InfiniteRadialAnimation;
class RippleButton;
class ScrollArea;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Data {
class Session;
} // namespace Data

namespace Conversation {

class TopicsList final
	: public Ui::RpWidget
	, private Ui::ActionListDelegate {
public:
	TopicsList(QWidget *parent, Window::SessionController *controller);
	~TopicsList();

	void setInnerFocus() {
		setFocus();
	}

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	class Skeleton;

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
	void setupChips();
	void selectCategory(const QString &slug);
	void load(bool reset);
	void refresh();
	void refreshPage(
		int generation,
		int pagesLeft,
		const QString &cursor,
		std::shared_ptr<QVector<Data::OpenConversation>> collected);
	void loadMore();
	void setItems(const QVector<Data::OpenConversation> &items);
	void appendItems(const QVector<Data::OpenConversation> &items);
	[[nodiscard]] bool visibleItem(const Data::OpenConversation &item) const;
	void removeCard(const QString &id);
	void sweepExpired();
	void scheduleTick();
	void tick();
	void updateOfflinePolling();
	void hintReceived(const QString &category);
	void contentChanged();
	void updateStateWidgets();
	void activate(int index);
	[[nodiscard]] bool isLoaderRow(int index) const;

	Window::SessionController *_controller = nullptr;
	CardPainter _painter;
	Ui::RippleButton *_compose = nullptr;
	Ui::ChipsRow *_chips = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	Ui::ActionList *_list = nullptr;
	Skeleton *_skeleton = nullptr;
	Ui::object_ptr<Ui::EmptyPlaceholder> _placeholder = { nullptr };
	std::unique_ptr<Ui::InfiniteRadialAnimation> _tailAnimation;

	std::vector<Card> _cards;
	QString _category;
	QString _next;
	std::optional<Api::Error> _error;
	QString _placeholderKey;
	int _generation = 0;
	int _pagesShown = 1;
	bool _loading = false;
	bool _loadedOnce = false;
	bool _dismissedLoaded = false;
	bool _dirtyWhileHidden = false;
	bool _watching = false;

	base::Timer _tickTimer;
	base::Timer _hintTimer;
	base::Timer _offlineTimer;

};

// The list as a section of its own (when no chat list hosts it, and for tests).
[[nodiscard]] Window::SectionFactory TopicsSectionFactory();

} // namespace Conversation
