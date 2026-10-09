// The social graph and the account: profiles, follow / mute / block,
// follower lists, search, people suggestions, profile edits, handle,
// birthday, synced settings, muted words and device sessions. HTTP only.
#pragma once

#include "api/api_client.h"
#include "data/data_social_types.h"

namespace Data {

class Session;

struct ProfilePatch {
	std::optional<QString> displayName;
	std::optional<QString> bio;
	std::optional<QString> link;
	std::optional<QString> metaLine;
	QString avatarId;        // a freshly uploaded avatar (Data::Upload, purpose Avatar)
	bool clearAvatar = false;

	[[nodiscard]] QJsonObject toJson() const;
};

struct SuggestionsPage {
	QVector<Profile> people; // Profile::reason / mutualCount / mutuals are set
	QString nextCursor;
};

class Graph final : public QObject {
	Q_OBJECT

public:
	explicit Graph(Session *session);

	// ---- profiles
	void profileByHandle(const QString &handle, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
	void profile(const QString &userId, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
	void relationship(const QString &userId, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail = nullptr);

	// ---- follow / mute / block (the answer is the new relationship)
	void follow(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
	void unfollow(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
	void removeFollower(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
	void block(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
	void unblock(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
	void mute(const QString &userId);
	void unmute(const QString &userId);
	void postNotifications(const QString &userId, base::Guard guard, Fn<void(bool)> done);
	void setPostNotifications(const QString &userId, bool on, base::Guard guard = base::Guard(), Fn<void(bool)> done = nullptr);

	// ---- follow requests (a private account's incoming requests)
	void followRequests(base::Guard guard, Fn<void(const QVector<Profile> &)> done, FailCallback fail = nullptr);
	void acceptFollowRequest(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);
	void declineFollowRequest(const QString &userId, base::Guard guard = base::Guard(), Fn<void(const GraphState &)> done = nullptr, FailCallback fail = nullptr);

	// ---- lists
	void followers(const QString &userId, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail = nullptr);
	void following(const QString &userId, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail = nullptr);
	void blocks(const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail = nullptr);
	void filters(base::Guard guard, Fn<void(const Filters &)> done, FailCallback fail = nullptr);
	void openConversationsOf(const QString &userId, base::Guard guard, Fn<void(const QVector<OpenConversation> &)> done);

	// ---- search and suggestions
	// type: users | posts | topics | conversations.
	void search(const QString &query, const QString &type, int offset, base::Guard guard, Fn<void(const SearchResults &)> done, FailCallback fail = nullptr);
	void searchPeople(const QString &query, base::Guard guard, Fn<void(const QVector<Profile> &)> done);
	void suggestions(const QString &cursor, int limit, base::Guard guard, Fn<void(const SuggestionsPage &)> done, FailCallback fail = nullptr);
	// Hides a suggestion; a failed request is remembered and retried on the next start.
	void dismissSuggestion(const QString &userId);
	void retryDismissals();

	// ---- my profile
	// `PATCH /v1/me/profile`, retried while an uploaded avatar is still processing.
	void updateProfile(const ProfilePatch &patch, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
	void setHandle(const QString &handle, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
	void handleAvailable(const QString &handle, base::Guard guard, Fn<void(bool)> done, FailCallback fail = nullptr);
	void setBirthday(const Birthday &birthday, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail = nullptr);
	void clearBirthday(base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);

	// ---- synced settings (one nested document: privacy, notifications, ...)
	// The cached document; empty until loadSettings() ran once.
	[[nodiscard]] const QJsonObject &settings() const {
		return _settings;
	}
	[[nodiscard]] bool settingsLoaded() const {
		return _settingsLoaded;
	}
	void loadSettings(bool force, base::Guard guard, Fn<void(const QJsonObject &)> done, FailCallback fail = nullptr);
	// A nested JSON merge patch, applied optimistically (rolled back on failure).
	void patchSettings(const QJsonObject &partial, base::Guard guard = base::Guard(), Fn<void(const QJsonObject &)> done = nullptr, FailCallback fail = nullptr);
	// settingValue("privacy.private_account")
	[[nodiscard]] QJsonValue settingValue(const QString &dotted) const;
	void setSetting(const QString &dotted, const QJsonValue &value);
	[[nodiscard]] static QJsonObject SettingPatch(const QString &dotted, const QJsonValue &value);
	[[nodiscard]] static QJsonValue DeepMerge(const QJsonValue &base, const QJsonValue &patch);

	// ---- muted words, hidden topics
	void addMutedWord(const QString &word, base::Guard guard = base::Guard(), Fn<void()> done = nullptr);
	void removeMutedWord(const QString &word, base::Guard guard = base::Guard(), Fn<void()> done = nullptr);
	void unhideTopic(const QString &id, base::Guard guard = base::Guard(), Fn<void()> done = nullptr);

	// ---- account
	void sessions(base::Guard guard, Fn<void(const QVector<DeviceSession> &)> done, FailCallback fail = nullptr);
	void revokeSession(const QString &id, base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void revokeOtherSessions(base::Guard guard = base::Guard(), Fn<void()> done = nullptr, FailCallback fail = nullptr);
	void accountPhone(base::Guard guard, Fn<void(const QString &)> done);
	// The latest data export ({} when there is none).
	void latestExport(base::Guard guard, Fn<void(const QJsonObject &)> done, FailCallback fail = nullptr);
	void requestExport(base::Guard guard, Fn<void(const QJsonObject &)> done, FailCallback fail = nullptr);

Q_SIGNALS:
	void settingsChanged();
	// My relationship with a user changed through one of the calls above.
	void relationshipChanged(const QString &userId, const Data::GraphState &state);

private:
	void relation(Api::Method method, const QString &path, const QString &userId, const QString &action, base::Guard guard, Fn<void(const GraphState &)> done, FailCallback fail);
	void list(const QString &path, const QString &cursor, base::Guard guard, Fn<void(const GraphList &)> done, FailCallback fail);
	void updateAttempt(const QJsonObject &body, int attempt, base::Guard guard, Fn<void(const Profile &)> done, FailCallback fail);
	void simple(Api::Method method, const QString &path, const QJsonObject &body, const QString &action, const QString &subject, base::Guard guard, Fn<void()> done);
	[[nodiscard]] QStringList pendingDismissals() const;
	void savePendingDismissals(const QStringList &list);

	Session *_session = nullptr;
	QJsonObject _settings;
	QStringList _pendingDismissals;
	bool _settingsLoaded = false;
	bool _dismissalsLoaded = false;

};

} // namespace Data

Q_DECLARE_METATYPE(Data::GraphState)
