/*
 * leg_b.c — Leg-B (outbound / called party) handlers
 *
 * Plays the collect prompt on loop after B answers.
 * Owns the DTMF accept/reject state machine and both timeout timers.
 * All async work (prompt start/done, accept transition, timers) posted
 * to worker pool — no per-call pthreads.
 */
#include "handlers.h"
#include "utils.h"
#include "config.h"
#include "prompt_mapping.h"
#include "runtime_config.h"
#include "worker.h"
#include "rtpengine.h"

#include <pjsua-lib/pjsua.h>
#include <pjmedia/sdp.h>
#include <pjmedia/wav_port.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define THIS_FILE "leg_b.c"

/* ── Forward declarations ────────────────────────────────────────────────── */
static void on_accept(pjsua_call_id call_b, cc_session_t *session);
static void on_reject(pjsua_call_id call_b, cc_session_t *session);
static void on_reject_mapped(pjsua_call_id call_b,
                             cc_session_t *session,
                             const char *status,
                             const char *reason,
                             char decision_digit,
                             cc_prompt_tag_t prompt_tag);

static const char *decision_name(char digit)
{
    if (digit == CC_DTMF_ACCEPT)
        return "ACCEPT";
    if (digit == CC_DTMF_REJECT)
        return "REJECT";
    return "NON_DTMF_END";
}

/* ── State callback ──────────────────────────────────────────────────────── */

void leg_b_on_call_state(pjsua_call_id call_id, cc_session_t *session)
{
    pjsua_call_info ci;
    pj_status_t status = pjsua_call_get_info(call_id, &ci);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B call info failed call=%d status=%d",
                   call_id, status));
        return;
    }

    PJ_LOG(3, (THIS_FILE, "[B] call_id=%d state=%.*s reason=%.*s",
               call_id,
               (int)ci.state_text.slen, ci.state_text.ptr,
               (int)ci.last_status_text.slen, ci.last_status_text.ptr));

    if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        /* Stamp B-confirmed monotonic time — free-period baseline starts here */
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            CC_SESSION_LOCK(session);
            if (session->b_confirmed_ms == 0)
                session->b_confirmed_ms = (long long)ts.tv_sec * 1000 +
                                          ts.tv_nsec / 1000000;
            CC_SESSION_UNLOCK(session);
        }
        /* Trigger collect prompt start now that B has answered.
         * If the media-state thread is already polling for CONFIRMED
         * it will unblock naturally; this call handles the case where
         * media was already active before CONFIRMED fired. */
        PJ_LOG(3, (THIS_FILE, "[B] CONFIRMED — attempting early collect prompt start"));
        leg_b_on_media_state(call_id, session);
    }

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        int already_accepted;
        int already_torn_down;
        int should_reject_a = 0;
        int should_hangup_a = 0;
        pjsua_call_id call_a = PJSUA_INVALID_ID;
        pjsua_player_id player_b = PJSUA_INVALID_ID;

        CC_SESSION_LOCK(session);

        if (session->player_b != PJSUA_INVALID_ID) {
            player_b = session->player_b;
            session->player_b = PJSUA_INVALID_ID;
        }
        session->b_prompt_starting = 0;
        session->b_on_hold = 0;
        {
            pjsua_player_id hold_pid = session->hold_player_a;
            session->hold_player_a = PJSUA_INVALID_ID;
            if (hold_pid != PJSUA_INVALID_ID) {
                CC_SESSION_UNLOCK(session);
                PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A hold MOH on B disconnect"));
                cc_stop_wav(hold_pid, PJSUA_INVALID_ID);
                CC_SESSION_LOCK(session);
            }
        }

        already_accepted  = session->accepted;
        already_torn_down = session->torn_down;
        call_a = session->call_a;

        if (!already_accepted && !already_torn_down) {
            session->decision_completed = 1;
            session->decision_digit = '\0';
            session->torn_down = 1;
            should_reject_a = 1;
        } else if (already_accepted && !already_torn_down) {
            session->torn_down = 1;
            should_hangup_a = 1;
        }

        CC_SESSION_UNLOCK(session);

        if (player_b != PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE, "[VOICE] Stop B collect prompt"));
            cc_stop_wav(player_b, PJSUA_INVALID_ID);
        }

        /* A's own DTMF is needed for the MCA wait below. */
        cc_rtpengine_unblock_media(session);

        cc_session_invalidate_b(session, call_id);

        if (should_reject_a) {

            /* B dropped before accepting — play rejection to A */
            PJ_LOG(3, (THIS_FILE, "[B] disconnected before accept — reject A"));

            if (ci.last_status == PJSIP_SC_TEMPORARILY_UNAVAILABLE) {
                /* 480: play UNAVAILABLE prompt, wait for A DTMF 1 for MCA */
                leg_a_play_mca_wait(session, CC_PROMPT_UNAVAILABLE);
            } else if (ci.last_status == PJSIP_SC_BUSY_HERE) {
                /* 486: play BUSY prompt, wait for A DTMF 1 for MCA */
                leg_a_play_mca_wait(session, CC_PROMPT_BUSY);
            } else if (ci.last_status == PJSIP_SC_DECLINE) {
                /* 603 Decline: B explicitly rejected — play rejected.wav.
                 * Defer mark_end so RTPengine stays up for A's treatment. */
                int treatment_armed = 0;
                CC_SESSION_LOCK(session);
                snprintf(session->final_status, sizeof(session->final_status),
                         "CANCELLED");
                snprintf(session->final_reason, sizeof(session->final_reason),
                         "REJECTED_BY_SPONSOR");
                CC_SESSION_UNLOCK(session);
                PJ_LOG(3, (THIS_FILE,
                           "[B] 603 Decline — play rejected (defer mark_end)"));
                leg_a_play_rejected_then_hangup(session);
                CC_SESSION_LOCK(session);
                treatment_armed = session->a_treatment_running;
                CC_SESSION_UNLOCK(session);
                if (!treatment_armed)
                    cc_session_mark_end(session, "CANCELLED",
                                        "REJECTED_BY_SPONSOR");
            } else {
                /* 503 No SBC / 408 / 500 / etc. — same MCA offer as 480/486.
                 * Do not mark_end yet: A may press 1 → SPONSOR_UNREACHABLE_MCA.
                 * Shared for local_bridge and UPDATE (A still on B2BUA media). */
                PJ_LOG(3, (THIS_FILE,
                           "[B] unreachable before accept status=%d — MCA wait "
                           "prompt=UNAVAILABLE",
                           ci.last_status));
                leg_a_play_mca_wait(session, CC_PROMPT_UNAVAILABLE);
            }
        }

        if (should_hangup_a) {
            if (cc_session_call_is_current(session, call_a, 1)) {
                PJ_LOG(3, (THIS_FILE, "[B] disconnected after accept — hangup A"));
                cc_session_mark_end(session, "COMPLETED", "NORMAL_CLEARING");
                cc_safe_hangup(call_a, PJSIP_SC_OK);
            }
        }

        cc_session_maybe_finalize(session);
    }
}


