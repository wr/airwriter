// SPDX-License-Identifier: GPL-3.0-or-later
// Which addresses are the settings page's own, the ones a reload has to find.
#include "test_util.h"
#include "webui_paths.h"

int main(void)
{
    static const char *const pages[] = {
        "/printers", "/airprint", "/appletalk", "/wifi", "/about", "/about/log", "/setup",
        "/printers/0", "/printers/12", "/about?x=1", "/printers/2?x=1",
    };
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++)
        CHECK_EQ_INT(webui_is_page(pages[i]), 1);

    static const char *const others[] = {
        "", "/", "/about/", "/aboutx", "/about/logs", "/about/log/", "/printers/", "/printers/a",
        "/printers/1a", "/printers/1/2", "/printersx", "/update", "/api/status", "/favicon.ico",
        "/wifi/1", "/Setup",
    };
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
        CHECK_EQ_INT(webui_is_page(others[i]), 0);

    TEST_END;
}
