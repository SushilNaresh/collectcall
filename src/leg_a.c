/*
 * leg_a.c — Leg-A (inbound / calling party) handlers
 *
 * A's leg is answered first. B-leg is started only after A-leg reaches
 * CONFIRMED state, which means A has sent ACK for 200 OK.
 * Blocking work (WAV+hangup, MCA wait) posted to worker pool.
 */
#include "handlers.h"
#include "b2bua.h"
#include "utils.h"
#include "config.h"
#include "prompt_mapping.h"
#include "runtime_config.h"
#include "worker.h"

#include <pjsua-lib/pjsua.h>
#include <pjsip/sip_util.h>
#include <pjmedia/sdp.h>
#include <pjmedia/wav_port.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define THIS_FILE "leg_a.c"

/* Return WAV player duration in ms (from pjmedia port info). Fallback 4000ms. */
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

/*
 * Atomically take the waiting-prompt player and compute remaining play time.
 * Does NOT sleep — the caller (worker event) does the deferral sleep.
 * Returns the player id (PJSUA_INVALID_ID if none) and sets *remaining_ms_out.
 */
static pjsua_player_id take_a_waiting_prompt(cc_session_t *session,
                                              int *remaining_ms_out,
                                              const char *reason)
{
    pjsua_player_id player_a = PJSUA_INVALID_ID;
    int remaining_ms = 0;

    if (!session) {
        *remaining_ms_out = 0;
        return PJSUA_INVALID_ID;
    }

    CC_SESSION_LOCK(session);
    if (session->player_a != PJSUA_INVALID_ID) {
        player_a = session->player_a;
        session->player_a = PJSUA_INVALID_ID;

        if (session->a_prompt_duration_ms > 0 && session->a_confirmed_ms > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            long long now_ms = (long long)ts.tv_sec * 1000 +
                               ts.tv_nsec / 1000000;
            long long elapsed = now_ms - session->a_confirmed_ms;
            long long total   = session->a_prompt_duration_ms;
            if (elapsed < total)
                remaining_ms = (int)(total - elapsed);
        }
    }
    CC_SESSION_UNLOCK(session);

    if (player_a != PJSUA_INVALID_ID && remaining_ms > 0)
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] A waiting prompt still playing; waiting %dms before treatment: %s",
                   remaining_ms, reason ? reason : "unknown"));

    *remaining_ms_out = remaining_ms;
    return player_a;
}

/* ── State callback ──────────────────────────────────────────────────────── */

