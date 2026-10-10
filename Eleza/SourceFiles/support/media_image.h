// Images and userpics from the session's media cache (Data::Media) for widgets.
//
// A widget that shows one image:
//   view->setSource(Support::MakeImageSource(&session, Data::MediaRef::PostMedia(post, media),
//       Data::MediaVariant::Feed, { .blurhash = meta.blurhash, .aspect = { meta.width, meta.height } }));
//   view->setSource(Support::MakeMessageImageSource(&session, message));   // a photo / video poster
//
// A userpic widget bound to a user (follows card changes and, when asked, presence):
//   const auto pic = Ui::CreateChild<Support::UserpicButton>(this, &session, userId, st::userpicSizeRow);
//   pic->setShowOnline(true);
//   pic->setClickedCallback([=] { controller->openRoute(Window::Routes::User(userId)); });
//
// Userpics inside a custom painted list (chat list, members, search results):
//   Support::UserpicPainter _userpics{ &session, [=](const QString &userId) { updateRowsOf(userId); } };
//   ... in paintRow:  _userpics.paint(p, rect, user);          // image, or the initial until it loads
//                     _userpics.paint(p, rect, chat.peerUserId);  // by id: the card comes from Data::Users
//
// Everything here keeps the Data::MediaHandle alive while the image is in use,
// and survives the session ending (it just stops loading).
#pragma once

#include "data/data_media.h"
#include "data/data_types.h"
#include "ui/image/image_source.h"
#include "ui/image/userpic.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>

#include <functional>
#include <memory>

namespace Main {
class Session;
} // namespace Main

namespace Support {

struct ImageOptions {
	// Shown (blurred by the view) until the image is loaded.
	QImage preview;
	// Decoded into the preview when there is no `preview` image.
	QString blurhash;
	// Proportions of the media for the blurhash preview (any units).
	QSize aspect;
	// Decode no larger than this box in device pixels (empty: the size of the
	// first request, rounded up so resizing a little does not refetch).
	QSize fit;
	// Keep the original pixels whatever the target size is (viewers).
	bool original = false;
};

[[nodiscard]] std::shared_ptr<Ui::ImageSource> MakeImageSource(
	Main::Session *session,
	const Data::MediaRef &ref,
	Data::MediaVariant variant = Data::MediaVariant::Thumb,
	ImageOptions options = {});

// The picture of a photo / gif message or the poster of a video; the preview
// comes from the message's blurhash. Null for a message without visual media.
[[nodiscard]] std::shared_ptr<Ui::ImageSource> MakeMessageImageSource(
	Main::Session *session,
	const Data::Message &message,
	Data::MediaVariant variant = Data::MediaVariant::Thumb);

// Null when the user has no avatar (the widget shows the initial then).
[[nodiscard]] std::shared_ptr<Ui::ImageSource> UserpicSource(
	Main::Session *session,
	const Data::User &user,
	Data::MediaVariant variant = Data::MediaVariant::Thumb);

// The placeholder kind for a card (deleted accounts get the neutral circle).
[[nodiscard]] Ui::UserpicKind UserpicKindFor(const Data::User &user);

class UserpicButton : public Ui::UserpicWidget {
public:
	UserpicButton(
		QWidget *parent,
		Main::Session *session,
		const QString &userId,
		int size);

	void setUserId(const QString &userId);
	[[nodiscard]] QString userId() const {
		return _userId;
	}
	// The presence dot (subscribes to the user's presence while shown).
	void setShowOnline(bool show);
	// «Saved messages», an anonymous counterpart: no user card behind it. `id`
	// picks the color of an initial (a chat id, a mask ref).
	void setSpecial(
		Ui::UserpicKind kind,
		const QString &name = QString(),
		const QString &id = QString());

	~UserpicButton();

private:
	void refresh();
	void refreshOnline();
	void watch(bool watching);

	QPointer<Main::Session> _session;
	QString _userId;
	QString _avatarId;
	int _size = 0;
	bool _showOnline = false;
	bool _watching = false;
	bool _special = false;

};

// A read-only alias for places that only show the picture.
using UserpicView = UserpicButton;

class UserpicPainter final : public QObject {
public:
	// `update(userId)` is called when that user's picture arrived: repaint the
	// rows that show it.
	UserpicPainter(
		Main::Session *session,
		std::function<void(const QString &userId)> update,
		QObject *parent = nullptr);

	void paint(
		QPainter &p,
		const QRect &rect,
		const Data::User &user);
	// Looks the card up in Data::Users (and asks for it when unknown).
	void paint(QPainter &p, const QRect &rect, const QString &userId);
	// The picture of somebody without a card: a mask, «Saved messages».
	void paintSpecial(
		QPainter &p,
		const QRect &rect,
		Ui::UserpicKind kind,
		const QString &id = QString(),
		const QString &name = QString());

	// Drops prepared images (the list was hidden, the palette changed).
	void clear();

private:
	struct Entry {
		QString avatarId;
		Data::MediaHandlePtr handle;
		QImage prepared;
		int size = 0;
		double ratio = 0.;
		bool failed = false;
	};

	QPointer<Main::Session> _session;
	std::function<void(const QString &userId)> _update;
	QHash<QString, Entry> _entries;

};

} // namespace Support
