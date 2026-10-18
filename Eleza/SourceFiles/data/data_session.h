// Everything the signed-in user's screens read: one object that owns the
// stores and wires them together.
//
//   auto &data = session.data();          // Main::Session::data()
//   data.chats().list(Data::Chats::Section::Messages);
//   data.groups().group(groupId);         // a group is a row of the same list
//   data.messages().history(chatId).items;
//   data.outbox().sendText(chatId, text);
//   data.users().get(userId);
//   data.media().request(Data::MediaRef::Avatar(userId, avatarId), Data::MediaVariant::Thumb);
//
// Stores keep typed structs, emit Qt signals when they change, read the
// local database first and then live on realtime updates (no polling).
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"

#include <QtCore/QObject>

namespace Api {
class Client;
class ClientConfig;
} // namespace Api

namespace Storage {
class Database;
} // namespace Storage

namespace Data {

class Transport;
class Users;
class Calls;
class Chats;
class Groups;
class Messages;
class Sync;
class Outbox;
class Upload;
class Media;
class Stickers;
class Notifications;
class Feed;
class Graph;
class Conversations;
class Stories;

class Session final : public QObject {
	Q_OBJECT

public:
	struct Config {
		QString userId;
		Transport *transport = nullptr;          // required
		Api::Client *api = nullptr;              // required
		Storage::Database *database = nullptr;   // null: nothing is cached (tests)
		Api::ClientConfig *clientConfig = nullptr;
		QString cacheDir;                        // media cache directory ("" = memory only)
		qint64 mediaCacheLimit = qint64(512) << 20;
	};

	explicit Session(Config config, QObject *parent = nullptr);
	~Session();

	// Loads what the database remembers (profile, chat list, outbox, pts),
	// then starts sync and the outbox and reads the inbox once. `ready` runs
	// when the cached state is in the stores (the UI can show it at once).
	void start(Fn<void()> ready = nullptr);
	// Stops timers and in-flight work; the stores keep their state.
	void stop();
	[[nodiscard]] bool started() const {
		return _started;
	}

	[[nodiscard]] QString userId() const {
		return _config.userId;
	}
	[[nodiscard]] Transport &transport() const {
		return *_config.transport;
	}
	[[nodiscard]] Api::Client &api() const {
		return *_config.api;
	}
	// Null when nothing is cached.
	[[nodiscard]] Storage::Database *local() const {
		return _config.database;
	}
	[[nodiscard]] Api::ClientConfig *clientConfig() const {
		return _config.clientConfig;
	}
	[[nodiscard]] const Config &config() const {
		return _config;
	}
	[[nodiscard]] int cardsBatchSize() const;

	[[nodiscard]] Users &users() const {
		return *_users;
	}
	[[nodiscard]] Chats &chats() const {
		return *_chats;
	}
	[[nodiscard]] Messages &messages() const {
		return *_messages;
	}
	[[nodiscard]] Groups &groups() const {
		return *_groups;
	}
	[[nodiscard]] Sync &sync() const {
		return *_sync;
	}
	[[nodiscard]] Outbox &outbox() const {
		return *_outbox;
	}
	[[nodiscard]] Upload &upload() const {
		return *_upload;
	}
	[[nodiscard]] Media &media() const {
		return *_media;
	}
	[[nodiscard]] Stickers &stickers() const {
		return *_stickers;
	}
	[[nodiscard]] Notifications &notifications() const {
		return *_notifications;
	}
	[[nodiscard]] Feed &feed() const {
		return *_feed;
	}
	[[nodiscard]] Graph &graph() const {
		return *_graph;
	}
	[[nodiscard]] Conversations &conversations() const {
		return *_conversations;
	}
	[[nodiscard]] Calls &calls() const {
		return *_calls;
	}
	[[nodiscard]] Stories &stories() const {
		return *_stories;
	}

	// Reports a failed fire-and-forget action (see actionFailed).
	void reportFailure(const QString &action, const QString &subject, const Api::Error &error);

Q_SIGNALS:
	// An action without its own callback failed ("chat.mute", "message.react",
	// "draft.save", ...): one place for the UI to show a toast.
	// `subject` is the chat / post / user id it was about.
	void actionFailed(const QString &action, const QString &subject, const Api::Error &error);

private:
	Config _config;
	Users *_users = nullptr;
	Chats *_chats = nullptr;
	Messages *_messages = nullptr;
	Groups *_groups = nullptr;
	Upload *_upload = nullptr;
	Media *_media = nullptr;
	Outbox *_outbox = nullptr;
	Notifications *_notifications = nullptr;
	Conversations *_conversations = nullptr;
	Sync *_sync = nullptr;
	Stickers *_stickers = nullptr;
	Feed *_feed = nullptr;
	Graph *_graph = nullptr;
	Calls *_calls = nullptr;
	Stories *_stories = nullptr;
	bool _started = false;

};

} // namespace Data
