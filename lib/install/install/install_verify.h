// What the elevated Windows updater checks on its own before it puts a new
// version into a directory only administrators may change (Program Files).
//
// The staging directory and the request lie among the user's files, where
// anything running as the user can write: the checks the app made there
// (signature of the manifest, hash of the package) say nothing about what
// the elevated step finally reads. So the app keeps the signed manifest it
// accepted, byte for byte, next to the staged version:
//
//   <updates>/<version>/                the unpacked package
//   <updates>/<version>.update.json      update.json as downloaded
//   <updates>/<version>.update.json.sig  its detached signature
//
// and the elevated step
//   1. verifies the signature with the keys built into it (updater_keys.h),
//   2. takes the version and the list of files with their SHA-256 for its
//      platform from that manifest ("packages" -> <platform> -> "files"),
//   3. copies the staged files into <install>/_update_<version>/, out of the
//      user's reach, and hashes each copy there: the set of files and every
//      hash must be exactly the signed ones (Request::verify, install_swap.h).
//
// Plain C++ like the rest of the library; the tests run it on Linux.
#pragma once

#include "install/install_crypto.h"
#include "install/install_file_ops.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Install {

inline constexpr auto kManifestSuffix = ".update.json";
inline constexpr auto kSignatureSuffix = ".update.json.sig";
// The limits of the app (updater_manifest.h).
inline constexpr auto kMaxManifestBytes = std::size_t(256) * 1024;
inline constexpr auto kMaxSignatureBytes = std::size_t(1024);

struct SignedPackage {
	std::string version;
	// Relative '/'-separated path -> SHA-256 of the file.
	std::map<std::string, Sha256Digest> files;
};

// The keys of updater_keys.h (base64 of 32 bytes); malformed ones are left out.
[[nodiscard]] std::vector<Ed25519PublicKey> DecodePublicKeys(
	const std::vector<std::string_view> &base64);

// `signatureFile` (base64 of 64 bytes) must be a signature of the exact bytes
// of `manifest` by one of `keys`; only then is the manifest read. nullopt,
// with the reason in `error`, when anything is off.
[[nodiscard]] std::optional<SignedPackage> ReadSignedPackage(
	std::string_view manifest,
	std::string_view signatureFile,
	const std::vector<Ed25519PublicKey> &keys,
	std::string_view platform,
	std::string &error);

// <0, 0, >0 like strcmp; nullopt when either is not "<n>.<n>.<n>".
[[nodiscard]] std::optional<int> CompareVersions(
	std::string_view a,
	std::string_view b);

// `files` (the relative paths of every file under `directory`) are exactly
// the signed ones, and each has the signed SHA-256.
[[nodiscard]] bool VerifyFiles(
	FileOps &ops,
	const Path &directory,
	const std::vector<std::string> &files,
	const SignedPackage &package,
	std::string &error);

} // namespace Install
