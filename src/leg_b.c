/*
 * leg_b.c — Leg-B (outbound / called party) handlers
 *
 * Plays the collect prompt on loop after B answers.
 * Owns the DTMF accept/reject state machine and both timeout timers.
 */
#include "handlers.h"
#include "utils.h"
#include "config.h"
#include "prompt_mapping.h"
#include "runtime_config.h"

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

/* WAV player duration in ms. Fallback 4000ms. */
static int cc_player_duration_ms(pjsua_player_id pid)
{
    pjmedia_port *port = NULL;
    pj_ssize_t data_len;
    const pjmedia_port_info *info;
    int bytes_per_sample;
    int duration_ms;

    if (pid == PJSUA_INVALID_ID)
        return 4000;
    if (pjsua_player_get_port(pid, &port) != PJ_SUCCESS || !port)
        return 4000;
    data_len = pjmedia_wav_player_get_len(port);
    if (data_len <= 0)
        return 4000;
    info = &port->info;
    bytes_per_sample = (info->fmt.det.aud.bits_per_sample / 8) *
                       info->fmt.det.aud.channel_count;
    if (bytes_per_sample <= 0 || info->fmt.det.aud.clock_rate == 0)
        return 4000;
    duration_ms = (int)((long long)data_len * 1000 /
                        (info->fmt.det.aud.clock_rate * bytes_per_sample));
    return duration_ms > 0 ? duration_ms : 4000;
}

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

        cc_session_maybe_finalize(session);
    }
}


/* Thread: wait for B collect prompt to finish, then start DTMF timer (fix 4b) */
void *cc_b_prompt_done_thread(void *opaque)
{
    typedef struct { cc_session_t *s; int ms; } bpd_arg_t;
    bpd_arg_t    *arg = (bpd_arg_t *)opaque;
    cc_session_t *s   = arg->s;
    int           wait_ms = arg->ms;
    pj_thread_desc desc;
    pj_thread_t   *th = NULL;

    pj_bzero(desc, sizeof(desc));
    pj_thread_register("cc_bpd", desc, &th);
    free(arg);

    /* After 1s, log RTP state to confirm B is actually sending RTP back.
     * If remote/src is still invalid:0 here, B's RTP path is not live
     * and B cannot hear the prompt. */
    int rtp_checked = 0;
    int remaining = wait_ms;
    while (remaining > 0) {
        int slice = remaining > 100 ? 100 : remaining;
        cc_sleep_ms(slice);
        remaining -= slice;

        if (!rtp_checked && (wait_ms - remaining) >= 1000) {
            rtp_checked = 1;
            pjsua_call_id call_b_id;
            CC_SESSION_LOCK(s);
            call_b_id = s->call_b;
            CC_SESSION_UNLOCK(s);
            if (call_b_id != PJSUA_INVALID_ID)
                cc_log_call_rtp_info(call_b_id, "B-rtp-1s");
        }

        int done;
        CC_SESSION_LOCK(s);
        done = s->torn_down || s->final_cleanup_started;
        CC_SESSION_UNLOCK(s);
        if (done) {
            /* torn down — still signal b_collect_done so on_accept spin-wait unblocks */
            CC_SESSION_LOCK(s);
            s->b_collect_done = 1;
            CC_SESSION_UNLOCK(s);
            cc_session_maybe_finalize(s);
            cc_session_release_reason(s, "b-prompt-done");
            return NULL;
        }
    }

    CC_SESSION_LOCK(s);
    s->b_collect_done = 1;
    CC_SESSION_UNLOCK(s);

    PJ_LOG(3, (THIS_FILE, "[B] Collect prompt finished"));

    cc_session_maybe_finalize(s);
    cc_session_release_reason(s, "b-prompt-done");
    return NULL;
}

