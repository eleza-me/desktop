// The shared media of a chat or a group by kind (docs/shared-media.md of the
// backend): the server's pages (`chat.media` / `groups.media`, newest first,
// cursor = the smallest seq held), the size of every tab (`chat.media.counts`
// / `groups.media.counts`) and the plain rules the screens share: which tabs
// there are, what each one asks the server for, how its cached rows and the
// server's make one list.
//
//   Data::LoadSharedMedia(&session.data(), chatId, Data::SharedMediaKinds(tab), before, 0,
//       base::Guard(this), [=](const Data::SharedMediaPage &page) { … }, [=](const Api::Error &error) { … });
//   Data::LoadSharedMediaCounts(&session.data(), chatId, base::Guard(this),
//       [=](const Data::SharedMediaCounts &counts) { … });
//
// A page is not a stretch of the history: it is parsed straight from the
// server's messages (a chat that is not cached included) and never written to
// the chat's cache.
#pragma once

#include "api/api_error.h"
#include "base/weak_guard.h"
#include "data/data_types.h"

#include <QtCore/QJsonObject>

#include <array>
#include <optional>

namespace Data {

class Session;

// The tabs in the order of the screen (Telegram's).
enum class SharedMediaTab {
	Media, // photos and videos
	Files,
	Links,
	Music,
	Voice, // voice messages and round videos
	Gifs,
};
inline constexpr auto kSharedMediaTabs = 6;

// The `kinds` of the tab's server page.
[[nodiscard]] QStringList SharedMediaKinds(SharedMediaTab tab);
// A grid of squares (photos and videos, gifs) rather than a list of rows.
[[nodiscard]] bool SharedMediaIsGrid(SharedMediaTab tab);
// The tab lists this message (what a tab picks from the chat's cache).
[[nodiscard]] bool SharedMediaMatches(SharedMediaTab tab, const Message &message);
// The address a message of the «links» tab leads to: the preview's, a
// text link's, else the first http(s) address of the text.
[[nodiscard]] QString SharedMediaLink(const Message &message);

// The sizes of the tabs (GET …/media/counts): `photo_video` → Media, `file`,
// `link`, `audio` → Music, `gif`, `voice` + `video_note` → Voice. A tab whose
// kind reached the server's cap is `capped`: its number is a floor ("10000+").
struct SharedMediaCounts {
	std::array<int, kSharedMediaTabs> count = {};
	std::array<bool, kSharedMediaTabs> capped = {};

	[[nodiscard]] int of(SharedMediaTab tab) const {
		return count[int(tab)];
	}
	[[nodiscard]] bool cappedAt(SharedMediaTab tab) const {
		return capped[int(tab)];
	}
	// Nullopt for an answer that is not one.
	[[nodiscard]] static std::optional<SharedMediaCounts> FromJson(
		const QJsonObject &json);
};

// The tabs to show, in screen order. Counts known: every tab the server
// counts something in, and every tab that has rows anyway (links the server
// cannot know of, found in the cache). Not known: the tabs that have rows
// (`hasRows`), plus the ones still being read (`unknown`): nothing is known to
// be empty about them yet. Never empty: Media stays, to say there is nothing.
[[nodiscard]] QVector<SharedMediaTab> SharedMediaVisibleTabs(
	const std::optional<SharedMediaCounts> &counts,
	const std::array<bool, kSharedMediaTabs> &hasRows,
	const std::array<bool, kSharedMediaTabs> &unknown);

// The seqs a tab shows, newest first and without repeats, from the server's
// pages read so far and the cached rows of the chat (both newest first).
// Before the server answered the cached rows are shown as they are. After it,
// the server is the truth for what it covers: a cached row newer than the
// newest it sent (it arrived after the page) joins them; one inside what the
// server covers and not named by it is left out (deleted, cleared, hidden
// from me). `unionAll` (links: the server knows only those sent since it runs
// the release) takes every cached row too.
[[nodiscard]] QVector<qint64> MergeSharedMedia(
	const QVector<qint64> &server,
	bool serverRead,
	const QVector<qint64> &cached,
	bool unionAll);

struct SharedMediaPage {
	QVector<Message> messages; // newest first
	bool hasMoreBefore = false;
};

// Whether an error of the media routes means "this server (or this chat:
// a live anonymous conversation) has no media pages": the screen then keeps
// to what the chat has cached. A chat I am not in is an error to show.
[[nodiscard]] bool SharedMediaUnsupported(const Api::Error &error);

// `before`: older than that seq (0: from the newest); `limit` 0: the
// server's default.
void LoadSharedMedia(
	Session *session,
	const QString &chat,
	const QStringList &kinds,
	qint64 before,
	int limit,
	base::Guard guard,
	Fn<void(const SharedMediaPage &)> done,
	Fn<void(const Api::Error &)> fail = nullptr);

void LoadSharedMediaCounts(
	Session *session,
	const QString &chat,
	base::Guard guard,
	Fn<void(const SharedMediaCounts &)> done,
	Fn<void(const Api::Error &)> fail = nullptr);

} // namespace Data
