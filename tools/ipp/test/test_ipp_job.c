// SPDX-License-Identifier: GPL-3.0-or-later
//
// The job slot. What matters here is what the phone sees while a job is
// stalled, and that no request sequence can leave the printer held for good.
#include "test_util.h"
#include "ipp_job.h"
#include "ipp_attrs.h"
#include "ipp_proto.h"

#define URI "ipp://aw.local:631/ipp/print"

static void view(const ipp_job_rec_t *j, int stopped, uint8_t *st, const char **why)
{
    ipp_job_view(j, stopped, st, why);
}

int main(void)
{
    ipp_job_rec_t j = {0};
    uint8_t st;
    const char *why;

    /* Create-Job: pending, waiting for its document, with a job-uri the phone
     * can poll from the first response. */
    ipp_job_start(&j, 5, 100, 1, "Safari", "wells", URI);
    CHECK_EQ_INT(j.id, 5);
    CHECK(strcmp(j.uri, URI "/5") == 0);
    CHECK(strcmp(j.name, "Safari") == 0);
    CHECK(ipp_job_active(&j));
    view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PENDING);
    CHECK(strcmp(why, "job-incoming") == 0);

    /* A printer that is already stopped does not make a job with no document
     * look stalled: nothing of it has reached the printer. */
    view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PENDING);

    /* Send-Document for another job is refused and leaves this one alone. */
    CHECK_EQ_INT(ipp_job_claim_doc(&j, 6), IPP_STATUS_ERR_NOT_FOUND);
    CHECK_EQ_INT(j.awaiting_doc, 1);

    /* The right one claims it: printing. */
    CHECK_EQ_INT(ipp_job_claim_doc(&j, 5), IPP_STATUS_OK);
    CHECK_EQ_INT(j.awaiting_doc, 0);
    view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PROCESSING);
    CHECK(strcmp(why, "job-printing") == 0);

    /* The point of W-1040: out of paper mid-job reads as a paused job, not a
     * failing one, for as long as the printer is stopped. */
    view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_STOPPED);
    CHECK(strcmp(why, "printer-stopped") == 0);
    view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PROCESSING);

    /* A second Send-Document has no document left to take. */
    CHECK_EQ_INT(ipp_job_claim_doc(&j, 5), IPP_STATUS_ERR_NOT_POSSIBLE);

    /* A printing job is never reaped, however long the printer stays paused:
     * the stall limit belongs to serial_out, not to this timeout. */
    CHECK_EQ_INT(ipp_job_reap(&j, 100 + 100000), 0);
    CHECK_EQ_INT(j.state, IPP_JSTATE_PROCESSING);

    ipp_job_finish(&j, 1);
    CHECK(!ipp_job_active(&j));
    view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_COMPLETED);          /* done is done, stopped or not */
    CHECK(strcmp(why, "job-completed-successfully") == 0);

    /* Cancel-Job on a finished job: not possible (RFC 8011 4.3.3). */
    CHECK_EQ_INT(ipp_job_cancel(&j, 5), IPP_CANCEL_NOT_POSSIBLE);
    CHECK_EQ_INT(ipp_job_cancel(&j, 9), IPP_CANCEL_NOT_FOUND);
    CHECK_EQ_INT(j.state, IPP_JSTATE_COMPLETED);

    /* A failed transfer ends aborted. */
    ipp_job_start(&j, 6, 200, 0, "", "", URI);
    view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_PROCESSING);          /* Print-Job: printing at once */
    ipp_job_finish(&j, 0);
    view(&j, 0, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_ABORTED);
    CHECK(strcmp(why, "aborted-by-system") == 0);

    /* Cancel while printing: the paper has to stop, and the job ends
     * canceled even though the stream then fails as it unwinds. */
    ipp_job_start(&j, 7, 300, 0, "", "", URI);
    CHECK_EQ_INT(ipp_job_cancel(&j, -1), IPP_CANCEL_STOP_PRINT);   /* -1: current job */
    view(&j, 1, &st, &why);
    CHECK_EQ_INT(st, IPP_JSTATE_CANCELED);
    CHECK(strcmp(why, "job-canceled-by-user") == 0);
    ipp_job_finish(&j, 0);
    CHECK_EQ_INT(j.state, IPP_JSTATE_CANCELED);

    /* Cancel before the document came: nothing to stop, but the printer is
     * held by the job and must be freed. A late Send-Document is refused. */
    ipp_job_start(&j, 8, 400, 1, "", "", URI);
    CHECK_EQ_INT(ipp_job_cancel(&j, 8), IPP_CANCEL_RELEASE);
    CHECK(!ipp_job_active(&j));
    CHECK_EQ_INT(j.state, IPP_JSTATE_CANCELED);
    CHECK_EQ_INT(ipp_job_claim_doc(&j, 8), IPP_STATUS_ERR_NOT_POSSIBLE);
    CHECK_EQ_INT(ipp_job_reap(&j, 400 + 1000), 0);   /* and it is not freed twice */

    /* A Create-Job whose document never comes is reaped after the advertised
     * timeout, once, and not a second early. */
    ipp_job_start(&j, 9, 500, 1, "", "", URI);
    CHECK_EQ_INT(ipp_job_reap(&j, 500 + IPP_MULTI_OP_TIMEOUT_S - 1), 0);
    CHECK(ipp_job_active(&j));
    CHECK_EQ_INT(ipp_job_reap(&j, 500 + IPP_MULTI_OP_TIMEOUT_S), 1);
    CHECK(!ipp_job_active(&j));
    CHECK_EQ_INT(j.state, IPP_JSTATE_ABORTED);
    CHECK_EQ_INT(ipp_job_reap(&j, 500 + IPP_MULTI_OP_TIMEOUT_S + 1), 0);
    CHECK_EQ_INT(ipp_job_claim_doc(&j, 9), IPP_STATUS_ERR_NOT_POSSIBLE);

    /* uptime_s wrapping past zero must not make a fresh job look ancient. */
    ipp_job_start(&j, 10, 0xFFFFFFF0u, 1, "", "", URI);
    CHECK_EQ_INT(ipp_job_reap(&j, 0x10), 0);

    /* Job matching: -1 is the current job, anything else must be exact. */
    CHECK(ipp_job_matches(&j, -1));
    CHECK(ipp_job_matches(&j, 10));
    CHECK(!ipp_job_matches(&j, 11));

    /* Over-long names are cut to fit, never past the field. */
    char longname[IPP_NAME_MAX * 2];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    ipp_job_start(&j, 11, 0, 1, longname, longname, URI);
    CHECK_EQ_INT(strlen(j.name), IPP_NAME_MAX - 1);
    CHECK_EQ_INT(strlen(j.user), IPP_NAME_MAX - 1);

    TEST_END;
}