/* ── Media state callback ────────────────────────────────────────────────── */

void leg_b_on_media_state(pjsua_call_id call_id, cc_session_t *session)
{
    pjsua_call_info       ci;
    pjmedia_sdp_session  *sdp = NULL;
    cc_rtp_ep_t           ep;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] B media call info failed call=%d", call_id));
        return;
    }

    if (ci.media_cnt == 0 ||
        ci.media[0].status != PJSUA_CALL_MEDIA_ACTIVE)
        return;

    /*
     * This PJSUA build does not expose media session access here.
     * RTP endpoints are learned later from transport info before UPDATE.
     */
    (void)sdp;
    (void)ep;
    
   cc_log_call_rtp_info(call_id, "B");

    /* Isolate both legs from the master mix to prevent audio cross-leak */
    {
        pjsua_call_id call_a;
        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        CC_SESSION_UNLOCK(session);
        cc_isolate_call_from_master(call_id);   /* B */
        if (call_a != PJSUA_INVALID_ID)
            cc_isolate_call_from_master(call_a); /* A */
    }

    /* Record B answer timestamp (toll-free period start) */
    CC_SESSION_LOCK(session);
    if (session->b_answer_ts == 0)
        session->b_answer_ts = time(NULL);
    CC_SESSION_UNLOCK(session);

    CC_SESSION_LOCK(session);
    if (session->accepted || session->torn_down ||
        session->player_b != PJSUA_INVALID_ID ||
        session->b_prompt_starting)
    {
        int accepted = session->accepted;
        int torn_down = session->torn_down;
        CC_SESSION_UNLOCK(session);

        if (accepted)
            PJ_LOG(3, (THIS_FILE, "[B] media update ignored after accept; not restarting collect prompt"));
        else if (torn_down)
            PJ_LOG(3, (THIS_FILE, "[B] media update ignored after teardown; not restarting collect prompt"));
        else
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] B collect prompt already active/starting, skip"));
        return;
    }

    /* Whitelisted: skip collect prompt, auto-accept on CONFIRMED only */
    if (session->whitelisted) {
        CC_SESSION_UNLOCK(session);
        if (ci.state != PJSIP_INV_STATE_CONFIRMED) {
            PJ_LOG(3, (THIS_FILE,
                       "[WHITELIST] B-leg media active but not CONFIRMED (state=%d); deferring accept",
                       ci.state));
            return;
        }
        PJ_LOG(3, (THIS_FILE,
                   "[WHITELIST] B-leg CONFIRMED; skipping collect prompt, auto-accept (whitelisted=%d)",
                   session->whitelisted));
        /* Stamp b_prompt_start_ts at CONFIRMED — used as call_connected_ts
         * baseline in ev_accept_transition (no prompt thread runs for
         * whitelisted calls so it would otherwise remain 0). */
        CC_SESSION_LOCK(session);
        if (session->b_prompt_start_ts == 0)
            session->b_prompt_start_ts = time(NULL);
        /* No collect prompt plays for whitelisted calls — mark done immediately
         * so the on_accept spin-wait does not block. */
        session->b_collect_done = 1;
        CC_SESSION_UNLOCK(session);
        on_accept(call_id, session);
        return;
    }

    session->b_prompt_starting = 1;
    CC_SESSION_UNLOCK(session);

    /* Spawn a thread to do the CONFIRMED poll + RTP poll + WAV start.
     * Must not block the PJSUA callback thread — doing so holds the
     * PJSIP worker while PRACK/200 transactions are pending, which
     * causes reinv_timer_cb to time out acquiring the dialog lock. */
    if (cc_session_acquire_reason(session, "b-prompt-start")) {
        cc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type    = CC_EV_B_PROMPT_START;
        ev.session = session;
        ev.call_b  = call_id;
        snprintf(ev.reason, sizeof(ev.reason), "b-prompt-start");
        if (cc_worker_post(&ev) == 0)
            return;
        cc_session_release_reason(session, "b-prompt-start");
    }
    /* fallback: clear flag so a future media-state callback can retry */
    CC_SESSION_LOCK(session);
    session->b_prompt_starting = 0;
    CC_SESSION_UNLOCK(session);
}