/* Thread: wait for CONFIRMED, then RTP ready, then start collect prompt */
static void *cc_b_prompt_start_thread(void *opaque)
{
    typedef struct { cc_session_t *s; pjsua_call_id cid; } bps_arg_t;
    bps_arg_t    *arg     = (bps_arg_t *)opaque;
    cc_session_t *session = arg->s;
    pjsua_call_id call_id = arg->cid;
    pjsua_player_id pid;
    int keep_player = 0;
    const char *collect_prompt_path;
    pj_thread_desc desc;
    pj_thread_t   *th = NULL;

    pj_bzero(desc, sizeof(desc));
    pj_thread_register("cc_bps", desc, &th);
    free(arg);

    /* Poll until CONFIRMED (max 30s) */
    {
        int wait_ms = 0, aborted = 0;
        while (wait_ms < 30000) {
            pjsua_call_info ci2;
            if (pjsua_call_get_info(call_id, &ci2) == PJ_SUCCESS &&
                ci2.state == PJSIP_INV_STATE_CONFIRMED)
                break;
            cc_sleep_ms(100);
            wait_ms += 100;
            CC_SESSION_LOCK(session);
            aborted = session->accepted || session->torn_down ||
                      session->call_b != call_id;
            CC_SESSION_UNLOCK(session);
            if (aborted) break;
        }
        CC_SESSION_LOCK(session);
        aborted = aborted || session->accepted || session->torn_down ||
                  session->call_b != call_id;
        CC_SESSION_UNLOCK(session);
        if (aborted) goto done_clear;
        PJ_LOG(3, (THIS_FILE, "[B] CONFIRMED — starting collect prompt after %dms", wait_ms));
    }

    /* Poll until B's remote RTP endpoint is valid (max 3s) */
    {
        cc_rtp_ep_t rtp_ep;
        int rtp_wait_ms = 0, aborted = 0;
        while (rtp_wait_ms < 3000) {
            if (cc_get_call_remote_rtp(call_id, &rtp_ep) == PJ_SUCCESS &&
                rtp_ep.port != 0)
                break;
            cc_sleep_ms(100);
            rtp_wait_ms += 100;
            CC_SESSION_LOCK(session);
            aborted = session->accepted || session->torn_down ||
                      session->call_b != call_id;
            CC_SESSION_UNLOCK(session);
            if (aborted) break;
        }
        if (aborted) goto done_clear;
        PJ_LOG(3, (THIS_FILE, "[B] RTP ready after %dms (remote=%s:%d)",
                   rtp_wait_ms, rtp_ep.ip, rtp_ep.port));
    }

    collect_prompt_path = cc_prompt_get_path(CC_PROMPT_COLLECT_PROMPT);
    PJ_LOG(3, (THIS_FILE, "[VOICE] Start B collect prompt: %s", collect_prompt_path));

    CC_SESSION_LOCK(session);
    if (session->b_prompt_start_ts == 0)
        session->b_prompt_start_ts = time(NULL);
    CC_SESSION_UNLOCK(session);

    pid = cc_start_wav(call_id, collect_prompt_path, PJ_FALSE);

    CC_SESSION_LOCK(session);
    if (pid != PJSUA_INVALID_ID &&
        !session->accepted && !session->torn_down &&
        session->call_b == call_id &&
        session->player_b == PJSUA_INVALID_ID)
    {
        session->player_b = pid;
        keep_player = 1;
    }
    session->b_prompt_starting = 0;
    CC_SESSION_UNLOCK(session);

    if (keep_player) {
        int prompt_ms = cc_player_duration_ms(pid);
        PJ_LOG(3, (THIS_FILE, "[B] Collect prompt started (one-shot) player=%d duration=%dms",
                   pid, prompt_ms));
        CC_SESSION_LOCK(session);
        session->b_collect_done = 0;
        CC_SESSION_UNLOCK(session);
        /* Spawn thread to signal b_collect_done when prompt finishes.
         * DTMF timer starts in parallel — B can press 1/2 at any time,
         * but accept transition waits for b_collect_done before bridging. */
        typedef struct { cc_session_t *s; int ms; } bpd_arg_t;
        bpd_arg_t *bpd = malloc(sizeof(*bpd));
        if (bpd && cc_session_acquire_reason(session, "b-prompt-done")) {
            bpd->s  = session;
            bpd->ms = prompt_ms;
            pthread_t bpd_t;
            if (pthread_create(&bpd_t, NULL, cc_b_prompt_done_thread, bpd) == 0) {
                pthread_detach(bpd_t);
            } else {
                free(bpd);
                cc_session_release_reason(session, "b-prompt-done");
                /* fallback: mark done immediately so accept is not blocked */
                CC_SESSION_LOCK(session);
                session->b_collect_done = 1;
                CC_SESSION_UNLOCK(session);
            }
        } else {
            free(bpd);
            CC_SESSION_LOCK(session);
            session->b_collect_done = 1;
            CC_SESSION_UNLOCK(session);
        }
        leg_b_start_dtmf_timer(session);
    } else if (pid != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] B collect prompt became stale, destroying player=%d", pid));
        cc_stop_wav(pid, PJSUA_INVALID_ID);
    }

    cc_session_maybe_finalize(session);
    cc_session_release_reason(session, "b-prompt-start");
    return NULL;

