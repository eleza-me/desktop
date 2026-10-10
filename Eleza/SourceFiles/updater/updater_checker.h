// Updater::Checker — looks for a new version, downloads it in the background,
// verifies and unpacks it, and says when it is ready to be installed.
//
//   Idle -> Checking -> UpToDate
//                    -> Available            (waits for download(): automatic
//                                             updates are off, the connection
//                                             is metered, or this copy cannot
//                                             update itself at all)
//                    -> Downloading -> Preparing -> Ready
//                    -> Failed               (failure() says why)
//
// A check walks the manifest sources in order; the first one whose manifest
// downloads, carries a valid signature of a baked key, parses and is not older
// than the last accepted one wins. Nothing of a manifest is looked at before
// its signature was verified. The package is fetched from the mirrors the
// signed manifest names (in order, resuming a partial file with `Range`), must
// have exactly the declared size and SHA-256, and only then is unpacked into
// <updates>/<version>/ (updater_archive.h).
//
// The requests are plain: an own QNetworkAccessManager, no account, no
// cookies, https only (redirects too), size limits on everything.
// Nothing here blocks: hashing and unpacking run on the thread pool.
//
// What is kept in <updates>/state.json: when a check last succeeded, the
// publication time of the last accepted manifest (a replayed older manifest
// is refused), the version that is unpacked and ready, a version whose install
// failed (not tried again by itself) and the notes of the installed version.
#pragma once

#include "base/backoff.h"
#include "base/timer.h"
#include "updater/updater_archive.h"
#include "updater/updater_manifest.h"

#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtNetwork/QNetworkAccessManager>

class QNetworkReply;

namespace Updater {

enum class State {
	Idle,
	Checking,
	UpToDate,
	Available,
	Downloading,
	Preparing, // verifying and unpacking
	Ready,
	Failed,
};

enum class Failure {
	None,
	Network,   // no source / no mirror could be reached
	Signature, // an answer came, but nothing with a valid signed manifest
	Package,   // what the mirrors serve is not the signed package
	Unpack,    // the package could not be unpacked
	Disk,      // the staging directory cannot be written
	Install,   // the install step failed at the last restart
};

struct Info {
	QString version;
	QString notesFa;
	QString notesEn;
	qint64 size = 0;
	// The running version is below the minimum the manifest supports.
	bool mandatory = false;
};

struct Config {
	QString currentVersion;
	QString updatesDir; // <workdir>/updates
	QString platform;   // PlatformKey()
	QStringList sources;
	QVector<QByteArray> publicKeys;
	bool allowInsecureLoopback = false;
	// False: this copy is updated by something else (a package manager) or
	// cannot be replaced. New versions are announced, never downloaded.
	bool selfUpdate = true;
	// A file every package must have (relative): "Eleza.exe" / "Eleza".
	QString executable;
	// The «به‌روزرسانی خودکار» setting.
	Fn<bool()> autoDownload;
	int firstCheckDelayMs = 20 * 1000;
	int checkIntervalMs = 6 * 60 * 60 * 1000;
	int requestTimeoutMs = 30 * 1000; // without any progress
	UnpackLimits limits;
};

class Checker final : public QObject {
	Q_OBJECT

public:
	explicit Checker(Config config, QObject *parent = nullptr);
	~Checker();

	// Restores what the disk knows (a version that is ready) and schedules
	// the first check.
	void start();

	[[nodiscard]] State state() const {
		return _state;
	}
	[[nodiscard]] Failure failure() const {
		return _failure;
	}
	// Meaningful in Available / Downloading / Preparing / Ready, and in
	// Failed when the version is known.
	[[nodiscard]] const Info &info() const {
		return _info;
	}
	[[nodiscard]] qint64 received() const {
		return _received;
	}
	[[nodiscard]] qint64 total() const {
		return _info.size;
	}
	[[nodiscard]] bool selfUpdate() const {
		return _config.selfUpdate && !_config.platform.isEmpty();
	}
	[[nodiscard]] QString currentVersion() const {
		return _config.currentVersion;
	}
	// Invalid until a check succeeded once.
	[[nodiscard]] QDateTime lastChecked() const {
		return _lastChecked;
	}
	// The unpacked new version (state Ready).
	[[nodiscard]] QString readyDir() const;
	// «تازه‌های این نسخه»: the notes of the running version, when it came
	// through an update.
	[[nodiscard]] QString installedNotes(bool persian) const;

