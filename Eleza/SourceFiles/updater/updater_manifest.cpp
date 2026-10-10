#include "updater/updater_manifest.h"

#include "crypto/crypto_primitives.h"
#include "updater/updater_keys.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSysInfo>
#include <QtNetwork/QHostAddress>

namespace Updater {
namespace {

constexpr auto kFormat = 1;
constexpr auto kMaxMirrors = 8;
constexpr auto kMaxNotesLength = 16 * 1024;
constexpr auto kMaxVersionPart = 99999;

[[nodiscard]] bool IsLoopbackHost(const QString &host) {
	if (host.compare(u"localhost"_s, Qt::CaseInsensitive) == 0) {
		return true;
	}
	auto address = QHostAddress();
	return address.setAddress(host) && address.isLoopback();
}

[[nodiscard]] std::optional<Package> ParsePackage(
		const QJsonValue &value,
		bool allowInsecureLoopback) {
	if (!value.isObject()) {
		return std::nullopt;
	}
	const auto object = value.toObject();
	auto result = Package();

	// A JSON number is a double: only whole numbers that survive it exactly.
	const auto size = object.value(u"size"_s);
	if (!size.isDouble()) {
		return std::nullopt;
	}
	const auto number = size.toDouble();
	if (!(number >= 1.) || number > double(kMaxPackageBytes)) {
		return std::nullopt;
	}
	result.size = qint64(number);
	if (double(result.size) != number) {
		return std::nullopt;
	}

	const auto hash = object.value(u"sha256"_s).toString().toLatin1();
	if (hash.size() != 64) {
		return std::nullopt;
	}
	for (const auto ch : hash) {
		const auto hex = (ch >= '0' && ch <= '9')
			|| (ch >= 'a' && ch <= 'f')
			|| (ch >= 'A' && ch <= 'F');
		if (!hex) {
			return std::nullopt;
		}
	}
	result.sha256 = QByteArray::fromHex(hash);

	const auto urls = object.value(u"urls"_s);
	if (!urls.isArray()) {
		return std::nullopt;
	}
	const auto list = urls.toArray();
	if (list.isEmpty() || list.size() > kMaxMirrors) {
		return std::nullopt;
	}
	for (const auto &item : list) {
		if (!item.isString()) {
			return std::nullopt;
		}
		const auto url = QUrl(item.toString(), QUrl::StrictMode);
		if (!IsAllowedUrl(url, allowInsecureLoopback)) {
			return std::nullopt;
		}
		result.urls.push_back(url);
	}
	return result;
}

} // namespace

std::optional<Version> Version::Parse(const QString &text) {
	const auto parts = text.split(u'.');
	if (parts.size() != 3) {
		return std::nullopt;
	}
	auto result = Version();
	for (auto i = 0; i != 3; ++i) {
		const auto &part = parts[i];
		if (part.isEmpty() || part.size() > 5) {
			return std::nullopt;
		}
		auto value = 0;
		for (const auto ch : part) {
			// ASCII digits only: "١" or "+1" are not version numbers.
			if (ch < u'0' || ch > u'9') {
				return std::nullopt;
			}
			value = value * 10 + (ch.unicode() - u'0');
		}
		if (value > kMaxVersionPart) {
			return std::nullopt;
		}
		result.parts[i] = value;
	}
	return result;
}

QString Version::toString() const {
	return u"%1.%2.%3"_s.arg(parts[0]).arg(parts[1]).arg(parts[2]);
}

QString PlatformKey() {
	const auto architecture = QSysInfo::buildCpuArchitecture();
	if (architecture != u"x86_64") {
		return QString();
	}
#if defined(Q_OS_WIN)
	return u"windows-x64"_s;
#elif defined(Q_OS_LINUX) // Q_OS_WIN
	return u"linux-x86_64"_s;
#else // Q_OS_WIN || Q_OS_LINUX
	return QString();
#endif // !Q_OS_WIN && !Q_OS_LINUX
}

std::optional<QByteArray> DecodePublicKey(const QString &base64) {
	const auto decoded = QByteArray::fromBase64Encoding(
		base64.trimmed().toLatin1(),
		QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
	if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok
		|| decoded.decoded.size() != Crypto::kKeySize) {
		return std::nullopt;
	}
	return decoded.decoded;
}

QVector<QByteArray> BakedPublicKeys() {
	auto result = QVector<QByteArray>();
	for (const auto text : kPublicKeys) {
		if (const auto key = DecodePublicKey(QString::fromLatin1(text))) {
			result.push_back(*key);
		}
	}
	return result;
}

bool VerifySignature(
		const QByteArray &manifest,
		const QByteArray &signatureFile,
		const QVector<QByteArray> &publicKeys) {
	if (manifest.isEmpty()
		|| manifest.size() > kMaxManifestBytes
		|| signatureFile.size() > kMaxSignatureBytes) {
		return false;
	}
	const auto decoded = QByteArray::fromBase64Encoding(
		signatureFile.trimmed(),
		QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
	if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok
		|| decoded.decoded.size() != 64) {
		return false;
	}
	for (const auto &key : publicKeys) {
		if (Crypto::Ed25519Verify(key, manifest, decoded.decoded)) {
			return true;
		}
	}
	return false;
}

bool IsAllowedUrl(const QUrl &url, bool allowInsecureLoopback) {
	if (!url.isValid()
		|| url.isRelative()
		|| url.host().isEmpty()
		|| !url.userInfo().isEmpty()) {
		return false;
	}
	const auto scheme = url.scheme().toLower();
	if (scheme == u"https") {
		return true;
	}
	return allowInsecureLoopback
		&& (scheme == u"http")
		&& IsLoopbackHost(url.host());
}

ParsedManifest ParseManifest(
		const QByteArray &bytes,
		const ParseOptions &options) {
	const auto fail = [](ManifestError error) {
		auto result = ParsedManifest();
		result.error = error;
		return result;
	};
	auto parseError = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		return fail(ManifestError::Syntax);
	}
	const auto root = document.object();
	const auto format = root.value(u"format"_s);
	if (!format.isDouble() || format.toDouble() != double(kFormat)) {
		return fail(ManifestError::Format);
	} else if (root.value(u"channel"_s).toString() != u"stable") {
		return fail(ManifestError::Channel);
	}