pjsua_call_id leg_a_on_call_state(pjsua_call_id call_id,
                                  cc_session_t *session)
{
    pjsua_call_id deferred_hangup = PJSUA_INVALID_ID;
    pjsua_call_info ci;
    pj_status_t status = pjsua_call_get_info(call_id, &ci);
    if (status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] A call info failed call=%d status=%d",
                   call_id, status));
        return PJSUA_INVALID_ID;
    }

    PJ_LOG(3, (THIS_FILE, "[A] call_id=%d state=%.*s reason=%.*s",
               call_id,
               (int)ci.state_text.slen, ci.state_text.ptr,
               (int)ci.last_status_text.slen, ci.last_status_text.ptr));

    if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        /*
         * A-leg is now complete: INVITE -> 200 OK -> ACK.
         * Record the confirmed timestamp for free-period calculation.
         * Start "please wait" prompt immediately so caller hears audio
         * while the validation API runs (which blocks).
         */
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            CC_SESSION_LOCK(session);
            session->a_confirmed_ms = (long long)ts.tv_sec * 1000 +
                                      ts.tv_nsec / 1000000;
            CC_SESSION_UNLOCK(session);
        }

        leg_a_on_media_state(call_id, session);

        deferred_hangup = cc_start_b_leg_after_a_confirmed(session);
    }

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        pjsua_player_id player_a = PJSUA_INVALID_ID;
        pjsua_call_id b;
        int already_accepted;
        int start_teardown = 0;

        CC_SESSION_LOCK(session);
        if (session->player_a != PJSUA_INVALID_ID) {
            player_a = session->player_a;
            session->player_a = PJSUA_INVALID_ID;
        }
        session->a_prompt_starting = 0;
        session->a_on_hold = 0;
        {
            pjsua_player_id hold_pid = session->hold_player_b;
            session->hold_player_b = PJSUA_INVALID_ID;
            if (hold_pid != PJSUA_INVALID_ID) {
                CC_SESSION_UNLOCK(session);
                PJ_LOG(3, (THIS_FILE, "[VOICE] Stop B hold MOH on A disconnect"));
                cc_stop_wav(hold_pid, PJSUA_INVALID_ID);
                CC_SESSION_LOCK(session);
            }
        }
        already_accepted = session->accepted;
        b = session->call_b;

        if (!session->torn_down) {
            session->torn_down = 1;
            start_teardown = 1;
        }
        CC_SESSION_UNLOCK(session);

        if (player_a != PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A waiting prompt"));
            cc_stop_wav(player_a, PJSUA_INVALID_ID);
        }

        cc_session_invalidate_a(session, call_id);

        if (start_teardown) {
            if (already_accepted)
                cc_session_mark_end(session, "COMPLETED", "NORMAL_CLEARING");
            else
                cc_session_mark_end(session, "CANCELLED", "USER_ABANDONED");

            if (cc_session_call_is_current(session, b, 0))
                cc_safe_hangup(b, PJSIP_SC_OK);
        }
    }

    return deferred_hangup;
}

/* ── Media state callback ────────────────────────────────────────────────── */

void leg_a_on_media_state(pjsua_call_id call_id, cc_session_t *session)
{
    pjsua_call_info          ci;
    pjsua_call_media_info   *mi;
    pjmedia_sdp_session     *sdp = NULL;
    cc_rtp_ep_t              ep;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] A media call info failed call=%d", call_id));
        return;
    }

    if (ci.media_cnt == 0 ||
        ci.media[0].status != PJSUA_CALL_MEDIA_ACTIVE)
        return;

    mi = &ci.media[0];

    /*
     * This PJSUA build does not expose media session access here.
     * RTP endpoints are learned later from transport info before UPDATE.
     */
    (void)sdp;
    (void)ep;

    cc_log_call_rtp_info(call_id, "A");

    CC_SESSION_LOCK(session);
    if (session->accepted || session->torn_down ||
        session->player_a != PJSUA_INVALID_ID ||
        session->a_prompt_starting)
    {
        int accepted = session->accepted;
        int torn_down = session->torn_down;
        CC_SESSION_UNLOCK(session);

        if (accepted)
            PJ_LOG(3, (THIS_FILE, "[A] media update ignored after accept; not restarting waiting WAV"));
        else if (torn_down)
            PJ_LOG(3, (THIS_FILE, "[A] media update ignored after teardown; not restarting waiting WAV"));
        else
            PJ_LOG(3, (THIS_FILE, "[VOICE] A waiting prompt already active/starting, skip"));
        return;
    }
    session->a_prompt_starting = 1;
    CC_SESSION_UNLOCK(session);

    {
        cc_prompt_tag_t a_prompt = session->fundless
                                  ? CC_PROMPT_FUNDLESS
                                  : CC_PROMPT_WAITING;
        const char *waiting_path = cc_prompt_get_path(a_prompt);
        pjsua_player_id pid;
        int keep_player = 0;

        PJ_LOG(3, (THIS_FILE, "[VOICE] Start A waiting prompt: %s", waiting_path));
        pid = cc_start_wav(call_id, waiting_path, PJ_FALSE);

        CC_SESSION_LOCK(session);
        if (pid != PJSUA_INVALID_ID &&
            !session->accepted &&
            !session->torn_down &&
            session->call_a == call_id &&
            session->player_a == PJSUA_INVALID_ID)
        {
            session->player_a = pid;
            keep_player = 1;
        }
        session->a_prompt_starting = 0;
        CC_SESSION_UNLOCK(session);

        if (keep_player) {
            int wait_ms = cc_player_duration_ms(pid);
            PJ_LOG(3, (THIS_FILE,
                       "[A] Waiting WAV started (one-shot) player=%d duration=%dms",
                       pid, wait_ms));
            CC_SESSION_LOCK(session);
            session->a_prompt_done = 0;
            session->a_prompt_duration_ms = wait_ms;
            CC_SESSION_UNLOCK(session);
        } else if (pid != PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] A waiting prompt became stale, destroying player=%d",
                       pid));
            cc_stop_wav(pid, PJSUA_INVALID_ID);
        }
    }

    (void)mi;
}

