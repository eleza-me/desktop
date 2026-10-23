# Releasing Eleza Desktop

How a version gets from a commit to the people who run Eleza: the release
workflow, the signed update manifest the app updates itself from, the signing
key, the optional mirror on Eleza's own server, and how to try an update locally.

Related: `docs/BUILDING.md` (building and packaging by hand),
`Eleza/SourceFiles/updater/` and `lib/install/` (the code; every header starts
with a description).

## 1. How updates work

An installed copy checks for a new version about 20 seconds after it starts and
then every six hours (also from Settings → Updates and from the tray menu). What
it does, in this order:

1. **Manifest.** It asks the sources in order and takes the first one that
   passes every check:

   | # | Address |
   | --- | --- |
   | 1 | `https://eleza.me/desktop/updates/update.json` |
   | 2 | `https://github.com/eleza-me/desktop/releases/latest/download/update.json` |

   Next to each manifest is its detached signature, `update.json.sig`. The
   requests are plain HTTPS without the account (updates work signed out),
   without cookies, with every redirect checked (HTTPS only), a manifest of at
   most 256 KiB.
2. **Signature first.** The signature must verify, with one of the public keys
   compiled into the app, over the exact bytes that were downloaded. Only then
   is the JSON parsed. A source whose manifest does not verify is skipped.
3. **Rules.** The manifest must be `format` 1 on the `stable` channel. A version
   that is not strictly newer than the running one is never installed, and a
   manifest whose `published_at` is older than the last one this copy accepted
   is refused (a replayed old manifest, or a mirror that lags behind: the next
   source is asked).
4. **Package.** The package of the platform is downloaded from the mirrors the
   signed manifest lists, in order, resuming a partial file where the server
   allows it. It must have exactly the signed size and SHA-256; never a byte
   more than the signed size is written.
5. **Unpacking.** The zip is unpacked into `<data dir>/updates/<version>/` as
   hostile input: no absolute paths, `..`, drive letters, device names, links,
   duplicate names (also by case), encrypted entries; limits on the number and
   size of the files and on the compression ratio.
6. **Install on restart.** «به‌روزرسانی الزا» quits the app, swaps the files and
   starts the new version with the arguments of the old one.
   * Linux: the exiting process swaps the files itself and executes the new
     binary.
   * Windows: the app starts a copy of `Updater.exe` (no Qt, static runtime),
     which waits for the app to exit, swaps the files and starts `Eleza.exe`.
     If the install directory needs administrator rights (an install for all
     users under Program Files) it asks for them; the default per-user install
     never does. What runs with those rights is the `Updater.exe` of the
     install directory (which only an administrator can change), not the copy
     among the user's files; it only swaps the files of the directory it lies
     in, copies the new ones out of `<data dir>/updates/<version>/` and writes
     nothing there (the copy that started it writes the result).

   The swap moves every old file into `<install>/_backup_<version>/` and the new
   one in, file by file, with a journal written before every move. Anything
   that fails is undone and the old version starts again (the failure is shown,
   and that version is not tried again until the user asks). A swap that was
   cut off by a power loss is undone at the next start.

With «به‌روزرسانی خودکار» off (or on a metered connection) the new version is
only announced, with «دریافت». When the running version is below the manifest's
`min_supported` — or the API refuses it (HTTP 426, `api.min_client_version`) —
the blocking "update required" screen downloads the update whatever the settings
say.

### Which copies update themselves

| Copy | Updates |
| --- | --- |
| Windows, installer (per user, the default) or the portable zip | itself |
| Windows, installed for all users (`/ALLUSERS`) | itself, asking for administrator rights each time |
| Linux, the portable zip in a directory the user owns | itself |
| Linux, the `.deb` (`/usr/bin/Eleza`) | by the package manager; the app only says that a new version exists and opens the download page |
| A build directory, an AppImage / Flatpak / Snap, a directory the user cannot write | announced only |

