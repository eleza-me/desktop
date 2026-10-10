#!/usr/bin/env python3
"""Packs a deployed build directory into the archive that is both the portable
download and the update package.

    python3 packaging/make-package.py <directory> <output.zip>

    Windows: <directory> is the deployed folder (Eleza.exe, Updater.exe, the Qt
             libraries and plugins).
    Linux:   <directory> is what packaging/linux/make-portable.sh assembled.

What it does:

  * writes <directory>/eleza-files.list: every file of the package, one
    relative path per line. The file marks a copy that updates itself, and the
    next update removes what the list names and the new version no longer has.
    Run this before the installer is built so that the installer ships it too;
  * refuses anything the app's unpacker would refuse (links, names outside a
    conservative character set), so that a release cannot be published that no
    client can install;
  * writes a reproducible zip: sorted entries, fixed timestamps, Unix mode
    0755 for executables and 0644 for everything else, no directory entries;
  * prints the size and the SHA-256 of the archive.

Only the standard library is used (the release workflow runs it on both
platforms).
"""

import hashlib
import os
import re
import stat
import sys
import zipfile

FILE_LIST = "eleza-files.list"
# What the updater keeps for itself in an install directory.
RESERVED = re.compile(r"^(_backup_|_update_)", re.IGNORECASE)
COMPONENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
DEVICES = re.compile(r"^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\.|$)", re.IGNORECASE)
TIMESTAMP = (2000, 1, 1, 0, 0, 0)


def fail(message):
    sys.exit("make-package: " + message)


def check_name(relative):
    parts = relative.split("/")
    for part in parts:
        if (not COMPONENT.match(part) or part.endswith(".")
                or DEVICES.match(part) or len(part) > 200):
            fail("the updater would refuse this file name: " + relative)
    if RESERVED.match(parts[0]) or len(relative) > 400:
        fail("the updater would refuse this file name: " + relative)


def collect(root):
    result = []
    for directory, names, files in os.walk(root):
        names.sort()
        for name in names:
            if os.path.islink(os.path.join(directory, name)):
                fail("a link in the package: " + os.path.join(directory, name))
        for name in sorted(files):
            path = os.path.join(directory, name)
            if os.path.islink(path) or not os.path.isfile(path):
                fail("not a regular file: " + path)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            check_name(relative)
            result.append(relative)
    result.sort()
    folded = {}
    for relative in result:
        key = relative.lower()
        if key in folded:
            fail("two names that differ only by case: %s, %s" % (folded[key], relative))
        folded[key] = relative
    return result


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    output = os.path.abspath(sys.argv[2])
    if not os.path.isdir(root):
        fail("not a directory: " + root)
    if os.path.commonpath([root, output]) == root:
        fail("the archive must not be written into the package directory")

    list_path = os.path.join(root, FILE_LIST)
    if os.path.lexists(list_path):
        os.remove(list_path)
    files = collect(root)
    if not files:
        fail("the directory is empty")
    files = sorted(files + [FILE_LIST])
    with open(list_path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write("".join(name + "\n" for name in files))

    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative in files:
            path = os.path.join(root, relative.replace("/", os.sep))
            executable = (os.name != "nt") and bool(os.stat(path).st_mode & 0o111)
            info = zipfile.ZipInfo(relative, TIMESTAMP)
            info.create_system = 3  # Unix: the mode is in the upper 16 bits
            info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as stream:
                archive.writestr(info, stream.read(), compresslevel=9)

    digest = hashlib.sha256()
    with open(output, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    print("%s: %d files, %d bytes, sha256 %s" % (
        os.path.basename(output), len(files), os.path.getsize(output), digest.hexdigest()))


if __name__ == "__main__":
    main()