done_clear:
    CC_SESSION_LOCK(session);
    session->b_prompt_starting = 0;
    CC_SESSION_UNLOCK(session);
    cc_session_maybe_finalize(session);
    cc_session_release_reason(session, "b-prompt-start");
    return NULL;
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
         * baseline in run_accept_transition (no prompt thread runs for
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
        typedef struct { cc_session_t *s; pjsua_call_id cid; } bps_arg_t;
        bps_arg_t *arg = malloc(sizeof(*arg));
        if (arg) {
            arg->s   = session;
            arg->cid = call_id;
            pthread_t t;
            if (pthread_create(&t, NULL, cc_b_prompt_start_thread, arg) == 0) {
                pthread_detach(t);
                return;
            }
            free(arg);
        }
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

static void run_accept_transition(cc_session_t *session,
                                  pjsua_call_id call_a,
                                  pjsua_call_id call_b,
                                  int delay_ms)
{
    pjsua_player_id player_b = PJSUA_INVALID_ID;
    pjsua_player_id player_a = PJSUA_INVALID_ID;

    CC_SESSION_LOCK(session);
    if (!session->torn_down &&
        session->accepted &&
        session->call_a == call_a &&
        session->call_b == call_b &&
        session->player_b != PJSUA_INVALID_ID)
    {
        player_b = session->player_b;
        session->player_b = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(session);

    if (player_b != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop B collect prompt"));
        cc_stop_wav(player_b, PJSUA_INVALID_ID);
    }

    PJ_LOG(3, (THIS_FILE, "[B] ACCEPTED — charging/communication starts now"));
    if (!cc_session_call_is_current(session, call_a, 1)) {
        cc_safe_hangup(call_b, PJSIP_SC_OK);
        return;
    }

    {
        int whitelisted;
        CC_SESSION_LOCK(session);
        whitelisted = session->whitelisted;
        CC_SESSION_UNLOCK(session);

        if (whitelisted) {
            /*
             * Whitelisted flow:
             * 1. Play 4.1.wav to B (one-shot, full duration)
             * 2. A keeps hearing 4.wav (player_a) or silence if already done
             * 3. After 4.1.wav finishes, honour any remaining free-period
             *    delay before bridging — call must not connect before the
             *    toll-free period expires regardless of whitelist status.
             * 4. Stop 4.wav on A, then bridge.
             */
            pjsua_player_id b_conn_pid = PJSUA_INVALID_ID;
            const char *b_conn_path = cc_prompt_get_path(CC_PROMPT_B_CONNECTED);

            if (cc_session_call_is_current(session, call_b, 0))
                b_conn_pid = cc_start_wav(call_b, b_conn_path, PJ_FALSE);

            int b_conn_ms = cc_player_duration_ms(b_conn_pid);
            PJ_LOG(3, (THIS_FILE,
                       "[WHITELIST] Playing 4.1.wav to B: %s (%dms); A keeps 4.wav",
                       b_conn_path, b_conn_ms));
            cc_sleep_ms(b_conn_ms);

            if (b_conn_pid != PJSUA_INVALID_ID)
                cc_stop_wav(b_conn_pid, PJSUA_INVALID_ID);

            /* Honour remaining free-period after 4.1.wav finishes — recompute
             * from wall clock so prompt-wait time is not double-counted. */
            {
                int fp_remaining = 0;
                long long b_confirmed_ms;
                int free_period;
                CC_SESSION_LOCK(session);
                b_confirmed_ms = session->b_confirmed_ms;
                free_period    = session->free_period_ms > 0
                                 ? session->free_period_ms
                                 : cc_cfg_free_period_ms();
                CC_SESSION_UNLOCK(session);
                if (b_confirmed_ms > 0) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    long long now_ms = (long long)ts.tv_sec * 1000 +
                                       ts.tv_nsec / 1000000;
                    long long elapsed = now_ms - b_confirmed_ms;
                    long long rem = (long long)free_period - elapsed;
                    fp_remaining = rem > 0 ? (int)rem : 0;
                }
                if (fp_remaining > 0) {
                    PJ_LOG(3, (THIS_FILE,
                               "[FREE-PERIOD] whitelist: waiting remaining %dms after 4.1.wav",
                               fp_remaining));
                    cc_sleep_ms(fp_remaining);
                }
            }

            /* Stop 4.wav on A now that free period is satisfied */
            CC_SESSION_LOCK(session);
            if (session->player_a != PJSUA_INVALID_ID) {
                player_a = session->player_a;
                session->player_a = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(session);
            if (player_a != PJSUA_INVALID_ID) {
                PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A MOH (4.wav) after B-connected prompt"));
                cc_stop_wav(player_a, PJSUA_INVALID_ID);
                player_a = PJSUA_INVALID_ID;
            }
        } else {
            /* Non-whitelisted: stop A prompt, then free-period dial tone on both legs */
            CC_SESSION_LOCK(session);
            if (session->player_a != PJSUA_INVALID_ID) {
                player_a = session->player_a;
                session->player_a = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(session);
            if (player_a != PJSUA_INVALID_ID) {
                PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A waiting prompt before dial tone"));
                cc_stop_wav(player_a, PJSUA_INVALID_ID);
                player_a = PJSUA_INVALID_ID;
            }

            if (delay_ms > 0) {
                /* Recompute remaining free period now — prompt-wait consumed some of it */
                int actual_delay = 0;
                {
                    long long b_confirmed_ms;
                    int free_period;
                    CC_SESSION_LOCK(session);
                    b_confirmed_ms = session->b_confirmed_ms;
                    free_period    = session->free_period_ms > 0
                                     ? session->free_period_ms
                                     : cc_cfg_free_period_ms();
                    CC_SESSION_UNLOCK(session);
                    if (b_confirmed_ms > 0) {
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        long long now_ms = (long long)ts.tv_sec * 1000 +
                                           ts.tv_nsec / 1000000;
                        long long elapsed = now_ms - b_confirmed_ms;
                        long long remaining = (long long)free_period - elapsed;
                        actual_delay = remaining > 0 ? (int)remaining : 0;
                    }
                }
                if (actual_delay > 0) {
                    pjsua_player_id tone_a = PJSUA_INVALID_ID;
                    pjsua_player_id tone_b = PJSUA_INVALID_ID;
                    const char *dial_tone_path = cc_prompt_get_path(CC_PROMPT_DIAL_TONE);
                    PJ_LOG(3, (THIS_FILE,
                               "[FREE-PERIOD] waiting %dms before bridge/charging; playing dial tone",
                               actual_delay));
                    if (cc_session_call_is_current(session, call_a, 1))
                        tone_a = cc_start_wav(call_a, dial_tone_path, PJ_TRUE);
                    if (cc_session_call_is_current(session, call_b, 0))
                        tone_b = cc_start_wav(call_b, dial_tone_path, PJ_TRUE);
                    cc_sleep_ms(actual_delay);
                    if (tone_a != PJSUA_INVALID_ID)
                        cc_stop_wav(tone_a, PJSUA_INVALID_ID);
                    if (tone_b != PJSUA_INVALID_ID)
                        cc_stop_wav(tone_b, PJSUA_INVALID_ID);
                } else {
                    PJ_LOG(3, (THIS_FILE,
                               "[FREE-PERIOD] already elapsed during prompt wait; bridging immediately"));
                }
            }
        }
    }

    /* Set call_connected_ts to when B started hearing the collect prompt
     * (non-whitelisted) or when B answered (whitelisted, no prompt plays).
     * This is independent of the free-period bridge delay. */
    CC_SESSION_LOCK(session);
    if (session->call_connected_ts == 0)
        session->call_connected_ts = session->b_prompt_start_ts > 0
                                     ? session->b_prompt_start_ts
                                     : time(NULL);
    CC_SESSION_UNLOCK(session);

    CC_SESSION_LOCK(session);
    {
        int stale_state = session->torn_down ||
                          !session->accepted ||
                          session->call_a != call_a ||
                          session->call_b != call_b;
        CC_SESSION_UNLOCK(session);
        if (stale_state) {
            PJ_LOG(3, (THIS_FILE,
                       "[TIMER] skipped stale action: accept state changed"));
            return;
        }
    }

    if (!cc_session_call_is_current(session, call_a, 1) ||
        !cc_session_call_is_current(session, call_b, 0))
    {
        PJ_LOG(3, (THIS_FILE,
                   "[TIMER] skipped stale action: accept media A=%d B=%d",
                   call_a, call_b));
        return;
    }

    /* 3. Stop A's waiting player (no-op if already stopped before dial tone) */
    CC_SESSION_LOCK(session);
    if (session->player_a != PJSUA_INVALID_ID) {
        player_a = session->player_a;
        session->player_a = PJSUA_INVALID_ID;
    }
    CC_SESSION_UNLOCK(session);

    if (player_a != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A waiting prompt"));
        cc_stop_wav(player_a, PJSUA_INVALID_ID);
    }

    /* 4. Media-change signalling: bridge locally or exit RTP path via SIP. */
    {
        cc_media_mode_t media_mode = cc_cfg_media_mode();

        if (media_mode == CC_MEDIA_MODE_LOCAL_BRIDGE) {
            /* Stay in RTP path — bridge A<->B in PJSUA conference. */
            PJ_LOG(3, (THIS_FILE,
                       "[MEDIA-MODE] local_bridge: bridging A<->B in PJSUA conference"));
            cc_bridge_calls(call_a, call_b);
        } else if (media_mode == CC_MEDIA_MODE_REINVITE) {
            /* Bridge locally first so audio is uninterrupted during re-INVITE
             * negotiation, then unbridge once both 200 OKs arrive. */
            PJ_LOG(3, (THIS_FILE,
                       "[MEDIA-MODE] reinvite: bridging locally then sending re-INVITEs"));
            cc_bridge_calls(call_a, call_b);
            leg_a_send_reinvite_bypass(session);
            leg_b_send_reinvite_bypass(session);
#if CC_REINVITE_UNBRIDGE_AFTER_SEND
            PJ_LOG(3, (THIS_FILE,
                       "[MEDIA-MODE] unbridging after re-INVITE send"));
            cc_unbridge_calls(call_a, call_b);
#endif
        } else {
            /* UPDATE mode: send SIP UPDATEs to redirect RTP directly A<->B.
             * Do NOT bridge locally — the B2BUA must exit the RTP path. */
            PJ_LOG(3, (THIS_FILE,
                       "[MEDIA-MODE] update: sending SIP UPDATEs to exit RTP path"));
            leg_a_send_update_bypass(call_a, session);
            leg_b_send_update_bypass(call_b, session);
        }
    }

}

typedef struct {
    cc_session_t   *session;
    pjsua_call_id   call_a;
    pjsua_call_id   call_b;
    int             delay_ms;
} accept_transition_arg_t;

static void *accept_transition_thread(void *opaque)
{
    accept_transition_arg_t *arg = (accept_transition_arg_t *)opaque;
    cc_session_t *session = arg->session;
    int delay_ms = arg->delay_ms;
    pj_thread_desc desc;
    pj_thread_t *this_thread = NULL;
    pj_status_t thread_status;

    pj_bzero(desc, sizeof(desc));
    thread_status = pj_thread_register("cc_accept", desc, &this_thread);
    if (thread_status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] accept transition PJ thread registration failed: %d",
                   thread_status));
        free(arg);
        cc_session_maybe_finalize(session);
        cc_session_release_reason(session, "accept-transition-register-failed");
        return NULL;
    }

    /*
     * Wait for collect prompt to finish OR free period to expire,
     * whichever comes first.
     *
     * - Prompt finishes first (prompt < free period): b_collect_done fires,
     *   we exit and the remaining free period is slept in run_accept_transition.
     * - Free period expires first (prompt > free period): we stop the prompt
     *   ourselves, set b_collect_done, and proceed to bridge immediately.
     */
    {
        long long b_confirmed_ms;
        int free_period;
        CC_SESSION_LOCK(session);
        b_confirmed_ms = session->b_confirmed_ms;
        free_period    = session->free_period_ms > 0
                         ? session->free_period_ms
                         : cc_cfg_free_period_ms();
        CC_SESSION_UNLOCK(session);

        int waited_ms = 0;
        for (;;) {
            int done;
            CC_SESSION_LOCK(session);
            done = session->b_collect_done || session->torn_down;
            CC_SESSION_UNLOCK(session);
            if (done) break;

            /* Check if free period has expired — stop prompt early if so */
            if (b_confirmed_ms > 0) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                long long now_ms = (long long)ts.tv_sec * 1000 +
                                   ts.tv_nsec / 1000000;
                if ((now_ms - b_confirmed_ms) >= free_period) {
                    pjsua_player_id player_b = PJSUA_INVALID_ID;
                    CC_SESSION_LOCK(session);
                    if (session->player_b != PJSUA_INVALID_ID) {
                        player_b = session->player_b;
                        session->player_b = PJSUA_INVALID_ID;
                    }
                    session->b_collect_done = 1;
                    CC_SESSION_UNLOCK(session);
                    if (player_b != PJSUA_INVALID_ID) {
                        PJ_LOG(3, (THIS_FILE,
                                   "[FREE-PERIOD] prompt cut at free period expiry; stopping player"));
                        cc_stop_wav(player_b, PJSUA_INVALID_ID);
                    }
                    break;
                }
            }

            cc_sleep_ms(50);
            waited_ms += 50;
            if (waited_ms % 1000 == 0)
                PJ_LOG(3, (THIS_FILE,
                           "[B] waiting for collect prompt to finish before bridge (%dms)",
                           waited_ms));
        }
        if (waited_ms > 0)
            PJ_LOG(3, (THIS_FILE,
                       "[B] collect prompt finished after %dms wait; proceeding to bridge",
                       waited_ms));
    }

    run_accept_transition(session, arg->call_a, arg->call_b, delay_ms);

    free(arg);
    cc_session_maybe_finalize(session);
    cc_session_release_reason(session, "accept-transition-complete");
    return NULL;
}

