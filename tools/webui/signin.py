#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Signs in to an AirWriter with a password (W-1156), for curl.

    python3 tools/webui/signin.py http://10.0.1.115
    curl -b ~/.config/airwriter/cookies.txt http://10.0.1.115/api/status

It signs in as the settings page does: the board sends a challenge and this
answers with an HMAC keyed by PBKDF2 of the password, so the password never
crosses the network. The cookie lands in a curl cookie jar (mode 600). A board
with no password needs none of this.
"""
import argparse, getpass, hashlib, hmac, http.cookiejar, json, os, pathlib, sys, urllib.error, urllib.request

JAR = pathlib.Path.home() / ".config/airwriter/cookies.txt"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("board", help="http://<board>, or just its address")
    ap.add_argument("--jar", default=str(JAR), help="curl cookie jar to write (default %(default)s)")
    ap.add_argument("--password-stdin", action="store_true", help="read the password from stdin")
    a = ap.parse_args()
    base = a.board.rstrip("/")
    if "://" not in base:
        base = "http://" + base
    pw = sys.stdin.readline().rstrip("\n") if a.password_stdin else getpass.getpass("AirWriter's password: ")

    jar_path = pathlib.Path(a.jar).expanduser()
    jar = http.cookiejar.MozillaCookieJar(str(jar_path))
    if jar_path.exists():
        jar.load(ignore_discard=True, ignore_expires=True)
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

    for tries in range(2):
        try:
            n = json.load(op.open(base + "/api/auth/nonce", timeout=10))
            key = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(n["salt"]), n["iter"], 32)
            proof = hmac.new(key, b"signin" + bytes.fromhex(n["nonce"]), "sha256").hexdigest()
            body = json.dumps({"nonce": n["nonce"], "proof": proof}).encode()
            op.open(urllib.request.Request(base + "/api/auth/signin", data=body), timeout=10)
            break
        except urllib.error.HTTPError as e:
            if e.code == 400 and not tries:
                continue                    # a challenge from before a restart: once more
            print(e.read().decode("utf-8", "replace").strip() or "error %d" % e.code, file=sys.stderr)
            return 1
        except (OSError, ValueError, KeyError) as e:
            print("can't reach AirWriter at %s: %s" % (base, e), file=sys.stderr)
            return 2

    jar_path.parent.mkdir(parents=True, exist_ok=True)
    old = os.umask(0o077)
    try:
        jar.save(ignore_discard=True, ignore_expires=True)
    finally:
        os.umask(old)
    os.chmod(jar_path, 0o600)
    print("signed in; curl -b %s %s/api/status" % (jar_path, base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
