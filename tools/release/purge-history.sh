#!/bin/sh
# W-1054: purge what can't go public from AirWriter's history. Run only when no
# session has an open branch or worktree (every commit hash changes), from a
# clean main that already contains wells/public-release.
# Validated as a dry run on a mirror on 2026-10-15: 120 MB -> 45 MB, no PDFs,
# no old USB-C model, gitleaks clean.
set -e
REPO=${1:-/Users/wells/Projects/airwriter}
cd "$REPO"
REL=main   # the branch holding the replacement files
LIST=$(mktemp)
keep_model=$(git rev-parse $REL:hardware/airwriter/lib/airwriter.3dshapes/USB_C_Receptacle_HRO_TYPE-C-31-M-12.step)
keep_step=$(git rev-parse $REL:hardware/airwriter/fab/airwriter-3d.step)
keep_zip=$(git rev-parse $REL:hardware/airwriter/production/airwriter.zip)
# Every historical version of the three files that embed the old model, by path.
git log --all --pretty=format: --raw --no-abbrev -- \
    hardware/airwriter/lib/airwriter.3dshapes/USB_C_Receptacle_HRO_TYPE-C-31-M-12.step \
    hardware/airwriter/fab/airwriter-3d.step \
    hardware/airwriter/production/airwriter.zip |
  awk 'NF>=5 {print $3; print $4}' | grep -v '^0\{40\}$' | sort -u |
  grep -v -e "$keep_model" -e "$keep_step" -e "$keep_zip" > "$LIST"
echo "stripping $(wc -l < "$LIST") blobs"
git filter-repo --force --invert-paths \
    --path docs/reference/ImageWriter_II_Technical_Reference.pdf \
    --path docs/reference/Inside_AppleTalk_2nd_ed.pdf \
    --path-glob 'hardware/airwriter/_restore_backup_*' \
    --strip-blobs-with-ids "$LIST"
git gc --prune=now --aggressive
echo "pdfs left: $(git rev-list --objects --all | grep -ci '\.pdf$')"
gitleaks git --log-opts="--all" --no-banner --redact
