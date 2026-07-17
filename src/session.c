/*
 * session.c — CollectCallSession lifecycle
 */
#include "session.h"
#include "config.h"
#include "runtime_config.h"

#include <pj/pool.h>
#include <string.h>
#include <stdlib.h>

cc_session_t *cc_session_create(pj_pool_factory *pf)
{
    pj_pool_t    *pool;
    cc_session_t *s;

    pool = pj_pool_create(pf, "cc_session",
                          CC_POOL_INIT_SIZE, CC_POOL_INC_SIZE, NULL);
    if (!pool)
        return NULL;

    s = (cc_session_t *)pj_pool_zalloc(pool, sizeof(*s));
    if (!s) {
        pj_pool_release(pool);
        return NULL;
    }

    s->pool          = pool;
    s->ref_count     = 1;
    s->final_cleanup_started = 0;
    s->call_a        = PJSUA_INVALID_ID;
    s->call_b        = PJSUA_INVALID_ID;
    s->acc_id        = PJSUA_INVALID_ID;
    s->b_number[0]   = '\0';
    s->service_key[0] = '\0';
    s->b_dial_number[0] = '\0';
    s->b_leg_started = 0;
    s->b_origination_pending = 0;
    s->call_id[0]    = '\0';
    s->caller_msisdn[0] = '\0';
    s->caller_msisdn_raw[0] = '\0';
    s->caller_msisdn_normalized[0] = '\0';
    s->caller_msisdn_source[0] = '\0';
    s->dialed_number_raw[0] = '\0';
    s->dialed_number_digits[0] = '\0';
    s->dialed_number_source[0] = '\0';
    s->matched_prefix[0] = '\0';
    s->sponsor_msisdn_raw[0] = '\0';
    s->sponsor_msisdn_normalized[0] = '\0';
    s->icid[0]       = '\0';
    s->call_start_ts = 0;
    s->free_period_ms = cc_cfg_free_period_ms();
    s->call_connected_ts = 0;
    s->call_end_ts   = 0;
    s->end_reported  = 0;
    s->final_status[0] = '\0';
    s->final_reason[0] = '\0';
    s->decision_completed = 0;
    s->decision_digit = '\0';
    s->accepted      = 0;
    s->torn_down     = 0;
    s->a_prompt_starting = 0;
    s->b_prompt_starting = 0;
    s->a_treatment_running = 0;
    s->ring_timer_started = 0;
    s->dtmf_timer_started = 0;
    s->bypass_mode   = BYPASS_NONE;
    s->fwd_hdr_count = 0;
    s->player_a      = PJSUA_INVALID_ID;
    s->player_b      = PJSUA_INVALID_ID;

    memset(&s->rtp_a, 0, sizeof(s->rtp_a));
    memset(&s->rtp_b, 0, sizeof(s->rtp_b));

    s->update_a_pending = 0;
    s->update_b_pending = 0;
    s->reinvite_a_pending = 0;
    s->reinvite_b_pending = 0;

    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        pj_pool_release(pool);
        return NULL;
    }

    PJ_LOG(3, ("session", "[SESSION] create session=%p refs=%u",
               s, s->ref_count));
    return s;
}

void cc_session_destroy(cc_session_t *s)
{
    if (!s) return;
    PJ_LOG(3, ("session", "[SESSION] destroyed session=%p", s));
    pthread_mutex_destroy(&s->lock);
    pj_pool_release(s->pool);   /* frees s itself (pool-allocated) */
}

int cc_session_acquire(cc_session_t *s)
{
    return cc_session_acquire_reason(s, "unspecified");
}

