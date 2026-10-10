// The header of a profile: userpic, stats, name, handle, meta, link, bio,
// mutuals, the action buttons — and the smaller blocks of the profile screen
// (the open-conversation card, the locked / blocked states).
#pragma once

#include "data/data_social_types.h"
#include "profile/profile_logic.h"
#include "support/media_image.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"

namespace Ui {
class FlatLabel;
class IconButton;
class LinkButton;
class RoundButton;
class SettingsButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Profile {

class FollowButton;

// A number over a caption («۸۶ پست»); clickable ones open a list.
class StatButton final : public Ui::RippleButton {
public:
	explicit StatButton(QWidget *parent);

	void setValue(qint64 value, const QString &label);
	void setClickable(bool clickable);

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	QString _value;
	QString _label;
	bool _clickable = false;

};

class Cover final : public Ui::RpWidget {
	Q_OBJECT

public:
	Cover(QWidget *parent, Window::SessionController *controller);

	void setProfile(const Data::Profile &profile, const ButtonModel &model);
	void setFollow(FollowUi state, bool busy);
	void setFollowers(qint64 count);
	// "" hides the line.
	void setPresence(const QString &text, bool online);
	// The «درخواست‌های دنبال کردن» row of my own profile.
	void setRequests(bool shown, int count);
	[[nodiscard]] QWidget *moreButton() const;

Q_SIGNALS:
	void photoClicked();
	void followClicked();
	void messageClicked();
	void moreClicked();
	void editClicked();
	void shareClicked();
	void followersClicked();
	void followingClicked();
	void requestsClicked();
	void handleClicked();

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refreshMeta();

	Window::SessionController *_controller = nullptr;
	Data::Profile _profile;
	ButtonModel _model;
	Support::UserpicButton *_userpic = nullptr;
	StatButton *_posts = nullptr;
	StatButton *_followers = nullptr;
	StatButton *_following = nullptr;
	Ui::LinkButton *_handle = nullptr;
	Ui::LinkButton *_link = nullptr;
	Ui::FlatLabel *_bio = nullptr;
	FollowButton *_follow = nullptr;
	Ui::RoundButton *_message = nullptr;
	Ui::IconButton *_more = nullptr;
	Ui::RoundButton *_edit = nullptr;
	Ui::RoundButton *_share = nullptr;
	Ui::SettingsButton *_requests = nullptr;
	Support::UserpicPainter _mutualPics;

	Ui::Text::String _name;
	Ui::Text::String _meta;
	Ui::Text::String _mutuals;
	QString _presence;
	bool _online = false;
	bool _requestsShown = false;

	// Layout of the painted parts (logical coordinates).
	int _nameTop = 0;
	int _nameWidth = 0;
	int _presenceLeft = 0;
	int _presenceTop = 0;
	int _metaTop = 0;
	int _metaHeight = 0;
	int _linkIconTop = 0;
	int _mutualsTop = 0;
	int _mutualsTextLeft = 0;
	int _mutualsTextWidth = 0;

};

// The night card with somebody's open anonymous conversation.
class ConversationCard final : public Ui::RippleButton {
public:
	explicit ConversationCard(QWidget *parent);

	void setConversation(const Data::OpenConversation &conversation);
	[[nodiscard]] const QString &conversationId() const {
		return _id;
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	void refreshTime();
	[[nodiscard]] bool expired() const;

	QString _id;
	qint64 _expiresAt = 0;
	Ui::Text::String _topic;
	QString _time;
	base::Timer _timer;

};

// «این حساب خصوصی است», «… را مسدود کرده‌ای»: a glyph in a circle, a title,
// a text and an optional button.
class StateBlock final : public Ui::RpWidget {
public:
	struct Descriptor {
		const style::icon *icon = nullptr;
		QString title;
		QString text;
		QString buttonText;
		bool buttonLight = true;
		Fn<void()> buttonCallback;
	};
	StateBlock(QWidget *parent, Descriptor descriptor);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const style::icon *_icon = nullptr;
	Ui::Text::String _title;
	Ui::Text::String _text;
	Ui::RoundButton *_button = nullptr;
	int _titleTop = 0;
	int _textTop = 0;

};

} // namespace Profile