/* ── DTMF callback ───────────────────────────────────────────────────────── */

void leg_b_on_dtmf(pjsua_call_id call_id, int digit, cc_session_t *session)
{
    if ((char)digit == CC_DTMF_ACCEPT)
        on_accept(call_id, session);
    else if ((char)digit == CC_DTMF_REJECT)
        on_reject(call_id, session);
    else
        PJ_LOG(3, (THIS_FILE,
                   "[DTMF] B-leg digit=%c ignored; valid decisions are 1=ACCEPT and 2=REJECT",
                   (char)digit));
}

/* ── SIP UPDATE to exit RTP path ─────────────────────────────────────────── */

void leg_b_send_update_bypass(pjsua_call_id call_id, cc_session_t *session)
{
    /* Non-blocking: post CC_EV_UPDATE_B_BYPASS to worker pool.
     * The worker polls b_reinvite_active + RTP readiness via re-post
     * every 50ms — no worker is blocked sleeping. */
    cc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_UPDATE_B_BYPASS;
    ev.session = session;
    ev.call_b  = call_id;
    CC_SESSION_LOCK(session);
    ev.call_a  = session->call_a;
    CC_SESSION_UNLOCK(session);
    snprintf(ev.reason, sizeof(ev.reason), "update-b-bypass");
    if (cc_worker_post(&ev) != 0)
        PJ_LOG(1, (THIS_FILE, "[B] UPDATE bypass post failed"));
}

/* ── Accept / Reject FSM ─────────────────────────────────────────────────── */

