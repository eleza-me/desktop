// The boxes of the chat screen: sending files, a location, the chat's theme,
// and the questions asked before deleting / forwarding.
#pragma once

#include "data/data_types.h"
#include "ui/layers/box_content.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace History {

struct FilesToSend {
	QStringList paths;
	QString caption;
	QVector<Data::Entity> entities;
	bool asFiles = false; // without compression, under their own names
	bool spoiler = false; // photos / videos hidden until clicked
};

// Previews of the chosen files, a caption, «send as files» and «hide with a
// spoiler». `send` is called once, right before the box closes.
[[nodiscard]] Ui::object_ptr<Ui::BoxContent> SendFilesBox(
	Main::Session *session,
	const QStringList &paths,
	const QString &caption,
	Fn<void(const FilesToSend &)> send);

// Coordinates typed by hand or pasted as a map link (a desktop has no GPS).
[[nodiscard]] Ui::object_ptr<Ui::BoxContent> LocationBox(
	Fn<void(const Data::Location &)> send);
// "35.6892, 51.3890", "geo:35.68,51.38", a map link with "@lat,lng" or
// "q=lat,lng". Nullopt when no coordinates are found.
[[nodiscard]] std::optional<Data::Location> ParseLocation(const QString &text);

[[nodiscard]] Ui::object_ptr<Ui::BoxContent> ChatThemeBox(
	Main::Session *session,
	const QString &chatId);

// «Delete this message?» with the «also delete for …» checkbox where the
// server allows it. `done` runs after the messages were removed.
void ShowDeleteMessagesBox(
	Window::SessionController *controller,
	const QString &chatId,
	const QVector<Data::Message> &messages,
	Fn<void()> done = nullptr);

// The chat chooser. One chat: it opens with the messages in its composer's
// bar (a comment can be added); several: the messages go out at once.
void ShowForwardBox(
	Window::SessionController *controller,
	const QVector<Data::Message> &messages,
	Fn<void()> done = nullptr);

// Clearing the history for me / deleting the chat (with «also delete for …»).
void ShowClearHistoryBox(
	Window::SessionController *controller,
	const QString &chatId);
void ShowDeleteChatBox(
	Window::SessionController *controller,
	const QString &chatId);

} // namespace History
