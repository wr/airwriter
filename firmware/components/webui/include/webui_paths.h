// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings page's own addresses, so that a reload of /about or /printers/2 finds the page.
#pragma once

#include <stddef.h>
#include <string.h>

static inline int webui_is_page(const char *uri)
{
    static const char *const names[] = {
        "/printers", "/airprint", "/appletalk", "/wifi", "/about", "/about/log", "/setup",
    };
    size_t len = strcspn(uri, "?");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i]) == len && !memcmp(names[i], uri, len)) return 1;

    const size_t head = sizeof "/printers/" - 1;    // a printer's details: /printers/2
    if (len <= head || memcmp(uri, "/printers/", head)) return 0;
    for (size_t i = head; i < len; i++)
        if (uri[i] < '0' || uri[i] > '9') return 0;
    return 1;
}
