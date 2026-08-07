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

static const char *disconnect_before_accept_reason(pjsip_status_code code)
{
    if (code >= 300 &&
        code != PJSIP_SC_BUSY_HERE &&
        code != PJSIP_SC_DECLINE)
    {
        return "SPONSOR_UNREACHABLE_NoMCA";
    }

    return "REJECTED_BY_SPONSOR";
}

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
                /* 603 Decline: B explicitly rejected */
                cc_session_mark_end(session, "CANCELLED", "REJECTED_BY_SPONSOR");
                leg_a_play_rejected_then_hangup(session);
            } else {
                /* 408 no-answer, 487 cancelled, 503 unreachable, etc. */
                cc_session_mark_end(session,
                                    "CANCELLED",
                                    disconnect_before_accept_reason(
                                        ci.last_status));
                leg_a_play_prompt_then_hangup(session,
                                             CC_PROMPT_NOT_AVAILABLE_TO_PAY,
                                             PJSIP_SC_TEMPORARILY_UNAVAILABLE);
            }
        }

        if (should_hangup_a) {
            if (cc_session_call_is_current(session, call_a, 1)) {
                PJ_LOG(3, (THIS_FILE, "[B] disconnected after accept — hangup A"));
                cc_session_mark_end(session, "COMPLETED", "NORMAL_CLEARING");
                cc_safe_hangup(call_a, PJSIP_SC_OK);
            }
        }
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
    pjsua_msg_data msg_data;
    pj_status_t status;
    pjsua_call_id call_a;
    pjsua_call_id call_b;
    cc_rtp_ep_t rtp_a;
    cc_rtp_ep_t rtp_b;

    /*
     * Send UPDATE after B accepts. If both RTP endpoints are known,
     * cc_on_call_sdp_created() rewrites this leg's SDP before send.
     */

	/* Learn current A/B remote RTP endpoints before sending UPDATE.
	 * B-leg UPDATE SDP must advertise A-party RTP IP/port.
	 */
        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        call_b = session->call_b;
        CC_SESSION_UNLOCK(session);

        if (call_a == PJSUA_INVALID_ID || call_b == PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE,
                       "[TIMER] skipped stale action: B UPDATE"));
            return;
        }

        /* B-leg must be CONFIRMED before we redirect B's RTP to A */
        {
            pjsua_call_info ci_b;
            if (pjsua_call_get_info(call_b, &ci_b) != PJ_SUCCESS ||
                ci_b.state != PJSIP_INV_STATE_CONFIRMED) {
                PJ_LOG(1, (THIS_FILE,
                           "[B] UPDATE skipped: B-leg not CONFIRMED (call_b=%d)",
                           call_b));
                return;
            }
        }

        /* Wait up to 5s for both:
         *   1. SBC re-INVITE on B-leg to complete (b_reinvite_active == 0)
         *   2. B's src_rtp_name valid (RTP packets from post-re-INVITE MGW)
         *
         * These must be checked simultaneously: b_reinvite_active clears at
         * CONFIRMED but RTP from the new MGW IP may not have arrived yet.
         * Checking them in sequence risks arming with the stale 183 endpoint
         * if RTP restarts before the flag clears or vice versa. */
        {
            int wait_ms = 0;
            for (; wait_ms < 5000; wait_ms += 50) {
                int ri_active, torn;
                CC_SESSION_LOCK(session);
                ri_active = session->b_reinvite_active;
                torn = session->torn_down || session->call_b != call_b;
                CC_SESSION_UNLOCK(session);
                if (torn) return;
                if (!ri_active &&
                    cc_get_call_remote_rtp(call_b, &rtp_b) == PJ_SUCCESS &&
                    rtp_b.port != 0)
                    break;
                cc_sleep_ms(50);
            }
            PJ_LOG(3, (THIS_FILE,
                       "[B] B RTP endpoint after %dms wait: %s:%d",
                       wait_ms, rtp_b.ip, rtp_b.port));
        }

        /* Wait up to 1s for A's src_rtp_name — A has been confirmed since
         * call start so this is typically 0ms. */
        {
            int rtp_wait_ms = 0;
            cc_rtp_ep_t rtp_a_check;
            while (rtp_wait_ms < 1000) {
                int torn;
                if (cc_get_call_remote_rtp(call_a, &rtp_a_check) == PJ_SUCCESS &&
                    rtp_a_check.port != 0)
                    break;
                cc_sleep_ms(50);
                rtp_wait_ms += 50;
                CC_SESSION_LOCK(session);
                torn = session->torn_down || session->call_a != call_a;
                CC_SESSION_UNLOCK(session);
                if (torn) return;
            }
            PJ_LOG(3, (THIS_FILE,
                       "[B] A RTP endpoint after %dms wait: %s:%d",
                       rtp_wait_ms, rtp_a_check.ip, rtp_a_check.port));
        }

	if (cc_get_call_remote_rtp(call_a, &rtp_a) == PJ_SUCCESS &&
	    cc_get_call_remote_rtp(call_b, &rtp_b) == PJ_SUCCESS)
	{
	    CC_SESSION_LOCK(session);
            if (session->call_a == call_a &&
                session->call_b == call_b &&
                !session->torn_down)
            {
                session->rtp_a = rtp_a;
                session->rtp_b = rtp_b;
	        session->update_b_pending = 1;
            }
	    CC_SESSION_UNLOCK(session);

	    PJ_LOG(3, (THIS_FILE,
	               "[B] UPDATE rewrite armed: B will receive A RTP %s:%d",
	               rtp_a.ip, rtp_a.port));
	} else {
	    PJ_LOG(1, (THIS_FILE,
	               "[B] Cannot arm UPDATE rewrite: RTP endpoints not ready"));
	}


    pjsua_msg_data_init(&msg_data);

    PJ_LOG(3, (THIS_FILE, "[B] Sending basic SIP UPDATE"));

    /* Retry up to 2s if dialog has a pending transaction (mirrors A-leg) */
    {
        int retry_ms = 0;
        do {
            status = pjsua_call_update(call_id, 0, &msg_data);
            if (status == PJ_SUCCESS || retry_ms >= 2000)
                break;
            cc_sleep_ms(100);
            retry_ms += 100;
        } while (1);
    }

    if (status == PJ_SUCCESS) {
        CC_SESSION_LOCK(session);
        session->update_b_sent = 1;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[B] SIP UPDATE sent"));
    } else {
        CC_SESSION_LOCK(session);
        session->update_b_pending = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE, "[B] SIP UPDATE failed: %d", status));
    }
}

