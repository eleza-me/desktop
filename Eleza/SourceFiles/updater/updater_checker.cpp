#include "updater/updater_checker.h"

#include "base/platform_file.h"
#include "base/random.h"
#include "crypto/crypto_primitives.h"
#include "install/install_paths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkInformation>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Updater {
namespace {

constexpr auto kMaxRedirects = 5;
constexpr auto kProgressInterval = 120; // ms between two progressChanged()
constexpr auto kRetryFirst = 5 * 60.; // seconds
constexpr auto kRetryMax = 6 * 60 * 60.;
constexpr auto kIntervalJitter = 0.1;
constexpr auto kStalePartAge = qint64(7) * 24 * 60 * 60; // seconds
constexpr auto kHashChunk = qint64(1) << 20;

const auto kStateFile = u"state.json"_s;
const auto kDownloadDir = u"download"_s;

[[nodiscard]] bool IsRedirect(int status) {
	return status == 301
		|| status == 302
		|| status == 303
		|| status == 307
		|| status == 308;
}

[[nodiscard]] QJsonObject InfoToJson(const Info &info) {
	return QJsonObject{
		{ u"version"_s, info.version },
		{ u"notes_fa"_s, info.notesFa },
		{ u"notes_en"_s, info.notesEn },
		{ u"size"_s, double(info.size) },
		{ u"mandatory"_s, info.mandatory },
	};
}

[[nodiscard]] Info InfoFromJson(const QJsonObject &object) {
	auto result = Info();
	const auto version = object.value(u"version"_s).toString();
	if (!Version::Parse(version)) {
		return result;
	}
	result.version = version;
	result.notesFa = object.value(u"notes_fa"_s).toString();
	result.notesEn = object.value(u"notes_en"_s).toString();
	result.size = base::SafeInt64(object.value(u"size"_s).toDouble());
	result.mandatory = object.value(u"mandatory"_s).toBool();
	return result;
}

// The first byte position of "Content-Range: bytes 100-199/200" and the
// total; -1 for what is missing or malformed.
struct ContentRange {
	qint64 from = -1;
	qint64 total = -1;
};
[[nodiscard]] ContentRange ParseContentRange(const QByteArray &header) {
	auto result = ContentRange();
	const auto value = header.trimmed();
	if (!value.startsWith("bytes ")) {
		return result;
	}
	const auto dash = value.indexOf('-', 6);
	const auto slash = value.indexOf('/', 6);
	if (dash < 0 || slash < dash) {
		return result;
	}
	auto ok = false;
	const auto from = value.mid(6, dash - 6).trimmed().toLongLong(&ok);
	if (!ok || from < 0) {
		return result;
	}
	result.from = from;
	const auto total = value.mid(slash + 1).trimmed().toLongLong(&ok);
	if (ok && total >= 0) {
		result.total = total;
	}
	return result;
}

struct Prepared {
	Failure failure = Failure::None;
	QString detail;
};

// On a worker thread: the package is exactly what the manifest signed, then
// it is unpacked into `target`.
[[nodiscard]] Prepared Prepare(
		const QString &archive,
		const Package &package,
		const QString &target,
		const QString &executable,
		const UnpackLimits &limits) {
	auto result = Prepared();
	{
		auto file = QFile(archive);
		if (!file.open(QIODevice::ReadOnly) || file.size() != package.size) {
			result.failure = Failure::Package;
			result.detail = u"size"_s;
			return result;
		}
		auto hasher = Crypto::Sha256Hasher();
		auto left = package.size;
		while (left > 0) {
			const auto chunk = file.read(qMin(left, kHashChunk));
			if (chunk.isEmpty()) {
				break;
			}
			hasher.feed(chunk);
			left -= chunk.size();
		}
		if (left != 0 || hasher.result() != package.sha256) {
			result.failure = Failure::Package;
			result.detail = u"sha256"_s;
			return result;
		}
	}
	if (!base::platform::RemoveRecursively(target)
		|| !base::platform::RemoveRecursively(target + u".tmp"_s)) {
		result.failure = Failure::Disk;
		return result;
	}
	const auto unpacked = Unpack(archive, target, limits);
	if (!unpacked.ok()) {
		result.failure = (unpacked.error == UnpackError::Write)
			? Failure::Disk
			: Failure::Unpack;
		result.detail = u"%1 %2"_s.arg(int(unpacked.error)).arg(unpacked.entry);
		return result;
	}
	const auto main = QFileInfo(QDir(target).filePath(executable));
	auto runnable = main.isFile() && !main.isSymLink();
#ifndef Q_OS_WIN
	runnable = runnable && main.isExecutable();
#endif // !Q_OS_WIN
	if (!runnable) {
		base::platform::RemoveRecursively(target);
		result.failure = Failure::Unpack;
		result.detail = u"no executable"_s;
	}
	return result;
}

} // namespace

