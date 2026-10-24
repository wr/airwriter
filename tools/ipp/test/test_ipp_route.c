// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which printer a request is for (W-1082): the LaserWriter answers at its own
// resource, the ImageWriter everywhere else, as it always has.
#include "test_util.h"
#include "ipp_req.h"

static const char *const NONE[IPP_PRINTERS] = { NULL };

// W-1088: a slot per shared printer, the port printer's at every path it has
// always answered at, the others at their own; a path of the ipp/p<n> or
// ipp/laser shape that no printer holds is not found, never the port's.
static void test_route_slots(void)
{
    const char *res[IPP_PRINTERS] = { "ipp/print", "ipp/p3", NULL, "ipp/p5" };
    const char *legacy[IPP_PRINTERS] = { NULL, "ipp/laser", NULL, NULL };
    CHECK_EQ_INT(ipp_route("/ipp/p3", res, legacy), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser", res, legacy), 1);
    CHECK_EQ_INT(ipp_route("/ipp/p5", res, legacy), 3);
    CHECK_EQ_INT(ipp_route("/ipp/p5/2", res, legacy), 3);
    CHECK_EQ_INT(ipp_route("/ipp/p4", res, legacy), -1);
    CHECK_EQ_INT(ipp_route("/ipp/print", res, legacy), 0);
    CHECK_EQ_INT(ipp_route("/ipp/printer", res, legacy), 0);
    CHECK_EQ_INT(ipp_route("/", res, legacy), 0);
    const char *no_port[IPP_PRINTERS] = { NULL, "ipp/p3", NULL, NULL };
    CHECK_EQ_INT(ipp_route("/", no_port, NONE), -1);
    CHECK_EQ_INT(ipp_route("/ipp/print", no_port, NONE), -1);
    CHECK_EQ_INT(ipp_route("/ipp/p3", no_port, NONE), 1);
}

int main(void)
{
    const char *both[IPP_PRINTERS] = { "ipp/print", "ipp/laser" };
    const char *iw_only[IPP_PRINTERS] = { "ipp/print", NULL };

    test_route_slots();

    CHECK_EQ_INT(ipp_route("/ipp/print", both, NONE), 0);
    CHECK_EQ_INT(ipp_route("/", both, NONE), 0);
    CHECK_EQ_INT(ipp_route("/anything", both, NONE), 0);
    CHECK_EQ_INT(ipp_route("/ipp/printer", both, NONE), 0);
    CHECK_EQ_INT(ipp_route("/ipp/laser", both, NONE), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser/", both, NONE), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser/7", both, NONE), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser?x", both, NONE), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laserwriter", both, NONE), 0);
    CHECK_EQ_INT(ipp_route("http://h.local:631/ipp/laser", both, NONE), 1);
    CHECK_EQ_INT(ipp_route("/ipp/laser", iw_only, NONE), -1);     // not shared: not found
    CHECK_EQ_INT(ipp_route("/ipp/print", iw_only, NONE), 0);

    TEST_END;
}
