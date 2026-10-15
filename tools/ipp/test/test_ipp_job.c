// SPDX-License-Identifier: GPL-3.0-or-later
//
// The job slot. What matters here is what a client polling the job sees while
// the printer is stalled, and that a finished job stays finished.
#include "test_util.h"
#include "ipp_job.h"
#include "ipp_proto.h"

#define URI "ipp://aw.local:631/ipp/print"

int main(void)
{
    ipp_job_rec_t j = {0};
    uint8_t st;
    const char *why;

    /* Before any job, the empty slot is not a job: nothing matches it, not
     * even "the current job", and there is nothing to cancel. */
    CHECK(!ipp_job_matches(&j, -1));
    CHECK(!ipp_job_matches(&j, 0));
    CHECK_EQ_INT(ipp_job_cancel(&j, -1), IPP_CANCEL_NOT_FOUND);

    /* Print-Job: printing at once, with a job-uri a client can poll. */
    ipp_job_start(&j, 5, 100, "Safari", "wells", URI);
    CHECK_EQ_INT(j.id, 5);
    CHECK(strcmp(j.uri, URI "/5") == 0);
    CHECK(strcmp(j.name, "Safari") == 0);
    CHECK(ipp_job_active(&j));
    ipp_job_view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PROCESSING);
    CHECK(strcmp(why, "job-printing") == 0);

    /* The point of W-1040: out of paper mid-job reads as a paused job, not a
     * failing one, for as long as the printer is stopped. */
    ipp_job_view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_STOPPED);
    CHECK(strcmp(why, "printer-stopped") == 0);
    ipp_job_view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PROCESSING);

    ipp_job_finish(&j, 1);
    CHECK(!ipp_job_active(&j));
    ipp_job_view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_COMPLETED);          /* done is done, stopped or not */
    CHECK(strcmp(why, "job-completed-successfully") == 0);

    /* Cancel-Job on a finished job: not possible (RFC 8011 4.3.3). */
    CHECK_EQ_INT(ipp_job_cancel(&j, 5), IPP_CANCEL_NOT_POSSIBLE);
    CHECK_EQ_INT(ipp_job_cancel(&j, 9), IPP_CANCEL_NOT_FOUND);
    CHECK_EQ_INT(j.state, IPP_JSTATE_COMPLETED);

    /* A failed transfer ends aborted. */
    ipp_job_start(&j, 6, 200, "", "", URI);
    ipp_job_finish(&j, 0);
    ipp_job_view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_ABORTED);
    CHECK(strcmp(why, "aborted-by-system") == 0);

    /* Cancel while printing: the paper has to stop, and the job ends
     * canceled even though the stream then fails as it unwinds. */
    ipp_job_start(&j, 7, 300, "", "", URI);
    CHECK_EQ_INT(ipp_job_cancel(&j, -1), IPP_CANCEL_STOP_PRINT);   /* -1: current job */
    ipp_job_view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_CANCELED);
    CHECK(strcmp(why, "job-canceled-by-user") == 0);
    ipp_job_finish(&j, 0);
    CHECK_EQ_INT(j.state, IPP_JSTATE_CANCELED);

    /* W-1042: the reply goes back when the upload ends, and the job stays
     * processing -- holding the printer -- until the sink reports it printed.
     * That report ends it once; a second one frees nothing. A separate record,
     * so the slot above keeps job 7 for the matching checks below. */
    ipp_job_rec_t k = {0};
    CHECK_EQ_INT(ipp_job_done(&k, 1), 0);             /* empty slot: nothing printing */
    ipp_job_start(&k, 12, 600, "", "", URI);
    CHECK_EQ_INT(k.printing, 1);
    ipp_job_view(&k, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_STOPPED);             /* paused after the reply */
    CHECK_EQ_INT(ipp_job_done(&k, 1), 1);
    CHECK_EQ_INT(k.state, IPP_JSTATE_COMPLETED);
    CHECK_EQ_INT(ipp_job_done(&k, 0), 0);
    CHECK_EQ_INT(k.state, IPP_JSTATE_COMPLETED);

    /* A print that fails after the reply ends aborted. */
    ipp_job_start(&k, 13, 700, "", "", URI);
    CHECK_EQ_INT(ipp_job_done(&k, 0), 1);
    CHECK_EQ_INT(k.state, IPP_JSTATE_ABORTED);

    /* Cancel-Job while the buffered job prints: canceled at once, but the
     * printer stays held until the paper stops, and the done that follows
     * keeps it canceled. */
    ipp_job_start(&k, 14, 800, "", "", URI);
    CHECK_EQ_INT(ipp_job_cancel(&k, 14), IPP_CANCEL_STOP_PRINT);
    CHECK_EQ_INT(k.state, IPP_JSTATE_CANCELED);
    CHECK_EQ_INT(k.printing, 1);
    CHECK_EQ_INT(ipp_job_done(&k, 0), 1);
    CHECK_EQ_INT(k.state, IPP_JSTATE_CANCELED);
    CHECK_EQ_INT(ipp_job_cancel(&k, 14), IPP_CANCEL_NOT_POSSIBLE);

    /* Job matching: -1 is the current job, anything else must be exact. */
    CHECK(ipp_job_matches(&j, -1));
    CHECK(ipp_job_matches(&j, 7));
    CHECK(!ipp_job_matches(&j, 8));

    /* Over-long names are cut to fit, never past the field. */
    char longname[IPP_NAME_MAX * 2];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    ipp_job_start(&j, 11, 0, longname, longname, URI);
    CHECK_EQ_INT(strlen(j.name), IPP_NAME_MAX - 1);
    CHECK_EQ_INT(strlen(j.user), IPP_NAME_MAX - 1);

    TEST_END;
}
