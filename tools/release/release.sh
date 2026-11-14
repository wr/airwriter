#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# W-1048: build, check, sign and publish an AirWriter firmware release.
#
#   tools/release/release.sh [--branch] [--dry-run] vX.Y.Z [notes-file]
#
# Builds an image per main board from HEAD in a throwaway worktree (W-1109:
# rev A's, airwriter-esp32s3.bin, and rev 2026.11's, airwriter-2026.11.bin),
# each from a fresh sdkconfig made of the defaults alone -- never a local
# sdkconfig, which holds Wi-Fi credentials -- and signs them with the release
# key. Then checks each: its version is the tag, its project is its board's, no
# local Wi-Fi credential or purge-list string is in it, no bench option is on,
# and the signature verifies against the committed public key. Only then does
# it tag HEAD, push the tag, and publish both images and their manifest with
# gh. Boards find it through releases/latest/download/airwriter.json, each by
# its own key in it (SPEC.md 3.12).
#
# Run from a clean main that matches origin. --branch allows a pushed branch
# instead (the first releases, cut before their branch merged; never rebase a
# branch after it). --dry-run builds and checks, and stops before the tag.
set -euo pipefail

KEY=${AIRWRITER_SIGNING_KEY:-$HOME/.config/airwriter/firmware-signing-key.pem}
SCRUB=$HOME/.config/airwriter/purge-scrub.txt
TARGET=esp32s3
# Each board: its key in airwriter.json (CONFIG_AIRWRITER_UPDATE_TARGET), its
# -DAIRWRITER_BOARD, and its image's project name. Rev A keeps the chip's name,
# which every release before rev 2026.11 used.
BOARDS="esp32s3:rev-a:airwriter 2026.11:2026.11:airwriter-2026.11"

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
SHA=$(git rev-parse HEAD)        # what is built is what is tagged
BR=$(git rev-parse --abbrev-ref HEAD)
[ "$BR" = main ] || [ $branch_ok = 1 ] || die "on $BR, not main (--branch to allow)"
if [ $dry = 0 ]; then
    git fetch -q origin
    [ "$SHA" = "$(git rev-parse "@{u}" 2>/dev/null)" ] ||
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
git worktree add -q --detach "$TMP/src" "$SHA"
FW=$TMP/src/firmware
# version.txt wins over git describe (IDF), so the image reads exactly $VER.
echo "$VER" > "$FW/version.txt"
printf 'CONFIG_SECURE_BOOT_SIGNING_KEY="%s"\n' "$KEY" > "$TMP/sdkconfig.release"

# ---- build and check each board's image
fail=0
bad() { echo "release: FAIL: $*" >&2; fail=1; }
OUT=$TMP/out
mkdir -p "$OUT"
manifest="\"version\":\"$VER\""
assets=()

