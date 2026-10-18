#include "data/data_bots.h"

#include "api/api_client.h"
#include "base/unixtime.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_users.h"

namespace Data {
namespace {

// The server waits up to bots.inline.answer_wait (8 s by default, 25 s at
// most) for the bot.
constexpr auto kInlineTimeoutMs = 30 * 1000;

[[nodiscard]] QString HandleKey(const QString &handle) {
	auto result = handle.trimmed().toLower();
	if (result.startsWith(u'@')) {
		result.remove(0, 1);
	}
	return result;
}

} // namespace

BotCommand BotCommand::FromJson(const QJsonObject &json) {
	auto result = BotCommand();
	result.command = Json::String(json, "command"_L1);
	if (result.command.startsWith(u'/')) {
		result.command.remove(0, 1);
	}
	result.description = Json::String(json, "description"_L1);
	return result;
}

BotInfo BotInfo::FromJson(const QJsonObject &json) {
	auto result = BotInfo();
	result.id = Json::String(json, "id"_L1);
	result.handle = Json::String(json, "handle"_L1);
	result.name = Json::Name(json, "name"_L1);
	result.about = Json::String(json, "about"_L1);
	result.description = Json::String(json, "description"_L1);
	result.verified = Json::Bool(json, "verified"_L1);
	result.official = Json::Bool(json, "official"_L1);
	for (const auto &command : Json::List<BotCommand>(json.value("commands"_L1))) {
		if (!command.command.isEmpty()) {
			result.commands.push_back(command);
		}
	}
	const auto inlineMode = json.value("inline"_L1).toObject();
	result.inlineEnabled = Json::Bool(inlineMode, "enabled"_L1);
	result.inlinePlaceholder = Json::String(inlineMode, "placeholder"_L1);
	return result;
}

InlineResult InlineResult::FromJson(const QJsonObject &json) {
	auto result = InlineResult();
	result.type = Json::String(json, "type"_L1);
	result.id = Json::String(json, "id"_L1);
	result.title = Json::String(json, "title"_L1);
	result.description = Json::String(json, "description"_L1);
	result.thumbUrl = Json::String(json, "thumb_url"_L1);
	return result;
}

InlineResults InlineResults::FromJson(const QJsonObject &json) {
	auto result = InlineResults();
	result.queryId = Json::String(json, "query_id"_L1);
	for (const auto &item : Json::List<InlineResult>(json.value("results"_L1))) {
		if (!item.id.isEmpty()) {
			result.results.push_back(item);
		}
	}
	result.nextOffset = Json::String(json, "next_offset"_L1);
	result.answered = Json::Bool(json, "answered"_L1);
	return result;
}

Bots::Bots(Session *session)
: QObject(session)
, _session(session) {
}

const BotInfo *Bots::info(const QString &botId) const {
	const auto i = _entries.constFind(botId);
	return (i != _entries.cend()) ? &i->info : nullptr;
}

void Bots::load(
		const QString &botId,
		base::Guard guard,
		Fn<void(const BotInfo &)> done,
		FailCallback fail) {
	if (botId.isEmpty()) {
		return;
	}
	if (const auto i = _entries.constFind(botId); i != _entries.cend()) {
		const auto stale = (base::unixtime::nowMs() - i->loadedAt) > kInfoTtlMs;
		const auto copy = i->info;
		if (stale && !_loading.contains(botId)) {
			// Answered now; whatever changed comes with `changed`.
			_loading.insert(botId, {});
			fetch(botId);
		}
		if (done && guard.alive()) {
			done(copy);
		}
		return;
	}
	const auto fresh = !_loading.contains(botId);
	_loading[botId].push_back({ std::move(guard), std::move(done), std::move(fail) });
	if (fresh) {
		fetch(botId);
	}
}

void Bots::fetch(const QString &botId) {
	_session->api().request(Api::Get, u"/v1/bots/"_s + PathSegment(botId))
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto info = BotInfo::FromJson(result);
			if (info.id.isEmpty()) {
				info.id = botId;
			}
			store(info);
			const auto waiters = _loading.take(botId);
			const auto copy = _entries.value(botId).info;
			for (const auto &waiter : waiters) {
				if (waiter.done && waiter.guard.alive()) {
					waiter.done(copy);
				}
			}
		})
		.fail([=, this](const Api::Error &error) {
			const auto waiters = _loading.take(botId);
			for (const auto &waiter : waiters) {
				if (waiter.fail && waiter.guard.alive()) {
					waiter.fail(error);
				}
			}
		})
		.send();
}

void Bots::store(const BotInfo &info) {
	auto &entry = _entries[info.id];
	entry.info = info;
	entry.loadedAt = base::unixtime::nowMs();
	if (!info.handle.isEmpty()) {
		_handles.insert(HandleKey(info.handle), info.id);
	}
	Q_EMIT changed(info.id);
}

Bots::HandleState Bots::handleState(const QString &handle) const {
	const auto i = _handles.constFind(HandleKey(handle));
	if (i == _handles.cend()) {
		return HandleState::Unknown;
	} else if (i->isEmpty() || !_entries.contains(*i)) {
		return HandleState::NotBot;
	}
	return HandleState::Bot;
}

const BotInfo *Bots::byHandle(const QString &handle) const {
	const auto i = _handles.constFind(HandleKey(handle));
	return (i != _handles.cend() && !i->isEmpty()) ? info(*i) : nullptr;
}

void Bots::resolve(const QString &handle, base::Guard guard, Fn<void()> done) {
	const auto key = HandleKey(handle);
	if (key.isEmpty() || handleState(key) != HandleState::Unknown) {
		if (done && guard.alive()) {
			done();
		}
		return;
	}
	const auto fresh = !_resolving.contains(key);
	_resolving[key].push_back({ std::move(guard), std::move(done) });
	if (!fresh) {
		return;
	}
	const auto finish = [=, this] {
		const auto waiters = _resolving.take(key);
		for (const auto &[guard, done] : waiters) {
			if (done && guard.alive()) {
				done();
			}
		}
	};
	_session->api().request(Api::Get, u"/v1/users/by-handle/"_s + PathSegment(key))
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			const auto profile = Profile::FromJson(result);
			if (!profile.deleted && profile.valid()) {
				_session->users().seed(QVector<User>{ profile.card() });
			}
			if (!profile.isBot || profile.id.isEmpty() || profile.deleted) {
				_handles.insert(key, QString());
				finish();
				return;
			}
			load(profile.id, this, [=, this](const BotInfo &info) {
				// A bot renamed meanwhile still answers to the typed handle.
				_handles.insert(key, info.id);
				finish();
			}, [=](const Api::Error &) {
				finish();
			});
		})
		.fail([=, this](const Api::Error &error) {
			if (error.notFound()) {
				_handles.insert(key, QString());
			}
			finish();
		})
		.send();
}

void Bots::inlineQuery(
		const QString &botId,
		const QString &query,
		const QString &offset,
		base::Guard guard,
		Fn<void(const InlineResults &)> done,
		FailCallback fail) {
	auto body = QJsonObject{ { u"query"_s, query } };
	Json::Put(body, "offset"_L1, offset);
	_session->api().request(
			Api::Post,
			u"/v1/bots/%1/inline-query"_s.arg(PathSegment(botId)))
		.body(body)
		.noIdempotencyKey()
		.timeout(kInlineTimeoutMs)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			if (done) {
				done(InlineResults::FromJson(result));
			}
		})
		.fail(Guarded(guard, std::move(fail)))
		.send();
}

} // namespace Data
