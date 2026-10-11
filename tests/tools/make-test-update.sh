#!/bin/sh
# Makes a test release that a build with the debug tools can update to:
#
#   tests/tools/make-test-update.sh <version> <package dir> <base url> <output dir> [key.pem]
#
#   <version>      the version the package is ("0.3.0")
#   <package dir>  a portable build of that version
#                  (packaging/linux/make-portable.sh, or a deployed Windows folder)
#   <base url>     where <output dir> will be served ("http://127.0.0.1:8765")
#   <output dir>   gets the package, update.json and update.json.sig
#   [key.pem]      an Ed25519 private key; a throw-away one is generated into
#                  <output dir>/../test-update-key.pem when it is not given
#
# Prints the public key to pass to the app:
#
#   Eleza --update-url=<base url>/update.json --update-key=<printed key> [--update-now]
#
# Never use the release key here, and never commit a key this script made.
set -eu

if [ "$#" -lt 4 ]; then
	sed -n '2,20p' "$0" >&2
	exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
version="$1"
package="$2"
base="${3%/}"
output="$4"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
key="${5:-$output/../test-update-key.pem}"

case "$(uname -s)" in
	Linux) platform="linux-x86_64" ;;
	*) platform="windows-x64" ;;
esac

if [ ! -f "$key" ]; then
	(umask 077 && openssl genpkey -algorithm ed25519 -out "$key")
fi
# The raw 32 bytes of the public key are the end of its DER form.
public="$(openssl pkey -in "$key" -pubout -outform DER | tail -c 32 | openssl base64 -A)"

archive="$output/Eleza-$version-$platform.zip"
rm -f "$archive" "$output/update.json" "$output/update.json.sig"
python3 "$root/packaging/make-package.py" "$package" "$archive"
python3 "$root/packaging/make-update-manifest.py" \
	--version "$version" \
	--packages "$output" \
	--output "$output/update.json" \
	--mirror "$base/{file}" \
	--allow-missing
ELEZA_UPDATE_SIGNING_KEY="$(cat "$key")" \
	"$root/packaging/sign-update-manifest.sh" "$output/update.json" 2>/dev/null

echo "public key: $public"
