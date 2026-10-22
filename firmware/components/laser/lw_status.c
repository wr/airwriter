// SPDX-License-Identifier: GPL-3.0-or-later
#include "lw_status.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

// Where `word` first starts in s, ignoring case, or NULL.
static const char *find_ci(const char *s, const char *word)
{
    size_t k = strlen(word);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < k && p[i] && tolower((unsigned char)p[i]) == word[i]) i++;
        if (i == k) return p;
    }
    return NULL;
}

// The printer's own state out of a status string, its fields joined by
// spaces: "status: ..." and any PrinterError, never the job:, user: or
// source: fields, which carry the job's own name ("job: Pajamas"). Review I1.
static void state_text(const char *s, char *out, size_t cap, int *has_job)
{
    static const char *const names[] = { "job:", "user:", "source:", "title:" };
    size_t k = 0;
    *has_job = 0;
    while (*s && k + 1 < cap) {
        const char *end = strchr(s, ';');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        const char *f = s;
        while (f < s + n && (*f == ' ' || *f == '%' || *f == '[')) f++;
        int skip = 0;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            size_t m = strlen(names[i]);
            if ((size_t)(s + n - f) >= m && !strncasecmp(f, names[i], m)) {
                skip = 1;
                if (i == 0) *has_job = 1;
            }
        }
        if (!skip) {
            size_t m = (size_t)(s + n - f);
            if (k + m + 2 > cap) m = cap - k - 2;
            memcpy(out + k, f, m);
            k += m;
            out[k++] = ' ';
        }
        s = end ? end + 1 : s + n;
    }
    out[k] = '\0';
}

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
    char text[256];
    int job;
    state_text(status, text, sizeof text, &job);
    status = text;
    if (has(status, jam))          { out->reason = "media-jam-error";   out->stopped = 1; }
    else if (has(status, no_ink))  { out->reason = "toner-empty-error"; out->stopped = 1; }
    else if (has(status, low_ink))   out->reason = "toner-low-warning";
    else if (has(status, paper))   { out->reason = "media-empty-error"; out->stopped = 1; }
    else if (has(status, door))    { out->reason = "door-open-error";   out->stopped = 1; }
    else if (has(status, fault))   { out->reason = "other-error";       out->stopped = 1; }
    else if (has(status, working) || (job && !find_ci(status, "idle"))) out->busy = 1;
}

int lw_ps_error(const char *line, char *msg, size_t cap)
{
    if (!line || !cap || !find_ci(line, "%%[")) return 0;
    const char *p = find_ci(line, "error:");
    if (!p) return 0;
    p += strlen("error:");
    while (*p == ' ') p++;
    size_t n = 0;
    while (p[n] && strncmp(p + n, " ]%%", 4) && strncmp(p + n, "]%%", 3)) n++;
    if (n >= cap) n = cap - 1;
    memcpy(msg, p, n);
    msg[n] = '\0';
    return 1;
}
