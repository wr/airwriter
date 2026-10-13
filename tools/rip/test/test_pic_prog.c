// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the PIC12F1840 LVP programmer (pic_prog_core) against a
// pin-level simulated part (pic_sim).
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pic_prog_core.h"
#include "pic_sim.h"

#define TASHTALK_HEX "../../firmware/components/pic_prog/tashtalk-2.1.3.hex"

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f && "run from tools/rip");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static pic_image_t img;   // 8 KB; keep it off the stack
static pic_sim_t   sim;

static void load_tashtalk(int lvp)
{
    size_t len;
    char *hex = slurp(TASHTALK_HEX, &len);
    int line = 0;
    assert(pic_hex_parse(hex, len, &img, &line) == PIC_HEX_OK);
    free(hex);
    if (lvp) pic_image_enable_lvp(&img);
}

static void test_parse_tashtalk(void)
{
    load_tashtalk(0);
    assert(img.has_config);
    assert(img.prog[0] == 0x3217);            // first record of the upstream file
    assert(img.config1 == 0x09C4);
    assert(img.config2 == 0x1FFF);            // upstream ships LVP off
    unsigned used = 0, last = 0;
    for (unsigned i = 0; i < PIC_PROG_WORDS; i++)
        if (img.prog[i] != PIC_BLANK) { used++; last = i; }
    assert(last == 0xFC7);
    assert(used > 3000);
    pic_image_enable_lvp(&img);
    assert(img.config2 == 0x3FFF);
    printf("  parse tashtalk: %u words used, last 0x%03X\n", used, last);
}

static void test_parse_errors(void)
{
    int line;
    const char *bad_sum = ":020000001732B6\n:00000001FF\n";
    assert(pic_hex_parse(bad_sum, strlen(bad_sum), &img, &line) == PIC_HEX_CHECKSUM && line == 1);

    const char *no_eof = ":020000001732B5\n";
    assert(pic_hex_parse(no_eof, strlen(no_eof), &img, &line) == PIC_HEX_NO_EOF);

    // One byte of data EEPROM (0x1E000): refused rather than silently dropped.
    const char *eeprom = ":020000040001F9\n:01E00000AA75\n:00000001FF\n";
    assert(pic_hex_parse(eeprom, strlen(eeprom), &img, &line) == PIC_HEX_RANGE && line == 2);

    const char *junk = "hello\n";
    assert(pic_hex_parse(junk, strlen(junk), &img, &line) == PIC_HEX_SYNTAX);

    // Program words only, no configuration: parses, but flashing refuses it.
    const char *nocfg = ":020000001732B5\n:00000001FF\n";
    assert(pic_hex_parse(nocfg, strlen(nocfg), &img, &line) == PIC_HEX_OK && !img.has_config);
    pic_sim_init(&sim, 0x1B85);
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_ERR_IMAGE);
    assert(sim.lvp_entries == 0);
}

static void test_flash_blank(void)
{
    load_tashtalk(1);
    pic_sim_init(&sim, 0x1B85);
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_OK);
    assert(r.err == PIC_OK && r.devid == 0x1B85);
    assert(memcmp(sim.prog, img.prog, sizeof img.prog) == 0);
    assert(sim.cfg[7] == 0x09C4 && sim.cfg[8] == 0x3FFF);
    assert(sim.cfg[9] == 0x28D7 && sim.cfg[10] == 0x1D48);   // calibration untouched
    assert(pic_sim_errors(&sim) == 0);
    assert(!sim.in_prog && sim.mclr && !sim.dat_driving);   // out of LVP, running
    const uint16_t cfg[PIC_CFG_WORDS] = { 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF,
                                          0x1B85, 0x09C4, 0x3FFF, 0x28D7, 0x1D48 };
    assert(memcmp(r.cfg, cfg, sizeof cfg) == 0);             // the logged read-back
    printf("  flash blank: ok in %.2f s simulated\n", sim.now_us / 1e6);
}