int cc_session_acquire_reason(cc_session_t *s, const char *reason)
{
    int acquired = 0;
    unsigned refs = 0;

    if (!s)
        return 0;

    CC_SESSION_LOCK(s);
    if (s->ref_count > 0 && !s->final_cleanup_started) {
        s->ref_count++;
        refs = s->ref_count;
        acquired = 1;
    }
    CC_SESSION_UNLOCK(s);

    if (acquired) {
        PJ_LOG(4, ("session",
                   "[SESSION] acquire reason=%s session=%p refs=%u",
                   reason ? reason : "unspecified", s, refs));
    }

    return acquired;
}

void cc_session_release(cc_session_t *s)
{
    cc_session_release_reason(s, "unspecified");
}

void cc_session_release_reason(cc_session_t *s, const char *reason)
{
    int destroy = 0;
    unsigned refs = 0;

    if (!s)
        return;

    CC_SESSION_LOCK(s);
    if (s->ref_count == 0) {
        PJ_LOG(1, ("session",
                   "[ERROR] session=%p release with zero references", s));
    } else {
        s->ref_count--;
        refs = s->ref_count;
        destroy = (s->ref_count == 0);
    }
    CC_SESSION_UNLOCK(s);

    PJ_LOG(4, ("session",
               "[SESSION] release reason=%s session=%p refs=%u",
               reason ? reason : "unspecified", s, refs));

    if (destroy)
        cc_session_destroy(s);
}

void cc_session_maybe_finalize(cc_session_t *s)
{
    int release_owner = 0;
    unsigned refs = 0;

    if (!s)
        return;

    CC_SESSION_LOCK(s);
    if (!s->final_cleanup_started &&
        s->call_a == PJSUA_INVALID_ID &&
        s->call_b == PJSUA_INVALID_ID &&
        !s->b_origination_pending)
    {
        s->final_cleanup_started = 1;
        refs = s->ref_count;
        release_owner = 1;
    }
    CC_SESSION_UNLOCK(s);

    if (release_owner) {
        PJ_LOG(3, ("session",
                   "[SESSION] final cleanup session=%p refs=%u",
                   s, refs));
        cc_session_release_reason(s, "initial-owner");
    }
}

static void cc_session_invalidate_leg(cc_session_t *s,
                                      pjsua_call_id call_id,
                                      int is_a_leg)
{
    int invalidated = 0;

    if (!s || call_id == PJSUA_INVALID_ID)
        return;

    if (pjsua_call_get_user_data(call_id) == s) {
        pjsua_call_set_user_data(call_id, NULL);
    }

    CC_SESSION_LOCK(s);
    if (is_a_leg && s->call_a == call_id) {
        s->call_a = PJSUA_INVALID_ID;
        s->update_a_pending = 0;
        s->reinvite_a_pending = 0;
        invalidated = 1;
    } else if (!is_a_leg && s->call_b == call_id) {
        s->call_b = PJSUA_INVALID_ID;
        s->update_b_pending = 0;
        s->reinvite_b_pending = 0;
        invalidated = 1;
    }
    CC_SESSION_UNLOCK(s);

    if (invalidated) {
        PJ_LOG(3, ("session", "[SESSION] %s-leg invalidated call=%d session=%p",
                   is_a_leg ? "A" : "B", call_id, s));
    }
}

void cc_session_invalidate_a(cc_session_t *s, pjsua_call_id call_id)
{
    cc_session_invalidate_leg(s, call_id, 1);
}

void cc_session_invalidate_b(cc_session_t *s, pjsua_call_id call_id)
{
    cc_session_invalidate_leg(s, call_id, 0);
}

int cc_session_call_is_current(cc_session_t *s,
                               pjsua_call_id call_id,
                               int is_a_leg)
{
    int matches;

    if (!s || call_id == PJSUA_INVALID_ID)
        return 0;

    CC_SESSION_LOCK(s);
    matches = is_a_leg ? (s->call_a == call_id) : (s->call_b == call_id);
    CC_SESSION_UNLOCK(s);

    if (!matches || pjsua_call_get_user_data(call_id) != s)
        return 0;

    return pjsua_call_is_active(call_id) == PJ_TRUE;
}
