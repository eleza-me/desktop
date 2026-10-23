#include "data/data_shared_media.h"

#include "data/data_chats.h"
#include "data/data_group_rules.h"
#include "data/data_groups.h"
#include "data/data_json.h"
#include "data/data_session.h"
#include "data/data_transport.h"

#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

#include <algorithm>
#include <climits>

namespace Data {
namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QVector<qint64> Union(
		const QVector<qint64> &a,
		const QVector<qint64> &b) {
	auto result = QVector<qint64>();
	result.reserve(a.size() + b.size());
	auto seen = QSet<qint64>();
	for (const auto list : { &a, &b }) {
		for (const auto seq : *list) {
			if (seq > 0 && !seen.contains(seq)) {
				seen.insert(seq);
				result.push_back(seq);
			}
		}
	}
	std::sort(result.begin(), result.end(), std::greater<>());
	return result;
}

} // namespace

QStringList SharedMediaKinds(SharedMediaTab tab) {
	switch (tab) {
	case SharedMediaTab::Media: return { u"photo"_s, u"video"_s };
	case SharedMediaTab::Files: return { u"file"_s };
	case SharedMediaTab::Links: return { u"link"_s };
	case SharedMediaTab::Music: return { u"audio"_s };
	case SharedMediaTab::Voice: return { u"voice"_s, u"video_note"_s };
	case SharedMediaTab::Gifs: return { u"gif"_s };
	}
	return {};
}

bool SharedMediaIsGrid(SharedMediaTab tab) {
	return (tab == SharedMediaTab::Media) || (tab == SharedMediaTab::Gifs);
}

bool SharedMediaMatches(SharedMediaTab tab, const Message &message) {
	if (message.seq <= 0 || !message.visible()) {
		return false;
	}
	switch (tab) {
	case SharedMediaTab::Media:
		return (message.kind == MessageKind::Photo
			|| message.kind == MessageKind::Video);
	case SharedMediaTab::Files: return message.kind == MessageKind::File;
	case SharedMediaTab::Links:
		switch (message.kind) {
		case MessageKind::Text:
		case MessageKind::Photo:
		case MessageKind::Video:
		case MessageKind::Gif:
		case MessageKind::File:
			return !SharedMediaLink(message).isEmpty();
		default: return false;
		}
	case SharedMediaTab::Music: return message.kind == MessageKind::Audio;
	case SharedMediaTab::Voice:
		return (message.kind == MessageKind::Voice
			|| message.kind == MessageKind::VideoNote);
	case SharedMediaTab::Gifs: return message.kind == MessageKind::Gif;
	}
	return false;
}

