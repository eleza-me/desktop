// What a shipped build trusts for updates: where the manifest is looked for
// and the public keys its signature is checked with. docs/RELEASING.md has the
// whole procedure (signing, key rotation, hosting).
#pragma once

namespace Updater {

// Ed25519 public keys (the raw 32 bytes, base64). A manifest signed by any of
// them is accepted, so a key is rotated by shipping a build that trusts the
// old and the new one before the signer switches.
//
// packaging/update-public-keys.py reads the quoted strings between the two
// marker lines: the release workflow checks the signature it has just made
// against exactly these keys.
inline constexpr const char *kPublicKeys[] = {
	// ELEZA-UPDATE-KEYS-BEGIN
	"zBxbxS+zjNXeLugZPtboVLpysdO3HzFgaieVFubhkrI=",
	// ELEZA-UPDATE-KEYS-END
};

// Tried in this order; the first one that downloads and verifies wins. The
// detached signature is next to each: <address>.sig.
inline constexpr const char *kManifestSources[] = {
	"https://eleza.me/desktop/updates/update.json",
	"https://github.com/eleza-me/desktop/releases/latest/download/update.json",
};

// Where a copy that cannot update itself sends the user.
inline constexpr auto kDownloadPage = "https://eleza.me";

} // namespace Updater