The rule: `eleza-files.list` (written by `packaging/make-package.py`) must lie
next to the executable, on Windows together with `Updater.exe`; on Linux the
directory must belong to the user, be writable and not be under `/usr`.

## 2. The manifest

`update.json`, UTF-8, served exactly as generated:

```json
{
	"format": 1,
	"channel": "stable",
	"version": "0.4.0",
	"published_at": "2026-10-11T10:00:00Z",
	"min_supported": "0.1.0",
	"notes": { "fa": "…", "en": "…" },
	"packages": {
		"windows-x64": {
			"size": 35123456,
			"sha256": "<64 hex digits>",
			"urls": [
				"https://eleza.me/desktop/updates/Eleza-0.4.0-windows-x64.zip",
				"https://github.com/eleza-me/desktop/releases/download/v0.4.0/Eleza-0.4.0-windows-x64.zip"
			]
		},
		"linux-x86_64": { "size": 9123456, "sha256": "…", "urls": [ "…" ] }
	}
}
```

* `version`, `min_supported`: exactly `x.y.z`. Versions below `min_supported`
  must update before they can be used. `min_supported` and the mirrors come from
  `packaging/update-config.json`.
* `published_at`: UTC, ending in `Z`.
* `urls`: one to eight HTTPS addresses, tried in order.
* `update.json.sig`: the Ed25519 signature (64 bytes) of the bytes of
  `update.json`, as one line of base64. Made with
  `openssl pkeyutl -sign -rawin`; nothing may reformat the manifest after it was
  signed.

The packages are zip archives whose root is the install directory
(`Eleza.exe`, `Updater.exe`, the Qt libraries… / `Eleza`, …) plus
`eleza-files.list`. They are the same files people download as the portable
build.

## 3. Cutting a release

1. **Version.** Set it in `project(Eleza VERSION x.y.z …)` of the root
   `CMakeLists.txt` and in `vcpkg.json` (`version-string`).
2. **Notes.** Write `packaging/release-notes/x.y.z.fa.md` and `x.y.z.en.md`: a
   few short plain-text lines. They are shown on the Updates page of the app
   («تازه‌های نسخهٔ …») and the English one becomes the text of the GitHub
   Release.
3. **Minimum version** (rarely). To force everybody below some version to
   update, raise `min_supported` in `packaging/update-config.json`.
4. Merge all of that, then tag the merge commit and push the tag:

   ```sh
   git tag v0.4.0 && git push origin v0.4.0
   ```

   (or run the *release* workflow by hand with the version; it creates the tag).

What `.github/workflows/release.yml` does then:

1. checks that the tag, `CMakeLists.txt` and `vcpkg.json` name the same version;
2. builds and tests both platforms (`build.yml`: Release, without the debug
   tools) and packs `Eleza-x.y.z-windows-x64.zip`,
   `Eleza-x.y.z-windows-x64-setup.exe`, `Eleza-x.y.z-linux-x86_64.zip` and the
   `.deb`;
3. checks the packages, writes `update.json`
   (`packaging/make-update-manifest.py`);
4. signs it with the secret `ELEZA_UPDATE_SIGNING_KEY`
   (`packaging/sign-update-manifest.sh`) and verifies the signature with the
   public keys read from the source of that very commit
   (`packaging/update-public-keys.py`) — a signature no build would accept stops
   the release;
5. creates the GitHub Release `vx.y.z` with all files, `update.json` and
   `update.json.sig` (as a draft first, so that it becomes "latest" only when
   everything is uploaded);
6. if the mirror secrets are set, uploads the same files to the bucket behind
   `https://eleza.me/desktop/updates/` (section 5).

From that moment running copies find the update at their next check.

**A release cannot be taken back** by deleting it: copies that saw it remember
its `published_at` and refuse older manifests. To withdraw a bad version,
release a higher one.

## 4. The signing key

The app trusts the Ed25519 public keys listed in
`Eleza/SourceFiles/updater/updater_keys.h` (the raw 32 bytes, base64). Whoever
holds a matching private key can make every installed copy run their code:
treat it like the keys to the product.