/* ── Answer 200 OK ───────────────────────────────────────────────────────── */

void leg_a_answer_200(pjsua_call_id call_id)
{
    pjsua_call_setting cs;
    pjsua_call_setting_default(&cs);

    pj_status_t status = pjsua_call_answer2(call_id, &cs,
                                             PJSIP_SC_OK, NULL, NULL);
    if (status == PJ_SUCCESS)
        PJ_LOG(3, (THIS_FILE, "[A] 200 OK sent"));
    else
        PJ_LOG(1, (THIS_FILE, "[A] answer 200 failed: %d", status));
}

/* ── SIP UPDATE to exit RTP path ─────────────────────────────────────────── */

void leg_a_send_update_bypass(pjsua_call_id call_id, cc_session_t *session)
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
	 * A-leg UPDATE SDP must advertise B-party RTP IP/port.
	 */
        CC_SESSION_LOCK(session);
        call_a = session->call_a;
        call_b = session->call_b;
        CC_SESSION_UNLOCK(session);

        if (call_a == PJSUA_INVALID_ID || call_b == PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE,
                       "[TIMER] skipped stale action: A UPDATE"));
            return;
        }

        /* B-leg must be CONFIRMED before we redirect A's RTP to it */
        {
            pjsua_call_info ci_b;
            if (pjsua_call_get_info(call_b, &ci_b) != PJ_SUCCESS ||
                ci_b.state != PJSIP_INV_STATE_CONFIRMED) {
                PJ_LOG(1, (THIS_FILE,
                           "[A] UPDATE skipped: B-leg not CONFIRMED (call_b=%d)",
                           call_b));
                return;
            }
        }

        /* Wait up to 5s for both:
         *   1. SBC re-INVITE on B-leg to complete (b_reinvite_active == 0)
         *   2. B's src_rtp_name to be valid (actual RTP packets received)
         *
         * Trace analysis showed the SBC sends a re-INVITE after 200 OK
         * (late-offer pattern) which moves B's media to a NEW MGW IP.
         * The 183 early-media endpoint and the post-re-INVITE endpoint are
         * on different MGW IPs (e.g. .92 vs .91). Polling src_rtp_name
         * alone is insufficient — it may return the stale 183 address if
         * polled before the re-INVITE sendrecv completes and RTP restarts
         * from the new MGW. Both conditions must be true simultaneously. */
        {
            int wait_ms = 0;
            int torn = 0;
            for (; wait_ms < 5000; wait_ms += 50) {
                int ri_active;
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
                       "[A] B RTP endpoint after %dms wait: %s:%d",
                       wait_ms, rtp_b.ip, rtp_b.port));
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
	        session->update_a_pending = 1;
            }
	    CC_SESSION_UNLOCK(session);

	    PJ_LOG(3, (THIS_FILE,
	               "[A] UPDATE rewrite armed: A will receive B RTP %s:%d",
	               rtp_b.ip, rtp_b.port));
	} else {
	    PJ_LOG(1, (THIS_FILE,
	               "[A] Cannot arm UPDATE rewrite: RTP endpoints not ready"));
	}

    pjsua_msg_data_init(&msg_data);

    PJ_LOG(3, (THIS_FILE, "[A] Sending basic SIP UPDATE"));

    /* Retry up to 2s if dialog has a pending transaction (e.g. INVITE retransmit) */
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
        session->update_a_sent = 1;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[A] SIP UPDATE sent"));
    } else {
        CC_SESSION_LOCK(session);
        session->update_a_pending = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE, "[A] SIP UPDATE failed: %d", status));
    }
}

