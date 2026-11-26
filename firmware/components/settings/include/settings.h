// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything a user can change, stored in NVS. The Kconfig values are only
// build-time defaults: whatever is in NVS wins, so a device can be
// reconfigured from the web UI without a rebuild.
#pragma once
#include <stdint.h>
#include "esp_err.h"

#define SET_SSID_MAX  33
#define SET_PASS_MAX  65
#define SET_HOST_MAX  40
#define SET_QUEUE_MAX 32
#define SET_DITHER_MAX 12

typedef struct {
    char ssid[SET_SSID_MAX];
    char pass[SET_PASS_MAX];
    char hostname[SET_HOST_MAX];     // without .local
    char queue[SET_QUEUE_MAX];       // the printer's name everywhere: name, else model
    char model[SET_QUEUE_MAX];       // the printer model picked on the page (W-1079)
    char name[SET_QUEUE_MAX];        // a name of the user's own, or "" for the model's
    char dither[SET_DITHER_MAX];     // atkinson | floyd | bayer | bluenoise | threshold
    uint8_t gain;                    // dot gain at a 50% tint the screens make up for,
                                     // percent (W-1068); CONFIG_AIRWRITER_DOT_GAIN
    uint8_t auto_update;             // install new releases by itself (W-1048); off
    uint8_t ribbon;                  // the last ribbon self-ID reported (W-1064):
                                     // 0 unknown, 1 black, 2 colour (iw_ribbon_t)
    uint8_t airprint;                // advertise and take AirPrint jobs (W-1079); on
    int8_t  lt_port;                 // the AppleTalk port (W-1079): 1 on, 0 off; on
    uint8_t rainbow;                 // Rainbow status lights (W-1109): the page's switch; off
    uint8_t theme;                   // the page's colours, About's Appearance: SET_THEME_*; Platinum
    // W-1179, rev 2026.11: what each socket (0 J2, port 1; 1 J4, port 2) is set to do,
    // SET_PORT_*, and the job it last did (ports.h's port_job_t; 0 none yet), so a restart
    // keeps what was worked out. No control on the page: /api/general's port1 and port2.
    uint8_t port_set[2];
    uint8_t port_job[2];
    // ... and whether LocalTalk had been heard on it in that job: AppleTalk goes out on a
    // socket only once it has (W-1179), and a restart keeps that.
    uint8_t port_heard[2];
    int16_t top_margin;              // 1/144 in: + blank paper ahead of each AirPrint
                                     // page and the test page, - drops their first
                                     // rows; CONFIG_AIRWRITER_TOP_MARGIN
} settings_t;

enum { SET_PORT_AUTO = 0, SET_PORT_PRINTER = 1, SET_PORT_LOCALTALK = 2 };

#define SET_TOP_MIN (-216)           // -1 1/2 in
#define SET_TOP_MAX 144              // +1 in

// Fills every field: NVS first, then the Kconfig default, then a built-in.
// `mac_suffix` is the four hex digits used for the default hostname.
esp_err_t settings_load(settings_t *out, const char *mac_suffix);

// W-1088: stores what a board would otherwise only default to, so a later
// default cannot move it. With no host name stored, host_fmt's (with the MAC
// suffix): a board set up before keeps "imagewriter-%s", a new one gets
// "airwriter-%s". With no AppleTalk port setting, on. Anything stored is kept.
esp_err_t settings_first_boot(const char *host_fmt, const char *mac_suffix);

esp_err_t settings_save_wifi(const char *ssid, const char *pass);
// Whether Wi-Fi was saved before this restart, once: the panel greets the
// first join with green as confirmation (W-1049). Kept in RTC memory, so it
// survives the restart that follows the save, not a power cut.
int       settings_take_wifi_fresh(void);
// Empty strings and a gain below 0 keep what is stored.
esp_err_t settings_save_general(const char *hostname, const char *queue,
                                const char *dither, int gain);
esp_err_t settings_save_auto_update(int on);
esp_err_t settings_save_rainbow(int on);
// The settings page's colour theme: 0 is Platinum, the page's own colours; all are named as the
// page's stylesheet names them (data-theme). The board only keeps the choice, so that every
// browser that opens the page sees the same one.
#define SET_THEME_COUNT 4
const char *settings_theme_name(int theme);      // "platinum" when out of range
int       settings_theme_from_name(const char *name);  // -1 for no such theme
esp_err_t settings_save_theme(int theme);
// The printer's model and the user's own name for it (W-1079). NULL keeps
// each; an empty name goes back to the model's. Also stores the name in
// effect as "queue", which firmware from before W-1079 reads.
esp_err_t settings_save_printer(const char *model, const char *name);
// AirPrint and the AppleTalk port, on (1) or off (0); -1 keeps each.
esp_err_t settings_save_switches(int airprint, int lt_port);
// Kept so a restart advertises what the printer last said, not black until it
// answers again: what clients see must not flip back and forth (W-1064).
esp_err_t settings_save_ribbon(uint8_t ribbon);
// W-1179: socket i's setting (SET_PORT_*) and the job it last did (port_job_t, 0 to 2).
esp_err_t settings_save_port_set(int i, int set);
esp_err_t settings_save_port_job(int i, int job);
esp_err_t settings_save_port_heard(int i, int heard);
// SET_TOP_MIN to SET_TOP_MAX, 1/144 in.
esp_err_t settings_save_top_margin(int top);

// W-1088: a fresh setup's finish sheet ("AirWriter is ready"): on once the
// setup page saves Wi-Fi, off for good when the person closes it.
int       settings_fresh(void);
esp_err_t settings_set_fresh(int on);

// Wipes the stored settings. The device falls back to its build defaults on
// the next boot, which is what the config button holds for.
esp_err_t settings_erase(void);