* **Where it lives.** In the repository secret `ELEZA_UPDATE_SIGNING_KEY`
  (GitHub → Settings → Secrets and variables → Actions): the whole PEM file of
  the PKCS#8 private key, as written by

  ```sh
  openssl genpkey -algorithm ed25519 -out eleza-update-key.pem
  ```

  The matching line for `updater_keys.h`:

  ```sh
  openssl pkey -in eleza-update-key.pem -pubout -outform DER | tail -c 32 | openssl base64 -A
  ```

  The workflow never prints the key; it is written to a temporary file readable
  by the job only and removed when the step ends. Restricting who may push `v*`
  tags and run workflows (branch / tag protection, or a protected *environment*
  for the publish job) is what keeps the secret usable only for real releases.
* **Back it up.** A secret cannot be read back from GitHub. Keep the PEM file in
  at least two places that do not depend on GitHub (a password manager of the
  owners, an encrypted offline copy). **If the key is lost, no installed copy
  can ever be updated by itself again** — every user has to install by hand a
  build that trusts a new key.
* **Never** put a private key into the repository, a log or an issue. The test
  tools generate their own throw-away keys.

### Rotating the key

Planned (the old key is still safe):

1. Generate the new key pair; back up the private key.
2. Add the new public key as a second line between the markers of
   `updater_keys.h`. Release this version, **still signed with the old key**.
   Every copy that takes this update trusts both keys.
3. Wait until the copies in use have moved past that version (raise
   `min_supported` to it to be sure: older copies are then made to update, which
   they still can, because the manifest is signed with the old key).
4. Replace the secret `ELEZA_UPDATE_SIGNING_KEY` with the new private key and
   release. From here manifests are signed with the new key.
5. A release or two later remove the old public key from `updater_keys.h`.

The release workflow protects the order: it refuses to publish a manifest whose
signature does not verify with a key of the commit being released.

If the key leaked: do steps 1, 2 and 4 at once and release immediately; copies
that have not taken that release yet remain exposed to whoever holds the old
key until they do (they still trust it). There is no revocation short of that.

## 5. The mirror on eleza.me

The first manifest source is Eleza's own server, so that updates do not depend
on GitHub being reachable. It is plain static hosting; the app tolerates it
being absent, slow or stale and falls back to GitHub by itself.

Expected public mapping:

```
https://eleza.me/desktop/updates/update.json
https://eleza.me/desktop/updates/update.json.sig
https://eleza.me/desktop/updates/Eleza-<version>-windows-x64.zip
https://eleza.me/desktop/updates/Eleza-<version>-linux-x86_64.zip
https://eleza.me/desktop/updates/Eleza-<version>-windows-x64-setup.exe   (for the download page)
```

The release workflow uploads these to an S3-compatible bucket (ArvanCloud Object
Storage) when all four secrets exist:

| Secret | Example |
| --- | --- |
| `ELEZA_UPDATE_S3_ENDPOINT` | `https://s3.ir-thr-at1.arvanstorage.ir` |
| `ELEZA_UPDATE_S3_BUCKET` | `eleza-desktop` |
| `ELEZA_UPDATE_S3_ACCESS_KEY` | the access key of a user that may write to the bucket |
| `ELEZA_UPDATE_S3_SECRET_KEY` | its secret key |

The objects are written under the key prefix `desktop/updates/` (the repository
*variable* `ELEZA_UPDATE_S3_PREFIX` changes it) with a public-read ACL. Make
`https://eleza.me/desktop/updates/<file>` serve `<bucket>/desktop/updates/<file>`:
a CDN or reverse-proxy rule in front of the bucket, or the bucket's own domain
behind a rewrite. Requirements for whatever serves it:

* HTTPS with a certificate the system trusts; redirects only to HTTPS;
* the files byte for byte (no minifying of `update.json`, no HTML error page
  with status 200);
