// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"

int main(void)
{
    const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    memsrc_t src = { data, sizeof data, 0 };
    uint8_t got[2];

    CHECK_EQ_INT(memsrc_read(&src, got, 2), 1);
    CHECK_EQ_INT(got[0], 0xDE);
    CHECK_EQ_INT(src.pos, 2);
    CHECK_EQ_INT(memsrc_read(&src, got, 99), 0);   /* short read fails */
    TEST_END;
}