Checker::Checker(Config config, QObject *parent)
: QObject(parent)
, _config(std::move(config))
, _current(Version::Parse(_config.currentVersion).value_or(Version()))
, _timer([this] { startCheck(false); })
, _backoff(kRetryFirst, kRetryMax, 0.2, 8) {
	// The requests are followed by hand: every hop is checked.
	_manager.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
	// One job at a time: cleaning never runs next to unpacking.
	_pool.setMaxThreadCount(1);
	// "Is this a metered connection?" needs a backend; without one the
	// answer is simply "no".
	QNetworkInformation::loadBackendByFeatures(
		QNetworkInformation::Feature::Metered);
}

Checker::~Checker() {
	abortReply();
	// A package that is being unpacked finishes first: the worker uses
	// nothing of this object, its result is dropped.
	_pool.waitForDone();
}

QString Checker::versionDir(const QString &version) const {
	return QDir(_config.updatesDir).filePath(version);
}

QString Checker::partPath() const {
	return QDir(_config.updatesDir).filePath(kDownloadDir
		+ u'/'
		+ QString::fromLatin1(_package.sha256.toHex().left(32))
		+ u".zip.part"_s);
}

QString Checker::readyDir() const {
	return (_state == State::Ready && !_ready.version.isEmpty())
		? versionDir(_ready.version)
		: QString();
}

QString Checker::installedNotes(bool persian) const {
	if (_installedVersion != _config.currentVersion) {
		return QString();
	}
	const auto &first = persian ? _installedNotesFa : _installedNotesEn;
	const auto &second = persian ? _installedNotesEn : _installedNotesFa;
	return first.isEmpty() ? second : first;
}

bool Checker::metered() const {
	const auto information = QNetworkInformation::instance();
	return information
		&& information->supports(QNetworkInformation::Feature::Metered)
		&& information->isMetered();
}

void Checker::setState(State state, Failure failure, bool always) {
	if (_state == state && _failure == failure && !always) {
		return;
	}
	_state = state;
	_failure = failure;
	Q_EMIT stateChanged();
}

// ---- what the disk remembers

void Checker::loadStored() {
	const auto bytes = base::platform::ReadFile(
		QDir(_config.updatesDir).filePath(kStateFile));
	if (!bytes) {
		return;
	}
	const auto root = QJsonDocument::fromJson(*bytes).object();
	_lastChecked = QDateTime::fromString(
		root.value(u"last_checked"_s).toString(),
		Qt::ISODate);
	_lastPublished = QDateTime::fromString(
		root.value(u"published_at"_s).toString(),
		Qt::ISODate);
	const auto failed = root.value(u"failed_version"_s).toString();
	_failedVersion = Version::Parse(failed) ? failed : QString();
	_ready = InfoFromJson(root.value(u"ready"_s).toObject());
	const auto installed = InfoFromJson(root.value(u"installed"_s).toObject());
	_installedVersion = installed.version;
	_installedNotesFa = installed.notesFa;
	_installedNotesEn = installed.notesEn;
}

void Checker::saveStored() {
	auto root = QJsonObject();
	if (_lastChecked.isValid()) {
		root.insert(u"last_checked"_s, _lastChecked.toString(Qt::ISODate));
	}
	if (_lastPublished.isValid()) {
		root.insert(u"published_at"_s, _lastPublished.toString(Qt::ISODate));
	}
	if (!_failedVersion.isEmpty()) {
		root.insert(u"failed_version"_s, _failedVersion);
	}
	if (!_ready.version.isEmpty()) {
		root.insert(u"ready"_s, InfoToJson(_ready));
	}
	if (!_installedVersion.isEmpty()) {
		auto installed = Info();
		installed.version = _installedVersion;
		installed.notesFa = _installedNotesFa;
		installed.notesEn = _installedNotesEn;
		root.insert(u"installed"_s, InfoToJson(installed));
	}
	if (!base::platform::WriteFileAtomic(
			QDir(_config.updatesDir).filePath(kStateFile),
			QJsonDocument(root).toJson(QJsonDocument::Indented),
			true)) {
		qWarning("Updater: could not save the state file.");
	}
}

