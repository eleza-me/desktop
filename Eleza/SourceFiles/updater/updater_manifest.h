// The update manifest: `update.json` and its detached signature.
//
//   { "format": 1, "channel": "stable", "version": "0.2.0",
//     "published_at": "2026-10-11T10:00:00Z", "min_supported": "0.1.0",
//     "notes": { "fa": "…", "en": "…" },
//     "packages": {
//       "windows-x64":  { "size": 35123456, "sha256": "<64 hex>", "urls": [ "https://…zip", … ] },
//       "linux-x86_64": { … } } }
//
// `update.json.sig` is the Ed25519 signature (64 bytes) of the exact bytes of
// update.json, as base64 text. The order is fixed: VerifySignature() over the
// bytes as downloaded first, ParseManifest() only for bytes that passed.
#pragma once

#include "base/basic_types.h"

#include <QtCore/QDateTime>
#include <QtCore/QUrl>

#include <array>
#include <compare>

namespace Updater {

inline constexpr auto kMaxManifestBytes = qint64(256) * 1024;
inline constexpr auto kMaxSignatureBytes = qint64(1024);
inline constexpr auto kMaxPackageBytes = qint64(2) << 30;

struct Version {
	// Major, minor, patch.
	std::array<int, 3> parts = { 0, 0, 0 };

	// Exactly "<number>.<number>.<number>", nothing else.
	[[nodiscard]] static std::optional<Version> Parse(const QString &text);
	[[nodiscard]] QString toString() const;

	friend auto operator<=>(const Version &, const Version &) = default;
};

struct Package {
	qint64 size = 0;
	QByteArray sha256; // 32 bytes
	QVector<QUrl> urls; // mirrors, in the order to try them
};

struct Manifest {
	Version version;
	Version minSupported;
	QDateTime publishedAt; // UTC
	QString notesFa;
	QString notesEn;
	std::optional<Package> package; // for the platform that was asked for
};

enum class ManifestError {
	None,
	Syntax,   // not a JSON object
	Format,   // a "format" this build does not know
	Channel,  // not the stable channel
	Version,  // "version" / "min_supported" are not versions, or contradict
	Date,     // "published_at" is not a UTC time
	Package,  // the package of this platform is malformed
};

struct ParsedManifest {
	ManifestError error = ManifestError::None;
	Manifest manifest;

	[[nodiscard]] bool ok() const {
		return error == ManifestError::None;
	}
};

struct ParseOptions {
	QString platform; // "windows-x64", "linux-x86_64"; empty: no package wanted
	// Test servers of a build with the debug tools: http:// on the loopback
	// interface next to https://.
	bool allowInsecureLoopback = false;
};

// "windows-x64" / "linux-x86_64"; empty where no packages are published.
[[nodiscard]] QString PlatformKey();

// The keys of updater_keys.h, decoded.
[[nodiscard]] QVector<QByteArray> BakedPublicKeys();
// Base64 -> 32 raw bytes; nullopt for anything else.
[[nodiscard]] std::optional<QByteArray> DecodePublicKey(const QString &base64);

// True when `signatureFile` (base64 of 64 bytes, surrounding whitespace
// allowed) is the signature of `manifest` by one of the keys.
[[nodiscard]] bool VerifySignature(
	const QByteArray &manifest,
	const QByteArray &signatureFile,
	const QVector<QByteArray> &publicKeys);

// Only for bytes whose signature was verified.
[[nodiscard]] ParsedManifest ParseManifest(
	const QByteArray &bytes,
	const ParseOptions &options);

// https:// without credentials; with `allowInsecureLoopback` also http:// to
// 127.0.0.1 / ::1 / localhost.
[[nodiscard]] bool IsAllowedUrl(const QUrl &url, bool allowInsecureLoopback);

enum class Verdict {
	UpToDate,  // nothing newer than what runs
	Available, // newer, and there is a package for this platform
	NoPackage, // newer, but nothing to install here
	Stale,     // older than a manifest that was accepted before: a replay
};

struct Evaluation {
	Verdict verdict = Verdict::UpToDate;
	// The running version is older than "min_supported".
	bool mandatory = false;
};

// Never offers a version that is not strictly newer than `current`, and
// refuses a manifest published before the last one that was accepted.
[[nodiscard]] Evaluation Evaluate(
	const Manifest &manifest,
	const Version &current,
	const QDateTime &lastAccepted);

} // namespace Updater
