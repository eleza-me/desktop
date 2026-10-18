// Typed models of the chat API. Each struct is parsed once from wire JSON
// (snake_case, the same shape over HTTP and the realtime connection) with
// FromJson and serialized back with toJson (for the local cache and for
// request bodies). Views never keep wire JSON around.
//
// Timestamps: `...At` fields are unix milliseconds unless a comment says
// seconds (the API mixes both; the comments follow the server).
#pragma once

#include "base/basic_types.h"
#include "data/data_group_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QMetaType>

namespace Data {

// A person's card (CardOut): what lists, bubbles and headers show.
struct User {
	QString id;
	QString handle;
	QString displayName;
	QString avatarId;
	bool verified = false;
	bool deleted = false;
	bool isBot = false;

	[[nodiscard]] bool valid() const {
		return !id.isEmpty();
	}
	[[nodiscard]] static User FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
	friend bool operator==(const User &, const User &) = default;
};

struct Birthday {
	QString calendar; // jalali | gregorian
	int month = 0;
	int day = 0;
	int year = 0;     // 0: not shared
	bool today = false;

	[[nodiscard]] static Birthday FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

// A full profile (ProfileOut / the graph's user rows / suggestions): the card
// plus whatever the endpoint added.
struct Profile : User {
	QString bio;
	QString link;
	QString metaLine;
	QString province;
	bool isPrivate = false;
	qint64 followers = 0;
	qint64 following = 0;
	qint64 posts = 0;
	QString visibility;   // full | limited
	QString followState;  // none | requested | following
	bool followsYou = false;
	bool blockedByYou = false;
	bool me = false;
	qint64 since = 0;     // unix seconds (follow lists, blocks)
	QVector<User> mutuals;
	int mutualsTotal = 0;
	std::optional<Birthday> birthday;
	QString reason;       // suggestions: mutual | popular | follows_you | province
	int mutualCount = 0;  // suggestions