	auto result = ParsedManifest();
	auto &manifest = result.manifest;
	const auto version = Version::Parse(root.value(u"version"_s).toString());
	if (!version) {
		return fail(ManifestError::Version);
	}
	manifest.version = *version;
	if (const auto minimum = root.value(u"min_supported"_s);
		!minimum.isUndefined()) {
		const auto parsed = Version::Parse(minimum.toString());
		// A minimum above the offered version could never be reached.
		if (!parsed || *parsed > manifest.version) {
			return fail(ManifestError::Version);
		}
		manifest.minSupported = *parsed;
	}

	const auto published = root.value(u"published_at"_s).toString();
	manifest.publishedAt = QDateTime::fromString(published, Qt::ISODate);
	if (!published.endsWith(u'Z')
		|| !manifest.publishedAt.isValid()
		|| manifest.publishedAt.timeSpec() != Qt::UTC) {
		return fail(ManifestError::Date);
	}

	const auto notes = root.value(u"notes"_s).toObject();
	manifest.notesFa = notes.value(u"fa"_s).toString().left(kMaxNotesLength);
	manifest.notesEn = notes.value(u"en"_s).toString().left(kMaxNotesLength);

	if (!options.platform.isEmpty()) {
		const auto packages = root.value(u"packages"_s).toObject();
		if (const auto found = packages.value(options.platform);
			!found.isUndefined()) {
			manifest.package = ParsePackage(
				found,
				options.allowInsecureLoopback);
			if (!manifest.package) {
				return fail(ManifestError::Package);
			}
		}
	}
	return result;
}

Evaluation Evaluate(
		const Manifest &manifest,
		const Version &current,
		const QDateTime &lastAccepted) {
	auto result = Evaluation();
	if (lastAccepted.isValid() && manifest.publishedAt < lastAccepted) {
		result.verdict = Verdict::Stale;
		return result;
	}
	if (manifest.version <= current) {
		result.verdict = Verdict::UpToDate;
		return result;
	}
	result.mandatory = (current < manifest.minSupported);
	result.verdict = manifest.package
		? Verdict::Available
		: Verdict::NoPackage;
	return result;
}

} // namespace Updater
