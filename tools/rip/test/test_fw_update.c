// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the firmware-update core (fw_update_core, W-1048): versions,
// the release manifest, the URLs built from it, and when auto-update acts.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fw_update_core.h"

static void ver(const char *s, int ok, int a, int b, int c)
{
    fwu_ver_t v = { 9, 9, 9 };
    assert(fwu_parse_version(s, &v) == ok);
    assert(v.maj == a && v.min == b && v.pat == c);
}

static const char *MANIFEST = "{\"version\":\"v0.1.1\",\"esp32s3\":\"airwriter-esp32s3.bin\"}";

static void manifest(void)
{
    char v[32], f[65];
    assert(fwu_parse_manifest(MANIFEST, "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_OK);
    assert(!strcmp(v, "v0.1.1") && !strcmp(f, "airwriter-esp32s3.bin"));
    assert(fwu_parse_manifest(MANIFEST, "esp32c6", v, sizeof v, f, sizeof f) == FWU_M_NO_TARGET);
    assert(fwu_parse_manifest("{ \"version\" : \"v1.2.3\" ,\n \"esp32s3\": \"a.bin\" }",
                              "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_OK);
    assert(!strcmp(v, "v1.2.3") && !strcmp(f, "a.bin"));
    assert(fwu_parse_manifest("{\"version\":\"v1.2\",\"esp32s3\":\"a.bin\"}",
                              "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_BAD);
    assert(fwu_parse_manifest("{\"version\":\"v1.2.3\",\"esp32s3\":\"../a\"}",
                              "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_BAD);
    assert(fwu_parse_manifest("{\"version\":\"v1.2.3-rc.1\",\"esp32s3\":\"a.bin\"}",
                              "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_BAD);
    assert(fwu_parse_manifest("{\"version\":\"v0.1.1\",\"esp32s3\":\"airwr",
                              "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_BAD);
    assert(fwu_parse_manifest("", "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_BAD);
    assert(fwu_parse_manifest("<html>Not Found</html>", "esp32s3", v, sizeof v, f, sizeof f)
           == FWU_M_BAD);
    // A key that only contains the target's name is not the target.
    assert(fwu_parse_manifest("{\"version\":\"v0.1.1\",\"xesp32s3\":\"a.bin\"}",
                              "esp32s3", v, sizeof v, f, sizeof f) == FWU_M_NO_TARGET);
    // A value too long for the caller's buffer is refused, not cut short.
    assert(fwu_parse_manifest(MANIFEST, "esp32s3", v, sizeof v, f, 8) == FWU_M_BAD);
}

static void urls(void)
{
    char u[256];
    assert(fwu_manifest_url(u, sizeof u, "wr/airwriter"));
    assert(!strcmp(u, "https://github.com/wr/airwriter/releases/latest/download/airwriter.json"));
    assert(fwu_asset_url(u, sizeof u, "wr/airwriter", "v0.1.1", "airwriter-esp32s3.bin"));
    assert(!strcmp(u, "https://github.com/wr/airwriter/releases/download/v0.1.1/airwriter-esp32s3.bin"));
    char small[16];
    assert(!fwu_asset_url(small, sizeof small, "wr/airwriter", "v0.1.1", "airwriter-esp32s3.bin"));
    assert(!fwu_manifest_url(small, sizeof small, "wr/airwriter"));
    // Nothing unchecked goes into a URL.
    assert(!fwu_asset_url(u, sizeof u, "wr/airwriter", "v0.1", "a.bin"));
    assert(!fwu_asset_url(u, sizeof u, "wr/airwriter", "v0.1.1", "../a.bin"));
}

static const fwu_timing_t TM = { .first_s = 600, .interval_s = 86400, .retry_s = 3600,
                                 .idle_s = 600 };

// Auto on, a release build, joined at `base` + 20 s, nothing checked yet.
static fwu_inputs_t fresh(int64_t base)
{
    fwu_inputs_t in = {
        .auto_on = 1, .key_ok = 1, .running_release = 1,
        .now_s = base + 20, .joined_s = base + 20, .last_check_s = -1, .last_job_s = -1,
    };
    return in;
}

static void policy(int64_t base)
{
    fwu_inputs_t in = fresh(base);

    in.now_s = base + 619;
    assert(fwu_policy(&in, &TM) == FWU_WAIT);
    in.now_s = base + 620;
    assert(fwu_policy(&in, &TM) == FWU_CHECK);

    // Off, never joined, setup mode, another key: nothing at all.
    fwu_inputs_t g = in;
    g.auto_on = 0;     assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.joined_s = -1;  assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.setup_mode = 1; assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.key_ok = 0;     assert(fwu_policy(&g, &TM) == FWU_WAIT);

    // Checked at 620 and found something newer: install, while idle.
    in.last_check_s = base + 620;
    in.known_newer = 1;
    in.now_s = base + 621;
    assert(fwu_policy(&in, &TM) == FWU_INSTALL);
    g = in; g.newer_rolled_back = 1; assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.install_tried = 1;     assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.on_trial = 1;          assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.busy = 1;              assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.running_release = 0;   assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.known_newer = 0;       assert(fwu_policy(&g, &TM) == FWU_WAIT);

    // A job 300 s ago is too recent; 600 s ago is idle enough.
    g = in; g.last_job_s = in.now_s - 300; assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g = in; g.last_job_s = in.now_s - 600; assert(fwu_policy(&g, &TM) == FWU_INSTALL);

    // The next check is a day after the last one, and wins over an install
    // decided on a day-old answer.
    g = in; g.known_newer = 0;
    g.now_s = base + 620 + 86399; assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g.now_s = base + 620 + 86400; assert(fwu_policy(&g, &TM) == FWU_CHECK);

    // A failed check comes back an hour later, not a day.
    g = in; g.known_newer = 0; g.check_failed = 1;
    g.now_s = base + 4219; assert(fwu_policy(&g, &TM) == FWU_WAIT);
    g.now_s = base + 4220; assert(fwu_policy(&g, &TM) == FWU_CHECK);
}

int main(void)
{
    ver("v1.2.3", 1, 1, 2, 3);
    ver("v0.1.0-3-gabc1234-dirty", 1, 0, 1, 0);
    ver("v10.20.30", 1, 10, 20, 30);
    ver("ca0fc40-dirty", 0, 0, 0, 0);
    ver("", 0, 0, 0, 0);
    ver("v1.2", 0, 0, 0, 0);
    ver("1.2.3", 0, 0, 0, 0);
    ver("v1..3", 0, 0, 0, 0);
    ver("v99999.1.1", 0, 0, 0, 0);         // no part past four digits

    assert(fwu_is_release("v0.1.0"));
    assert(fwu_is_release("v10.20.30"));
    assert(!fwu_is_release("v0.1.0-3-gabc1234-dirty"));
    assert(!fwu_is_release("ca0fc40-dirty"));
    assert(!fwu_is_release("v0.1.0-rc.1"));
    assert(!fwu_is_release(""));

    assert(fwu_valid_tag("v0.1.1"));
    assert(fwu_valid_tag("v0.1.1-tamper"));
    assert(fwu_valid_tag("v0.1.1-rc.2"));
    assert(!fwu_valid_tag("v0.1.1-"));
    assert(!fwu_valid_tag("v0.1.1/x"));
    assert(!fwu_valid_tag("v0.1.1-a/b"));
    assert(!fwu_valid_tag("v0.1.1-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));   // 40 chars
    assert(!fwu_valid_tag("0.1.1"));

    assert(fwu_valid_file("airwriter-esp32s3.bin"));
    assert(!fwu_valid_file("../x"));
    assert(!fwu_valid_file(".hidden"));
    assert(!fwu_valid_file("a/b"));
    assert(!fwu_valid_file("a b"));
    assert(!fwu_valid_file(""));
    char long_name[66];
    memset(long_name, 'a', 65);
    long_name[65] = '\0';
    assert(!fwu_valid_file(long_name));
    long_name[64] = '\0';
    assert(fwu_valid_file(long_name));

    assert(fwu_newer("v0.1.1", "v0.1.0"));
    assert(!fwu_newer("v0.1.0", "v0.1.0"));
    assert(!fwu_newer("v0.1.0", "v0.1.0-3-gabc1234"));
    assert(fwu_newer("v0.1.0", "ca0fc40-dirty"));
    assert(!fwu_newer("v0.2.0", "v0.10.0"));
    assert(fwu_newer("v1.0.0", "v0.99.99"));
    assert(!fwu_newer("garbage", "v0.1.0"));
    assert(!fwu_newer("v0.1.1-rc.1", "v0.1.0"));     // only a release is ever newer

    manifest();
    urls();
    policy(0);
    policy(60LL * 86400);           // past the 49.7 days a 32-bit ms clock lasts

    printf("fw_update: ok\n");
    return 0;
}