* `update.json` and `update.json.sig` not cached for long (the workflow sets
  `Cache-Control: no-cache`); the zip files may be cached for ever, their names
  never repeat;
* `Range` requests are welcome (resumed downloads) but not required.

Without the secrets the step is skipped with a notice; nothing else changes.

Nothing on the mirror has to be trusted: a manifest is accepted only with a
valid signature and a package only with the signed hash. A mirror that serves
an old manifest is skipped; one that serves a wrong package costs a download and
the next mirror is used.

## 6. Testing an update locally

Never with the release key: the test tools make their own.

### The whole way, on Linux

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && ninja -C build
tests/tools/update-e2e.sh build
```

The script makes a portable copy of the current build, builds "the next
version" from the same sources (`-DELEZA_TEST_VERSION`, which exists for this
only), publishes it on a local server signed with a fresh key, and runs the
copy headless with

```
--update-url=http://127.0.0.1:<port>/update.json --update-key=<public key> --update-now
```

It first serves a manifest that was changed after signing (must be refused,
nothing downloaded), then the good one, and checks that the copy became the new
version, that the same process now runs the new binary with its old arguments
and that nothing is left behind.

The three flags exist only in a build with the debug tools (`ELEZA_DEBUG_TOOLS`,
a Debug build); a release build has the built-in sources and keys and nothing
else. `--update-url` also allows `http://` on the loopback interface;
`--update-key` *replaces* the built-in keys.

### By hand (any platform)

```sh
# a portable build of the version to offer, then:
tests/tools/make-test-update.sh 0.4.0 <its directory> http://127.0.0.1:8765 /tmp/served
python3 -m http.server 8765 --bind 127.0.0.1 --directory /tmp/served
# in a packaged copy of an older debug build:
Eleza --update-url=http://127.0.0.1:8765/update.json --update-key=<the printed key>
```

Then use Settings → Updates. A build directory does not update itself (no
`eleza-files.list`): make a portable copy first
(`packaging/linux/make-portable.sh build <dir> <dir>.zip`, or on Windows the
deployed folder after `packaging/make-package.py`).

### The states of the UI, without a server

`--update-state=checking|uptodate|available|announced|downloading|preparing|ready|failed|failed-install|required|required-ready`
shows the update UI in that state (screenshots; nothing is downloaded).

### Automated tests

* `tests/core/test_updater.cpp` — the manifest signature (good, a changed byte,
  a wrong key, a cut signature), versions, replay and downgrade rules, hostile
  archives, and the checker against a local HTTP server (source and mirror
  fallback, resume, wrong packages).
* `tests/app/test_install_swap.cpp` — the swap with a failure and with a "power
  cut" injected at every file operation, recovery from the journal.
* `tests/tools/updater-smoke.py` — the real `Updater.exe` on a small fake
  install directory; ctest runs it on Windows
  (`python tests/tools/updater-smoke.py --wine <wine>` runs a cross-compiled
  updater on Linux).

## 7. Checklist for the first release with updates

- [ ] The private key behind
      `zBxbxS+zjNXeLugZPtboVLpysdO3HzFgaieVFubhkrI=` is in the secret
      `ELEZA_UPDATE_SIGNING_KEY` and backed up in two other places.
- [ ] Tags `v*` and the *release* workflow can be pushed / run only by the
      people who may release.
- [ ] Optional: the four `ELEZA_UPDATE_S3_*` secrets, and
      `https://eleza.me/desktop/updates/update.json` answers with the uploaded
      file after a trial run.
- [ ] `https://eleza.me` (the page copies that cannot update themselves open)
      offers the downloads.
- [ ] The Windows job is green, including `test_install_swap`,
      `test_updater` and `test_updater_smoke`; install the produced setup on a
      real Windows machine, then release a second version and watch the first
      one update (also once from an `/ALLUSERS` install, for the administrator
      prompt).
- [ ] The same on Linux with the portable zip; the `.deb` only announces.
