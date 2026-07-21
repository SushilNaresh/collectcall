/*
 * leg_a.c — Leg-A (inbound / calling party) handlers
 *
 * A's leg is answered first. B-leg is started only after A-leg reaches
 * CONFIRMED state, which means A has sent ACK for 200 OK.
 */
#include "handlers.h"
#include "b2bua.h"
#include "utils.h"
#include "config.h"
#include "prompt_mapping.h"
#include "runtime_config.h"

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

    /* Add small buffer for safety */
    return duration_ms > 0 ? duration_ms + 500 : 4000;
}

static void stop_a_waiting_prompt_before_treatment(cc_session_t *session,
                                                   const char *reason)
{
    pjsua_player_id player_a = PJSUA_INVALID_ID;
    int remaining_ms = 0;

    if (!session)
        return;

    CC_SESSION_LOCK(session);
    if (session->player_a != PJSUA_INVALID_ID) {
        player_a = session->player_a;
        session->player_a = PJSUA_INVALID_ID;

        /* Compute how much of the waiting prompt is still left to play */
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

    if (player_a != PJSUA_INVALID_ID) {
        if (remaining_ms > 0) {
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] A waiting prompt still playing; waiting %dms before treatment: %s",
                       remaining_ms, reason ? reason : "unknown"));
            cc_sleep_ms(remaining_ms);
        }
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] Stop A waiting prompt before treatment: %s",
                   reason ? reason : "unknown"));
        cc_stop_wav(player_a, PJSUA_INVALID_ID);
    }
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

        cc_session_maybe_finalize(session);
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

    status = pjsua_call_update(call_id, 0, &msg_data);

    if (status == PJ_SUCCESS)
        PJ_LOG(3, (THIS_FILE, "[A] SIP UPDATE sent"));
    else
        PJ_LOG(1, (THIS_FILE, "[A] SIP UPDATE failed: %d", status));
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

typedef struct {
    cc_session_t       *session;
    const char         *wav_path;
    pjsip_status_code   code;
    pjsua_call_id       expected_call_a;
} wav_hangup_arg_t;

static void *wav_then_hangup_thread(void *arg)
{
    wav_hangup_arg_t *a = (wav_hangup_arg_t *)arg;
    cc_session_t     *s = a->session;
    pj_thread_desc desc;
    pj_thread_t *this_thread = NULL;
    pj_status_t thread_status;

    pj_bzero(desc, sizeof(desc));
    thread_status = pj_thread_register("cc_wav_hang", desc, &this_thread);
    if (thread_status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] WAV PJ thread registration failed: %d",
                   thread_status));
        CC_SESSION_LOCK(s);
        s->a_treatment_running = 0;
        CC_SESSION_UNLOCK(s);
        free(a);
        cc_session_maybe_finalize(s);
        cc_session_release_reason(s, "wav-register-failed");
        return NULL;
    }

    pjsua_call_id     call_a = a->expected_call_a;

    if (cc_session_call_is_current(s, call_a, 1)) {
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] Play A treatment prompt then hangup: %s",
                   a->wav_path));
        pjsua_player_id pid = cc_start_wav(call_a, a->wav_path, PJ_FALSE);
        int wait_ms = cc_player_duration_ms(pid);
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] prompt duration wait=%dms", wait_ms));
        cc_sleep_ms(wait_ms);
        cc_stop_wav(pid, PJSUA_INVALID_ID);

        if (cc_session_call_is_current(s, call_a, 1)) {
            cc_safe_hangup(call_a, a->code);
        } else {
            PJ_LOG(3, (THIS_FILE,
                       "[WAV] skipped stale treatment call=%d", call_a));
        }
    } else {
        PJ_LOG(3, (THIS_FILE,
                   "[WAV] skipped stale treatment call=%d", call_a));
    }

    CC_SESSION_LOCK(s);
    s->a_treatment_running = 0;
    CC_SESSION_UNLOCK(s);
    free(a);
    cc_session_maybe_finalize(s);
    cc_session_release_reason(s, "wav-treatment-complete");
    return NULL;
}

static void spawn_wav_hangup(cc_session_t *session,
                              const char *wav_path,
                              pjsip_status_code code)
{
    wav_hangup_arg_t *arg = malloc(sizeof(*arg));
    pjsua_call_id call_a;
    pthread_t t;
    int rc;

    if (!arg) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] WAV treatment allocation failed"));
        return;
    }

    CC_SESSION_LOCK(session);
    if (session->a_treatment_running ||
        session->call_a == PJSUA_INVALID_ID)
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[WAV] skipped stale treatment"));
        free(arg);
        return;
    }
    session->a_treatment_running = 1;
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    if (!cc_session_acquire_reason(session, "wav-treatment-worker")) {
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[WAV] skipped stale treatment"));
        free(arg);
        return;
    }

    arg->session  = session;
    arg->wav_path = wav_path;
    arg->code     = code;
    arg->expected_call_a = call_a;

    rc = pthread_create(&t, NULL, wav_then_hangup_thread, arg);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] WAV treatment pthread_create failed: %d", rc));
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        CC_SESSION_UNLOCK(session);
        free(arg);
        cc_session_release_reason(session, "wav-create-failed");
        return;
    }

    rc = pthread_detach(t);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] WAV treatment pthread_detach failed: %d", rc));
    }
}