// Removes what earlier runs left: unpacked versions that are not the ready
// one, half-unpacked directories, partial downloads nobody came back for.
void Checker::cleanDirectory() {
	auto remove = QStringList();
	const auto root = QDir(_config.updatesDir);
	const auto entries = root.entryInfoList(
		QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
	for (const auto &entry : entries) {
		const auto name = entry.fileName();
		const auto version = name.endsWith(u".tmp"_s)
			? name.chopped(4)
			: name;
		if (Version::Parse(version)
			&& (name != _ready.version || entry.isSymLink())) {
			remove.push_back(entry.absoluteFilePath());
		}
	}
	const auto now = QDateTime::currentSecsSinceEpoch();
	const auto parts = QDir(root.filePath(kDownloadDir)).entryInfoList(
		QDir::Files | QDir::Hidden);
	for (const auto &part : parts) {
		const auto age = now - part.lastModified().toSecsSinceEpoch();
		if (!part.fileName().endsWith(u".zip.part"_s) || age > kStalePartAge) {
			remove.push_back(part.absoluteFilePath());
		}
	}
	if (remove.isEmpty()) {
		return;
	}
	_pool.start([remove] {
		for (const auto &path : remove) {
			const auto info = QFileInfo(path);
			if (info.isSymLink() || info.isFile()) {
				QFile::remove(path);
			} else {
				base::platform::RemoveRecursively(path);
			}
		}
	});
}

void Checker::start() {
	if (_started) {
		return;
	}
	_started = true;
	base::platform::EnsureDirectory(_config.updatesDir, true);
	loadStored();

	const auto ready = Version::Parse(_ready.version);
	const auto usable = ready
		&& (*ready > _current)
		&& selfUpdate()
		&& QFileInfo(QDir(versionDir(_ready.version)).filePath(
			_config.executable)).isFile();
	if (usable) {
		_info = _ready;
		setState(State::Ready);
	} else if (!_ready.version.isEmpty()) {
		if (ready && *ready == _current) {
			// Installed without a word from the install step.
			_installedVersion = _ready.version;
			_installedNotesFa = _ready.notesFa;
			_installedNotesEn = _ready.notesEn;
		}
		_ready = Info();
		saveStored();
	}
	cleanDirectory();
	_timer.callOnce(_config.firstCheckDelayMs);
}

void Checker::installFinished(const QString &version, bool installed) {
	if (!_started) {
		base::platform::EnsureDirectory(_config.updatesDir, true);
		loadStored();
	}
	if (installed) {
		if (_ready.version == version) {
			_installedVersion = version;
			_installedNotesFa = _ready.notesFa;
			_installedNotesEn = _ready.notesEn;
		}
		_failedVersion = QString();
	} else {
		// Not again by itself: «بررسی به‌روزرسانی» clears it.
		_failedVersion = version;
	}
	if (_ready.version == version) {
		_ready = Info();
	}
	saveStored();
	if (_started) {
		if (_state == State::Ready && _info.version == version) {
			setState(installed ? State::UpToDate : State::Failed, installed
				? Failure::None
				: Failure::Install);
		}
		cleanDirectory();
	}
}

void Checker::scheduleNext(bool failed) {
	if (failed) {
		_timer.callOnce(int(_backoff.next() * 1000.));
		return;
	}
	_backoff.reset();
	const auto jitter = 1. - kIntervalJitter
		+ base::RandomUnit() * 2. * kIntervalJitter;
	_timer.callOnce(int(_config.checkIntervalMs * jitter));
}

// ---- checking

void Checker::checkNow() {
	if (_state == State::Downloading || _state == State::Preparing) {
		return;
	}
	if (!_failedVersion.isEmpty()) {
		// The user asks again: one more try for the version that failed.
		_failedVersion = QString();
		saveStored();
	}
	if (_checking) {
		// Already on its way; make it speak.
		_manual = true;
		if (_quiet) {
			_quiet = false;
			setState(State::Checking);
		}
		return;
	}
	startCheck(true);
}

void Checker::setRequired(bool required) {
	if (_required == required) {
		return;
	}
	_required = required;
	if (!required) {
		return;
	}
	if (_state == State::Available && selfUpdate() && !_package.urls.isEmpty()) {
		download();
	} else if (_state != State::Ready) {
		checkNow();
	}
}

void Checker::startCheck(bool manual) {
	if (_checking || _state == State::Downloading || _state == State::Preparing) {
		if (!manual) {
			// The timer fired in the middle of something: later.
			_timer.callOnce(_config.checkIntervalMs / 6);
		}
		return;
	}
	_timer.cancel();
	_checking = true;
	_manual = manual;
	// A background check behind «آماده» / «در دسترس» does not hide them.
	_quiet = !manual
		&& (_state == State::Ready || _state == State::Available);
	_sourceIndex = 0;
	_sawStale = false;
	_sawAnswer = false;
	if (!_quiet) {
		setState(State::Checking);
	}
	fetchSource();
}

void Checker::fetchSource() {
	if (_sourceIndex >= _config.sources.size()) {
		checkFailed();
		return;
	}
	_redirects = 0;
	get(Fetch::Manifest, QUrl(_config.sources[_sourceIndex]), 0);
}

void Checker::sourceFailed(Failure failure) {
	if (failure == Failure::Signature) {
		_sawAnswer = true;
	}
	++_sourceIndex;
	fetchSource();
}

void Checker::manifestReceived() {
	// The order matters: the signature over the bytes as they came, and
	// only then a look inside.
	if (!VerifySignature(_manifestBytes, _buffer, _config.publicKeys)) {
		qWarning(
			"Updater: source %d: the signature does not verify.",
			_sourceIndex + 1);
		sourceFailed(Failure::Signature);
		return;
	}
	auto options = ParseOptions();
	options.platform = _config.platform;
	options.allowInsecureLoopback = _config.allowInsecureLoopback;
	const auto parsed = ParseManifest(_manifestBytes, options);
	if (!parsed.ok()) {
		qWarning(
			"Updater: source %d: a signed manifest that cannot be used (%d).",
			_sourceIndex + 1,
			int(parsed.error));
		sourceFailed(Failure::Signature);
		return;
	}
	const auto evaluation = Evaluate(parsed.manifest, _current, _lastPublished);
	if (evaluation.verdict == Verdict::Stale) {
		// A mirror that lags behind — or somebody replaying an old manifest.
		qWarning(
			"Updater: source %d: older than the last accepted manifest.",
			_sourceIndex + 1);
		_sawStale = true;
		++_sourceIndex;
		fetchSource();
		return;
	}
	checkFinished(parsed.manifest, evaluation);
}

void Checker::checkFailed() {
	_checking = false;
	if (_sawStale) {
		// Every source that answered is behind what we know: nothing newer
		// can be had right now.
		_lastChecked = QDateTime::currentDateTimeUtc();
		saveStored();
		scheduleNext(false);
		if (!_quiet) {
			if (!_ready.version.isEmpty()) {
				_info = _ready;
				setState(State::Ready);
			} else {
				setState(State::UpToDate);
			}
		}
		return;
	}
	scheduleNext(true);
	if (_quiet) {
		return;
	}
	if (!_ready.version.isEmpty()) {
		// What is unpacked stays offered.
		_info = _ready;
		setState(State::Ready);
		return;
	}
	setState(
		State::Failed,
		_sawAnswer ? Failure::Signature : Failure::Network);
}

void Checker::checkFinished(
		const Manifest &manifest,
		const Evaluation &evaluation) {
	_checking = false;
	_lastChecked = QDateTime::currentDateTimeUtc();
	_lastPublished = manifest.publishedAt;
	scheduleNext(false);

	const auto dropReady = [&] {
		if (!_ready.version.isEmpty()) {
			_ready = Info();
			cleanDirectory();
		}
	};
	const auto version = manifest.version.toString();
	auto info = Info();
	info.version = version;
	info.notesFa = manifest.notesFa;
	info.notesEn = manifest.notesEn;
	info.size = manifest.package ? manifest.package->size : 0;
	info.mandatory = evaluation.mandatory;

	const auto offered = (evaluation.verdict == Verdict::Available)
		|| (evaluation.verdict == Verdict::NoPackage && evaluation.mandatory);
	if (!offered) {
		// Also a release that was taken back: what was unpacked for it goes.
		dropReady();
		saveStored();
		_info = Info();
		setState(State::UpToDate);
		return;
	}
	if (_ready.version != version) {
		dropReady();
	}
	_info = info;
	if (!selfUpdate() || !manifest.package) {
		// Announced only.
		saveStored();
		_package = Package();
		setState(State::Available, Failure::None, true);
		return;
	}
	_package = *manifest.package;
	_packageVersion = version;
	if (_ready.version == version) {
		_ready = info;
		saveStored();
		setState(State::Ready, Failure::None, true);
		return;
	}
	saveStored();
	if (_failedVersion == version && !_required) {
		setState(State::Failed, Failure::Install);
		return;
	}
	const auto automatic = _config.autoDownload && _config.autoDownload();
	if (_required
		|| std::exchange(_userAsked, false)
		|| (automatic && (_manual || !metered()))) {
		startDownload();
	} else {
		setState(State::Available, Failure::None, true);
	}
}

// ---- downloading

void Checker::download() {
	if (_state != State::Available || !selfUpdate()) {
		return;
	}
	if (_package.urls.isEmpty()) {
		// Known from an earlier run only: ask again what to fetch.
		_userAsked = true;
		startCheck(true);
		return;
	}
	startDownload();
}

void Checker::startDownload() {
	const auto directory = QDir(_config.updatesDir).filePath(kDownloadDir);
	if (!base::platform::EnsureDirectory(directory, true)) {
		setState(State::Failed, Failure::Disk);
		scheduleNext(true);
		return;
	}
	_timer.cancel();
	_part.close();
	_part.setFileName(partPath());
	if (QFileInfo(_part.fileName()).isSymLink()
		|| _part.size() > _package.size) {
		QFile::remove(_part.fileName());
	}
	_received = _part.exists() ? _part.size() : 0;
	// Bytes of an earlier run: nobody knows which mirror they are from.
	_partFirstMirror = -1;
	_mirrorIndex = 0;
	_rangeRefused = false;
	_rangeBad = false;
	_mirrorMisbehaved = false;
	setState(State::Downloading);
	Q_EMIT progressChanged();
	if (_received == _package.size) {
		// All of it was here already (the app was closed while unpacking).
		packageReceived();
		return;
	}
	if (!_part.open(QIODevice::WriteOnly | QIODevice::Append)) {
		setState(State::Failed, Failure::Disk);
		scheduleNext(true);
		return;
	}
	fetchPackage();
}

void Checker::fetchPackage() {
	if (_mirrorIndex >= _package.urls.size()) {
		_part.close();
		// Mirrors that answered with something else than the package are
		// not a problem of the connection.
		setState(
			State::Failed,
			_mirrorMisbehaved ? Failure::Package : Failure::Network);
		scheduleNext(true);
		return;
	}
	if (_rangeRefused && _received > 0) {
		_part.resize(0);
		_received = 0;
		Q_EMIT progressChanged();
	}
	_redirects = 0;
	get(Fetch::Package, _package.urls[_mirrorIndex], _received);
}

void Checker::mirrorFailed(bool keepPart) {
	if (!keepPart && _received > 0) {
		_part.resize(0);
		_received = 0;
		Q_EMIT progressChanged();
	}
	++_mirrorIndex;
	_rangeRefused = false;
	fetchPackage();
}

void Checker::packageReceived() {
	_part.close();
	_preparing = true;
	setState(State::Preparing);

	const auto archive = partPath();
	const auto package = _package;
	const auto version = _packageVersion;
	const auto target = versionDir(version);
	const auto executable = _config.executable;
	const auto limits = _config.limits;
	const auto weak = QPointer<Checker>(this);
	_pool.start([=] {
		const auto result = Prepare(archive, package, target, executable, limits);
		// Back on the main thread; the checker may be gone by then.
		const auto application = QCoreApplication::instance();
		if (!application) {
			return;
		}
		QMetaObject::invokeMethod(application, [=] {
			if (weak) {
				weak->prepared(version, result.failure, result.detail);
			}
		}, Qt::QueuedConnection);
	});
}

void Checker::prepared(
		const QString &version,
		Failure failure,
		const QString &detail) {
	if (!_preparing || version != _packageVersion) {
		return;
	}
	_preparing = false;
	QFile::remove(partPath());
	_received = 0;
	if (failure == Failure::None) {
		_ready = _info;
		saveStored();
		setState(State::Ready);
		scheduleNext(false);
		return;
	}
#ifdef ELEZA_DEBUG_TOOLS
	qWarning(
		"Updater: the package of %s was refused (%d: %s).",
		qPrintable(version),
		int(failure),
		qPrintable(detail));
#else // ELEZA_DEBUG_TOOLS
	Q_UNUSED(detail);
	qWarning("Updater: the package was refused (%d).", int(failure));
#endif // !ELEZA_DEBUG_TOOLS
	if (failure == Failure::Package) {
		_mirrorMisbehaved = true;
		if (_partFirstMirror != _mirrorIndex) {
			// The file was begun by an earlier run or by another mirror
			// and only finished by this one: the beginning may have been
			// the bad part, so this mirror once more, alone.
			--_mirrorIndex;
		}
	}
	if (failure == Failure::Package && ++_mirrorIndex < _package.urls.size()) {
		// The bytes were not the signed package: from the first byte.
		_rangeRefused = false;
		if (!_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			setState(State::Failed, Failure::Disk);
			scheduleNext(true);
			return;
		}
		setState(State::Downloading);
		Q_EMIT progressChanged();
		fetchPackage();
		return;
	}
	if (failure == Failure::Unpack) {
		// The same package would fail the same way: not again by itself.
		_failedVersion = version;
		saveStored();
	}
	setState(State::Failed, failure);
	scheduleNext(true);
}

// ---- one request

void Checker::abortReply() {
	if (const auto reply = _reply.data()) {
		_reply = nullptr;
		reply->disconnect(this);
		reply->abort();
		reply->deleteLater();
	}
	_fetch = Fetch::None;
}

void Checker::get(Fetch what, const QUrl &url, qint64 rangeFrom) {
	abortReply();
	_fetch = what;
	_url = url;
	_rangeFrom = rangeFrom;
	_bodyStarted = false;
	_tooLarge = false;
	_buffer.clear();

	auto request = QNetworkRequest(url);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	// Nothing of the user goes with these requests and nothing is kept.
	request.setAttribute(
		QNetworkRequest::CookieLoadControlAttribute,
		QNetworkRequest::Manual);
	request.setAttribute(
		QNetworkRequest::CookieSaveControlAttribute,
		QNetworkRequest::Manual);
	request.setAttribute(
		QNetworkRequest::AuthenticationReuseAttribute,
		QNetworkRequest::Manual);
	request.setAttribute(
		QNetworkRequest::CacheLoadControlAttribute,
		QNetworkRequest::AlwaysNetwork);
	request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"Eleza-Desktop/%1 (%2)"_s.arg(
			_config.currentVersion,
			_config.platform.isEmpty() ? u"other"_s : _config.platform));
	// The bytes as they are stored: sizes, ranges and the signature are
	// about exactly those.
	request.setRawHeader("Accept-Encoding", "identity");
	if (what != Fetch::Package) {
		request.setRawHeader("Cache-Control", "no-cache");
	} else if (rangeFrom > 0) {
		request.setRawHeader(
			"Range",
			"bytes=" + QByteArray::number(rangeFrom) + '-');
	}
	request.setTransferTimeout(_config.requestTimeoutMs);

	if (!IsAllowedUrl(url, _config.allowInsecureLoopback)) {
		// As if the server had failed, on the next turn of the loop.
		QTimer::singleShot(0, this, [this, what] {
			if (_fetch == what && !_reply) {
				_fetch = Fetch::None;
				if (what == Fetch::Package) {
					mirrorFailed(true);
				} else {
					sourceFailed(Failure::Network);
				}
			}
		});
		return;
	}
	const auto reply = _manager.get(request);
	_reply = reply;
	connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
		if (reply == _reply) {
			replyReadyRead();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		if (reply == _reply) {
			replyFinished();
		} else {
			reply->deleteLater();
		}
	});
}

