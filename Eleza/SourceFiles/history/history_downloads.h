// Getting the files of messages onto the disk: a file / music / voice message
// is fetched into the media cache and copied to «Downloads/Eleza» under its
// own name; videos are fetched for the viewer. One object per session, so a
// download survives leaving the chat and every row shows the same state.
#pragma once

#include "data/data_media.h"
#include "data/data_types.h"
#include "history/history_view_element.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>

namespace Main {
class Session;
} // namespace Main

namespace History {

class Downloads final : public QObject {
	Q_OBJECT

public:
	[[nodiscard]] static Downloads *For(Main::Session *session);

	[[nodiscard]] View::DownloadState state(const Data::Message &message) const;
	// Where the saved copy is ("" when there is none any more).
	[[nodiscard]] QString savedPath(const Data::Message &message) const;

	// Fetches the file and saves a copy under its name; `done` gets the path.
	void save(
		const Data::Message &message,
		QObject *guard = nullptr,
		Fn<void(const QString &path)> done = nullptr);
	// Fetches a variant into the cache (a video for the viewer, a picture
	// for «save as»); `done` gets the cached file ("" on failure).
	void fetch(
		const Data::Message &message,
		Data::MediaVariant variant,
		QObject *guard,
		Fn<void(const QString &path)> done);
	void cancel(const Data::Message &message);

	// The variant that holds the bytes of a message's file.
	[[nodiscard]] static Data::MediaVariant FileVariant(
		const Data::Message &message);
	[[nodiscard]] static QString SuggestedName(const Data::Message &message);
	// A file that should not be launched by a click (programs, scripts).
	[[nodiscard]] static bool IsExecutable(const QString &path);

Q_SIGNALS:
	void changed(const QString &chat, qint64 seq);
	void failed(const QString &chat, qint64 seq);

private:
	struct Waiter {
		QPointer<QObject> guard;
		bool guarded = false;
		Fn<void(const QString &)> callback;
	};
	struct Job {
		Data::MediaHandlePtr handle;
		QString name;
		bool copy = false;
		QVector<Waiter> waiting;
	};

	explicit Downloads(Main::Session *session);

	[[nodiscard]] static QString Key(const Data::Message &message);
	void start(
		const Data::Message &message,
		Data::MediaVariant variant,
		bool copy,
		QObject *guard,
		Fn<void(const QString &)> done);
	void finished(const QString &key, const QString &chat, qint64 seq);
	void finish(
		const QString &key,
		const QString &chat,
		qint64 seq,
		const QString &path);
	void persist();

	Main::Session *_session = nullptr;
	QHash<QString, Job> _jobs;
	QHash<QString, QString> _saved; // key -> path
	bool _savedLoaded = false;

};

} // namespace History