void leg_a_play_rejected_then_hangup(cc_session_t *session)
{
    const char *rejected_path;

    stop_a_waiting_prompt_before_treatment(session, "rejected");

    rejected_path = cc_prompt_get_path(CC_PROMPT_REJECTED);

    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A rejected prompt then hangup: %s",
               rejected_path));
    spawn_wav_hangup(session, rejected_path, PJSIP_SC_DECLINE);
}

void leg_a_play_unavailable_then_hangup(cc_session_t *session)
{
    const char *unavailable_path;

    stop_a_waiting_prompt_before_treatment(session, "unavailable");

    unavailable_path = cc_prompt_get_path(CC_PROMPT_UNAVAILABLE);

    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A unavailable prompt then hangup: %s",
               unavailable_path));
    spawn_wav_hangup(session, unavailable_path,
                     PJSIP_SC_TEMPORARILY_UNAVAILABLE);
}

void leg_a_play_prompt_then_hangup(cc_session_t *session,
                                   cc_prompt_tag_t tag,
                                   pjsip_status_code code)
{
    const char *path;
    const char *tag_name = cc_prompt_tag_name(tag);

    stop_a_waiting_prompt_before_treatment(session, tag_name);

    path = cc_prompt_get_path(tag);

    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A prompt=%s then hangup: %s",
               tag_name, path));
    spawn_wav_hangup(session, path, code);
}

/* ── MCA flow: play UNAVAILABLE, wait for A DTMF 1, then MCA API ─────────── */

#define CC_MCA_DTMF_TIMEOUT_SEC 10

typedef struct {
    cc_session_t   *session;
    pjsua_call_id   expected_call_a;
    cc_prompt_tag_t prompt_tag;
} mca_wait_arg_t;