static int spawn_accept_transition(cc_session_t *session,
                                   pjsua_call_id call_a,
                                   pjsua_call_id call_b,
                                   int delay_ms)
{
    accept_transition_arg_t *arg;
    pthread_t thread;
    int rc;

    arg = malloc(sizeof(*arg));
    if (!arg ||
        !cc_session_acquire_reason(session, "accept-transition-worker"))
    {
        free(arg);
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] accept transition allocation/session retain failed"));
        return 0;
    }

    arg->session = session;
    arg->call_a = call_a;
    arg->call_b = call_b;
    arg->delay_ms = delay_ms;

    rc = pthread_create(&thread, NULL, accept_transition_thread, arg);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] accept transition pthread_create failed: %d",
                   rc));
        free(arg);
        cc_session_release_reason(session,
                                  "accept-transition-create-failed");
        return 0;
    }

    rc = pthread_detach(thread);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] accept transition pthread_detach failed: %d",
                   rc));
    }

    return 1;
}

static void on_accept(pjsua_call_id call_b, cc_session_t *session)
{
    char call_id[128];
    char completed_digit;
    pjsua_call_id call_a;
    int duplicate;
    int free_delay_ms = 0;

    CC_SESSION_LOCK(session);
    completed_digit = session->decision_digit;
    duplicate = session->decision_completed ||
                session->accepted ||
                session->torn_down;
    if (duplicate || session->call_b != call_b) {
        int current_call = session->call_b == call_b;
        CC_SESSION_UNLOCK(session);

        if (current_call) {
            PJ_LOG(3, (THIS_FILE,
                       "[DTMF] duplicate digit ignored; decision already completed=%s",
                       decision_name(completed_digit)));
        } else {
            PJ_LOG(3, (THIS_FILE,
                       "[DTMF] stale B-leg digit ignored call_id=%d",
                       call_b));
        }
        return;
    }

    session->decision_completed = 1;
    session->decision_digit = CC_DTMF_ACCEPT;
    session->accepted = 1;

    /*
     * Compute free-period delay: if B accepts before the free period
     * expires, defer call_connected_ts and bridge until it does.
     * A keeps hearing the waiting prompt during this delay.
     */
    if (session->b_confirmed_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        long long elapsed = now_ms - session->b_confirmed_ms;
        int free_period = cc_cfg_free_period_ms();
        if (elapsed < free_period)
            free_delay_ms = (int)(free_period - elapsed);
    }

    /* Defer call_connected_ts — will be set after free period in transition */
    snprintf(call_id, sizeof(call_id), "%s", session->call_id);
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    if (free_delay_ms > 0) {
        PJ_LOG(3, (THIS_FILE,
                   "[FREE-PERIOD] B accepted early; delaying bridge by %dms",
                   free_delay_ms));
    }

    PJ_LOG(3, (THIS_FILE, "[CALL-CONNECTED] callId=%s", call_id));
    PJ_LOG(3, (THIS_FILE,
               "[DTMF] accept media transition queued outside callback"));

    if (!spawn_accept_transition(session, call_a, call_b, free_delay_ms)) {
        PJ_LOG(2, (THIS_FILE,
                   "[DTMF] accept worker unavailable; running immediate fallback"));
        run_accept_transition(session, call_a, call_b, free_delay_ms);
    }
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

typedef struct {
    cc_session_t *session;
    int           timeout_sec;
    int           is_ring;    /* 1 = ring timer, 0 = DTMF timer */
    pjsua_call_id expected_call_b;
} timer_arg_t;

static void *timer_thread(void *arg)
{
    timer_arg_t  *ta = (timer_arg_t *)arg;
    cc_session_t *s  = ta->session;
    pj_thread_desc desc;
    pj_thread_t *this_thread = NULL;
    pj_status_t thread_status;

    pj_bzero(desc, sizeof(desc));
    thread_status = pj_thread_register("cc_timer", desc, &this_thread);
    if (thread_status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] timer PJ thread registration failed: %d",
                   thread_status));
        free(ta);
        cc_session_maybe_finalize(s);
        cc_session_release_reason(s, "timer-register-failed");
        return NULL;
    }

    int           t  = ta->timeout_sec;
    int           is_ring = ta->is_ring;
    pjsua_call_id call_b = ta->expected_call_b;
    int remaining_ms = t * 1000;
    int done = 0;
    free(ta);

    /*
     * Wake periodically so accepted/completed calls release their timer
     * references promptly instead of retaining the session for 30/60 seconds.
     */
    while (remaining_ms > 0) {
        int slice_ms = remaining_ms > 100 ? 100 : remaining_ms;

        cc_sleep_ms(slice_ms);
        remaining_ms -= slice_ms;

        CC_SESSION_LOCK(s);
        done = s->accepted || s->torn_down ||
               s->call_b != call_b ||
               s->final_cleanup_started;
        CC_SESSION_UNLOCK(s);

        if (done)
            break;
    }

    CC_SESSION_LOCK(s);
    done = s->accepted || s->torn_down ||
           s->call_b != call_b ||
           s->final_cleanup_started;
    CC_SESSION_UNLOCK(s);

    if (!done && cc_session_call_is_current(s, call_b, 0)) {
        if (is_ring) {
            PJ_LOG(2, (THIS_FILE, "[B] Ring timeout (%d s) — unavailable", t));
            CC_SESSION_LOCK(s);
            if (!s->accepted && !s->torn_down && s->call_b == call_b) {
                s->decision_completed = 1;
                s->decision_digit = '\0';
                s->torn_down = 1;
            } else {
                done = 1;
            }
            CC_SESSION_UNLOCK(s);
            if (!done) {
                cc_session_mark_end(s, "FAILED", "NO_ANSWER");
                if (cc_session_call_is_current(s, call_b, 0))
                    cc_safe_hangup(call_b, PJSIP_SC_REQUEST_TIMEOUT);
                else
                    PJ_LOG(3, (THIS_FILE,
                               "[TIMER] skipped stale action call=%d", call_b));
                leg_a_play_prompt_then_hangup(s, CC_PROMPT_NOT_AVAILABLE_TO_PAY,
                                             PJSIP_SC_TEMPORARILY_UNAVAILABLE);
            }
        } else {
            PJ_LOG(2, (THIS_FILE, "[B] DTMF timeout (%d s) — reject", t));
            on_reject_mapped(call_b,
                             s,
                             "FAILED",
                             "ELIGIBILITY_TIMEOUT",
                             '\0',
                             CC_PROMPT_NOT_AVAILABLE_TO_PAY);
        }
    } else {
        PJ_LOG(3, (THIS_FILE,
                   "[TIMER] skipped stale action call=%d type=%s",
                   call_b, is_ring ? "ring" : "dtmf"));
    }

    cc_session_maybe_finalize(s);
    cc_session_release_reason(s, is_ring ? "ring-timer-complete"
                                        : "dtmf-timer-complete");
    return NULL;
}

