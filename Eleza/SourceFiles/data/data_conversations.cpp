#include "data/data_conversations.h"

#include "api/api_client_config.h"
#include "data/data_chats.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"
#include "data/data_users.h"
#include "storage/storage_database.h"

namespace Data {
namespace {

const auto kDismissedKey = u"topics.dismissed"_s;
constexpr auto kMaxDismissed = 500;

} // namespace

Conversations::Conversations(Session *session)
: QObject(session)
, _session(session) {
}

ConversationConfig Conversations::config() const {
	auto result = ConversationConfig();
	// A default-constructed reader answers with the server's defaults.
	const auto own = _session->clientConfig();
	const auto fallback = Api::ClientConfig(nullptr, nullptr);
	const auto &source = own ? *own : fallback;
	for (const auto &category : source.conversationCategories()) {
		result.categories.push_back({ category.slug, category.title });
	}
	result.durations = source.conversationDurations();
	result.defaultDuration = source.conversationDefaultDuration();
	result.topicMaxChars = source.conversationTopicMaxChars();
	result.maxConcurrent = source.conversationMaxConcurrent();
	return result;
}

QString Conversations::categoryTitle(const QString &slug) const {
	if (slug.isEmpty()) {
		return QString();
	}
	for (const auto &category : config().categories) {
		if (category.slug == slug) {
			return category.title;
		}
	}
	return slug;
}

qint64 Conversations::DurationSeconds(const QString &duration) {
	const auto parsed = Api::ClientConfig::ParseDuration(duration);
	return (parsed > 0) ? parsed : 24 * 3600;
}

void Conversations::call(
		const QString &method,
		const QJsonObject &body,
		base::Guard guard,
		Fn<void(const QJsonObject &)> done,
		FailCallback fail) {
	_session->transport().rpc(method)
		.body(body)
		.guard(this)
		.done([=](const QJsonObject &result) {
			if (done && guard.alive()) {
				done(result);
			}
		})
		.fail([=, this](const Api::Error &error) {
			if (fail && guard.alive()) {
				fail(error);
			} else if (!fail) {
				_session->reportFailure(method, body.value("conversation"_L1).toString(), error);
			}
		})
		.send();
}

void Conversations::loadOpen(
		const QString &category,
		const QString &cursor,
		base::Guard guard,
		Fn<void(const OpenConversationsPage &)> done,
		FailCallback fail) {
	auto body = QJsonObject();
	Json::Put(body, "category"_L1, category);
	Json::Put(body, "cursor"_L1, cursor);
	_session->transport().rpc(u"conv.open"_s)
		.body(body)
		.guard(this)
		.done([=, this](const QJsonObject &result) {
			auto page = OpenConversationsPage();
			page.conversations = Json::List<OpenConversation>(result.value("conversations"_L1));
			page.nextCursor = Json::String(result, "next_cursor"_L1);
			auto speakers = QStringList();
			for (const auto &item : page.conversations) {
				if (!item.speakerId.isEmpty()) {
					speakers.push_back(item.speakerId);
				}
			}
			_session->users().request(speakers);
			if (category.isEmpty() && cursor.isEmpty()) {
				_firstPage = page;
			}
			if (done && guard.alive()) {
				done(page);
			}
		})
		.fail(Guarded(guard, fail))
		.send();
}

void Conversations::watchOpen(const QString &category) {
	_session->transport().subscribeTopics(category);
}

void Conversations::unwatchOpen() {
	_session->transport().unsubscribeTopics();
}

void Conversations::get(
		const QString &id,
		base::Guard guard,
		Fn<void(const Conversation &)> done,
		FailCallback fail) {
	call(u"conv.get"_s, QJsonObject{ { u"conversation"_s, id } }, guard, [=](const QJsonObject &result) {
		done(Conversation::FromJson(result));
	}, fail);
}

void Conversations::create(
		const NewConversation &request,
		base::Guard guard,
		Fn<void(const Conversation &)> done,
		FailCallback fail) {
	// `masked` has no explicit presence on the wire, so false simply stays out.
	call(u"conv.create"_s, QJsonObject{
		{ u"topic"_s, request.topic },
		{ u"category"_s, request.category },
		{ u"duration"_s, request.duration },
		{ u"masked"_s, request.masked },
	}, guard, [=](const QJsonObject &result) {
		done(Conversation::FromJson(result));
	}, fail);
}

void Conversations::join(
		const QString &id,
		bool masked,
		base::Guard guard,
		Fn<void(const Conversation &)> done,
		FailCallback fail) {
	call(u"conv.join"_s, QJsonObject{
		{ u"conversation"_s, id },
		{ u"masked"_s, masked },
	}, guard, [=](const QJsonObject &result) {
		done(Conversation::FromJson(result));
	}, fail);
}

void Conversations::end(const QString &id, base::Guard guard, Fn<void()> done, FailCallback fail) {
	call(u"conv.end"_s, QJsonObject{ { u"conversation"_s, id } }, guard, [=](const QJsonObject &) {
		if (done) {
			done();
		}
	}, fail);
}

void Conversations::requestContinue(
		const QString &id,
		base::Guard guard,
		Fn<void(const Conversation &)> done,
		FailCallback fail) {
	call(u"conv.continue"_s, QJsonObject{ { u"conversation"_s, id } }, guard, [=](const QJsonObject &result) {
		done(Conversation::FromJson(result));
	}, fail);
}

void Conversations::block(const QString &id, base::Guard guard, Fn<void()> done, FailCallback fail) {
	call(u"conv.block"_s, QJsonObject{ { u"conversation"_s, id } }, guard, [=](const QJsonObject &) {
		if (done) {
			done();
		}
	}, fail);
}

// ---- the chat of a conversation

void Conversations::findChat(
		const QString &conversationId,
		base::Guard guard,
		Fn<void(const QString &)> done,
		int timeoutMs) {
	const auto found = _session->chats().chatOfConversation(conversationId);
	if (!found.isEmpty()) {
		if (done && guard.alive()) {
			done(found);
		}
		return;
	}
	if (_finders.empty()) {
		connect(&_session->chats(), &Chats::listChanged, this, &Conversations::checkFinders, Qt::UniqueConnection);
	}
	auto finder = std::make_unique<Finder>();
	finder->conversationId = conversationId;
	finder->guard = std::move(guard);
	finder->done = std::move(done);
	const auto raw = finder.get();
	finder->timer = std::make_unique<base::Timer>([=, this] {
		for (auto i = 0; i != int(_finders.size()); ++i) {
			if (_finders[i].get() != raw) {
				continue;
			}
			if (!raw->reloaded) {
				// Half-way: the update that announces the row may have been
				// missed (no connection); read the inbox once.
				raw->reloaded = true;
				_session->chats().loadInbox(this, [=, this] { checkFinders(); });
				raw->timer->callOnce(timeoutMs / 2);
			} else {
				finishFinder(i, _session->chats().chatOfConversation(raw->conversationId));
			}
			return;
		}
	});
	finder->timer->callOnce(timeoutMs / 2);
	_finders.push_back(std::move(finder));
}

void Conversations::checkFinders() {
	for (auto i = int(_finders.size()); i-- > 0;) {
		const auto chat = _session->chats().chatOfConversation(_finders[i]->conversationId);
		if (!chat.isEmpty()) {
			finishFinder(i, chat);
		}
	}
}

void Conversations::finishFinder(int index, const QString &chat) {
	auto finder = std::move(_finders[index]);
	_finders.erase(_finders.begin() + index);
	if (finder->timer) {
		// May run inside the timer's own callback.
		finder->timer->cancel();
		finder->timer.release()->deleteLater();
	}
	if (finder->done && finder->guard.alive()) {
		finder->done(chat);
	}
}

// ---- dismissals

void Conversations::loadDismissed(base::Guard guard, Fn<void()> done) {
	const auto db = _session->local();
	if (!db) {
		if (done && guard.alive()) {
			done();
		}
		return;
	}
	db->value(kDismissedKey, this, [=, this](QJsonValue value) {
		for (const auto &id : Json::Strings(value)) {
			if (!_dismissed.contains(id)) {
				_dismissed.insert(id);
				_dismissedOrder.push_back(id);
			}
		}
		if (done && guard.alive()) {
			done();
		}
	});
}

void Conversations::dismiss(const QString &id) {
	if (id.isEmpty() || _dismissed.contains(id)) {
		return;
	}
	_dismissed.insert(id);
	_dismissedOrder.push_back(id);
	while (_dismissedOrder.size() > kMaxDismissed) {
		_dismissed.remove(_dismissedOrder.takeFirst());
	}
	if (const auto db = _session->local()) {
		db->setValue(kDismissedKey, Json::Array(_dismissedOrder));
	}
}

// ---- from sync

void Conversations::notifyState(
		const QString &chat,
		const QString &state,
		const std::optional<Conversation> &conversation,
		const QString &event) {
	Q_UNUSED(conversation);
	Q_EMIT stateChanged(chat, state, event);
}

void Conversations::notifyOpenListChanged(const QString &category) {
	Q_EMIT openListChanged(category);
}

} // namespace Data
