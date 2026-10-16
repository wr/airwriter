# Known-good slot — design

Linear: `W-1059 Known-good slot: factory reset boots the last version that worked`.
Written 2026-10-16. Amends SPEC §3.11 (W-1046's factory slot and power-up reset) and §3.12
(where an update lands).

## Intent

The power-up reset (test button held through power-up, W-1046) must put a board back on the
last firmware that worked on it, not on the version it shipped with. A board bought on 0.1.5
and kept current to 0.2.9 should come back on 0.2.9, with its settings erased.

Wells chose the mechanism (Linear, 2026-10-16): no `factory` partition; three equal app slots
and a small record, written twice, that names the known-good slot. The firmware moves the
record; nothing is ever copied between slots, so there is never a moment without one complete
good copy.

## Goals, non-goals, anti-goals

Goals
- The power-up reset erases settings and boots the known-good slot, or any valid slot if that
  one fails its check.
- An update never writes the running slot or the known-good slot.
- An image becomes known-good once it has been kept by the trial gate and has finished a real
  print.
- A power cut at any moment leaves a bootable board and a readable record.

Non-goals: changing the 8 s in-app reset (W-1045; settings only), updating the bootloader over
the air, secure boot, a web control for the record, writing the record from the bootloader.

Anti-goals, each a failure even if the reset works:
- any path where an update overwrites the only good image;
- a bootloader that can boot nothing after a reset when a valid image is on the flash;
- a USB flash of a release that does not boot that release.

## Flash layout (8 MB)

```
nvs,        data, nvs,       0x9000,   0x6000     unchanged
otadata,    data, ota,       0xf000,   0x2000     unchanged
phy_init,   data, phy,       0x11000,  0x1000     unchanged
known_good, data, undefined, 0x12000,  0x2000     new: the record, two 4 KB sectors
ota_0,      app,  ota_0,     0x20000,  0x200000   where factory was
ota_1,      app,  ota_1,     0x220000, 0x200000
ota_2,      app,  ota_2,     0x420000, 0x200000
storage,    data, spiffs,    0x620000, 0x1e0000   unchanged
```

nvs, otadata and phy_init keep their offsets, so the one USB flash that moves a board keeps its
Wi-Fi credentials and settings. `known_good` sits in the unused gap below 0x20000.

## The record

Each sector starts with one 16-byte copy: `magic` (`AWKG`), `seq`, `slot`, and a CRC-32 of the
first 12 bytes. A copy is valid if magic and CRC match and `seq` is not all ones. The current
copy is the valid one with the higher `seq`. The known-good slot is the current copy's `slot`;
no valid copy, or a slot past the table's last OTA slot, means ota_0.

Writing a new slot: erase the sector that does *not* hold the current copy, write the new copy
there with `seq + 1`, read it back. A cut during the erase or write leaves that copy invalid
and the old one current. This is how otadata itself is written.

`idf.py flash` writes a blank (all 0xFF) `known_good` alongside the blank otadata, so a USB
flash always starts at "known-good is ota_0", which is where `idf.py flash` puts the app
(ESP-IDF picks the factory slot if there is one, else ota_0).

## Promotion (firmware, `components/ota_health`)

The running image writes the record to name its own slot when both are true:
- it is kept: not on trial (ESP-IDF's `PENDING_VERIFY`), whether the trial gate just passed or
  it was never on trial (a USB flash, a reset);
- it has finished a real print since boot: an IPP job or a LocalTalk job that ran to its end,
  every byte taken by the printer. Not the test page, the cable test, a cancelled, failed or
  cut-short job. *(My call: the test page proves the serial path but not the job path, and
  "real" reads as a print someone sent.)*

It checks at the end of each such job and when the gate keeps the image. Nothing is written if
the record already names the running slot, so it is one small write per update. The write
happens before the job lets go of the job lock, so it never races an update's target choice.

## Where updates go (`components/fw_update`)

Both `/update` and the GitHub path ask ota_health for the target instead of
`esp_ota_get_next_update_partition`: the first slot after the running one, counting around,
that is neither the running slot nor the known-good slot. With three slots there is always one
(two when they are the same). `esp_https_ota` takes it as `partition.staging`. Rollback is
unchanged: a failed trial returns to the image that was running before the update, which ESP-IDF
reads from otadata's other entry.

On a board still on the W-1046 table (no `known_good` partition) the firmware falls back to
`esp_ota_get_next_update_partition`, never promotes, and logs that it needs one USB flash.

## The power-up reset (bootloader hook, `bootloader_components/airwriter_boot`)

ESP-IDF 5.5.1's bootloader offers `bootloader_before_init` and `bootloader_after_init` hooks;
partition selection (`select_partition_number`) runs after both. Its own
`CONFIG_BOOTLOADER_FACTORY_RESET` erases otadata and boots the factory slot, or ota_0 when there
is none, so it is turned off and the hook does the reset itself.

In `bootloader_after_init`, after the existing pin clamps:
1. `bootloader_common_check_long_hold_gpio_level(14, 5, low)`. Not held: return at once (the
   normal boot is untouched).
2. Read the partition table (own scan, silent) for the OTA slots, otadata, nvs and
   `known_good`. Read the record: known-good slot K.
3. Try slots in order — K, then the slot otadata currently boots, then the rest ascending — and
   take the first that passes `esp_image_verify` (the same image check the bootloader makes
   before it boots one).
4. Write an otadata entry that boots it: state `VALID`, `seq` the smallest past the active
   entry's with `(seq - 1) % 3 == slot`, into the sector *not* holding the active entry
   (exactly as `esp_ota_set_boot_partition` does). A cut here leaves the old entry in charge.
5. Erase nvs (`bootloader_common_erase_part_type_data("nvs", false)`).
6. Set the RTC retain flag the app already reads (`bootloader_common_set_rtc_retain_mem_
   factory_reset_state`); the panel's reset confirmation follows from it as today.
   `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y` keeps that RTC area reserved without the stock reset.

Then the stock selection boots the slot just written. If no slot verifies, the hook changes
nothing but nvs and the stock code tries every slot and resets, as it would have anyway; a USB
flash is the way back. The record is never written by the bootloader: a fallback image becomes
known-good the normal way, after its first real print.

The pin and hold time are the S3 board's, hard-coded in the hook (the bootloader build does not
see main's Kconfig); main.c fails the build if `CONFIG_AIRWRITER_TEST_BTN_PIN` is not 14 or if
`CONFIG_BOOTLOADER_FACTORY_RESET` is on.

## Pure core and tests

`components/ota_health/known_good_core.{c,h}`, no ESP-IDF: the record (CRC, make, valid,
current copy, slot, next write), the update target, the reset order, the otadata `seq` for a
slot, and the promote decision. The bootloader component compiles the same file by relative
path. `tools/rip/test/test_known_good.c` covers each, including a cut write (one copy garbage)
and every running/known-good pair for the target.

## Status

`/api/status` keeps `slot` (now `ota_0`..`ota_2`) and adds `known_good` (slot) and
`known_good_version`. No web page change.

## Moving board 1

One USB flash (`idf.py flash`): new bootloader, new table, blank otadata, blank record, the app
into ota_0 (where its factory slot was). Its old ota_0 and ota_1 contents now sit in ota_1 and
ota_2 and are harmless: the record says ota_0, and updates overwrite them.

## Bench (board 1, through the main session)

1. USB flash; `/api/status`: `slot` ota_0, `known_good` ota_0.
2. OTA build B: lands in ota_1, kept; print one job; `known_good` ota_1.
3. OTA build C: lands in ota_2; kept, no print. OTA build D from C: lands in ota_0, not ota_1
   (the known-good). Power-up reset → boots ota_1 (B), settings erased, panel shows the reset.
4. Power cut mid-update (during `/update`'s upload): boots the image that was running; the next
   update goes to the same slot.
5. Corrupt the known-good slot over USB (one sector in its image): power-up reset falls back to
   the slot otadata was booting.
6. Blank record (erase `known_good` over USB): power-up reset boots ota_0.

## Left to the builder

Log wording, field names inside the code, the order of checks inside the hook beyond the steps
above, and the trap and wiki wording.
