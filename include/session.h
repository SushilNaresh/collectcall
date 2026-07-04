#ifndef CC_SESSION_H
#define CC_SESSION_H

#include <pjsua-lib/pjsua.h>
#include <pj/pool.h>
#include <pthread.h>
#include <time.h>

/*
 * session.h — CollectCallSession
 *
 * One session is created per collect call pair.
 * It is the single source of truth shared between Leg-A callbacks,
 * Leg-B callbacks, and all timer threads.
 */

/* ── Bypass mode ────────────────────────────────────────────────────────── */
typedef enum {
    BYPASS_NONE   = 0,   /* not yet determined                             */
    BYPASS_DIRECT = 1,   /* VoLTE: inject peer RTP addr into UPDATE SDP    */
    BYPASS_MGW    = 2    /* CS/MGCF: X-MGW-Directive header in UPDATE      */
} cc_bypass_mode_t;

/* ── RTP endpoint ───────────────────────────────────────────────────────── */
typedef struct {
    char ip[64];
    int  port;
    int  valid;          /* non-zero when filled in                         */
} cc_rtp_ep_t;

/* ── Forwarded SIP header (name + value) ──────────────────────────────── */
#define CC_MAX_FWD_HDRS  16
typedef struct {
    char name[128];
    char *value;        /* exact value copied into the session pool */
} cc_sip_hdr_t;

/* ── Session ────────────────────────────────────────────────────────────── */
typedef struct cc_session {
    pj_pool_t          *pool;          /* memory pool for this session      */

    /* All mutable lifecycle/media fields below are guarded by this lock. */
    pthread_mutex_t     lock;
    unsigned            ref_count;
    int                 final_cleanup_started;

    pjsua_call_id       call_a;        /* inbound leg (PJSUA_INVALID_ID if gone) */
    pjsua_call_id       call_b;        /* outbound leg                      */
    pjsua_acc_id        acc_id;        /* account used for B-leg origination */

    char                b_number[64];  /* normalized B/sponsor number */
    char                service_key[64]; /* optional prefix from validation */
    char                b_dial_number[128]; /* final serviceKey + B number */
    int                 b_leg_started; /* 1 after A-leg ACK/CONFIRMED starts B-leg */
    int                 b_origination_pending;

    char                call_id[128];  /* SIP/API call id for later end_call mapping */
    char                caller_msisdn[64]; /* normalized caller for existing code */
    char                caller_msisdn_raw[128];
    char                caller_msisdn_normalized[64];
    char                caller_msisdn_source[16];
    char                dialed_number_raw[128];
    char                dialed_number_digits[128];
    char                dialed_number_source[16];
    char                matched_prefix[32];
    char                sponsor_msisdn_raw[128];
    char                sponsor_msisdn_normalized[64];
    char                icid[128];     /* P-Charging-Vector icid-value */
    time_t              call_start_ts;
    time_t              call_connected_ts;
    time_t              call_end_ts;
    int                 end_reported;  /* 1 after final [CALL-END] log */
    char                final_status[32];
    char                final_reason[64];

    int                 decision_completed; /* B decision/timeout claimed once */
    char                decision_digit; /* '1', '2', or '\0' for timeout */
    int                 accepted;      /* 1 after B presses DTMF_ACCEPT     */
    int                 torn_down;     /* 1 once teardown has started       */
    int                 a_prompt_starting;
    int                 b_prompt_starting;
    int                 a_treatment_running;
    int                 ring_timer_started;
    int                 dtmf_timer_started;

    cc_bypass_mode_t    bypass_mode;
    cc_rtp_ep_t         rtp_a;         /* A's RTP endpoint from SDP         */
    cc_rtp_ep_t         rtp_b;         /* B's RTP endpoint from SDP         */

    int                 update_a_pending; /* rewrite A UPDATE SDP with B RTP */
    int                 update_b_pending; /* rewrite B UPDATE SDP with A RTP */
    int                 reinvite_a_pending; /* rewrite A re-INVITE SDP with B RTP */
    int                 reinvite_b_pending; /* rewrite B re-INVITE SDP with A RTP */

    cc_sip_hdr_t        fwd_hdrs[CC_MAX_FWD_HDRS];
    int                 fwd_hdr_count;

    pjsua_player_id     player_a;      /* A-leg WAV player                  */
    pjsua_player_id     player_b;      /* B-leg WAV player                  */
} cc_session_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */
cc_session_t *cc_session_create(pj_pool_factory *pf);
void          cc_session_destroy(cc_session_t *s);
int           cc_session_acquire(cc_session_t *s);
void          cc_session_release(cc_session_t *s);
int           cc_session_acquire_reason(cc_session_t *s, const char *reason);
void          cc_session_release_reason(cc_session_t *s, const char *reason);
void          cc_session_maybe_finalize(cc_session_t *s);
void          cc_session_invalidate_a(cc_session_t *s, pjsua_call_id call_id);
void          cc_session_invalidate_b(cc_session_t *s, pjsua_call_id call_id);
int           cc_session_call_is_current(cc_session_t *s,
                                         pjsua_call_id call_id,
                                         int is_a_leg);

/* ── Convenience lock/unlock ────────────────────────────────────────────── */
#define CC_SESSION_LOCK(s)   pthread_mutex_lock(&(s)->lock)
#define CC_SESSION_UNLOCK(s) pthread_mutex_unlock(&(s)->lock)

#endif /* CC_SESSION_H */
