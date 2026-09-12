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
#include "rtpengine.h"
#include "worker.h"

#include <pjsua-lib/pjsua.h>
#include <pjsip/sip_util.h>
#include <pjmedia/sdp.h>
#include <pjmedia/wav_port.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define THIS_FILE "leg_a.c"

/*
 * After A is CONFIRMED, Zoiper often has no Allow:UPDATE — send re-INVITE
 * so the phone moves RTP to RTPengine A-facing ports (initial 200 may still
 * advertise PJSUA sockets if answer reused early SDP).
 */
static int leg_a_reinvite_advertise_rtpengine(pjsua_call_id call_id,
                                              cc_session_t *session)
{
    cc_rtp_ep_t tgt;
    pjsua_msg_data msg_data;
    pj_status_t status;
    int already;
    pjsua_call_info ci;

    if (!session || !cc_rtpengine_enabled())
        return 0;

    if (!cc_rtpengine_sdp_target(session, 1, &tgt) || !tgt.valid) {
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] A re-INVITE skipped — no A-facing endpoint"));
        return -1;
    }

    /* Don't stack an INVITE on top of an in-progress invite/UPDATE (load). */
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        if (ci.state != PJSIP_INV_STATE_CONFIRMED) {
            PJ_LOG(2, (THIS_FILE,
                       "[RTPENGINE] A re-INVITE deferred — call state=%d",
                       (int)ci.state));
            return -1;
        }
    }

    CC_SESSION_LOCK(session);
    already = session->rtpengine_a_reinvite_done;
    if (!already)
        session->reinvite_a_pending = 1;
    CC_SESSION_UNLOCK(session);

    if (already) {
        PJ_LOG(3, (THIS_FILE, "[RTPENGINE] A re-INVITE already done, skip"));
        return 0;
    }

    pjsua_msg_data_init(&msg_data);
    status = pjsua_call_reinvite(call_id, 0, &msg_data);
    if (status != PJ_SUCCESS) {
        CC_SESSION_LOCK(session);
        session->reinvite_a_pending = 0;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(1, (THIS_FILE,
                   "[RTPENGINE] A re-INVITE failed status=%d (keep trying prompt)",
                   status));
        return -1;
    }

    CC_SESSION_LOCK(session);
    session->rtpengine_a_reinvite_done = 1;
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[RTPENGINE] A re-INVITE sent — advertise media %s:%d",
               tgt.ip, tgt.port));
    return 0;
}