void leg_b_send_reinvite_bypass(cc_session_t *session)
{
    /* Non-blocking: post CC_EV_REINVITE_B_BYPASS to worker pool.
     * The worker polls b_reinvite_active + RTP readiness via 50ms re-post
     * before sending pjsua_call_reinvite — no worker is blocked sleeping. */
    cc_event_t ev;
    pjsua_call_id call_a, call_b;

    if (!session)
        return;

    CC_SESSION_LOCK(session);
    call_a = session->call_a;
    call_b = session->call_b;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_REINVITE_B_BYPASS;
    ev.session = session;
    ev.call_a  = call_a;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "reinvite-b-bypass");
    if (cc_worker_post(&ev) != 0)
        PJ_LOG(1, (THIS_FILE, "[B] re-INVITE bypass post failed"));
}

static int spawn_accept_transition(cc_session_t *session,
                                   pjsua_call_id call_a,
                                   pjsua_call_id call_b)
{
    cc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_ACCEPT_TRANSITION;
    ev.session = session;
    ev.call_a  = call_a;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "accept-transition-worker");

    CC_SESSION_LOCK(session);
    session->accept_transition_pending = 1;
    CC_SESSION_UNLOCK(session);

    if (cc_worker_post(&ev) != 0) {
        CC_SESSION_LOCK(session);
        session->accept_transition_pending = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE, "[ERROR] accept transition worker post failed"));
        return 0;
    }
    return 1;
}

static void on_accept(pjsua_call_id call_b, cc_session_t *session)
{
    char call_id[128];
    char completed_digit;
    pjsua_call_id call_a;
    int duplicate;

    CC_SESSION_LOCK(session);
    completed_digit = session->decision_digit;
    duplicate = session->decision_completed ||
                session->accepted ||
                session->torn_down;
    if (duplicate || session->call_b != call_b) {
        int current_call = session->call_b == call_b;
        CC_SESSION_UNLOCK(session);
        if (current_call)
            PJ_LOG(3, (THIS_FILE,
                       "[DTMF] duplicate digit ignored; decision already completed=%s",
                       decision_name(completed_digit)));
        else
            PJ_LOG(3, (THIS_FILE,
                       "[DTMF] stale B-leg digit ignored call_id=%d", call_b));
        return;
    }

    session->decision_completed = 1;
    session->decision_digit = CC_DTMF_ACCEPT;
    session->accepted = 1;
    snprintf(call_id, sizeof(call_id), "%s", session->call_id);
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE, "[CALL-CONNECTED] callId=%s", call_id));
    PJ_LOG(3, (THIS_FILE, "[DTMF] accept media transition queued outside callback"));

    if (!spawn_accept_transition(session, call_a, call_b))
        PJ_LOG(1, (THIS_FILE, "[ERROR] accept transition post failed — call will not bridge"));
}

static void on_reject(pjsua_call_id call_b, cc_session_t *session)
{
    on_reject_mapped(call_b,
                     session,
                     "CANCELLED",
                     "REJECTED_BY_SPONSOR",
                     CC_DTMF_REJECT,
                     CC_PROMPT_REJECTED);
}

