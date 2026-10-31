// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a StyleWriter's state shows (W-1103): the IPP reason and message for each sw_state_t,
// and the classes the board's panel and page draw from (ready, a fault, neither). The words
// are the phone's, so the two the design fixes are checked letter for letter.
#include "test_util.h"
#include "sw_status.h"

static int ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && !strcmp(s + n - t, tail);
}

int main(void)
{
    // The design's two sentences.
    CHECK(!strcmp(sw_state_ipp(SWS_NO_PAPER).message, "Needs attention: check paper"));
    CHECK(!strcmp(sw_state_ipp(SWS_NO_PAPER).reason, "media-empty-error"));
    CHECK(!strcmp(sw_state_ipp(SWS_OFFLINE).message, "Offline"));
    CHECK(!strcmp(sw_state_ipp(SWS_OFFLINE).reason, "other"));

    // Each fault the session names has its own IPP reason.
    CHECK(!strcmp(sw_state_ipp(SWS_JAM).reason, "media-jam-error"));
    CHECK(!strcmp(sw_state_ipp(SWS_CART_MISSING).reason, "marker-supply-missing-error"));
    CHECK(!strcmp(sw_state_ipp(SWS_INK_OUT).reason, "marker-supply-empty-error"));
    CHECK(!strcmp(sw_state_ipp(SWS_DOOR).reason, "door-open-error"));
    CHECK(!strcmp(sw_state_ipp(SWS_WASTE).reason, "marker-waste-full-error"));

    // A printer that can print says nothing.
    CHECK(sw_state_ipp(SWS_READY).reason == NULL && sw_state_ipp(SWS_READY).message == NULL);
    CHECK(sw_state_ipp(SWS_PRINTING).reason == NULL && sw_state_ipp(SWS_PRINTING).message == NULL);

    // Every state: a reason and a message together or not at all; a stopped printer's reason
    // reads as an error ("-error", or "other", as the offline state has always used); its
    // message starts as the two fixed ones do; a fault is never ready; the classes are
    // disjoint and cover every state.
    for (int s = SWS_READY; s <= SWS_FAULT; s++) {
        sw_state_t st = (sw_state_t)s;
        sw_ipp_t p = sw_state_ipp(st);
        CHECK((p.reason == NULL) == (p.message == NULL));
        int ready = sw_state_ready(st), fault = sw_state_fault(st);
        CHECK(!(ready && fault));
        CHECK_EQ_INT(ready || fault || st == SWS_OFFLINE, 1);
        CHECK_EQ_INT(p.reason == NULL, ready);
        if (p.reason) {
            CHECK(ends_with(p.reason, "-error") || !strcmp(p.reason, "other"));
            CHECK(!strncmp(p.message, "Needs attention: ", 17) || !strcmp(p.message, "Offline"));
            CHECK(fault == !strncmp(p.message, "Needs attention: ", 17));
        }
    }
    CHECK(sw_state_ready(SWS_READY) && sw_state_ready(SWS_PRINTING));
    CHECK(!sw_state_ready(SWS_OFFLINE) && !sw_state_fault(SWS_OFFLINE));
    CHECK(sw_state_fault(SWS_FAULT) && sw_state_fault(SWS_CART_JAM));

    // The reasons are literals the server compares by address: the same state gives the same one.
    CHECK(sw_state_ipp(SWS_JAM).reason == sw_state_ipp(SWS_JAM).reason);

    // Silence while idle. A StyleWriter that stops answering is offline, but the original
    // ignores the plain question until it is reset: switched off between uses, as these are,
    // it would read as offline after every power-on until a job, so its silence while idle
    // says nothing and it reads as ready. Only a job's full sequence that gets nothing, or a
    // job it went silent during, shows it offline, until it answers or the next job.
    CHECK_EQ_INT(sw_idle_silent(SW_ORIGINAL, 0), SWS_READY);
    CHECK_EQ_INT(sw_idle_silent(SW_ORIGINAL, 1), SWS_OFFLINE);
    for (int m = SW_II; m < SW_MODELS; m++) {
        CHECK_EQ_INT(sw_idle_silent((sw_model_t)m, 0), SWS_OFFLINE);
        CHECK_EQ_INT(sw_idle_silent((sw_model_t)m, 1), SWS_OFFLINE);
    }
    CHECK_EQ_INT(sw_idle_silent(SW_NONE, 0), SWS_OFFLINE);
    // Ready claims nothing to the phone, so a job is taken and its full sequence finds out.
    CHECK(sw_state_ipp(sw_idle_silent(SW_ORIGINAL, 0)).reason == NULL);

    TEST_END;
}