void leg_a_on_rtpengine_a_ep_changed(cc_session_t *session)
{
    pjsua_call_id call_a;
    pjsua_player_id old_pid;
    char play_file[256];
    int play_loop;
    cc_rtp_ep_t tgt;

    if (!session || !cc_rtpengine_enabled())
        return;

    CC_SESSION_LOCK(session);
    call_a = session->call_a;
    old_pid = session->player_a;
    session->player_a = PJSUA_INVALID_ID;
    snprintf(play_file, sizeof(play_file), "%s", session->rtpengine_a_play_file);
    play_loop = session->rtpengine_a_play_loop;
    /* Force a fresh advertise of the new A-facing ports. */
    session->rtpengine_a_reinvite_done = 0;
    session->rtpengine_a_advertised = 0;
    CC_SESSION_UNLOCK(session);

    if (call_a == PJSUA_INVALID_ID)
        return;

    if (old_pid != PJSUA_INVALID_ID)
        cc_stop_wav(old_pid, PJSUA_INVALID_ID);

    if (cc_rtpengine_sdp_target(session, 1, &tgt) && tgt.valid) {
        PJ_LOG(3, (THIS_FILE,
                   "[RTPENGINE] A-facing changed — re-INVITE to %s:%d and "
                   "restart play file=%s",
                   tgt.ip, tgt.port,
                   play_file[0] ? play_file : "<none>"));
        (void)leg_a_reinvite_advertise_rtpengine(call_a, session);
        CC_SESSION_LOCK(session);
        session->rtpengine_a_advertised = 1;
        CC_SESSION_UNLOCK(session);
    }

    if (play_file[0] != '\0' && !session->torn_down) {
        pjsua_player_id pid = cc_start_wav(call_a, play_file,
                                           play_loop ? PJ_TRUE : PJ_FALSE);
        if (pid != PJSUA_INVALID_ID) {
            CC_SESSION_LOCK(session);
            if (session->call_a == call_a &&
                !session->torn_down &&
                session->player_a == PJSUA_INVALID_ID)
            {
                session->player_a = pid;
                PJ_LOG(3, (THIS_FILE,
                           "[VOICE] A play restarted after A-facing change "
                           "player=%d file=%s",
                           pid, play_file));
            } else {
                CC_SESSION_UNLOCK(session);
                cc_stop_wav(pid, PJSUA_INVALID_ID);
                return;
            }
            CC_SESSION_UNLOCK(session);
        }
    }
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

/*
 * For validation-reject / ineligible treatment: finish WAITING (1.1) before
 * NOT_AVAILABLE (1.45). If media never started 1.1 (common when torn_down is
 * set first), start it here and return its full duration as remaining_ms.
 */
static pjsua_player_id ensure_a_waiting_for_treatment(cc_session_t *session,
                                                      int *remaining_ms_out,
                                                      const char *reason)
{
    pjsua_player_id player_a;
    pjsua_call_id call_a;
    int fundless = 0;
    int attempt;

    player_a = take_a_waiting_prompt(session, remaining_ms_out, reason);
    if (player_a != PJSUA_INVALID_ID)
        return player_a;

    /* Media path may be mid-start; wait briefly then take again. */
    for (attempt = 0; attempt < 40; attempt++) {
        int starting = 0;

        CC_SESSION_LOCK(session);
        starting = session->a_prompt_starting;
        if (!starting && session->player_a != PJSUA_INVALID_ID) {
            CC_SESSION_UNLOCK(session);
            return take_a_waiting_prompt(session, remaining_ms_out, reason);
        }
        CC_SESSION_UNLOCK(session);
        if (!starting)
            break;
        usleep(5000);
    }

    CC_SESSION_LOCK(session);
    call_a = session->call_a;
    fundless = session->fundless;
    if (call_a == PJSUA_INVALID_ID || session->a_treatment_running) {
        CC_SESSION_UNLOCK(session);
        *remaining_ms_out = 0;
        return PJSUA_INVALID_ID;
    }
    /* Waiting already ran to completion (player cleared / elapsed). */
    if (session->a_prompt_duration_ms > 0 && session->a_confirmed_ms > 0) {
        struct timespec ts;
        long long now_ms;
        long long elapsed;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        elapsed = now_ms - session->a_confirmed_ms;
        if (elapsed >= session->a_prompt_duration_ms) {
            CC_SESSION_UNLOCK(session);
            *remaining_ms_out = 0;
            return PJSUA_INVALID_ID;
        }
    }
    session->a_prompt_starting = 1;
    CC_SESSION_UNLOCK(session);

    {
        cc_prompt_tag_t a_prompt = fundless ? CC_PROMPT_FUNDLESS
                                            : CC_PROMPT_WAITING;
        const char *waiting_path = cc_prompt_get_path(a_prompt);
        pjsua_player_id pid;
        int wait_ms = 0;

        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] Start A waiting prompt before treatment (%s): %s",
                   reason ? reason : "unknown", waiting_path));
        pid = cc_start_wav(call_a, waiting_path, PJ_FALSE);

        CC_SESSION_LOCK(session);
        session->a_prompt_starting = 0;
        if (pid != PJSUA_INVALID_ID &&
            session->call_a == call_a &&
            !session->accepted)
        {
            struct timespec ts;
            wait_ms = cc_wav_player_duration_ms(pid);
            session->a_prompt_done = 0;
            session->a_prompt_duration_ms = wait_ms;
            if (session->a_confirmed_ms <= 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                session->a_confirmed_ms = (long long)ts.tv_sec * 1000 +
                                          ts.tv_nsec / 1000000;
            }
            /* Ownership stays with the WAV_HANGUP event, not session->player_a. */
            CC_SESSION_UNLOCK(session);
            *remaining_ms_out = wait_ms > 0 ? wait_ms : 0;
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] A waiting prompt armed for treatment "
                       "player=%d duration=%dms",
                       (int)pid, wait_ms));
            return pid;
        }
        CC_SESSION_UNLOCK(session);

        if (pid != PJSUA_INVALID_ID)
            cc_stop_wav(pid, PJSUA_INVALID_ID);
    }

    *remaining_ms_out = 0;
    return PJSUA_INVALID_ID;
}