/* ── Play WAV then hangup (runs in thread) ───────────────────────────────── */

/* SIP re-INVITE to test RTP bypass */
void leg_a_send_reinvite_bypass(cc_session_t *session)
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

    PJ_LOG(3, (THIS_FILE, "[REINVITE] Preparing A-leg re-INVITE"));

    if (call_a == PJSUA_INVALID_ID || call_b == PJSUA_INVALID_ID) {
        PJ_LOG(1, (THIS_FILE,
                   "[REINVITE] A-leg re-INVITE skipped: invalid call ids A=%d B=%d",
                   call_a, call_b));
        return;
    }

    if (cc_get_call_remote_rtp(call_a, &rtp_a) != PJ_SUCCESS ||
        cc_get_call_remote_rtp(call_b, &rtp_b) != PJ_SUCCESS)
    {
        PJ_LOG(1, (THIS_FILE,
                   "[REINVITE] A-leg re-INVITE skipped: RTP endpoints not ready"));
        return;
    }

    CC_SESSION_LOCK(session);
    if (session->call_a == call_a &&
        session->call_b == call_b &&
        !session->torn_down)
    {
        session->rtp_a = rtp_a;
        session->rtp_b = rtp_b;
        session->reinvite_a_pending = 1;
    }
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[REINVITE] A-leg SDP target B RTP %s:%d",
               rtp_b.ip, rtp_b.port));

    pjsua_msg_data_init(&msg_data);

    status = pjsua_call_reinvite(call_a, 0, &msg_data);

    if (status == PJ_SUCCESS) {
        PJ_LOG(3, (THIS_FILE, "[REINVITE] A-leg re-INVITE sent"));
    } else {
        CC_SESSION_LOCK(session);
        session->reinvite_a_pending = 0;
        CC_SESSION_UNLOCK(session);

        PJ_LOG(1, (THIS_FILE,
                   "[REINVITE] A-leg re-INVITE failed: %d",
                   status));
    }
}

static void spawn_wav_hangup(cc_session_t *session,
                              const char *wav_path,
                              pjsip_status_code code,
                              pjsua_player_id player_a,
                              int wait_ms)
{
    cc_event_t ev;
    pjsua_call_id call_a;

    CC_SESSION_LOCK(session);
    if (session->a_treatment_running ||
        session->call_a == PJSUA_INVALID_ID)
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[WAV] skipped stale treatment"));
        if (player_a != PJSUA_INVALID_ID)
            cc_stop_wav(player_a, PJSUA_INVALID_ID);
        return;
    }
    session->a_treatment_running = 1;
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type       = CC_EV_WAV_HANGUP_A;
    ev.session    = session;
    ev.call_a     = call_a;
    ev.wav_path   = wav_path;
    ev.sip_code   = (int)code;
    ev.player_a   = player_a;
    ev.wait_ms    = wait_ms;
    snprintf(ev.reason, sizeof(ev.reason), "wav-treatment-worker");

    /* wait_ms > 0: deferral via timer queue (1 timer thread, no per-call threads)
     * wait_ms == 0: post directly to worker pool */
    if (cc_worker_post_delayed(&ev, wait_ms) != 0) {
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        CC_SESSION_UNLOCK(session);
        if (player_a != PJSUA_INVALID_ID)
            cc_stop_wav(player_a, PJSUA_INVALID_ID);
        PJ_LOG(1, (THIS_FILE, "[ERROR] WAV treatment worker post failed"));
    }
}

