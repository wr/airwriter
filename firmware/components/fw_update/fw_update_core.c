// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware-update core (fw_update_core.h).
#include "fw_update_core.h"

#include <stdio.h>
#include <string.h>

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_alnum(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 1-4 digits at *p; advances past them.
static int part(const char **p, int *out)
{
    int n = 0, v = 0;
    while (is_digit(**p)) {
        if (++n > 4) return 0;
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    *out = v;
    return n > 0;
}

// Parses vX.Y.Z at the start of `s`; returns where it ended, or NULL.
static const char *triple(const char *s, fwu_ver_t *v)
{
    const char *p = s;
    if (*p++ != 'v') return NULL;
    if (!part(&p, &v->maj) || *p++ != '.') return NULL;
    if (!part(&p, &v->min) || *p++ != '.') return NULL;
    if (!part(&p, &v->pat)) return NULL;
    return p;
}

int fwu_parse_version(const char *s, fwu_ver_t *out)
{
    fwu_ver_t v;
    const char *end = s ? triple(s, &v) : NULL;
    if (!end || (*end && *end != '-')) {
        out->maj = out->min = out->pat = 0;
        return 0;
    }
    *out = v;
    return 1;
}

int fwu_is_release(const char *s)
{
    fwu_ver_t v;
    const char *end = s ? triple(s, &v) : NULL;
    return end && !*end;
}

int fwu_valid_tag(const char *s)
{
    fwu_ver_t v;
    const char *end = s ? triple(s, &v) : NULL;
    if (!end || strlen(s) >= 32) return 0;
    if (!*end) return 1;
    if (*end++ != '-' || !*end) return 0;
    for (; *end; end++)
        if (!is_alnum(*end) && *end != '.') return 0;
    return 1;
}

int fwu_valid_file(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 64 || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++)
        if (!is_alnum(s[i]) && s[i] != '.' && s[i] != '_' && s[i] != '-') return 0;
    return 1;
}

static int cmp(const fwu_ver_t *a, const fwu_ver_t *b)
{
    if (a->maj != b->maj) return a->maj < b->maj ? -1 : 1;
    if (a->min != b->min) return a->min < b->min ? -1 : 1;
    if (a->pat != b->pat) return a->pat < b->pat ? -1 : 1;
    return 0;
}

int fwu_newer(const char *latest, const char *running)
{
    fwu_ver_t l, r;
    if (!fwu_is_release(latest)) return 0;
    fwu_parse_version(latest, &l);
    fwu_parse_version(running, &r);     // a bare hash reads 0.0.0
    return cmp(&l, &r) > 0;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// The string value of "key" in a flat object of our own making: 1 copied,
// 0 no such key, -1 malformed, escaped or too long for `out`.
static int get_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[40];
    if ((size_t)snprintf(pat, sizeof pat, "\"%s\"", key) >= sizeof pat) return -1;
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = skip_ws(p + strlen(pat));
    if (*p++ != ':') return -1;
    p = skip_ws(p);
    if (*p++ != '"') return -1;
    size_t n = 0;
    for (; *p != '"'; p++) {
        if (!*p || *p == '\\' || n + 1 >= cap) return -1;
        out[n++] = *p;
    }
    out[n] = '\0';
    return 1;
}

fwu_manifest_t fwu_parse_manifest(const char *json, const char *target,
                                  char *ver, size_t vcap, char *file, size_t fcap)
{
    if (!json || get_str(json, "version", ver, vcap) != 1 || !fwu_is_release(ver))
        return FWU_M_BAD;
    int got = get_str(json, target, file, fcap);
    if (got == 0) return FWU_M_NO_TARGET;
    if (got < 0 || !fwu_valid_file(file)) return FWU_M_BAD;
    return FWU_M_OK;
}

// owner/name, each [A-Za-z0-9._-]: a fork may name its own.
static int valid_repo(const char *s)
{
    const char *slash = s ? strchr(s, '/') : NULL;
    if (!slash || slash == s || !slash[1] || strchr(slash + 1, '/')) return 0;
    for (const char *p = s; *p; p++)
        if (p != slash && !is_alnum(*p) && *p != '.' && *p != '_' && *p != '-') return 0;
    return 1;
}

int fwu_manifest_url(char *out, size_t cap, const char *repo)
{
    if (!valid_repo(repo)) return 0;
    int n = snprintf(out, cap, "https://github.com/%s/releases/latest/download/airwriter.json",
                     repo);
    return n > 0 && (size_t)n < cap;
}

int fwu_asset_url(char *out, size_t cap, const char *repo, const char *tag, const char *file)
{
    if (!valid_repo(repo) || !fwu_valid_tag(tag) || !fwu_valid_file(file)) return 0;
    int n = snprintf(out, cap, "https://github.com/%s/releases/download/%s/%s",
                     repo, tag, file);
    return n > 0 && (size_t)n < cap;
}

fwu_action_t fwu_policy(const fwu_inputs_t *in, const fwu_timing_t *tm)
{
    if (!in->auto_on || in->setup_mode || !in->key_ok || in->joined_s < 0) return FWU_WAIT;

    int64_t due = in->last_check_s < 0
        ? in->joined_s + tm->first_s
        : in->last_check_s + (in->check_failed ? tm->retry_s : tm->interval_s);
    if (in->now_s >= due) return FWU_CHECK;

    if (!in->known_newer || !in->running_release || in->newer_rolled_back ||
        in->install_tried || in->on_trial || in->busy)
        return FWU_WAIT;
    // Uptime starts at boot, so a board that has printed nothing since has
    // been idle all along.
    int64_t idle = in->now_s - (in->last_job_s < 0 ? 0 : in->last_job_s);
    return idle >= tm->idle_s ? FWU_INSTALL : FWU_WAIT;
}