QString SharedMediaLink(const Message &message) {
	if (message.linkPreview && !message.linkPreview->url.isEmpty()) {
		return message.linkPreview->url;
	}
	for (const auto &entity : message.entities) {
		if (entity.type == u"text_link" && !entity.url.isEmpty()) {
			return entity.url;
		}
	}
	static const auto kUrl = QRegularExpression(
		u"https?://[^\\s<>\"]+"_s,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = kUrl.match(message.text);
	return match.hasMatch() ? match.captured(0) : QString();
}

std::optional<SharedMediaCounts> SharedMediaCounts::FromJson(
		const QJsonObject &json) {
	if (!json.contains("photo_video"_L1)) {
		return std::nullopt;
	}
	const auto number = [&](QLatin1StringView key) {
		return int(std::clamp(Json::Int64(json, key), qint64(0), qint64(INT_MAX / 4)));
	};
	auto result = SharedMediaCounts();
	const auto set = [&](SharedMediaTab tab, int value) {
		result.count[int(tab)] = value;
	};
	set(SharedMediaTab::Media, number("photo_video"_L1));
	set(SharedMediaTab::Files, number("file"_L1));
	set(SharedMediaTab::Links, number("link"_L1));
	set(SharedMediaTab::Music, number("audio"_L1));
	set(SharedMediaTab::Gifs, number("gif"_L1));
	set(SharedMediaTab::Voice, number("voice"_L1) + number("video_note"_L1));
	for (const auto &kind : Json::Strings(json.value("capped"_L1))) {
		const auto tab = (kind == u"photo_video")
			? std::make_optional(SharedMediaTab::Media)
			: (kind == u"file")
			? std::make_optional(SharedMediaTab::Files)
			: (kind == u"link")
			? std::make_optional(SharedMediaTab::Links)
			: (kind == u"audio")
			? std::make_optional(SharedMediaTab::Music)
			: (kind == u"gif")
			? std::make_optional(SharedMediaTab::Gifs)
			: (kind == u"voice" || kind == u"video_note")
			? std::make_optional(SharedMediaTab::Voice)
			: std::nullopt;
		if (tab) {
			result.capped[int(*tab)] = true;
		}
	}
	return result;
}

QVector<SharedMediaTab> SharedMediaVisibleTabs(
		const std::optional<SharedMediaCounts> &counts,
		const std::array<bool, kSharedMediaTabs> &hasRows,
		const std::array<bool, kSharedMediaTabs> &unknown) {
	auto result = QVector<SharedMediaTab>();
	for (auto i = 0; i != kSharedMediaTabs; ++i) {
		const auto tab = SharedMediaTab(i);
		const auto shown = counts
			? (counts->of(tab) > 0 || hasRows[i])
			: (hasRows[i] || unknown[i]);
		if (shown) {
			result.push_back(tab);
		}
	}
	if (result.isEmpty()) {
		result.push_back(SharedMediaTab::Media);
	}
	return result;
}

QVector<qint64> MergeSharedMedia(
		const QVector<qint64> &server,
		bool serverRead,
		const QVector<qint64> &cached,
		bool unionAll) {
	if (!serverRead) {
		return Union(cached, {});
	} else if (unionAll) {
		return Union(server, cached);
	}
	const auto newest = server.isEmpty()
		? qint64(0)
		: *std::max_element(server.begin(), server.end());
	auto newer = QVector<qint64>();
	for (const auto seq : cached) {
		// Nothing from the server: whatever is cached was deleted or
		// cleared (or is hidden from me), unless it came after the page.
		if (seq > newest && newest > 0) {
			newer.push_back(seq);
		}
	}
	return Union(server, newer);
}

bool SharedMediaUnsupported(const Api::Error &error) {
	if (error.code == u"chat.media_not_supported") {
		return true;
	} else if (!error.notFound()) {
		return false;
	}
	// Without a code of the media routes a 404 is the route itself missing
	// (a server from before them).
	return error.code != u"chat.not_found"
		&& error.code != u"group.not_found"
		&& error.code != u"group.disabled"
		&& error.code != u"chat.media_kinds_invalid";
}

void LoadSharedMedia(
		Session *session,
		const QString &chat,
		const QStringList &kinds,
		qint64 before,
		int limit,
		base::Guard guard,
		Fn<void(const SharedMediaPage &)> done,
		Fn<void(const Api::Error &)> fail) {
	const auto group = IsGroupId(chat);
	auto body = QJsonObject{
		{ group ? u"group"_s : u"chat"_s, chat },
		{ u"kinds"_s, kinds.join(u',') },
	};
	if (before > 0) {
		body.insert(u"before"_s, QString::number(before));
	}
	if (limit > 0) {
		body.insert(u"limit"_s, QString::number(limit));
	}
	session->transport().rpc(group ? u"groups.media"_s : u"chat.media"_s)
		.body(body)
		.guard(guard)
		.done([=](const QJsonObject &result) {
			auto page = SharedMediaPage();
			page.hasMoreBefore = Json::Bool(result, "has_more_before"_L1);
			const auto row = session->chats().chat(chat);
			auto info = row
				? row->info
				: (group ? Groups::InfoFor(Group()) : ChatInfo());
			if (info.chat.isEmpty()) {
				info.chat = chat;
			}
			const auto me = session->userId();
			auto seen = QSet<qint64>();
			for (const auto &item : result.value("messages"_L1).toArray()) {
				auto message = Message::FromJson(item.toObject());
				if (message.seq <= 0
					|| !message.visible()
					|| seen.contains(message.seq)) {
					continue;
				}
				seen.insert(message.seq);
				if (message.chat.isEmpty()) {
					message.chat = chat;
				}
				message.resolveLocal(&info, me);
				page.messages.push_back(std::move(message));
			}
			std::sort(page.messages.begin(), page.messages.end(), [](
					const Message &a,
					const Message &b) {
				return a.seq > b.seq;
			});
			if (page.messages.isEmpty()) {
				// A page with nothing in it is the end, whatever it says.
				page.hasMoreBefore = false;
			}
			if (done) {
				done(page);
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail) {
				fail(error);
			}
		})
		.send();
}

void LoadSharedMediaCounts(
		Session *session,
		const QString &chat,
		base::Guard guard,
		Fn<void(const SharedMediaCounts &)> done,
		Fn<void(const Api::Error &)> fail) {
	const auto group = IsGroupId(chat);
	session->transport().rpc(
			group ? u"groups.media.counts"_s : u"chat.media.counts"_s)
		.body(QJsonObject{ { group ? u"group"_s : u"chat"_s, chat } })
		.guard(guard)
		.done([=](const QJsonObject &result) {
			const auto counts = SharedMediaCounts::FromJson(result);
			if (counts) {
				if (done) {
					done(*counts);
				}
			} else if (fail) {
				fail(Api::Error::Local(0, u"decoding"_s, QString()));
			}
		})
		.fail([=](const Api::Error &error) {
			if (fail) {
				fail(error);
			}
		})
		.send();
}

} // namespace Data
