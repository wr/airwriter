// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which printer a request is for (W-1082): the LaserWriter answers at its own
// resource, the ImageWriter everywhere else, as it always has.
#include "test_util.h"
#include "ipp_req.h"

int main(void)
{
    const char *both[IPP_PRINTERS] = { "ipp/print", "ipp/laser" };
    const char *iw_only[IPP_PRINTERS] = { "ipp/print", NULL };

    CHECK_EQ_INT(ipp_route("/ipp/print", both), 0);
    CHECK_EQ_INT(ipp_route("/", both), 0);
    CHECK_EQ_INT(ipp_route("/anything", both), 0);
    CHECK_EQ_INT(ipp_route("/ipp/printer", both), 0);
    CHECK_EQ_INT(ipp_route("/ipp/laser", both), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser/", both), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser/7", both), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser?x", both), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laserwriter", both), 0);
    CHECK_EQ_INT(ipp_route("http://h.local:631/ipp/laser", both), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser", iw_only), -1);     // not shared: not found
    CHECK_EQ_INT(ipp_route("/ipp/print", iw_only), 0);

    TEST_END;
}