static void *mca_wait_thread(void *arg)
{
    mca_wait_arg_t *a = (mca_wait_arg_t *)arg;
    cc_session_t   *s = a->session;
    pjsua_call_id   call_a = a->expected_call_a;
    cc_prompt_tag_t prompt_tag = a->prompt_tag;
    pj_thread_desc desc;
    pj_thread_t *this_thread = NULL;
    pj_status_t thread_status;

    pj_bzero(desc, sizeof(desc));
    thread_status = pj_thread_register("cc_mca_wait", desc, &this_thread);
    if (thread_status != PJ_SUCCESS) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] MCA wait PJ thread registration failed: %d",
                   thread_status));
        CC_SESSION_LOCK(s);
        s->a_treatment_running = 0;
        s->mca_waiting = 0;
        CC_SESSION_UNLOCK(s);
        free(a);
        cc_session_maybe_finalize(s);
        cc_session_release_reason(s, "mca-wait-register-failed");
        return NULL;
    }

    free(a);

    if (!cc_session_call_is_current(s, call_a, 1)) {
        PJ_LOG(3, (THIS_FILE, "[MCA] skipped stale call=%d", call_a));
        CC_SESSION_LOCK(s);
        s->a_treatment_running = 0;
        s->mca_waiting = 0;
        CC_SESSION_UNLOCK(s);
        cc_session_maybe_finalize(s);
        cc_session_release_reason(s, "mca-wait-stale");
        return NULL;
    }

    /* Play the specified prompt (loop) so A hears it while waiting for DTMF */
    const char *prompt_path = cc_prompt_get_path(prompt_tag);
    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A prompt (loop) for MCA wait: %s",
               prompt_path));
    pjsua_player_id pid = cc_start_wav(call_a, prompt_path, PJ_FALSE);

    CC_SESSION_LOCK(s);
    if (pid != PJSUA_INVALID_ID &&
        s->call_a == call_a &&
        s->player_a == PJSUA_INVALID_ID)
    {
        s->player_a = pid;
    }
    CC_SESSION_UNLOCK(s);

    /* Wait for DTMF 1 with timeout */
    int remaining_ms = CC_MCA_DTMF_TIMEOUT_SEC * 1000;
    int decided = 0;

    while (remaining_ms > 0) {
        int slice_ms = remaining_ms > 100 ? 100 : remaining_ms;
        cc_sleep_ms(slice_ms);
        remaining_ms -= slice_ms;

        CC_SESSION_LOCK(s);
        decided = s->mca_decided;
        CC_SESSION_UNLOCK(s);

        if (decided)
            break;

        if (!cc_session_call_is_current(s, call_a, 1)) {
            decided = -1;
            break;
        }
    }

    /* Stop the looping prompt */
    pjsua_player_id player_a = PJSUA_INVALID_ID;
    CC_SESSION_LOCK(s);
    if (s->player_a != PJSUA_INVALID_ID) {
        player_a = s->player_a;
        s->player_a = PJSUA_INVALID_ID;
    }
    s->mca_waiting = 0;
    CC_SESSION_UNLOCK(s);

    if (player_a != PJSUA_INVALID_ID)
        cc_stop_wav(player_a, PJSUA_INVALID_ID);

    if (decided == 1) {
        /* A pressed 1 — send MCA via end-call API */
        PJ_LOG(3, (THIS_FILE, "[MCA] A pressed 1 — sending MCA notification"));
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_MCA");

        if (cc_session_call_is_current(s, call_a, 1)) {
            const char *mca_path = cc_prompt_get_path(CC_PROMPT_MCA_SENT);
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] Play A MCA_SENT prompt: %s", mca_path));
            pjsua_player_id mca_pid = cc_start_wav(call_a, mca_path, PJ_FALSE);
            int mca_wait_ms = cc_player_duration_ms(mca_pid);
            int mca_cap = cc_cfg_free_period_ms();
            if (mca_wait_ms > mca_cap)
                mca_wait_ms = mca_cap;
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] MCA_SENT prompt duration wait=%dms", mca_wait_ms));
            cc_sleep_ms(mca_wait_ms);
            cc_stop_wav(mca_pid, PJSUA_INVALID_ID);

            if (cc_session_call_is_current(s, call_a, 1))
                cc_safe_hangup(call_a, PJSIP_SC_OK);
        }
    } else if (decided == 2) {
        /* A pressed other key — no MCA */
        PJ_LOG(3, (THIS_FILE, "[MCA] A pressed other key — no MCA"));
        cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");

        if (cc_session_call_is_current(s, call_a, 1)) {
            const char *not_sent_path = cc_prompt_get_path(CC_PROMPT_MCA_NOT_SENT);
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] Play A MCA_NOT_SENT prompt: %s", not_sent_path));
            pjsua_player_id ns_pid = cc_start_wav(call_a, not_sent_path, PJ_FALSE);
            int ns_wait_ms = cc_player_duration_ms(ns_pid);
            int ns_cap = cc_cfg_free_period_ms();
            if (ns_wait_ms > ns_cap)
                ns_wait_ms = ns_cap;
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] MCA_NOT_SENT prompt duration wait=%dms", ns_wait_ms));
            cc_sleep_ms(ns_wait_ms);
            cc_stop_wav(ns_pid, PJSUA_INVALID_ID);

            if (cc_session_call_is_current(s, call_a, 1))
                cc_safe_hangup(call_a, PJSIP_SC_OK);
        }
    } else {
        /* Timeout or A disconnected — hangup with NoMCA */
        if (decided == 0) {
            PJ_LOG(3, (THIS_FILE, "[MCA] DTMF timeout — no MCA"));
            cc_session_mark_end(s, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
        }

        if (cc_session_call_is_current(s, call_a, 1))
            cc_safe_hangup(call_a, PJSIP_SC_TEMPORARILY_UNAVAILABLE);
    }

    CC_SESSION_LOCK(s);
    s->a_treatment_running = 0;
    CC_SESSION_UNLOCK(s);

    cc_session_maybe_finalize(s);
    cc_session_release_reason(s, "mca-wait-complete");
    return NULL;
}

void leg_a_play_mca_wait(cc_session_t *session, cc_prompt_tag_t prompt_tag)
{
    mca_wait_arg_t *arg;
    pjsua_call_id call_a;
    pthread_t t;
    int rc;

    stop_a_waiting_prompt_before_treatment(session, "mca-wait");

    arg = malloc(sizeof(*arg));
    if (!arg) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] MCA wait allocation failed"));
        return;
    }

    CC_SESSION_LOCK(session);
    if (session->a_treatment_running ||
        session->call_a == PJSUA_INVALID_ID)
    {
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE, "[MCA] skipped: treatment running or no A-leg"));
        free(arg);
        return;
    }
    session->a_treatment_running = 1;
    session->mca_waiting = 1;
    session->mca_decided = 0;
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    if (!cc_session_acquire_reason(session, "mca-wait-worker")) {
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        session->mca_waiting = 0;
        CC_SESSION_UNLOCK(session);
        free(arg);
        return;
    }

    arg->session = session;
    arg->expected_call_a = call_a;
    arg->prompt_tag = prompt_tag;

    rc = pthread_create(&t, NULL, mca_wait_thread, arg);
    if (rc != 0) {
        PJ_LOG(1, (THIS_FILE,
                   "[ERROR] MCA wait pthread_create failed: %d", rc));
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        session->mca_waiting = 0;
        CC_SESSION_UNLOCK(session);
        free(arg);
        cc_session_release_reason(session, "mca-wait-create-failed");
        return;
    }

    pthread_detach(t);
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