void Checker::replyReadyRead() {
	const auto reply = _reply.data();
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status != 200 && status != 206) {
		// The body of an error or of a redirect is of no interest.
		reply->readAll();
		return;
	}
	if (_fetch != Fetch::Package) {
		const auto limit = (_fetch == Fetch::Manifest)
			? kMaxManifestBytes
			: kMaxSignatureBytes;
		_buffer.append(reply->read(limit + 1 - _buffer.size()));
		if (_buffer.size() > limit) {
			_tooLarge = true;
			reply->abort();
		}
		return;
	}
	if (!_bodyStarted) {
		_bodyStarted = true;
		if (status == 200 && _rangeFrom > 0) {
			// The whole file again: the server does not do ranges.
			_part.resize(0);
			_received = 0;
		} else if (status == 206) {
			const auto range = ParseContentRange(
				reply->rawHeader("Content-Range"));
			if (range.from != _rangeFrom
				|| (range.total >= 0 && range.total != _package.size)) {
				// Not the part that was asked for.
				_rangeBad = true;
				reply->abort();
				return;
			}
		}
	}
	while (reply->bytesAvailable() > 0) {
		const auto chunk = reply->read(256 * 1024);
		if (chunk.isEmpty()) {
			break;
		}
		// Never a byte more than the manifest declares.
		if (chunk.size() > _package.size - _received
			|| _part.write(chunk) != chunk.size()) {
			_tooLarge = true;
			reply->abort();
			return;
		}
		if (_received == 0) {
			_partFirstMirror = _mirrorIndex;
		}
		_received += chunk.size();
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - _lastProgressAt >= kProgressInterval) {
		_lastProgressAt = now;
		Q_EMIT progressChanged();
	}
}

