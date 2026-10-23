#include "data/data_session.h"

#include "api/api_client_config.h"
#include "data/data_bots.h"
#include "data/data_calls.h"
#include "data/data_chats.h"
#include "data/data_conversations.h"
#include "data/data_feed.h"
#include "data/data_graph.h"
#include "data/data_groups.h"
#include "data/data_media.h"
#include "data/data_messages.h"
#include "data/data_notes.h"
#include "data/data_notifications.h"
#include "data/data_outbox.h"
#include "data/data_stickers.h"
#include "data/data_stories.h"
#include "data/data_sync.h"
#include "data/data_transport.h"
#include "data/data_upload.h"
#include "data/data_users.h"

#include <utility>

namespace Data {

Session::Session(Config config, QObject *parent)
: QObject(parent)
, _config(std::move(config)) {
	Q_ASSERT(_config.transport && _config.api);
	qRegisterMetaType<Api::Error>();
	// The stores that use others come last (and go first: ~Session).
	_users = new Users(this);
	_chats = new Chats(this);
	_messages = new Messages(this);
	_upload = new Upload(this);
	_media = new Media(this);
	_outbox = new Outbox(this);
	_groups = new Groups(this);
	_notifications = new Notifications(this);
	_conversations = new Conversations(this);
	_sync = new Sync(this);
	_stickers = new Stickers(this);
	_feed = new Feed(this);
	_graph = new Graph(this);
	_calls = new Calls(this);
	_bots = new Bots(this);
	_stories = new Stories(this);
	_notes = new Notes(this);

	// A card that arrives or changes renames the rows that show it.
	connect(_users, &Users::changed, _chats, [this](const QStringList &ids) {
		for (const auto &id : ids) {
			_chats->applyCard(_users->peek(id));
		}
	});
}

Session::~Session() {
	stop();
	// Qt would delete the children in the order they were created, the
	// stores the others use first: they go in the reverse order, each while
	// the stores it uses are still there.
	delete std::exchange(_notes, nullptr);
	delete std::exchange(_stories, nullptr);
	delete std::exchange(_bots, nullptr);
	delete std::exchange(_calls, nullptr);
	delete std::exchange(_graph, nullptr);
	delete std::exchange(_feed, nullptr);
	delete std::exchange(_stickers, nullptr);
	delete std::exchange(_sync, nullptr);
	delete std::exchange(_conversations, nullptr);
	delete std::exchange(_notifications, nullptr);
	delete std::exchange(_groups, nullptr);
	delete std::exchange(_outbox, nullptr);
	delete std::exchange(_media, nullptr);
	delete std::exchange(_upload, nullptr);
	delete std::exchange(_messages, nullptr);
	delete std::exchange(_chats, nullptr);
	delete std::exchange(_users, nullptr);
}

int Session::cardsBatchSize() const {
	return _config.clientConfig ? _config.clientConfig->cardsBatchMax() : 50;
}

void Session::start(Fn<void()> ready) {
	if (_started) {
		if (ready) {
			ready();
		}
		return;
	}
	_started = true;
	_users->loadCachedMe(this, nullptr);
	_conversations->loadDismissed();
	_chats->loadCached(this, [=, this](bool) {
		_groups->start();
		_sync->start(this, [=, this] {
			_outbox->start(this, [=, this] {
				if (!_started) {
					return;
				}
				// The cached state is on screen; now the server's.
				if (ready) {
					ready();
				}
				_chats->loadInbox();
				// The inbox does not list groups: their own catch-up (the
				// connection asks again with every Hello).
				_groups->requestSync();
				_users->refreshMe();
				_notifications->refreshUnread();
				_graph->retryDismissals();
			});
		});
	});
}

void Session::stop() {
	if (!_started) {
		return;
	}
	_started = false;
	_feed->flushViews();
	_stories->flushSeen();
	_sync->stop();
	_groups->stop();
	_outbox->stop();
}

void Session::reportFailure(
		const QString &action,
		const QString &subject,
		const Api::Error &error) {
	// The subject is a chat / user / post id: for the log of a build with the
	// debug tools only.
#ifdef ELEZA_DEBUG_TOOLS
	qInfo(
		"Data: %s (%s) failed: %s",
		qPrintable(action),
		qPrintable(subject),
		qPrintable(error.forLog()));
#else // ELEZA_DEBUG_TOOLS
	qInfo(
		"Data: %s failed: %s",
		qPrintable(action),
		qPrintable(error.forLog()));
#endif // ELEZA_DEBUG_TOOLS
	Q_EMIT actionFailed(action, subject, error);
}

} // namespace Data