void leg_a_play_rejected_then_hangup(cc_session_t *session)
{
    int wait_ms = 0;
    pjsua_player_id player_a = take_a_waiting_prompt(session, &wait_ms, "rejected");
    const char *rejected_path = cc_prompt_get_path(CC_PROMPT_REJECTED);
    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A rejected prompt then hangup: %s",
               rejected_path));
    spawn_wav_hangup(session, rejected_path, PJSIP_SC_DECLINE, player_a, wait_ms);
}

void leg_a_play_unavailable_then_hangup(cc_session_t *session)
{
    int wait_ms = 0;
    pjsua_player_id player_a = take_a_waiting_prompt(session, &wait_ms, "unavailable");
    const char *unavailable_path = cc_prompt_get_path(CC_PROMPT_UNAVAILABLE);
    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A unavailable prompt then hangup: %s",
               unavailable_path));
    spawn_wav_hangup(session, unavailable_path,
                     PJSIP_SC_TEMPORARILY_UNAVAILABLE, player_a, wait_ms);
}

void leg_a_play_prompt_then_hangup(cc_session_t *session,
                                   cc_prompt_tag_t tag,
                                   pjsip_status_code code)
{
    int wait_ms = 0;
    const char *tag_name = cc_prompt_tag_name(tag);
    pjsua_player_id player_a = take_a_waiting_prompt(session, &wait_ms, tag_name);
    const char *path = cc_prompt_get_path(tag);
    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A prompt=%s then hangup: %s",
               tag_name, path));
    spawn_wav_hangup(session, path, code, player_a, wait_ms);
}

/* ── MCA flow: play UNAVAILABLE, wait for A DTMF 1, then MCA API ─────────── */

void leg_a_play_mca_wait(cc_session_t *session, cc_prompt_tag_t prompt_tag)
{
    cc_event_t ev;
    pjsua_call_id call_a;
    int wait_ms = 0;
    pjsua_player_id player_a = take_a_waiting_prompt(session, &wait_ms, "mca-wait");

    /* Stop the waiting prompt now (no sleep — MCA wait loop handles timing) */
    if (player_a != PJSUA_INVALID_ID) {
        if (wait_ms > 0)
            cc_sleep_ms(wait_ms);
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A waiting prompt before treatment: mca-wait"));
        cc_stop_wav(player_a, PJSUA_INVALID_ID);
    }

    CC_SESSION_LOCK(session);
    if (session->a_treatment_running ||
        session->call_a == PJSUA_INVALID_ID)
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[MCA] skipped: treatment running or no A-leg"));
        return;
    }
    session->a_treatment_running = 1;
    session->mca_waiting = 1;
    session->mca_decided = 0;
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type       = CC_EV_MCA_WAIT;
    ev.session    = session;
    ev.call_a     = call_a;
    ev.prompt_tag = (int)prompt_tag;
    snprintf(ev.reason, sizeof(ev.reason), "mca-wait-worker");

    if (cc_worker_post(&ev) != 0) {
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        session->mca_waiting = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE, "[ERROR] MCA wait worker post failed"));
    }
}

/* ── A-leg DTMF handler for MCA decision ─────────────────────────────────── */

void leg_a_on_dtmf_mca(pjsua_call_id call_id, int digit, cc_session_t *session)
{
    (void)call_id;

    CC_SESSION_LOCK(session);
    if (session->mca_decided || !session->mca_waiting) {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[DTMF] A-leg digit=%c MCA duplicate/late — ignored",
                   (char)digit));
        return;
    }

    if ((char)digit == CC_DTMF_ACCEPT) {
        session->mca_decided = 1;  /* 1 = send MCA */
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[DTMF] A-leg digit=1 — MCA accepted"));
    } else {
        session->mca_decided = 2;  /* 2 = don't send MCA */
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[DTMF] A-leg digit=%c — MCA declined",
                   (char)digit));
    }
}
