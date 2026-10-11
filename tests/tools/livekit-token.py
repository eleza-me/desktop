#!/usr/bin/env python3
"""Mints a room token for a local LiveKit server (the call test bed).

    python3 tests/tools/livekit-token.py --room <room> --identity <name>
        [--key devkey] [--secret secret] [--ttl 600]

The token is the JWT the server expects (HS256 over the API secret, the room
grant in the "video" claim): what the Eleza backend mints for every call
participant. `livekit-server --dev` accepts the key "devkey" with the secret
"secret". Only the standard library is used.
"""

import argparse
import base64
import hashlib
import hmac
import json
import time


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")


def mint(key, secret, room, identity, ttl=600, now=None):
    now = int(time.time() if now is None else now)
    header = {"alg": "HS256", "typ": "JWT"}
    claims = {
        "iss": key,
        "sub": identity,
        "nbf": now - 10,
        "exp": now + ttl,
        "video": {
            "room": room,
            "roomJoin": True,
            "canPublish": True,
            "canSubscribe": True,
        },
    }
    signed = b64(json.dumps(header, separators=(",", ":")).encode()) + b"." + \
        b64(json.dumps(claims, separators=(",", ":")).encode())
    signature = hmac.new(secret.encode(), signed, hashlib.sha256).digest()
    return (signed + b"." + b64(signature)).decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--room", required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--key", default="devkey")
    parser.add_argument("--secret", default="secret")
    parser.add_argument("--ttl", type=int, default=600)
    args = parser.parse_args()
    print(mint(args.key, args.secret, args.room, args.identity, args.ttl))


if __name__ == "__main__":
    main()
