// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware-update core (W-1048), pure C: release versions, the manifest
// a release carries, the URLs built from it, and when auto-update acts. No
// ESP-IDF here; fw_update.c does the fetching, the flashing and the clock.
//
// Nothing read from the network goes into a URL unchecked, and nothing here
// is trusted for safety: the image's signature, checked by esp_ota_end, is
// what decides whether it installs.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { int maj, min, pat; } fwu_ver_t;

// 1 if `s` starts with vX.Y.Z (each part 1-4 digits) followed by its end or
// a '-' (git describe's dev suffix); otherwise 0, with 0.0.0 in `out`.
int fwu_parse_version(const char *s, fwu_ver_t *out);
// Exactly vX.Y.Z: what a release build's version reads.
int fwu_is_release(const char *s);
// A tag an owner may install by name: a release, or one with a -suffix of
// letters, digits and dots (a prerelease). Under 32 characters.
int fwu_valid_tag(const char *s);
// A release asset's file name: 1-64 of [A-Za-z0-9._-], not starting with '.'.
int fwu_valid_file(const char *s);
// 1 if `latest` is a release whose version is past `running`'s. A dev build
// past v0.1.0 (v0.1.0-3-g...) counts as v0.1.0, so v0.1.0 is not offered to it.
int fwu_newer(const char *latest, const char *running);

typedef enum {
    FWU_M_OK,
    FWU_M_BAD,              // not a manifest, or a version or file name that fails the checks
    FWU_M_NO_TARGET,        // a good manifest with no image for this chip
} fwu_manifest_t;

// The release's airwriter.json, a flat object: {"version":"v0.1.1",
// "esp32s3":"airwriter-esp32s3.bin"}. `target` is the chip's name.
fwu_manifest_t fwu_parse_manifest(const char *json, const char *target,
                                  char *ver, size_t vcap, char *file, size_t fcap);

// Both return 0, leaving `out` unusable, if a part fails its check or the
// URL does not fit.
int fwu_manifest_url(char *out, size_t cap, const char *repo);
int fwu_asset_url(char *out, size_t cap, const char *repo, const char *tag, const char *file);

typedef struct {
    int64_t first_s;        // after the first Wi-Fi join, the first check
    int64_t interval_s;     // between checks
    int64_t retry_s;        // after a check that failed
    int64_t idle_s;         // no job begun or ended for this long before an install
} fwu_timing_t;

// Times are uptime seconds; -1 is never.
typedef struct {
    int auto_on;            // the owner's setting
    int setup_mode;         // on the setup access point: no internet
    int key_ok;             // the running image is signed with the release key
    int on_trial;           // the last update is still being checked (W-1046)
    int busy;               // a job holds the job lock
    int running_release;    // the running version is a release, not a dev build
    int known_newer;        // the last check found a newer release
    int newer_rolled_back;  // and it is the version the bootloader last rolled back
    int install_tried;      // an install was tried since the last check
    int check_failed;       // the last check got no answer
    int64_t now_s, joined_s, last_check_s, last_job_s;
} fwu_inputs_t;

typedef enum { FWU_WAIT, FWU_CHECK, FWU_INSTALL } fwu_action_t;

// What auto-update does now. Off, it does nothing: a person checks from the
// web page. It never replaces a dev build, never retries the version that
// rolled back, and tries an install once per check.
fwu_action_t fwu_policy(const fwu_inputs_t *in, const fwu_timing_t *tm);
