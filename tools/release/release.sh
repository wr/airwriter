#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# W-1048: build, check, sign and publish an AirWriter firmware release.
#
#   tools/release/release.sh [--branch] [--dry-run] vX.Y.Z [notes-file]
#
# Builds the S3 image from HEAD in a throwaway worktree, from a fresh sdkconfig
# made of sdkconfig.defaults alone -- never the local sdkconfig, which holds
# Wi-Fi credentials -- and signs it with the release key. Then checks it: its
# version is the tag, no local Wi-Fi credential or purge-list string is in it,
# no bench option is on, and the signature verifies against the committed
# public key. Only then does it tag HEAD, push the tag, and publish the image
# and its manifest with gh. Boards find it through
# releases/latest/download/airwriter.json (SPEC.md 3.12).
#
# Run from a clean main that matches origin. --branch allows a pushed branch
# instead (the first releases, cut before their branch merged; never rebase a
# branch after it). --dry-run builds and checks, and stops before the tag.
set -euo pipefail

KEY=${AIRWRITER_SIGNING_KEY:-$HOME/.config/airwriter/firmware-signing-key.pem}
SCRUB=$HOME/.config/airwriter/purge-scrub.txt
TARGET=esp32s3

die() { echo "release: $*" >&2; exit 1; }

branch_ok=0 dry=0
while [ $# -gt 0 ]; do
    case $1 in
        --branch)  branch_ok=1; shift ;;
        --dry-run) dry=1; shift ;;
        -*)        die "unknown option $1" ;;
        *)         break ;;
    esac
done
[ $# -ge 1 ] || die "usage: release.sh [--branch] [--dry-run] vX.Y.Z [notes-file]"
VER=$1 NOTES=${2:-}
[[ $VER =~ ^v[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$ ]] || die "$VER is not vX.Y.Z"
[ -z "$NOTES" ] || [ -f "$NOTES" ] || die "no notes file $NOTES"
[ -f "$KEY" ] || die "no signing key at $KEY"

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
[ -z "$(git status --porcelain)" ] || die "the working tree is not clean"
BR=$(git rev-parse --abbrev-ref HEAD)
[ "$BR" = main ] || [ $branch_ok = 1 ] || die "on $BR, not main (--branch to allow)"
if [ $dry = 0 ]; then
    git fetch -q origin
    [ "$(git rev-parse HEAD)" = "$(git rev-parse "@{u}" 2>/dev/null)" ] ||
        die "$BR is not pushed, or not level with origin"
    git rev-parse -q --verify "refs/tags/$VER" >/dev/null && die "tag $VER exists"
    [ -z "$(git ls-remote --tags origin "refs/tags/$VER")" ] || die "tag $VER exists on origin"
    command -v gh >/dev/null || die "gh is not installed"
fi

if ! command -v idf.py >/dev/null; then
    # shellcheck disable=SC1091
    . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/airwriter-release.XXXXXX")
cleanup() { git -C "$ROOT" worktree remove --force "$TMP/src" 2>/dev/null || true; }
trap cleanup EXIT
git worktree add -q --detach "$TMP/src" HEAD
FW=$TMP/src/firmware
# version.txt wins over git describe (IDF), so the image reads exactly $VER.
echo "$VER" > "$FW/version.txt"
printf 'CONFIG_SECURE_BOOT_SIGNING_KEY="%s"\n' "$KEY" > "$TMP/sdkconfig.release"

echo "release: building $VER for $TARGET"
idf.py -C "$FW" -B "$TMP/build" -D SDKCONFIG="$TMP/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="$FW/sdkconfig.defaults;$TMP/sdkconfig.release" \
    -D IDF_TARGET=$TARGET build > "$TMP/build.log" 2>&1 ||
    { tail -30 "$TMP/build.log"; die "build failed (log: $TMP/build.log)"; }
IMG=$TMP/build/airwriter.bin

# ---- checks
fail=0
bad() { echo "release: FAIL: $*" >&2; fail=1; }

# esp_app_desc_t sits after the image header (24 bytes) and the first segment's
# (8): version at +16, project_name at +48.
read_str() { python3 -I -c 'import sys; f=open(sys.argv[1],"rb"); f.seek(int(sys.argv[2])); print(f.read(32).split(b"\0")[0].decode())' "$IMG" "$1"; }
[ "$(read_str 48)" = "$VER" ] || bad "the image says $(read_str 48), not $VER"
[ "$(read_str 80)" = airwriter ] || bad "the image's project is $(read_str 80)"

for opt in AIRWRITER_DEV_MODE AIRWRITER_LT_BENCH AIRWRITER_UART_LOOPBACK; do
    grep -q "^CONFIG_$opt=y" "$TMP/sdkconfig" && bad "CONFIG_$opt is on"
done
grep -q '^CONFIG_AIRWRITER_WIFI_SSID=""' "$TMP/sdkconfig" || bad "the release sdkconfig has a Wi-Fi SSID"
grep -q '^CONFIG_AIRWRITER_WIFI_PASSWORD=""' "$TMP/sdkconfig" || bad "the release sdkconfig has a Wi-Fi password"

# Strings that must not be in a public binary: this checkout's bench Wi-Fi and
# the history purge's list (its left-hand sides). Never printed.
contains() { python3 -I -c 'import sys; sys.exit(0 if sys.argv[2].encode() in open(sys.argv[1],"rb").read() else 1)' "$IMG" "$1"; }
for k in AIRWRITER_WIFI_SSID AIRWRITER_WIFI_PASSWORD; do
    v=$(sed -n "s/^CONFIG_$k=\"\(.*\)\"$/\1/p" "$ROOT/firmware/sdkconfig" 2>/dev/null || true)
    [ -n "$v" ] && contains "$v" && bad "the image contains the local $k"
done
if [ -f "$SCRUB" ]; then
    while IFS= read -r line; do
        s=${line%%==>*}
        [ -n "$s" ] && contains "$s" && bad "the image contains a purge-list string"
    done < "$SCRUB"
fi

espsecure.py verify_signature --version 2 \
    --keyfile "$FW/keys/airwriter-release.pub.pem" "$IMG" > "$TMP/verify.log" 2>&1 ||
    { cat "$TMP/verify.log"; bad "the signature does not verify against the committed key"; }

[ $fail = 0 ] || die "checks failed; nothing was published"

OUT=$TMP/out
mkdir -p "$OUT"
cp "$IMG" "$OUT/airwriter-$TARGET.bin"
printf '{"version":"%s","%s":"airwriter-%s.bin"}\n' "$VER" "$TARGET" "$TARGET" > "$OUT/airwriter.json"
echo "release: $VER checks pass ($(wc -c < "$OUT/airwriter-$TARGET.bin" | tr -d ' ') bytes, signed)"

if [ $dry = 1 ]; then
    trap - EXIT; cleanup
    echo "release: dry run; assets in $OUT"
    exit 0
fi

git tag -a "$VER" -m "AirWriter $VER" HEAD
git push -q origin "refs/tags/$VER"
if [ -n "$NOTES" ]; then notes=(--notes-file "$NOTES"); else notes=(--notes ""); fi
gh release create "$VER" "$OUT/airwriter-$TARGET.bin" "$OUT/airwriter.json" \
    --title "AirWriter $VER" --verify-tag "${notes[@]}"
echo "release: published $VER"
