// Small form controls of the conversation boxes.
//
//   ChipFlow        chips that wrap onto several lines (categories, durations):
//                   one of them is selected
//   IdentityPicker  «با نام X» | «ناشناس», two options side by side
//   ExpiryHeader    ring + «N ساعت مانده» + the category tag (+ a "more" button)
#pragma once

#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"

#include <QtCore/QPointer>

#include <functional>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Support {
class UserpicButton;
} // namespace Support

namespace Conversation {

class ChipFlow final : public Ui::RpWidget {
public:
	struct Item {
		QString id;
		QString text;
	};

	explicit ChipFlow(QWidget *parent);

	void setItems(const std::vector<Item> &items);
	void setActive(const QString &id);
	[[nodiscard]] QString active() const {
		return _active;
	}
	void setChangedCallback(std::function<void(const QString &id)> callback) {
		_changed = std::move(callback);
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	struct Chip {
		QString id;
		Ui::PillButton *button = nullptr;
	};

	std::vector<Chip> _chips;
	QString _active;
	std::function<void(const QString &id)> _changed;

};

class IdentityPicker final : public Ui::RpWidget {
public:
	IdentityPicker(QWidget *parent, Main::Session *session);

	void setAnonymous(bool anonymous);
	[[nodiscard]] bool anonymous() const {
		return _anonymous;
	}
	void setChangedCallback(std::function<void(bool anonymous)> callback) {
		_changed = std::move(callback);
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	class Option;

	Option *_named = nullptr;
	Option *_masked = nullptr;
	bool _anonymous = true;
	std::function<void(bool anonymous)> _changed;

};

class ExpiryHeader final : public Ui::RpWidget {
public:
	ExpiryHeader(QWidget *parent, qint64 expiresAt, const QString &category);

	// A trailing "more" button; `callback` gets the button to anchor a menu.
	void setMoreCallback(std::function<void(QWidget *anchor)> callback);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void scheduleTick();

	qint64 _expiresAt = 0;
	QString _category;
	Ui::IconButton *_more = nullptr;

};

} // namespace Conversation
