#pragma once

typedef enum {                 // priority order, highest wins
    PS_BOOT = 0, PS_SETUP, PS_WIFI_CONNECTING,
    PS_BLOCKED, PS_PRINTING, PS_ERROR, PS_READY,
} ps_state_t;

typedef enum {
    PS_EV_WIFI_CONNECTING, PS_EV_WIFI_UP, PS_EV_SETUP_MODE,
    PS_EV_JOB_START, PS_EV_JOB_DONE, PS_EV_JOB_FAIL,
    PS_EV_PRINTER_BLOCKED, PS_EV_PRINTER_CLEARED,
} ps_event_t;

typedef struct { unsigned wifi_up:1, connecting:1, setup:1,
                 printing:1, blocked:1, error:1; } ps_t;

void       ps_init(ps_t *ps);
ps_state_t ps_post(ps_t *ps, ps_event_t ev);   // returns derived state
ps_state_t ps_state(const ps_t *ps);
const char *ps_name(ps_state_t st);            // "ready", "printing", ...
