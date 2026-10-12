#!/usr/bin/env python3
"""Writes update.json, the manifest the app checks for new versions
(Eleza/SourceFiles/updater/updater_manifest.h describes the format).

    python3 packaging/make-update-manifest.py --version 0.4.0 \
        --packages dist --output dist/update.json

  --version       the version being released ("0.4.0")
  --packages      the directory with the package of every platform, named as
                  packaging/update-config.json says
                  (Eleza-<version>-windows-x64.zip, Eleza-<version>-linux-x86_64.zip)
  --output        where to write the manifest
  --config        another configuration file (default: update-config.json next
                  to this script): channel, min_supported, the mirrors
  --notes-dir     release notes: <version>.fa.md / <version>.en.md
                  (default: packaging/release-notes; a missing file is an
                  empty note)
  --published-at  the publication time, UTC, as 2026-10-11T10:00:00Z
                  (default: now)
  --mirror        an address pattern instead of the configured mirrors (may be
                  repeated; {file} and {version} are replaced) — for tests
  --allow-missing do not fail when the package of a platform is not there

The manifest is signed afterwards (packaging/sign-update-manifest.sh): the
signature is over the exact bytes written here, so nothing may reformat the
file between the two steps.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys

VERSION = re.compile(r"^[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}$")
MAX_NOTES = 16 * 1024


def fail(message):
    sys.exit("make-update-manifest: " + message)


def version_key(text):
    return tuple(int(part) for part in text.split("."))


def read_notes(directory, version, language):
    path = os.path.join(directory, "%s.%s.md" % (version, language))
    if not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8") as stream:
        text = stream.read().strip()
    if len(text) > MAX_NOTES:
        fail("the notes are longer than %d characters: %s" % (MAX_NOTES, path))
    return text


def describe(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return os.path.getsize(path), digest.hexdigest()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--packages", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--config", default=os.path.join(here, "update-config.json"))
    parser.add_argument("--notes-dir", default=os.path.join(here, "release-notes"))
    parser.add_argument("--published-at")
    parser.add_argument("--mirror", action="append")
    parser.add_argument("--allow-missing", action="store_true")
    arguments = parser.parse_args()

    version = arguments.version
    if not VERSION.match(version):
        fail("not a version (x.y.z): " + version)
    with open(arguments.config, encoding="utf-8") as stream:
        config = json.load(stream)
    minimum = config.get("min_supported", "0.0.0")
    if not VERSION.match(minimum) or version_key(minimum) > version_key(version):
        fail("min_supported (%s) must be a version not above %s" % (minimum, version))
    mirrors = arguments.mirror or config["mirrors"]
    if not mirrors or len(mirrors) > 8:
        fail("between one and eight mirrors are needed")

    published = arguments.published_at
    if not published:
        published = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    if not re.match(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$", published):
        fail("--published-at must look like 2026-10-11T10:00:00Z")

    packages = {}
    for platform, pattern in sorted(config["platforms"].items()):
        name = pattern.format(version=version)
        path = os.path.join(arguments.packages, name)
        if not os.path.isfile(path):
            if arguments.allow_missing:
                continue
            fail("the package of %s is missing: %s" % (platform, path))
        size, sha256 = describe(path)
        packages[platform] = {
            "size": size,
            "sha256": sha256,
            "urls": [mirror.format(file=name, version=version) for mirror in mirrors],
        }
    if not packages:
        fail("no packages in " + arguments.packages)

    manifest = {
        "format": 1,
        "channel": config.get("channel", "stable"),
        "version": version,
        "published_at": published,
        "min_supported": minimum,
        "notes": {
            "fa": read_notes(arguments.notes_dir, version, "fa"),
            "en": read_notes(arguments.notes_dir, version, "en"),
        },
        "packages": packages,
    }
    text = json.dumps(manifest, ensure_ascii=False, indent="\t") + "\n"
    if len(text.encode("utf-8")) > 256 * 1024:
        fail("the manifest is larger than the 256 KiB the app accepts")
    with open(arguments.output, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    for platform, package in packages.items():
        print("%s: %d bytes, sha256 %s" % (platform, package["size"], package["sha256"]))
    print("wrote %s (version %s, published %s)" % (arguments.output, version, published))


if __name__ == "__main__":
    main()