bool Checker::followRedirect() {
	const auto reply = _reply.data();
	const auto target = reply->attribute(
		QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (target.isEmpty() || ++_redirects > kMaxRedirects) {
		return false;
	}
	const auto next = _url.resolved(target);
	// A redirect never leaves https (release assets move to another host,
	// which is fine: what arrives is checked against the signed manifest).
	if (!IsAllowedUrl(next, _config.allowInsecureLoopback)) {
		return false;
	}
	get(_fetch, next, _rangeFrom);
	return true;
}

void Checker::replyFinished() {
	const auto reply = _reply.data();
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto error = reply->error();
	const auto what = _fetch;
	if (IsRedirect(status) && !_tooLarge && followRedirect()) {
		reply->deleteLater();
		return;
	}
	// What arrived before the end of a successful answer.
	if (error == QNetworkReply::NoError && (status == 200 || status == 206)) {
		replyReadyRead();
	}
	const auto complete = (error == QNetworkReply::NoError)
		&& !_tooLarge
		&& (status == 200 || (status == 206 && what == Fetch::Package));
	_reply = nullptr;
	_fetch = Fetch::None;
	reply->deleteLater();

	switch (what) {
	case Fetch::Manifest: {
		if (!complete || _buffer.isEmpty()) {
			// An answer that is no manifest (too large) counts as one.
			sourceFailed(_tooLarge ? Failure::Signature : Failure::Network);
			return;
		}
		_manifestBytes = std::exchange(_buffer, QByteArray());
		// Next to the manifest as the source names it (not next to where
		// a redirect led: a release asset has an address of its own).
		auto signature = QUrl(_config.sources[_sourceIndex]);
		signature.setPath(signature.path() + u".sig"_s);
		_redirects = 0;
		get(Fetch::Signature, signature, 0);
	} break;
	case Fetch::Signature: {
		if (!complete) {
			sourceFailed(Failure::Signature);
			return;
		}
		manifestReceived();
	} break;
	case Fetch::Package: {
		_part.flush();
		Q_EMIT progressChanged();
		if (_tooLarge) {
			// More than the signed size: not our package.
			_mirrorMisbehaved = true;
			mirrorFailed(false);
		} else if (std::exchange(_rangeBad, false) || status == 416) {
			// The range was not honoured (or our part is not a part of
			// what this mirror has): once more from the first byte.
			if (_rangeFrom > 0 && !std::exchange(_rangeRefused, true)) {
				fetchPackage();
			} else {
				mirrorFailed(false);
			}
		} else if (complete && _received == _package.size) {
			packageReceived();
		} else {
			// Cut off, or an error answer: what is here is still the
			// beginning of the right file, the next mirror goes on from it.
			mirrorFailed(true);
		}
	} break;
	case Fetch::None: break;
	}
}

#ifdef ELEZA_DEBUG_TOOLS
void Checker::debugSetState(
		State state,
		Failure failure,
		const Info &info,
		qint64 received) {
	abortReply();
	_timer.cancel();
	_checking = false;
	_info = info;
	_received = received;
	if (state == State::Ready) {
		_ready = info;
	}
	_lastChecked = QDateTime::currentDateTimeUtc();
	_state = state;
	_failure = failure;
	Q_EMIT stateChanged();
	Q_EMIT progressChanged();
}
#endif // ELEZA_DEBUG_TOOLS

} // namespace Updater
