// Host test for the printer_state reducer. Mirrors test_serial_writer.c style.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "printer_state_core.h"

int main(void)
{
    ps_t ps;
    ps_init(&ps);
    assert(ps_state(&ps) == PS_BOOT);

    assert(ps_post(&ps, PS_EV_WIFI_CONNECTING) == PS_WIFI_CONNECTING);
    assert(ps_post(&ps, PS_EV_WIFI_UP) == PS_READY);

    // A job runs and completes.
    assert(ps_post(&ps, PS_EV_JOB_START) == PS_PRINTING);
    assert(ps_post(&ps, PS_EV_JOB_DONE) == PS_READY);

    // Paper out mid-job outranks printing; clearing it resumes printing.
    ps_post(&ps, PS_EV_JOB_START);
    assert(ps_post(&ps, PS_EV_PRINTER_BLOCKED) == PS_BLOCKED);
    assert(ps_post(&ps, PS_EV_PRINTER_CLEARED) == PS_PRINTING);

    // A failed job shows error until the next job starts.
    assert(ps_post(&ps, PS_EV_JOB_FAIL) == PS_ERROR);
    assert(ps_post(&ps, PS_EV_JOB_START) == PS_PRINTING);
    ps_post(&ps, PS_EV_JOB_DONE);

    // Blocked while idle (printer unplugged) still reads blocked, not ready.
    assert(ps_post(&ps, PS_EV_PRINTER_BLOCKED) == PS_BLOCKED);
    ps_post(&ps, PS_EV_PRINTER_CLEARED);

    // Setup mode outranks everything.
    ps_init(&ps);
    assert(ps_post(&ps, PS_EV_SETUP_MODE) == PS_SETUP);
    ps_post(&ps, PS_EV_PRINTER_BLOCKED);
    assert(ps_state(&ps) == PS_SETUP);

    assert(strcmp(ps_name(PS_READY), "ready") == 0);
    assert(strcmp(ps_name(PS_BLOCKED), "blocked") == 0);

    printf("test_printer_state: ok\n");
    return 0;
}