static void spawn_timer(cc_session_t *session, int timeout_sec, int is_ring)
{
    timer_arg_t *arg;
    pjsua_call_id call_b;
    pthread_t t;
    int rc;

    CC_SESSION_LOCK(session);
    if (session->torn_down ||
        session->call_b == PJSUA_INVALID_ID ||
        (is_ring ? session->ring_timer_started
                 : session->dtmf_timer_started))
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[TIMER] duplicate/stale timer start skipped type=%s",
                   is_ring ? "ring" : "dtmf"));
        return;
    }

    if (is_ring)
        session->ring_timer_started = 1;
    else
        session->dtmf_timer_started = 1;
    call_b = session->call_b;
    CC_SESSION_UNLOCK(session);

    arg = malloc(sizeof(*arg));
    if (!arg || !cc_session_acquire_reason(session,
                                            is_ring ? "ring-timer"
                                                    : "dtmf-timer")) {
        free(arg);
        CC_SESSION_LOCK(session);
        if (is_ring)
            session->ring_timer_started = 0;
        else
            session->dtmf_timer_started = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] timer allocation/session retain failed type=%s",
                   is_ring ? "ring" : "dtmf"));
        return;
    }

    arg->session     = session;
    arg->timeout_sec = timeout_sec;
    arg->is_ring     = is_ring;
    arg->expected_call_b = call_b;

    rc = pthread_create(&t, NULL, timer_thread, arg);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] timer pthread_create failed type=%s status=%d",
                   is_ring ? "ring" : "dtmf", rc));
        free(arg);
        CC_SESSION_LOCK(session);
        if (is_ring)
            session->ring_timer_started = 0;
        else
            session->dtmf_timer_started = 0;
        CC_SESSION_UNLOCK(session);
        cc_session_release_reason(session,
                                  is_ring ? "ring-timer-create-failed"
                                          : "dtmf-timer-create-failed");
        return;
    }

    rc = pthread_detach(t);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] timer pthread_detach failed type=%s status=%d",
                   is_ring ? "ring" : "dtmf", rc));
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
