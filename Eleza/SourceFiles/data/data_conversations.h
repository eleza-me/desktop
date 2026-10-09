// Anonymous talker / listener conversations: the open-topics list, one
// conversation's view, create / join / end / continue / block, finding the
// chat a conversation lives in, and the local «do not show again» list.
//
// The calls use the `conv.*` realtime methods (HTTP when the connection is
// down). The open list is read once and then refreshed on the server's
// hints (watchOpen + openListChanged) - never on a timer.
#pragma once

#include "base/timer.h"
#include "data/data_social_types.h"

#include <QtCore/QSet>

namespace Data {

class Session;

struct ConversationCategory {
	QString slug;
	QString title;
};

struct ConversationConfig {
	QVector<ConversationCategory> categories;
	QStringList durations;      // "1h", "6h", "24h"
	QString defaultDuration;
	int topicMaxChars = 120;
	int maxConcurrent = 3;
};

struct NewConversation {
	QString topic;
	QString category;
	QString duration;           // one of ConversationConfig::durations
	bool masked = false;        // hide my identity behind a mask
};

class Conversations final : public QObject {
	Q_OBJECT

public:
	explicit Conversations(Session *session);

	// The feature's limits (client-config, with the server's defaults).
	[[nodiscard]] ConversationConfig config() const;
	[[nodiscard]] QString categoryTitle(const QString &slug) const;
	// "24h" -> 86400 (a day when the text is not a duration).
	[[nodiscard]] static qint64 DurationSeconds(const QString &duration);

	// ---- open topics
	void loadOpen(const QString &category, const QString &cursor, base::Guard guard, Fn<void(const OpenConversationsPage &)> done, FailCallback fail = nullptr);
	// The first page of «all categories» as last loaded (instant reopen).
	[[nodiscard]] const std::optional<OpenConversationsPage> &cachedOpen() const {
		return _firstPage;
	}
	// Ask the server for change hints of a category ("" = all) while the
	// topics screen is visible; openListChanged() follows.
	void watchOpen(const QString &category);
	void unwatchOpen();

	// ---- one conversation
	void get(const QString &id, base::Guard guard, Fn<void(const Conversation &)> done, FailCallback fail = nullptr);
	void create(const NewConversation &request, base::Guard guard, Fn<void(const Conversation &)> done, FailCallback fail = nullptr);
	void join(const QString &id, bool masked, base::Guard guard, Fn<void(const Conversation &)> done, FailCallback fail = nullptr);
	void end(const QString &id, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	// Asks to continue as a direct chat (both sides must ask).
	void requestContinue(const QString &id, base::Guard guard, Fn<void(const Conversation &)> done, FailCallback fail = nullptr);
	void block(const QString &id, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);

	// The chat a conversation lives in. The inbox row is projected
	// asynchronously: `done` gets the chat id as soon as the row appears, or
	// an empty string after `timeoutMs`.
	void findChat(const QString &conversationId, base::Guard guard, Fn<void(const QString &)> done, int timeoutMs = 6000);

	// ---- local dismissals (per account, on this device)
	[[nodiscard]] bool isDismissed(const QString &id) const {
		return _dismissed.contains(id);
	}
	void dismiss(const QString &id);
	[[nodiscard]] QSet<QString> dismissed() const {
		return _dismissed;
	}
	void loadDismissed(base::Guard guard = base::Guard(), Fn<void()> done = nullptr);

	// ---- used by Data::Sync
	void notifyState(const QString &chat, const QString &state, const std::optional<Conversation> &conversation, const QString &event);
	void notifyOpenListChanged(const QString &category);

Q_SIGNALS:
	// A conversation's chat changed state (a listener joined, it ended, ...).
	// `event` is the server's name for what happened ("joined", "ended", "continue_requested", ...).
	void stateChanged(const QString &chat, const QString &state, const QString &event);
	// The open-topics list of `category` ("" = any) changed: reload the visible page once.
	void openListChanged(const QString &category);

private:
	struct Finder {
		QString conversationId;
		base::Guard guard;
		Fn<void(const QString &)> done;
		std::unique_ptr<base::Timer> timer;
		bool reloaded = false;
	};

	void call(const QString &method, const QJsonObject &body, base::Guard guard, Fn<void(const QJsonObject &)> done, FailCallback fail);
	void checkFinders();
	void finishFinder(int index, const QString &chat);

	Session *_session = nullptr;
	std::optional<OpenConversationsPage> _firstPage;
	QSet<QString> _dismissed;
	QStringList _dismissedOrder;
	std::vector<std::unique_ptr<Finder>> _finders;

};

} // namespace Data