for board in $BOARDS; do
    IFS=: read -r key arg proj <<< "$board"
    echo "release: building $VER for $key"
    BLD=$TMP/build-$key CFG=$TMP/sdkconfig-$key
    idf.py -C "$FW" -B "$BLD" -D SDKCONFIG="$CFG" -D AIRWRITER_BOARD="$arg" \
        -D SDKCONFIG_DEFAULTS="$FW/sdkconfig.defaults;$TMP/sdkconfig.release" \
        -D IDF_TARGET=$TARGET build > "$TMP/build-$key.log" 2>&1 ||
        { tail -30 "$TMP/build-$key.log"; die "the $key build failed (log: $TMP/build-$key.log)"; }
    IMG=$BLD/$proj.bin

    # esp_app_desc_t sits after the image header (24 bytes) and the first
    # segment's (8): version at +16, project_name at +48.
    read_str() { python3 -I -c 'import sys; f=open(sys.argv[1],"rb"); f.seek(int(sys.argv[2])); print(f.read(32).split(b"\0")[0].decode())' "$IMG" "$1"; }
    [ "$(read_str 48)" = "$VER" ] || bad "$key: the image says $(read_str 48), not $VER"
    [ "$(read_str 80)" = "$proj" ] || bad "$key: the image's project is $(read_str 80), not $proj"
    grep -q "^CONFIG_AIRWRITER_UPDATE_TARGET=\"$key\"$" "$CFG" ||
        bad "$key: the image would look for another key in airwriter.json"

    for opt in AIRWRITER_DEV_MODE AIRWRITER_LT_BENCH AIRWRITER_UART_LOOPBACK; do
        grep -q "^CONFIG_$opt=y" "$CFG" && bad "$key: CONFIG_$opt is on"
    done
    grep -q '^CONFIG_AIRWRITER_WIFI_SSID=""' "$CFG" || bad "$key: the release sdkconfig has a Wi-Fi SSID"
    grep -q '^CONFIG_AIRWRITER_WIFI_PASSWORD=""' "$CFG" || bad "$key: the release sdkconfig has a Wi-Fi password"

    # Strings that must not be in a public binary: the local builds' bench Wi-Fi
    # and the history purge's list (its left-hand sides). Never printed.
    contains() { python3 -I -c 'import sys; sys.exit(0 if sys.argv[2].encode() in open(sys.argv[1],"rb").read() else 1)' "$IMG" "$1"; }
    for k in AIRWRITER_WIFI_SSID AIRWRITER_WIFI_PASSWORD; do
        for local in "$ROOT/firmware/sdkconfig" "$ROOT/firmware/sdkconfig.2026.11"; do
            v=$(sed -n "s/^CONFIG_$k=\"\(.*\)\"$/\1/p" "$local" 2>/dev/null || true)
            [ -n "$v" ] && contains "$v" && bad "$key: the image contains a local $k"
        done
    done
    if [ -f "$SCRUB" ]; then
        while IFS= read -r line; do
            s=${line%%==>*}
            [ -n "$s" ] && contains "$s" && bad "$key: the image contains a purge-list string"
        done < "$SCRUB"
    fi

    # The digest boards compare the running image's key with (Kconfig) must be
    # the committed key's, or every board would read the release as someone else's.
    espsecure.py digest_sbv2_public_key --keyfile "$FW/keys/airwriter-release.pub.pem" \
        --output "$TMP/key.digest" > /dev/null 2>&1 || bad "could not digest the committed key"
    want=$(xxd -p -c64 "$TMP/key.digest" 2>/dev/null || true)
    grep -q "^CONFIG_AIRWRITER_UPDATE_KEY_DIGEST=\"$want\"$" "$CFG" ||
        bad "$key: CONFIG_AIRWRITER_UPDATE_KEY_DIGEST is not the committed key's digest ($want)"

    espsecure.py verify_signature --version 2 \
        --keyfile "$FW/keys/airwriter-release.pub.pem" "$IMG" > "$TMP/verify.log" 2>&1 ||
        { cat "$TMP/verify.log"; bad "$key: the signature does not verify against the committed key"; }

    cp "$IMG" "$OUT/airwriter-$key.bin"
    manifest="$manifest,\"$key\":\"airwriter-$key.bin\""
    assets+=("$OUT/airwriter-$key.bin")
    echo "release: $key: $(wc -c < "$OUT/airwriter-$key.bin" | tr -d ' ') bytes, signed"
done

[ $fail = 0 ] || die "checks failed; nothing was published"
printf '{%s}\n' "$manifest" > "$OUT/airwriter.json"
echo "release: $VER checks pass"

if [ $dry = 1 ]; then
    trap - EXIT; cleanup
    echo "release: dry run; assets in $OUT"
    exit 0
fi

git tag -a "$VER" -m "AirWriter $VER" "$SHA"
git push -q origin "refs/tags/$VER"
if [ -n "$NOTES" ]; then notes=(--notes-file "$NOTES"); else notes=(--notes ""); fi
if ! gh release create "$VER" "${assets[@]}" "$OUT/airwriter.json" \
        --title "AirWriter $VER" --verify-tag "${notes[@]}"; then
    # Leave nothing half-made: a tag with no release is a retry that fails.
    git push -q origin ":refs/tags/$VER" || true
    git tag -d "$VER" > /dev/null
    die "gh release create failed; the tag is removed again"
fi
echo "release: published $VER"
