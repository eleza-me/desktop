// Wrappers that show / hide a widget with an animation.
//
//   const auto wrap = Ui::CreateChild<Ui::FadeWrap<Ui::IconButton>>(
//       this, Ui::object_ptr<Ui::IconButton>(this, st::topBarSearch));
//   wrap->toggle(shown, anim::type::normal);   // fades in / out
//   wrap->entity()->setClickedCallback(...);
//
//   Ui::SlideWrap<T>  — same API, but the wrap animates its height (rows of a
//                       VerticalLayout appearing / collapsing).
//
// The wrap has the size of the entity (plus padding for SlideWrap) and follows it.
#pragma once

#include "ui/rp_widget.h"
#include "ui/effects/animations.h"

#include <QtGui/QPixmap>

#include <optional>

namespace Ui {

class FadeWrapBase : public RpWidget {
public:
	FadeWrapBase(QWidget *parent, object_ptr<RpWidget> entity);

	FadeWrapBase *toggle(bool shown, anim::type animated);
	FadeWrapBase *show(anim::type animated) {
		return toggle(true, animated);
	}
	FadeWrapBase *hide(anim::type animated) {
		return toggle(false, animated);
	}
	FadeWrapBase *setDuration(int duration);
	[[nodiscard]] bool toggled() const {
		return _shown;
	}
	[[nodiscard]] bool animating() const {
		return _animation.animating();
	}
	void finishAnimating();

	[[nodiscard]] RpWidget *entityBase() const {
		return _entity;
	}
	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void animationCallback();

	RpWidget *_entity = nullptr;
	bool _shown = true;
	int _duration = 0;
	QPixmap _cache;
	Animations::Simple _animation;

};

template <typename Entity = RpWidget>
class FadeWrap final : public FadeWrapBase {
public:
	FadeWrap(QWidget *parent, object_ptr<Entity> entity)
	: FadeWrapBase(parent, Owned<RpWidget>(entity.release())) {
	}

	[[nodiscard]] Entity *entity() const {
		return static_cast<Entity*>(entityBase());
	}

};

class SlideWrapBase : public RpWidget {
public:
	SlideWrapBase(
		QWidget *parent,
		object_ptr<RpWidget> entity,
		const style::margins &padding = style::margins());

	SlideWrapBase *toggle(bool shown, anim::type animated);
	SlideWrapBase *show(anim::type animated) {
		return toggle(true, animated);
	}
	SlideWrapBase *hide(anim::type animated) {
		return toggle(false, animated);
	}
	SlideWrapBase *setDuration(int duration);
	[[nodiscard]] bool toggled() const {
		return _shown;
	}
	[[nodiscard]] bool animating() const {
		return _animation.animating();
	}
	void finishAnimating();

	[[nodiscard]] RpWidget *entityBase() const {
		return _entity;
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] int fullHeight() const;
	void animationCallback();

	RpWidget *_entity = nullptr;
	style::margins _padding;
	bool _shown = true;
	int _duration = 0;
	bool _inResize = false;
	Animations::Simple _animation;

};

template <typename Entity = RpWidget>
class SlideWrap final : public SlideWrapBase {
public:
	SlideWrap(
		QWidget *parent,
		object_ptr<Entity> entity,
		const style::margins &padding = style::margins())
	: SlideWrapBase(parent, Owned<RpWidget>(entity.release()), padding) {
	}

	[[nodiscard]] Entity *entity() const {
		return static_cast<Entity*>(entityBase());
	}

};

// Adds padding around a widget (a row with margins that paints a background).
class PaddingWrapBase : public RpWidget {
public:
	PaddingWrapBase(
		QWidget *parent,
		object_ptr<RpWidget> entity,
		const style::margins &padding);

	void setBackground(std::optional<style::color> color);
	[[nodiscard]] RpWidget *entityBase() const {
		return _entity;
	}
	[[nodiscard]] int naturalWidth() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	RpWidget *_entity = nullptr;
	style::margins _padding;
	std::optional<style::color> _bg;
	bool _inResize = false;

};

template <typename Entity = RpWidget>
class PaddingWrap final : public PaddingWrapBase {
public:
	PaddingWrap(
		QWidget *parent,
		object_ptr<Entity> entity,
		const style::margins &padding)
	: PaddingWrapBase(parent, Owned<RpWidget>(entity.release()), padding) {
	}

	[[nodiscard]] Entity *entity() const {
		return static_cast<Entity*>(entityBase());
	}

};

} // namespace Ui
