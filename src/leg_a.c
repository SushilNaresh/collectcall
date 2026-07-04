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

#include <pjsua-lib/pjsua.h>
#include <pjsip/sip_util.h>
#include <pjmedia/sdp.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define THIS_FILE "leg_a.c"

static void stop_a_waiting_prompt_before_treatment(cc_session_t *session,
                                                   const char *reason)
{
    pjsua_player_id player_a = PJSUA_INVALID_ID;

    if (!session)
        return;

    CC_SESSION_LOCK(session);
    if (session->player_a != PJSUA_INVALID_ID) {
        player_a = session->player_a;
        session->player_a = PJSUA_INVALID_ID;
    }

    CC_SESSION_UNLOCK(session);

    if (player_a != PJSUA_INVALID_ID) {
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
         * Start B-leg only now, matching the customer/reference flow.
         */
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
        const char *waiting_path = cc_prompt_get_path(CC_PROMPT_WAITING);
        pjsua_player_id pid;
        int keep_player = 0;

        PJ_LOG(3, (THIS_FILE, "[VOICE] Start A waiting prompt: %s", waiting_path));
        pid = cc_start_wav(call_id, waiting_path, PJ_TRUE);

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
            PJ_LOG(3, (THIS_FILE,
                       "[A] Waiting WAV started (loop) player=%d", pid));
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
        cc_sleep_ms(4000);
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