	// The user asked: checks now, and downloads what it finds even on a
	// metered connection when automatic updates are on.
	void checkNow();
	// Available -> Downloading (the user asked for this version).
	void download();
	// The update is required (the app is blocked until it is installed):
	// check now and download whatever the settings say.
	void setRequired(bool required);
	// What the install step reported at this start.
	void installFinished(const QString &version, bool installed);

#ifdef ELEZA_DEBUG_TOOLS
	// Screenshots: puts the checker into a state without any network.
	void debugSetState(State state, Failure failure, const Info &info, qint64 received);
#endif // ELEZA_DEBUG_TOOLS

Q_SIGNALS:
	void stateChanged();
	// received() changed while Downloading.
	void progressChanged();

private:
	enum class Fetch {
		None,
		Manifest,
		Signature,
		Package,
	};

	void loadStored();
	void saveStored();
	void cleanDirectory();
	void scheduleNext(bool failed);
	// `always`: tell the listeners even when only info() changed.
	void setState(
		State state,
		Failure failure = Failure::None,
		bool always = false);

	void startCheck(bool manual);
	void fetchSource();
	void sourceFailed(Failure failure);
	void manifestReceived();
	void checkFinished(const Manifest &manifest, const Evaluation &evaluation);
	void checkFailed();

	void startDownload();
	void fetchPackage();
	void mirrorFailed(bool keepPart);
	void packageReceived();
	void prepared(const QString &version, Failure failure, const QString &detail);

	void get(Fetch what, const QUrl &url, qint64 rangeFrom);
	void abortReply();
	void replyReadyRead();
	void replyFinished();
	[[nodiscard]] bool followRedirect();
	[[nodiscard]] bool metered() const;
	[[nodiscard]] QString partPath() const;
	[[nodiscard]] QString versionDir(const QString &version) const;

	const Config _config;
	const Version _current;
	QNetworkAccessManager _manager;
	QThreadPool _pool;
	base::Timer _timer;
	base::ExponentialBackoff _backoff;

	State _state = State::Idle;
	Failure _failure = Failure::None;
	Info _info;
	qint64 _received = 0;
	bool _required = false;
	bool _started = false;

	// ---- the check in progress
	bool _checking = false;
	bool _manual = false;
	// The check runs behind a state that stays on screen (Ready, Available).
	bool _quiet = false;
	int _sourceIndex = 0;
	bool _sawStale = false;
	bool _sawAnswer = false;
	QByteArray _manifestBytes;

	// ---- the download in progress
	Package _package;
	QString _packageVersion;
	int _mirrorIndex = 0;
	bool _rangeRefused = false; // this mirror was already asked again without a range
	bool _rangeBad = false; // the answer in progress is not the range asked for
	// The mirror whose bytes the partial file begins with (-1: an earlier
	// run's).
	int _partFirstMirror = -1;
	bool _mirrorMisbehaved = false;
	bool _userAsked = false;
	QFile _part;
	bool _preparing = false;

	// ---- the request in progress
	QPointer<QNetworkReply> _reply;
	Fetch _fetch = Fetch::None;
	QUrl _url;
	qint64 _rangeFrom = 0;
	int _redirects = 0;
	bool _bodyStarted = false;
	bool _tooLarge = false;
	QByteArray _buffer;
	qint64 _lastProgressAt = 0;

	// ---- stored
	QDateTime _lastChecked;
	QDateTime _lastPublished;
	QString _failedVersion;
	Info _ready;
	QString _installedVersion;
	QString _installedNotesFa;
	QString _installedNotesEn;

};

} // namespace Updater
