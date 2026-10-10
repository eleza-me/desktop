#!/bin/sh
# Signs update.json with the Ed25519 release key and writes update.json.sig
# next to it: the 64-byte signature of the exact bytes of the manifest, as one
# line of base64.
#
#   ELEZA_UPDATE_SIGNING_KEY="$(cat key.pem)" \
#       packaging/sign-update-manifest.sh dist/update.json [public-key.pem ...]
#
# The key is taken from the environment variable ELEZA_UPDATE_SIGNING_KEY: a
# PEM PKCS#8 Ed25519 private key, as written by
# `openssl genpkey -algorithm ed25519` (in the release workflow: the repository
# secret of the same name). It is never printed; it goes to a temporary file
# that only this user can read and that is removed when the script ends.
#
# After signing, the signature is verified with every public key file given
# (PEM; packaging/update-public-keys.py --pem-dir writes the ones compiled into
# the app) and must match at least one of them — a manifest signed with a key
# no build trusts must never be published. Without key files it is verified
# against the public half of the signing key only, with a warning.
#
# Needs OpenSSL 3 (`pkeyutl -rawin`).
set -eu

if [ "$#" -lt 1 ]; then
	echo "usage: ELEZA_UPDATE_SIGNING_KEY=<pem> $0 <update.json> [public-key.pem ...]" >&2
	exit 2
fi
manifest="$1"
shift
if [ ! -f "$manifest" ]; then
	echo "sign-update-manifest: no such file: $manifest" >&2
	exit 2
fi
if [ -z "${ELEZA_UPDATE_SIGNING_KEY:-}" ]; then
	cat >&2 <<'MESSAGE'
sign-update-manifest: ELEZA_UPDATE_SIGNING_KEY is not set.

The update manifest must be signed with the Ed25519 release key. In GitHub:
Settings -> Secrets and variables -> Actions -> New repository secret, name
ELEZA_UPDATE_SIGNING_KEY, value: the whole PEM file of the private key
(-----BEGIN PRIVATE KEY----- ... -----END PRIVATE KEY-----).
docs/RELEASING.md, "The signing key", has the details.
MESSAGE
	exit 1
fi

# A signature left by an earlier run must not survive a failure of this one.
rm -f "$manifest.sig"

umask 077
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM
printf '%s\n' "$ELEZA_UPDATE_SIGNING_KEY" > "$work/key.pem"

if ! openssl pkey -in "$work/key.pem" -noout 2>/dev/null; then
	echo "sign-update-manifest: ELEZA_UPDATE_SIGNING_KEY is not a PEM private key." >&2
	exit 1
fi
if ! openssl pkey -in "$work/key.pem" -pubout -out "$work/own-public.pem" 2>/dev/null \
	|| ! openssl pkey -pubin -in "$work/own-public.pem" -noout -text 2>/dev/null | grep -q 'ED25519'; then
	echo "sign-update-manifest: the signing key is not an Ed25519 key." >&2
	exit 1
fi

openssl pkeyutl -sign -rawin -inkey "$work/key.pem" -in "$manifest" -out "$work/signature.bin"
if [ "$(wc -c < "$work/signature.bin" | tr -d ' ')" != "64" ]; then
	echo "sign-update-manifest: the signature is not 64 bytes long." >&2
	exit 1
fi

verify() {
	openssl pkeyutl -verify -pubin -inkey "$1" -rawin -in "$manifest" \
		-sigfile "$work/signature.bin" >/dev/null 2>&1
}

if [ "$#" -eq 0 ]; then
	echo "sign-update-manifest: warning: no public keys given, checking against the signing key only." >&2
	verify "$work/own-public.pem" || {
		echo "sign-update-manifest: the signature does not verify." >&2
		exit 1
	}
else
	matched=""
	for key in "$@"; do
		if verify "$key"; then
			matched="$key"
			break
		fi
	done
	if [ -z "$matched" ]; then
		cat >&2 <<'MESSAGE'
sign-update-manifest: the signature does not verify with any of the public keys
compiled into the app (Eleza/SourceFiles/updater/updater_keys.h). The secret
ELEZA_UPDATE_SIGNING_KEY is not the private half of a trusted key: no client
would accept this manifest. No signature was written.
MESSAGE
		exit 1
	fi
	echo "signature verified with $(basename "$matched")"
fi

# One line of base64; written only now that it verified.
openssl base64 -A -in "$work/signature.bin" > "$work/signature.txt"
printf '\n' >> "$work/signature.txt"
cp "$work/signature.txt" "$manifest.sig"
chmod 644 "$manifest.sig"
echo "wrote $manifest.sig"