static void on_reject_mapped(pjsua_call_id call_b,
                             cc_session_t *session,
                             const char *status,
                             const char *reason,
                             char decision_digit,
                             cc_prompt_tag_t prompt_tag)
{
    pjsua_player_id player_b = PJSUA_INVALID_ID;
    pjsua_call_id call_a = PJSUA_INVALID_ID;
    char completed_digit;
    int treatment_armed = 0;
    long billable = -1;

    CC_SESSION_LOCK(session);
    completed_digit = session->decision_digit;
    if (session->decision_completed ||
        session->torn_down ||
        session->accepted)
    {
        CC_SESSION_UNLOCK(session);
        if (decision_digit != '\0') {
            PJ_LOG(3, (THIS_FILE,
                       "[DTMF] duplicate digit ignored; decision already completed=%s",
                       decision_name(completed_digit)));
        }
        return;
    }
    session->decision_completed = 1;
    session->decision_digit = decision_digit;
    session->torn_down = 1;
    /*
     * Defer mark_end / RTPengine delete until after A's treatment WAV
     * (1.45 / rejected). Same pattern as validation-reject and MCA resolve.
     */
    snprintf(session->final_status, sizeof(session->final_status), "%s",
             status ? status : "FAILED");
    snprintf(session->final_reason, sizeof(session->final_reason), "%s",
             reason ? reason : "REJECTED");

    /*
     * Stop the billing clock here — this is the decision point. mark_end
     * runs later (after A's treatment prompt) and would otherwise stamp the
     * end then, adding the prompt length to the duration: a 10 s no-DTMF
     * window was being reported as 16-18 s, and a digit-2 reject at 6 s as
     * 12 s. mark_end keeps a non-zero call_end_ts.
     */
    if (session->call_end_ts == 0) {
        session->call_end_ts = time(NULL);
        billable = session->call_connected_ts > 0
                   ? (long)(session->call_end_ts - session->call_connected_ts)
                   : (session->b_prompt_start_ts > 0
                      ? (long)(session->call_end_ts - session->b_prompt_start_ts)
                      : 0);
        if (billable < 0)
            billable = 0;
    }

    if (session->player_b != PJSUA_INVALID_ID) {
        player_b = session->player_b;
        session->player_b = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(session);

    if (billable >= 0)
        PJ_LOG(3, (THIS_FILE,
                   "[CDR] billing clock stopped at decision reason=%s "
                   "billable=%lds (A treatment excluded)",
                   reason ? reason : "", billable));

    if (player_b != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop B collect prompt"));
        cc_stop_wav(player_b, PJSUA_INVALID_ID);
    }

    /* Collect phase over — A must hear its treatment prompt and be able
     * to send DTMF again. */
    cc_rtpengine_unblock_media(session);

    PJ_LOG(3, (THIS_FILE,
               "[B] REJECTED reason=%s (defer mark_end for A treatment)",
               reason ? reason : ""));
    if (cc_session_call_is_current(session, call_b, 0))
        cc_safe_hangup(call_b, PJSIP_SC_OK);
    else
        PJ_LOG(3, (THIS_FILE,
                   "[TIMER] skipped stale action: reject B call=%d", call_b));
    leg_a_play_prompt_then_hangup(session, prompt_tag, PJSIP_SC_DECLINE);

    CC_SESSION_LOCK(session);
    treatment_armed = session->a_treatment_running;
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);
    if (!treatment_armed) {
        /* Prompt/worker post failed — hang up A and emit CDR now. */
        if (call_a != PJSUA_INVALID_ID &&
            (cc_session_call_is_current(session, call_a, 1) ||
             pjsua_call_is_active(call_a) == PJ_TRUE))
            cc_safe_hangup(call_a, PJSIP_SC_DECLINE);
        cc_session_mark_end(session, status, reason);
    }
}

/* ── Timer threads ───────────────────────────────────────────────────────── */

static void spawn_timer(cc_session_t *session, int timeout_sec, int is_ring)
{
    pjsua_call_id call_b;
    cc_event_t ev;

    CC_SESSION_LOCK(session);
    if (session->torn_down ||
        session->call_b == PJSUA_INVALID_ID ||
        (is_ring ? session->ring_timer_started
                 : session->dtmf_timer_started))
    {
        CC_SESSION_UNLOCK(session);
        return;
    }
    if (is_ring) session->ring_timer_started = 1;
    else         session->dtmf_timer_started = 1;
    call_b = session->call_b;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type        = is_ring ? CC_EV_RING_TIMER : CC_EV_DTMF_TIMER;
    ev.session     = session;
    ev.call_b      = call_b;
    ev.timeout_sec = timeout_sec;
    snprintf(ev.reason, sizeof(ev.reason),
             is_ring ? "ring-timer" : "dtmf-timer");

    if (cc_worker_post_delayed(&ev, timeout_sec * 1000) != 0) {
        CC_SESSION_LOCK(session);
        if (is_ring) session->ring_timer_started = 0;
        else         session->dtmf_timer_started = 0;
        CC_SESSION_UNLOCK(session);
    }
}

void leg_b_start_ring_timer(cc_session_t *session)
{
    spawn_timer(session, CC_B_RING_TIMEOUT_SEC, 1);
}

void leg_b_start_dtmf_timer(cc_session_t *session)
{
    /* DTMF window = free period: B must press 1 within the same window
     * during which the call is free. After this, ELIGIBILITY_TIMEOUT fires. */
    spawn_timer(session, cc_cfg_free_period_ms() / 1000, 0);
}

void leg_b_on_dtmf_timeout(pjsua_call_id call_b, cc_session_t *session)
{
    on_reject_mapped(call_b,
                     session,
                     "FAILED",
                     "ELIGIBILITY_TIMEOUT",
                     '\0',
                     CC_PROMPT_NOT_AVAILABLE_TO_PAY);
}
