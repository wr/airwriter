// SPDX-License-Identifier: GPL-3.0-or-later
#include "lw_status.h"
#include <ctype.h>
#include <string.h>

// Whether s contains any of the words, ignoring case.
static int has(const char *s, const char *const *words)
{
    for (; *words; words++) {
        size_t k = strlen(*words);
        for (const char *p = s; *p; p++) {
            size_t i = 0;
            while (i < k && p[i] && tolower((unsigned char)p[i]) == (*words)[i]) i++;
            if (i == k) return 1;
        }
    }
    return 0;
}

void lw_status_map(const char *status, lw_state_t *out)
{
    static const char *const jam[]     = { "jam", NULL };
    static const char *const no_ink[]  = { "no toner", "out of toner", "toner empty",
                                           "toner out", NULL };
    static const char *const low_ink[] = { "toner low", "low toner", NULL };
    static const char *const paper[]   = { "out of paper", "paper out", "no paper", "tray empty",
                                           "paper empty", "load paper", "add paper", NULL };
    static const char *const door[]    = { "cover open", "door open", "open cover",
                                           "open door", NULL };
    static const char *const fault[]   = { "printererror", NULL };
    static const char *const working[] = { "busy", "printing", "warming up", "job:", NULL };

    out->reason = "none";
    out->stopped = 0;
    out->busy = 0;
    if (!status) return;
    if (has(status, jam))          { out->reason = "media-jam-error";   out->stopped = 1; }
    else if (has(status, no_ink))  { out->reason = "toner-empty-error"; out->stopped = 1; }
    else if (has(status, low_ink))   out->reason = "toner-low-warning";
    else if (has(status, paper))   { out->reason = "media-empty-error"; out->stopped = 1; }
    else if (has(status, door))    { out->reason = "door-open-error";   out->stopped = 1; }
    else if (has(status, fault))   { out->reason = "other-error";       out->stopped = 1; }
    else if (has(status, working))   out->busy = 1;
}

int lw_ps_error(const char *line, char *msg, size_t cap)
{
    static const char *const err[] = { "%%[ error:", NULL };
    if (!line || !has(line, err) || !cap) return 0;
    const char *p = line;
    while (*p && strncmp(p, "rror:", 5)) p++;
    p += 5;
    while (*p == ' ') p++;
    size_t n = 0;
    while (p[n] && strncmp(p + n, " ]%%", 4) && strncmp(p + n, "]%%", 3)) n++;
    if (n >= cap) n = cap - 1;
    memcpy(msg, p, n);
    msg[n] = '\0';
    return 1;
}