/* ── Accept / Reject FSM ─────────────────────────────────────────────────── */

void leg_b_send_reinvite_bypass(cc_session_t *session)
{
    pjsua_call_id call_a;
    pjsua_call_id call_b;
    cc_rtp_ep_t rtp_a;
    cc_rtp_ep_t rtp_b;
    pjsua_msg_data msg_data;
    pj_status_t status;

    if (!session)
        return;

    CC_SESSION_LOCK(session);
    call_a = session->call_a;
    call_b = session->call_b;
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE, "[REINVITE] Preparing B-leg re-INVITE"));

    if (call_a == PJSUA_INVALID_ID || call_b == PJSUA_INVALID_ID) {
        PJ_LOG(1, (THIS_FILE,
                   "[REINVITE] B-leg re-INVITE skipped: invalid call ids A=%d B=%d",
                   call_a, call_b));
        return;
    }

    if (cc_get_call_remote_rtp(call_a, &rtp_a) != PJ_SUCCESS ||
        cc_get_call_remote_rtp(call_b, &rtp_b) != PJ_SUCCESS)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[REINVITE] B-leg re-INVITE skipped: RTP endpoints not ready"));
        return;
    }

    CC_SESSION_LOCK(session);
    if (session->call_a == call_a &&
        session->call_b == call_b &&
        !session->torn_down)
    {
        session->rtp_a = rtp_a;
        session->rtp_b = rtp_b;
        session->reinvite_b_pending = 1;
    }
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[REINVITE] B-leg SDP target A RTP %s:%d",
               rtp_a.ip, rtp_a.port));

    pjsua_msg_data_init(&msg_data);

    status = pjsua_call_reinvite(call_b, 0, &msg_data);

    if (status == PJ_SUCCESS) {
        PJ_LOG(3, (THIS_FILE, "[REINVITE] B-leg re-INVITE sent"));
    } else {
        CC_SESSION_LOCK(session);
        session->reinvite_b_pending = 0;
        CC_SESSION_UNLOCK(session);

        PJ_LOG(1, (THIS_FILE,
                   "[REINVITE] B-leg re-INVITE failed: %d",
                   status));
    }
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
    char completed_digit;

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

    if (session->player_b != PJSUA_INVALID_ID) {
        player_b = session->player_b;
        session->player_b = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(session);

    if (player_b != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop B collect prompt"));
        cc_stop_wav(player_b, PJSUA_INVALID_ID);
    }

    PJ_LOG(3, (THIS_FILE, "[B] REJECTED"));
    cc_session_mark_end(session, status, reason);
    if (cc_session_call_is_current(session, call_b, 0))
        cc_safe_hangup(call_b, PJSIP_SC_OK);
    else
        PJ_LOG(3, (THIS_FILE,
                   "[TIMER] skipped stale action: reject B call=%d", call_b));
    leg_a_play_prompt_then_hangup(session, prompt_tag, PJSIP_SC_DECLINE);
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

    if (cc_worker_post(&ev) != 0) {
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
