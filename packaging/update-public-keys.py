#!/usr/bin/env python3
"""Prints the update public keys that are compiled into the app, read straight
from the source (Eleza/SourceFiles/updater/updater_keys.h), so that what the
release workflow verifies against can never differ from what a build trusts.

    python3 packaging/update-public-keys.py            # one base64 key per line
    python3 packaging/update-public-keys.py --pem-dir keys/
                                                       # also keys/update-key-<n>.pem
                                                       # (for `openssl pkeyutl -verify -pubin`)
"""

import base64
import os
import re
import sys

SOURCE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "Eleza", "SourceFiles", "updater", "updater_keys.h")
BEGIN = "ELEZA-UPDATE-KEYS-BEGIN"
END = "ELEZA-UPDATE-KEYS-END"
# SubjectPublicKeyInfo of an Ed25519 key: this prefix, then the 32 raw bytes.
SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def fail(message):
    sys.exit("update-public-keys: " + message)


def read_keys():
    with open(SOURCE, encoding="utf-8") as stream:
        text = stream.read()
    begin = text.find(BEGIN)
    end = text.find(END)
    if begin < 0 or end < begin:
        fail("the key markers are missing in " + SOURCE)
    keys = re.findall(r'"([^"]*)"', text[begin:end])
    if not keys:
        fail("no keys between the markers in " + SOURCE)
    for key in keys:
        try:
            raw = base64.b64decode(key, validate=True)
        except ValueError:
            fail("not base64: " + key)
        if len(raw) != 32:
            fail("not a 32-byte key: " + key)
    return keys


def main():
    keys = read_keys()
    directory = None
    if len(sys.argv) == 3 and sys.argv[1] == "--pem-dir":
        directory = sys.argv[2]
    elif len(sys.argv) != 1:
        sys.exit(__doc__)
    for index, key in enumerate(keys, start=1):
        print(key)
        if directory:
            os.makedirs(directory, exist_ok=True)
            der = SPKI_PREFIX + base64.b64decode(key)
            body = base64.encodebytes(der).decode("ascii")
            path = os.path.join(directory, "update-key-%d.pem" % index)
            with open(path, "w", encoding="ascii", newline="\n") as stream:
                stream.write("-----BEGIN PUBLIC KEY-----\n" + body + "-----END PUBLIC KEY-----\n")


if __name__ == "__main__":
    main()