	[[nodiscard]] const User &card() const {
		return *this;
	}
	[[nodiscard]] static Profile FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct Participant {
	QString ref;        // sender id as it appears in Message::sender
	QString role;       // speaker | listener (conversations)
	bool masked = false;
	QString maskName;
	int maskAvatar = 0;
	QString userId;     // empty while masked
	bool me = false;
	bool bot = false;
	bool deleted = false;

	[[nodiscard]] static Participant FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

// An anonymous talker / listener conversation (ConvView).
struct Conversation {
	QString id;
	QString topic;
	QString category;
	QString state;          // open | active | readonly | ended | expired | closed | purged | continued
	qint64 expiresAt = 0;   // unix seconds
	Participant speaker;
	std::optional<Participant> listener;
	QString myRole;
	qint64 readonlyUntil = 0;
	bool canContinue = false;
	bool continueRequested = false;
	QString continuedInto;
	QString endReason;

	[[nodiscard]] static Conversation FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct Entity {
	QString type;     // bold | italic | spoiler | text_link | mention | pre | ...
	int offset = 0;   // UTF-16 code units
	int length = 0;
	QString url;
	QString language;
	QString user;     // a mention: the public id of the person it names

	[[nodiscard]] static Entity FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct Draft {
	QString text;
	qint64 replyToSeq = 0;
	QVector<Entity> entities;
	qint64 updatedAt = 0;

	[[nodiscard]] bool empty() const {
		return text.isEmpty();
	}
	[[nodiscard]] static Draft FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

// A chat's header as the server describes it (ChatView).
struct ChatInfo {
	QString chat;
	QString kind;           // direct | conversation
	QString state;
	qint64 lastSeq = 0;
	qint64 expiresAt = 0;
	QVector<Participant> participants;
	qint64 lastReadSeq = 0;
	qint64 peerReadSeq = 0;
	bool muted = false;
	qint64 mutedUntil = 0;
	bool archived = false;
	QString request;        // incoming | outgoing | ""
	bool canSend = true;
	bool peerDeleted = false;
	std::optional<Conversation> conversation;
	QString continuedInto;
	QString theme;
	bool saved = false;     // «Saved messages»
	std::optional<Draft> draft;
	QVector<qint64> pins;   // pinned message seqs
	int liveShares = 0;

	[[nodiscard]] bool valid() const {
		return !chat.isEmpty();
	}
	// The other side: the conversation's counterpart, or the first participant that is not me.
	[[nodiscard]] const Participant *peer() const;
	// Sender refs that mean "me" in this chat (my user id, my participant ref).
	[[nodiscard]] QStringList myRefs(const QString &myUserId) const;
	// User ids worth having cards for.
	[[nodiscard]] QStringList peopleIds() const;

	[[nodiscard]] static ChatInfo FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct MediaMeta {
	int width = 0;
	int height = 0;
	qint64 durationMs = 0;
	QString blurhash;
	QString mime;
	qint64 size = 0;
	QByteArray waveform;

	[[nodiscard]] static MediaMeta FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct Sticker {
	QString id;
	QString pack;
	QString emoji;
	QStringList emojis; // pack listings
	QString url;
	int width = 0;
	int height = 0;

	[[nodiscard]] static Sticker FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct LinkPreview {
	QString url;
	QString siteName;
	QString title;
	QString description;
	QString imageUrl;
	int imageWidth = 0;
	int imageHeight = 0;

	[[nodiscard]] static LinkPreview FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct Location {
	double lat = 0.;
	double lng = 0.;
	double accuracyM = 0.;
	QString label;
	QString address;

	[[nodiscard]] static Location FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct LiveInfo {
	qint64 period = 0;  // seconds
	qint64 until = 0;   // unix seconds
	bool stopped = false;

	[[nodiscard]] static LiveInfo FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct AudioMeta {
	QString title;
	QString performer;

	[[nodiscard]] static AudioMeta FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct CallRecord {
	QString outcome;
	qint64 durationS = 0;
	bool video = false; // the call was started as a video call

	[[nodiscard]] static CallRecord FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct Quote {
	QString text;
	int offset = 0;

	[[nodiscard]] static Quote FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct ViaBot {
	QString id;
	QString handle;
	QString name;

	[[nodiscard]] static ViaBot FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

struct MarkupButton {
	QString text;
	QString kind; // callback | url | switch_inline | ...
	QString url;
	std::optional<QString> query;

	[[nodiscard]] static MarkupButton FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

// A bot's keyboards under / instead of the composer (ClientMarkup).
struct ReplyMarkup {
	QVector<QVector<MarkupButton>> inlineKeyboard;
	QVector<QStringList> keyboard;
	bool resize = false;
	bool oneTime = false;
	bool persistent = false;
	QString placeholder;
	bool removeKeyboard = false;

	[[nodiscard]] bool empty() const {
		return inlineKeyboard.isEmpty() && keyboard.isEmpty() && !removeKeyboard;
	}
	[[nodiscard]] static ReplyMarkup FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

enum class MessageKind {
	Text,
	Photo,
	Video,
	Gif,
	Voice,
	VideoNote,
	Audio,
	File,
	Sticker,
	Location,
	LiveLocation,
	ForwardedPost, // "forwarded_post": a shared feed post (Message::forwardedPost)
	System,
	Call,
	Unknown,       // a kind this build does not know: Message::kindName has the wire value
};
[[nodiscard]] MessageKind MessageKindFromString(const QString &kind);
[[nodiscard]] QString MessageKindToString(MessageKind kind);
[[nodiscard]] bool MessageKindHasMedia(MessageKind kind);

// Delivery state of one of my messages.
enum class MessageStatus {
	Queued,
	Sending,
	Sent,
	Read,
	Failed,
};

// What a system message says, for the UI to word.
enum class ServiceEvent {
	None,       // not a service message we can show
	Text,       // Message::text carries the wording
	Joined,
	Ended,
	Pinned,
	ThemeChanged,
	Continued,
	Group,      // a note of a group (Data::GroupRules::NoteOf says which)
};

// One message: the wire view (MessageView) plus the local state the
// timeline needs. `seq` is 0 while the message exists only in the outbox.
struct Message {
	// ---- wire
	QString chat;
	qint64 seq = 0;
	QString id;
	QString sender;
	QString clientMessageId;
	MessageKind kind = MessageKind::Text;
	QString kindName;
	QString text;                // text, or the caption / file name of media
	qint64 replyToSeq = 0;
	QString attachment;          // media id; fetch through Data::Media (MediaRef::Attachment)
	std::optional<MediaMeta> media;
	QString forwardedPost;       // post id for MessageKind::ForwardedPost
	bool forwarded = false;      // forwarded from another chat
	QString system;              // system code ("conversation.joined:...")
	qint64 createdAt = 0;        // ms
	bool edited = false;
	bool deleted = false;
	bool underReview = false;
	bool played = false;         // voice / video note played by the peer
	std::optional<CallRecord> call;
	std::optional<Location> location;
	std::optional<LiveInfo> live;
	std::optional<AudioMeta> audio;
	QMap<QString, qint64> reactions; // emoji -> count
	QString myReaction;
	QVector<Entity> entities;
	bool mediaSpoiler = false;
	std::optional<Quote> quote;
	std::optional<LinkPreview> linkPreview;
	std::optional<Sticker> sticker;
	std::optional<ReplyMarkup> replyMarkup;
	std::optional<ViaBot> viaBot;

	// ---- local
	bool mine = false;
	MessageStatus status = MessageStatus::Sent;
	QString localPath;           // the file being sent (preview before the upload ends)
	double uploadProgress = -1.; // 0..1 while uploading, -1 otherwise
	QString errorCode;           // why a send failed (Api::Error::code)
	QString errorText;           // the server's wording, when it gave one

	// "seq:<n>" for server messages, "local:<client message id>" while queued.
	[[nodiscard]] QString key() const;
	[[nodiscard]] static QString KeyForSeq(qint64 seq);
	[[nodiscard]] static QString KeyForLocal(const QString &clientMessageId);
	[[nodiscard]] bool local() const {
		return seq <= 0;
	}
	// Deleted and moderated messages leave the timeline; system codes
	// nobody can word are dropped too. In a group a message a moderator
	// holds back stays as a placeholder (it may come back).
	[[nodiscard]] bool visible() const;
	[[nodiscard]] ServiceEvent serviceEvent() const;

	// The wire view.
	[[nodiscard]] static Message FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
	// The wire view + mine / status (the local cache).
	[[nodiscard]] static Message FromStored(const QJsonObject &json);
	[[nodiscard]] QJsonObject toStored() const;
	// Resolves `mine` and the read state against a chat header.
	void resolveLocal(const ChatInfo *info, const QString &myUserId);
};

// Server messages by seq, then local pending ones by creation time.
[[nodiscard]] bool MessageLess(const Message &a, const Message &b);

struct InboxEntry {
	QString chat;
	QString section;  // message | conversation | request
	QString state;
	qint64 lastSeq = 0;
	int unread = 0;
	bool unreadCapped = false;
	std::optional<bool> muted;
	qint64 mutedUntil = 0;
	std::optional<bool> pinned;
	std::optional<bool> archived;
	qint64 timeRingExpiresAt = 0;
	qint64 lastActivityAt = 0; // ms
	std::optional<Message> lastMessage;
	std::optional<ChatInfo> chatInfo;
	bool peerDeleted = false;
	std::optional<Draft> draft;

	[[nodiscard]] static InboxEntry FromJson(const QJsonObject &json);
};

// A row of the chat list: the inbox entry merged with what the client knows.
struct Chat {
	QString id;
	bool isConversation = false;
	QString section;        // message | conversation | request
	// The server-side name: the conversation's topic, a mask name, or the
	// peer's display name once its card is known. Empty for «Saved messages»
	// (saved) and deleted accounts (peerDeleted): the UI words those.
	QString title;
	QString peerUserId;
	QString peerRef;
	bool anonymous = false; // a masked counterpart
	bool saved = false;
	bool isBot = false;
	bool verified = false;
	bool peerDeleted = false;
	std::optional<Message> lastMessage; // for the preview line
	bool lastMine = false;
	qint64 lastActivityAt = 0; // ms
	int unread = 0;
	bool unreadCapped = false;
	bool muted = false;
	qint64 mutedUntil = 0;
	bool pinned = false;
	bool archived = false;
	QString request;        // incoming | outgoing | ""
	bool canSend = true;
	QString state;
	std::optional<Draft> draft;
	qint64 lastSeq = 0;
	qint64 peerReadSeq = 0;
	qint64 lastReadSeq = 0;
	std::optional<Conversation> conversation;
	QString theme;
	qint64 timeRingExpiresAt = 0;
	ChatInfo info;

	// ---- a group (Data::Groups keeps these)
	bool isGroup = false;
	std::optional<Group> group;  // the header as I see it
	qint64 groupPts = -1;        // the last change of the group applied to this row
	qint64 groupCachePts = -1;   // the pts its cached messages are current at (-1: none)
	int unreadMentions = 0;      // unread messages that mention me or reply to me

	[[nodiscard]] bool isRequest() const {
		return section == u"request";
	}
	// A conversation still waiting for its listener.
	[[nodiscard]] bool waitingForListener() const;

	[[nodiscard]] static Chat FromStored(const QJsonObject &json);
	[[nodiscard]] QJsonObject toStored() const;
};

// Pinned first, then by last activity (newest first).
[[nodiscard]] bool ChatLess(const Chat &a, const Chat &b);

struct Presence {
	QString user;
	QString status;       // online | offline | recently | last_week | last_month | long_ago
	qint64 lastSeenAt = 0;
	qint64 receivedAt = 0; // ms, local clock

	[[nodiscard]] bool online() const {
		return status == u"online";
	}
	[[nodiscard]] static Presence FromJson(const QJsonObject &json);
};

// «typing…» and friends in a chat.
struct Activity {
	QString kind;      // typing | recording_voice | uploading_photo | ...
	qint64 until = 0;  // ms, local clock
	QString user;      // a group: the member doing it (empty in a direct chat)
};

struct PinnedMessage {
	Message message;
	qint64 pinnedAt = 0;
	QString scope;     // both | me
	QString pinnedBy;  // groups: who pinned it
	bool hidden = false; // groups: the viewer cannot see the message itself

	[[nodiscard]] static PinnedMessage FromJson(const QJsonObject &json);
};

// A message waiting to be sent (persisted in the outbox table).
struct OutboxItem {
	QString clientMessageId;
	QString chat;
	QJsonObject body;        // the chat.send request minus `chat`
	QString kind;            // message kind on the wire
	qint64 createdAt = 0;    // ms
	int attempts = 0;
	QString filePath;        // a file to upload before sending
	QString fileName;
	QString mime;
	qint64 fileSize = 0;
	qint64 durationMs = 0;
	int width = 0;
	int height = 0;
	QString uploadId;        // resumable upload in progress
	QString mediaId;         // set once the upload finished
	bool failed = false;
	QString errorCode;
	QString errorText;
	QString previewText;     // what the local bubble shows when the body has no text

	[[nodiscard]] static OutboxItem FromJson(const QJsonObject &json);
	[[nodiscard]] QJsonObject toJson() const;
};

} // namespace Data

Q_DECLARE_METATYPE(Data::User)
Q_DECLARE_METATYPE(Data::Message)
Q_DECLARE_METATYPE(Data::Chat)
