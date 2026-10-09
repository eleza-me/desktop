// Small building blocks: unread badges, dividers, empty states, skeletons.
//
//   Ui::PaintBadge(p, Ui::BadgeText(unread), x, y, outerw, st::defaultBadge, muted);
//   new Ui::UnreadBadge(parent)->setCount(12);
//   new Ui::Divider(parent);                                   // a hairline row
//   new Ui::EmptyPlaceholder(parent, { .icon = &st::emptyChatsIcon, .title = ..., .text = ...,
//                                      .buttonText = ..., .buttonCallback = ... });
//   new Ui::SkeletonList(parent, Ui::SkeletonList::Kind::Chats);   // shimmering rows while loading
#pragma once

#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/effects/animations.h"
#include "styles/style_widgets.h"

#include <functional>

class Painter;

namespace Ui {

class RoundButton;

// "7", "99+" (digits follow the interface language through Ui::FormatNumber).
[[nodiscard]] QString BadgeText(int count, int limit = 99);

// Width of the badge for a text.
[[nodiscard]] int BadgeWidth(const QString &text, const style::Badge &st);

// Paints a pill with `text`; `x` is the distance from the trailing side of `outerw`
// to the badge (badges usually hug the trailing edge). Returns the badge width.
int PaintBadge(
	QPainter &p,
	const QString &text,
	int x,
	int y,
	int outerw,
	const style::Badge &st,
	bool muted = false);

// A small dot (unread marker without a number).
void PaintDot(QPainter &p, QPoint center, int diameter, style::color fg);

class UnreadBadge final : public RpWidget {
public:
	explicit UnreadBadge(
		QWidget *parent,
		const style::Badge &st = st::defaultBadge);

	void setCount(int count); // 0 hides the badge
	void setText(const QString &text);
	void setMuted(bool muted);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const style::Badge &_st;
	QString _text;
	bool _muted = false;

};

// A full-width separator line with optional insets.
class Divider final : public RpWidget {
public:
	explicit Divider(
		QWidget *parent,
		const style::Divider &st = st::defaultDivider);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const style::Divider &_st;

};

struct EmptyPlaceholderDescriptor {
	const style::icon *icon = nullptr;
	QString title;
	QString text;
	QString buttonText;
	std::function<void()> buttonCallback;
};

// Centered icon + title + text (+ button) for "nothing here yet" and error states.
class EmptyPlaceholder final : public RpWidget {
public:
	EmptyPlaceholder(
		QWidget *parent,
		EmptyPlaceholderDescriptor descriptor,
		const style::EmptyPlaceholder &st = st::defaultEmptyPlaceholder);

	void setTexts(const QString &title, const QString &text);
	// Height of the content block (to place it inside a list).
	[[nodiscard]] int contentHeight(int forWidth) const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void layout();

	const style::EmptyPlaceholder &_st;
	const style::icon *_icon = nullptr;
	Text::String _title;
	Text::String _text;
	RoundButton *_button = nullptr;
	int _contentTop = 0;

};

// A pill of text over the chat wallpaper ("pick a chat").
class ServiceLabel final : public RpWidget {
public:
	ServiceLabel(QWidget *parent, const QString &text);

	void setText(const QString &text);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	QString _text;

};

// Shimmer. Paint your own shapes with PaintSkeleton or use the widgets.
class SkeletonAnimation final {
public:
	explicit SkeletonAnimation(std::function<void()> update);

	void start();
	void stop();
	// Fills a rounded rect with the skeleton color and the moving shine.
	// `outer` is the widget rect the shine travels across.
	void paint(
		QPainter &p,
		const QRectF &rect,
		double radius,
		const QRect &outer) const;

private:
	std::function<void()> _update;
	Animations::Basic _animation;
	qint64 _started = 0;

};

class SkeletonList final : public RpWidget {
public:
	enum class Kind {
		Chats, // avatar + two lines
		Posts, // avatar + name + three lines of text
		Lines, // plain text lines
	};

	SkeletonList(QWidget *parent, Kind kind, int rows = 0);

protected:
	void paintEvent(QPaintEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] int rowHeight() const;

	Kind _kind = Kind::Chats;
	int _rows = 0; // 0 = fill the height
	SkeletonAnimation _animation;

};

} // namespace Ui
