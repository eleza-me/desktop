// Bots as people see them (the backend's docs/bot-api.md, ADR-0031 …):
//
//   * `GET /v1/bots/{id}` — a bot's public information: name, about, the
//     description an empty chat shows above «شروع», its commands and whether
//     it answers inline queries. Kept in memory for the session and read
//     again in the background when it is older than kInfoTtlMs;
//   * which handles are bots (inline mode looks a typed handle up once:
//     `GET /v1/users/by-handle/{handle}` must say `is_bot`);
//   * `POST /v1/bots/{id}/inline-query {query, offset}` — one page of an
//     inline query's results.
//
// HTTP only: none of these is a realtime method.
//
//   auto &bots = session.data().bots();
//   if (const auto info = bots.info(botId)) { … }        // cached, or null
//   bots.load(botId, this, [=](const Data::BotInfo &info) { … });
//   bots.resolve(u"weather_bot"_s, this, [=] { … bots.byHandle(…) … });
//   bots.inlineQuery(botId, u"tehran"_s, QString(), this, [=](const Data::InlineResults &page) { … });
#pragma once

#include "data/data_social_types.h"

#include <QtCore/QHash>

namespace Data {

class Session;

struct BotCommand {
	QString command;     // without the slash
	QString description;

	[[nodiscard]] static BotCommand FromJson(const QJsonObject &json);
	friend bool operator==(const BotCommand &, const BotCommand &) = default;
};

struct BotInfo {
	QString id;
	QString handle;
	QString name;
	QString about;
	QString description;  // shown in an empty chat, above «شروع»
	bool verified = false;
	bool official = false;
	QVector<BotCommand> commands;
	bool inlineEnabled = false;
	QString inlinePlaceholder;

	[[nodiscard]] static BotInfo FromJson(const QJsonObject &json);
};

struct InlineResult {
	QString type;        // "article"
	QString id;
	QString title;
	QString description;
	QString thumbUrl;

	[[nodiscard]] static InlineResult FromJson(const QJsonObject &json);
};

struct InlineResults {
	QString queryId;     // a result is sent with the query it came from
	QVector<InlineResult> results;
	QString nextOffset;  // empty: no more pages
	bool answered = false; // false: the bot did not answer in time

	[[nodiscard]] static InlineResults FromJson(const QJsonObject &json);
};

class Bots final : public QObject {
	Q_OBJECT

public:
	explicit Bots(Session *session);

	enum class HandleState {
		Unknown, // never looked up (or the lookup failed)
		NotBot,  // no such account, or a person
		Bot,     // byHandle() has its information
	};

	// What is known of a bot, or null.
	[[nodiscard]] const BotInfo *info(const QString &botId) const;
	// From memory (a stale entry is answered at once and read again in the
	// background, `changed` follows) or from the server.
	void load(
		const QString &botId,
		base::Guard guard,
		Fn<void(const BotInfo &)> done,
		FailCallback fail = nullptr);

	[[nodiscard]] HandleState handleState(const QString &handle) const;
	[[nodiscard]] const BotInfo *byHandle(const QString &handle) const;
	// Looks a handle up once; `done` runs when handleState() is no longer
	// Unknown, or the lookup failed (it stays Unknown then: asked again the
	// next time).
	void resolve(const QString &handle, base::Guard guard, Fn<void()> done);

	void inlineQuery(
		const QString &botId,
		const QString &query,
		const QString &offset,
		base::Guard guard,
		Fn<void(const InlineResults &)> done,
		FailCallback fail = nullptr);

	static constexpr auto kInfoTtlMs = qint64(10 * 60 * 1000);

Q_SIGNALS:
	void changed(const QString &botId);

private:
	struct Entry {
		BotInfo info;
		qint64 loadedAt = 0;
	};
	struct Waiter {
		base::Guard guard;
		Fn<void(const BotInfo &)> done;
		FailCallback fail;
	};

	void fetch(const QString &botId);
	void store(const BotInfo &info);

	Session *_session = nullptr;
	QHash<QString, Entry> _entries;                // bot id -> info
	QHash<QString, QString> _handles;              // lower-case handle -> bot id ("" = not a bot)
	QHash<QString, QVector<Waiter>> _loading;      // bot id -> callbacks
	QHash<QString, QVector<std::pair<base::Guard, Fn<void()>>>> _resolving;

};

} // namespace Data