static void test_flash_over_old_code(void)
{
    load_tashtalk(1);
    pic_sim_init(&sim, 0x1B85);
    for (unsigned i = 0; i < PIC_PROG_WORDS; i++) sim.prog[i] = (uint16_t)(i * 7 & 0x3FFF);
    sim.cfg[7] = 0x0000;                  // code-protected, everything off
    sim.cfg[8] = 0x2000;                  // LVP still on, or we couldn't be here
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_OK);
    assert(memcmp(sim.prog, img.prog, sizeof img.prog) == 0);
    assert(sim.cfg[7] == 0x09C4 && sim.cfg[8] == 0x3FFF);
    assert(pic_sim_errors(&sim) == 0);
}

static void test_unpatched_image_fails_config_verify(void)
{
    // Upstream TashTalk clears LVP, which LVP mode is not allowed to do: the
    // part keeps the bit set and the configuration verify says so. This is
    // why the image gets patched before it is written.
    load_tashtalk(0);
    pic_sim_init(&sim, 0x1B85);
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_ERR_VERIFY);
    assert(r.bad_addr == PIC_ADDR_CONFIG2 && r.want == 0x1FFF && r.got == 0x3FFF);
}

static void test_wrong_device_is_not_erased(void)
{
    load_tashtalk(1);
    pic_sim_init(&sim, 0x2705);           // a PIC12F1822, rev 5
    for (unsigned i = 0; i < PIC_PROG_WORDS; i++) sim.prog[i] = 0x1234;
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_ERR_WRONG_DEVICE);
    assert(r.devid == 0x2705);
    for (unsigned i = 0; i < PIC_PROG_WORDS; i++) assert(sim.prog[i] == 0x1234);
    assert(pic_sim_errors(&sim) == 0);
}

static void test_no_device(void)
{
    load_tashtalk(1);
    pic_sim_init(&sim, 0x1B85);
    sim.absent = 1;
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_ERR_NO_DEVICE);
    assert(r.devid == 0x0000);

    uint16_t id = 0xFFFF;
    assert(pic_read_id(&h, &id) == PIC_ERR_NO_DEVICE && id == 0);
}

static void test_stuck_bit_reported(void)
{
    load_tashtalk(1);
    pic_sim_init(&sim, 0x1B85);
    uint16_t want = img.prog[0x123];
    assert(want != 0);
    sim.stuck_addr = 0x123;
    sim.stuck_mask = want & (uint16_t)-want;  // its lowest set bit reads as 0
    pic_hal_t h = pic_sim_hal(&sim);
    pic_result_t r;
    assert(pic_flash(&h, &img, &r) == PIC_ERR_VERIFY);
    assert(r.bad_addr == 0x123);
    assert(r.want == want && r.got == (want & ~sim.stuck_mask));

    char json[256], expect[256];
    pic_result_json(&r, json, sizeof json);
    snprintf(expect, sizeof expect,
             "{\"ok\":false,\"err\":\"verify failed\",\"devid\":\"0x1B85\",\"rev\":5,"
             "\"addr\":\"0x0123\",\"want\":\"0x%04X\",\"got\":\"0x%04X\"}", r.want, r.got);
    assert(strcmp(json, expect) == 0);
}

static void test_read_id(void)
{
    pic_sim_init(&sim, 0x1B85);
    pic_hal_t h = pic_sim_hal(&sim);
    uint16_t id = 0;
    assert(pic_read_id(&h, &id) == PIC_OK && id == 0x1B85);
    assert(sim.lvp_entries == 1 && pic_sim_errors(&sim) == 0);
}

// The simulator's contention rule has to bite, or the ordering tests above
// prove nothing: driving DAT while the part runs TashTalk is a short.
static void test_sim_catches_contention(void)
{
    pic_sim_init(&sim, 0x1B85);
    pic_hal_t h = pic_sim_hal(&sim);
    h.dat_out(h.ctx, 0);                  // MCLR still high: RA0 is a UART TX
    assert(sim.err_contention == 1);
    h.mclr(h.ctx, 0);
    h.mclr(h.ctx, 1);                     // released into a driven DAT
    assert(sim.err_contention == 2);
}

int main(void)
{
    test_sim_catches_contention();
    test_parse_tashtalk();
    test_parse_errors();
    test_read_id();
    test_flash_blank();
    test_flash_over_old_code();
    test_unpatched_image_fails_config_verify();
    test_wrong_device_is_not_erased();
    test_no_device();
    test_stuck_bit_reported();
    printf("test_pic_prog: ok\n");
    return 0;
}