void leg_a_advertise_rtpengine_if_ready(cc_session_t *session)
{
    pjsua_call_id call_a;
    cc_rtp_ep_t tgt;
    int offer_pending;
    int advertised;
    long long t0;
    long long since_cb = -1;
    long long since_offer = -1;
    long long since_confirmed = -1;
    int reinv_rc;

    if (!session || !cc_rtpengine_enabled())
        return;

    memset(&tgt, 0, sizeof(tgt));
    t0 = cc_monotonic_ms();

    CC_SESSION_LOCK(session);
    call_a = session->call_a;
    offer_pending = session->rtpengine_a_offer_pending;
    advertised = session->rtpengine_a_advertised;
    if (session->a_invite_cb_ms > 0)
        since_cb = t0 - session->a_invite_cb_ms;
    if (session->a_offer_done_ms > 0)
        since_offer = t0 - session->a_offer_done_ms;
    if (session->a_confirmed_ms > 0)
        since_confirmed = t0 - session->a_confirmed_ms;
    if (session->torn_down || call_a == PJSUA_INVALID_ID) {
        CC_SESSION_UNLOCK(session);
        return;
    }
    CC_SESSION_UNLOCK(session);

    if (advertised)
        return;

    if (offer_pending) {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_need_advertise = 1;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=ADVERTISE_DEFER "
                   "reason=offer_pending since_cb_ms=%lld",
                   call_a, session->call_id, since_cb));
        return;
    }

    if (!cc_rtpengine_sdp_target(session, 1, &tgt) || !tgt.valid) {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_need_advertise = 1;
        CC_SESSION_UNLOCK(session);
        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=ADVERTISE_DEFER "
                   "reason=no_ep since_cb_ms=%lld",
                   call_a, session->call_id, since_cb));
        return;
    }

    CC_SESSION_LOCK(session);
    session->rtpengine_a_need_advertise = 0;
    session->a_advertise_ms = t0;
    CC_SESSION_UNLOCK(session);

    PJ_LOG(3, (THIS_FILE,
               "[A-TIMING] call_a=%d callId=%s phase=ADVERTISE_START "
               "ep=%s:%d since_cb_ms=%lld since_offer_ms=%lld "
               "since_confirmed_ms=%lld",
               call_a, session->call_id, tgt.ip, tgt.port,
               since_cb, since_offer, since_confirmed));

    reinv_rc = leg_a_reinvite_advertise_rtpengine(call_a, session);
    if (reinv_rc != 0) {
        PJ_LOG(2, (THIS_FILE,
                   "[RTPENGINE] A re-INVITE failed after offer — start prompt anyway"));
        /* Don't leave need_reinvite stuck: play-media still reaches the phone. */
        CC_SESSION_LOCK(session);
        session->rtpengine_a_reinvite_done = 1;
        CC_SESSION_UNLOCK(session);
    } else {
        CC_SESSION_LOCK(session);
        session->rtpengine_a_advertised = 1;
        CC_SESSION_UNLOCK(session);
    }

    PJ_LOG(3, (THIS_FILE,
               "[A-TIMING] call_a=%d callId=%s phase=ADVERTISE_DONE "
               "reinvite_rc=%d advertise_dur_ms=%lld since_cb_ms=%lld",
               call_a, session->call_id, reinv_rc,
               cc_monotonic_ms() - t0,
               session->a_invite_cb_ms > 0 ?
                   cc_monotonic_ms() - session->a_invite_cb_ms : -1));

    leg_a_on_media_state(call_a, session);
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
         * Offer-then-answer: 200 should already carry RTPengine ports
         * (rtpengine_a_advertised). Only re-INVITE if patch failed.
         */
        int advertised = 0;
        int offer_pending = 0;
        long long since_cb = -1;
        long long since_200 = -1;
        long long since_offer = -1;
        {
            long long now = cc_monotonic_ms();
            CC_SESSION_LOCK(session);
            session->a_confirmed_ms = now;
            advertised = session->rtpengine_a_advertised;
            offer_pending = session->rtpengine_a_offer_pending;
            if (session->a_invite_cb_ms > 0)
                since_cb = now - session->a_invite_cb_ms;
            if (session->a_200_sent_ms > 0)
                since_200 = now - session->a_200_sent_ms;
            if (session->a_offer_done_ms > 0)
                since_offer = now - session->a_offer_done_ms;
            if (advertised)
                session->rtpengine_a_reinvite_done = 1;
            CC_SESSION_UNLOCK(session);
        }

        PJ_LOG(3, (THIS_FILE,
                   "[A-TIMING] call_a=%d callId=%s phase=CONFIRMED "
                   "since_cb_ms=%lld since_200_ms=%lld since_offer_ms=%lld "
                   "offer_pending=%d advertised=%d",
                   call_id, session->call_id, since_cb, since_200, since_offer,
                   offer_pending, advertised));

        if (cc_rtpengine_enabled() && !advertised) {
            if (offer_pending) {
                CC_SESSION_LOCK(session);
                session->rtpengine_a_need_advertise = 1;
                CC_SESSION_UNLOCK(session);
                PJ_LOG(3, (THIS_FILE,
                           "[RTPENGINE] A CONFIRMED — wait for async offer before "
                           "re-INVITE/prompt (call_a=%d)", call_id));
            } else {
                PJ_LOG(3, (THIS_FILE,
                           "[RTPENGINE] A CONFIRMED — SDP not in 200; "
                           "fallback re-INVITE advertise (call_a=%d)", call_id));
                leg_a_advertise_rtpengine_if_ready(session);
            }
        } else {
            if (cc_rtpengine_enabled() && advertised) {
                PJ_LOG(3, (THIS_FILE,
                           "[RTPENGINE] A CONFIRMED — RE already in 200; "
                           "skip re-INVITE (call_a=%d)", call_id));
            }
            leg_a_on_media_state(call_id, session);
        }

        deferred_hangup = cc_start_b_leg_after_a_confirmed(session);
    }

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        pjsua_player_id player_a = PJSUA_INVALID_ID;
        pjsua_call_id b;
        int already_accepted;
        int start_teardown = 0;
        int need_deferred_end = 0;
        char status[32];
        char reason[64];

        CC_SESSION_LOCK(session);
        if (session->player_a != PJSUA_INVALID_ID) {
            player_a = session->player_a;
            session->player_a = PJSUA_INVALID_ID;
        }
        session->a_prompt_starting = 0;
        session->mca_waiting = 0;
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
        } else if (!session->end_reported && session->final_status[0] != '\0') {
            /* Deferred-treatment path (validation / DTMF timeout / MCA / …):
             * A hung up before HANGUP_A_ONLY — emit stored CDR now. */
            snprintf(status, sizeof(status), "%s", session->final_status);
            snprintf(reason, sizeof(reason), "%s", session->final_reason);
            need_deferred_end = 1;
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
        } else if (need_deferred_end) {
            cc_session_mark_end(session, status, reason);
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

    /*
     * RTPengine: wait until CONFIRMED (ACK received). If a re-INVITE is still
     * required to advertise RTPengine ports, wait for that too.
     */
    if (cc_rtpengine_enabled() && ci.state != PJSIP_INV_STATE_CONFIRMED) {
        PJ_LOG(4, (THIS_FILE,
                   "[VOICE] A waiting prompt deferred until CONFIRMED (rtpengine)"));
        return;
    }

    if (cc_rtpengine_enabled()) {
        int need_reinvite;
        int advertised;
        CC_SESSION_LOCK(session);
        advertised = session->rtpengine_a_advertised;
        need_reinvite = !advertised && !session->rtpengine_a_reinvite_done;
        CC_SESSION_UNLOCK(session);
        if (need_reinvite) {
            PJ_LOG(4, (THIS_FILE,
                       "[VOICE] A waiting prompt deferred until RTPengine "
                       "re-INVITE completes"));
            return;
        }
    }

    CC_SESSION_LOCK(session);
    /*
     * Do not block on torn_down alone: ineligible sets torn_down before
     * treatment, but A still needs WAITING (1.1) before 1.45. Skip only when
     * treatment already owns the A prompt path.
     */
    if (session->accepted || session->a_treatment_running ||
        session->player_a != PJSUA_INVALID_ID ||
        session->a_prompt_starting)
    {
        int accepted = session->accepted;
        int treatment = session->a_treatment_running;
        CC_SESSION_UNLOCK(session);

        if (accepted)
            PJ_LOG(3, (THIS_FILE, "[A] media update ignored after accept; not restarting waiting WAV"));
        else if (treatment)
            PJ_LOG(3, (THIS_FILE, "[A] media update ignored during treatment; not restarting waiting WAV"));
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
            !session->a_treatment_running &&
            session->call_a == call_id &&
            session->player_a == PJSUA_INVALID_ID)
        {
            session->player_a = pid;
            keep_player = 1;
        }
        session->a_prompt_starting = 0;
        CC_SESSION_UNLOCK(session);

        if (keep_player) {
            int wait_ms = cc_wav_player_duration_ms(pid);
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

    pj_status_t status = cc_call_answer2_serialized(call_id, &cs,
                                                    PJSIP_SC_OK, NULL, NULL,
                                                    NULL);
    if (status == PJ_SUCCESS)
        PJ_LOG(3, (THIS_FILE, "[A] 200 OK sent"));
    else
        PJ_LOG(1, (THIS_FILE, "[A] answer 200 failed: %d", status));
}

/* ── SIP UPDATE to exit RTP path ─────────────────────────────────────────── */

void leg_a_send_update_bypass(pjsua_call_id call_id, cc_session_t *session)
{
    /* Non-blocking: post CC_EV_UPDATE_A_BYPASS to worker pool.
     * The worker polls b_reinvite_active + RTP readiness via re-post
     * every 50ms — no worker is blocked sleeping. */
    cc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_UPDATE_A_BYPASS;
    ev.session = session;
    ev.call_a  = call_id;
    CC_SESSION_LOCK(session);
    ev.call_b  = session->call_b;
    CC_SESSION_UNLOCK(session);
    snprintf(ev.reason, sizeof(ev.reason), "update-a-bypass");
    if (cc_worker_post(&ev) != 0)
        PJ_LOG(1, (THIS_FILE, "[A] UPDATE bypass post failed"));
}

/* ── Play WAV then hangup (runs in thread) ───────────────────────────────── */

/* SIP re-INVITE to test RTP bypass */
void leg_a_send_reinvite_bypass(cc_session_t *session)
{
    /* Non-blocking: post CC_EV_REINVITE_A_BYPASS to worker pool.
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
    ev.type   = CC_EV_REINVITE_A_BYPASS;
    ev.session = session;
    ev.call_a  = call_a;
    ev.call_b  = call_b;
    snprintf(ev.reason, sizeof(ev.reason), "reinvite-a-bypass");
    if (cc_worker_post(&ev) != 0)
        PJ_LOG(1, (THIS_FILE, "[A] re-INVITE bypass post failed"));
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

    if (player_a != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] Stop A waiting prompt before treatment: rejected"));
        cc_stop_wav(player_a, PJSUA_INVALID_ID);
        player_a = PJSUA_INVALID_ID;
        wait_ms = 0;
    }
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

    if (player_a != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE,
                   "[VOICE] Stop A waiting prompt before treatment: unavailable"));
        cc_stop_wav(player_a, PJSUA_INVALID_ID);
        player_a = PJSUA_INVALID_ID;
        wait_ms = 0;
    }
    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A unavailable prompt then hangup (one-shot): %s",
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
    pjsua_player_id player_a = PJSUA_INVALID_ID;
    const char *path = cc_prompt_get_path(tag);
    int b_started = 0;

    CC_SESSION_LOCK(session);
    /* b_leg_started is set only after ELIGIBLE arms B originate — not at
     * validation start — so ineligible during 1.1 still defers treatment. */
    b_started = session->b_leg_started ||
                session->call_b != PJSUA_INVALID_ID;
    CC_SESSION_UNLOCK(session);

    if (b_started) {
        /*
         * B reject (DTMF 2) / DTMF timeout / no-answer: A is on MOH (4.wav).
         * Stop MOH immediately and play treatment, then hangup.
         */
        player_a = take_a_waiting_prompt(session, &wait_ms, tag_name);
        if (player_a != PJSUA_INVALID_ID) {
            PJ_LOG(3, (THIS_FILE,
                       "[VOICE] Stop A MOH/prompt before B-leg treatment: %s",
                       tag_name));
            cc_stop_wav(player_a, PJSUA_INVALID_ID);
            player_a = PJSUA_INVALID_ID;
            wait_ms = 0;
        }
        {
            pjsua_player_id hold_a = PJSUA_INVALID_ID;
            CC_SESSION_LOCK(session);
            if (session->hold_player_a != PJSUA_INVALID_ID) {
                hold_a = session->hold_player_a;
                session->hold_player_a = PJSUA_INVALID_ID;
            }
            CC_SESSION_UNLOCK(session);
            if (hold_a != PJSUA_INVALID_ID)
                cc_stop_wav(hold_a, PJSUA_INVALID_ID);
        }
    } else {
        /*
         * Ineligible / validation-reject before B: finish WAITING (1.1), then
         * treatment (e.g. 1.45).
         */
        player_a = ensure_a_waiting_for_treatment(session, &wait_ms, tag_name);
    }

    PJ_LOG(3, (THIS_FILE,
               "[VOICE] Play A prompt=%s then hangup (after waiting %dms): %s",
               tag_name, wait_ms, path));
    spawn_wav_hangup(session, path, code, player_a, wait_ms);
}

/* ── MCA flow: play UNAVAILABLE, wait for A DTMF 1, then MCA API ─────────── */

void leg_a_play_mca_wait(cc_session_t *session, cc_prompt_tag_t prompt_tag)
{
    cc_event_t ev;
    pjsua_call_id call_a;
    int wait_ms = 0;
    pjsua_player_id old_player;
    pjsua_player_id mca_pid = PJSUA_INVALID_ID;
    const char *path;

    /* A now needs both its own audio path and its DTMF: if the collect
     * phase left A's media blocked, digit 1 would never arrive. */
    cc_rtpengine_unblock_media(session);

    old_player = take_a_waiting_prompt(session, &wait_ms, "mca-wait");
    (void)wait_ms;
    /* Interrupt MOH / waiting WAV immediately so UNAVAILABLE can start. */
    if (old_player != PJSUA_INVALID_ID) {
        PJ_LOG(3, (THIS_FILE, "[VOICE] Stop A waiting prompt before treatment: mca-wait"));
        cc_stop_wav(old_player, PJSUA_INVALID_ID);
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

    path = cc_prompt_get_path(prompt_tag);
    PJ_LOG(3, (THIS_FILE,
               "[MCA] start prompt=%s path=%s on A call=%d (one-shot)",
               cc_prompt_tag_name(prompt_tag), path, call_a));
    mca_pid = cc_start_wav(call_a, path, PJ_FALSE);
    {
        int stored = 0;
        if (mca_pid != PJSUA_INVALID_ID) {
            CC_SESSION_LOCK(session);
            if (session->call_a == call_a && session->player_a == PJSUA_INVALID_ID) {
                session->player_a = mca_pid;
                stored = 1;
            }
            CC_SESSION_UNLOCK(session);
            if (!stored) {
                cc_stop_wav(mca_pid, PJSUA_INVALID_ID);
                mca_pid = PJSUA_INVALID_ID;
            }
        } else {
            PJ_LOG(1, (THIS_FILE, "[MCA] failed to start prompt %s on call=%d",
                       path, call_a));
        }
    }

    {
        int timeout_ms = cc_cfg_b_dtmf_timeout_sec() * 1000;
        int wav_ms = 0;
        cc_event_t tev;

        if (timeout_ms < 1)
            timeout_ms = 1;
        if (mca_pid != PJSUA_INVALID_ID)
            wav_ms = cc_wav_player_duration_ms(mca_pid);

        if (mca_pid != PJSUA_INVALID_ID && wav_ms > 0 && wav_ms < timeout_ms) {
            memset(&ev, 0, sizeof(ev));
            ev.type     = CC_EV_MCA_STOP_PROMPT;
            ev.session  = session;
            ev.call_a   = call_a;
            ev.player_a = mca_pid;
            snprintf(ev.reason, sizeof(ev.reason), "mca-stop-prompt");
            if (cc_worker_post_delayed(&ev, wav_ms) != 0) {
                PJ_LOG(3, (THIS_FILE,
                           "[MCA] stop-prompt timer not armed — timeout will stop WAV"));
            }
        }

        memset(&tev, 0, sizeof(tev));
        tev.type       = CC_EV_MCA_WAIT;
        tev.session    = session;
        tev.call_a     = call_a;
        tev.prompt_tag = (int)prompt_tag;
        snprintf(tev.reason, sizeof(tev.reason), "mca-timeout");
        if (cc_worker_post_delayed(&tev, timeout_ms) != 0) {
            pjsua_player_id pid = PJSUA_INVALID_ID;
            CC_SESSION_LOCK(session);
            session->a_treatment_running = 0;
            session->mca_waiting = 0;
            pid = session->player_a;
            session->player_a = PJSUA_INVALID_ID;
            CC_SESSION_UNLOCK(session);
            if (pid != PJSUA_INVALID_ID)
                cc_stop_wav(pid, PJSUA_INVALID_ID);
            PJ_LOG(1, (THIS_FILE, "[ERROR] MCA timeout timer not armed — hangup A"));
            cc_session_mark_end(session, "FAILED", "SPONSOR_UNREACHABLE_NoMCA");
            if (cc_session_call_is_current(session, call_a, 1))
                cc_safe_hangup(call_a, PJSIP_SC_TEMPORARILY_UNAVAILABLE);
        }
    }
}

/* ── A-leg DTMF handler for MCA decision ─────────────────────────────────── */

void leg_a_on_dtmf_mca(pjsua_call_id call_id, int digit, cc_session_t *session)
{
    cc_event_t ev;
    pjsua_call_id call_a;

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
        PJ_LOG(3, (THIS_FILE, "[DTMF] A-leg digit=1 — MCA accepted"));
    } else {
        session->mca_decided = 2;  /* 2 = don't send MCA */
        PJ_LOG(3, (THIS_FILE,
                   "[DTMF] A-leg digit=%c — MCA declined",
                   (char)digit));
    }
    session->mca_waiting = 0;
    call_a = session->call_a;
    CC_SESSION_UNLOCK(session);

    memset(&ev, 0, sizeof(ev));
    ev.type    = CC_EV_MCA_RESOLVE;
    ev.session = session;
    ev.call_a  = call_a;
    snprintf(ev.reason, sizeof(ev.reason), "mca-resolve");
    if (cc_worker_post(&ev) != 0) {
        PJ_LOG(1, (THIS_FILE, "[ERROR] MCA resolve post failed — hangup A"));
        cc_session_mark_end(session,
                            "FAILED",
                            (char)digit == CC_DTMF_ACCEPT
                                ? "SPONSOR_UNREACHABLE_MCA"
                                : "SPONSOR_UNREACHABLE_NoMCA");
        if (cc_session_call_is_current(session, call_a, 1))
            cc_safe_hangup(call_a, PJSIP_SC_OK);
        CC_SESSION_LOCK(session);
        session->a_treatment_running = 0;
        CC_SESSION_UNLOCK(session);
    }
    (void)call_id;
}
