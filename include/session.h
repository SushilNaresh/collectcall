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
#define CC_MAX_FWD_HDRS  8     /* we forward ~5-6 headers; was 16 */
typedef struct {
    char  name[32];   /* longest SIP header name is 25 chars; was 128 */
    char *value;      /* exact value copied into the session pool */
} cc_sip_hdr_t;

/* ── Session ────────────────────────────────────────────────────────────── */
typedef struct cc_session {
    pj_pool_t          *pool;          /* memory pool for this session      */

    /* Mutex is heap-allocated separately so it outlives pj_pool_release.
     * Any thread sleeping with a ref must be able to lock it after the
     * pool is freed. Freed in cc_session_destroy after mutex_destroy. */
    pthread_mutex_t    *lock;
    unsigned            ref_count;
    int                 final_cleanup_started;

    unsigned            session_serial; /* monotonic counter — survives call_id slot reuse */

    pjsua_call_id       call_a;        /* inbound leg (PJSUA_INVALID_ID if gone) */
    pjsua_call_id       call_b;        /* outbound leg                      */
    pjsua_acc_id        acc_id;        /* account used for B-leg origination */

    char                b_number[64];  /* normalized B/sponsor number */
    char                service_key[64]; /* optional prefix from validation */
    char                b_dial_number[128]; /* final serviceKey + B number */
    int                 b_validation_started; /* 1 once CONFIRMED kicked off initiate API */
    int                 b_leg_started; /* 1 after eligible — B originate armed (not pre-validation) */
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
    long long           a_confirmed_ms;  /* monotonic ms when A-leg CONFIRMED */
    /* A-leg setup timing (monotonic ms) — logged as [A-TIMING] at level 3 */
    long long           a_invite_cb_ms;      /* on_incoming_call entry */
    long long           a_100_sent_ms;       /* after pjsua_call_answer(100) */
    long long           a_answer_queued_ms;  /* after answer(+offer) posted */
    long long           a_200_worker_ms;     /* answer worker picked up event */
    long long           a_200_sent_ms;       /* after pjsua_call_answer2(200) */
    long long           a_offer_start_ms;    /* offer worker start */
    long long           a_offer_done_ms;     /* ng offer(+dummy) finished */
    long long           a_advertise_ms;      /* re-INVITE advertise attempt */
    long long           b_confirmed_ms;  /* monotonic ms when B-leg CONFIRMED */
    int                 free_period_ms;  /* snapshot of CC_FREE_PERIOD_MS at session start */
    time_t              b_answer_ts;     /* wall clock when B-leg answered (CONFIRMED) */
    time_t              b_prompt_start_ts; /* wall clock when 1.2.wav started playing to B */
    time_t              call_connected_ts;
    time_t              call_end_ts;
    int                 end_reported;  /* 1 after final [CALL-END] log */
    char                final_status[32];
    char                final_reason[64];

    int                 decision_completed; /* B decision/timeout claimed once */
    char                decision_digit; /* '1', '2', or '\0' for timeout */
    int                 accepted;      /* 1 after B presses DTMF_ACCEPT     */
    int                 whitelisted;   /* 1 if API returned ELIGIBLE+whitelisted; skip B prompt */
    int                 fundless;      /* 1 if matched prefix is in CC_FUNDLESS_PREFIXES */
    int                 torn_down;     /* 1 once teardown has started       */
    int                 a_prompt_starting;
    int                 a_prompt_done;       /* 1 after one-shot A waiting prompt finishes */
    int                 a_prompt_duration_ms; /* duration of A waiting prompt for B-leg wait */
    int                 b_prompt_starting;
    int                 b_collect_done;      /* 1 after one-shot B collect prompt finishes */
    int                 a_treatment_running;
    int                 mca_waiting;       /* 1 = waiting for A DTMF 1 to trigger MCA */
    int                 mca_decided;       /* 0=none, 1=send MCA, 2=decline */
    int                 accept_transition_pending; /* 1 while accept-transition event is queued/running */
    int                 ring_timer_started;
    int                 dtmf_timer_started;

    cc_bypass_mode_t    bypass_mode;
    cc_rtp_ep_t         rtp_a;         /* A's RTP endpoint from SDP         */
    cc_rtp_ep_t         rtp_b;         /* B's RTP endpoint from SDP         */

    /* RTPengine ng session (one per collect-call pair). */
    char                rtpengine_call_id[128];
    char                rtpengine_from_tag[64];
    char                rtpengine_to_tag[64];
    int                 rtpengine_offered;
    int                 rtpengine_answered;
    int                 rtpengine_deleted;
    int                 rtpengine_a_advertised; /* A SDP already has RTPengine ports */
    int                 rtpengine_a_answer_pending; /* answer-pool: deferred A 200 OK */
    int                 rtpengine_a_offer_pending;  /* general-pool: async ng offer */
    int                 rtpengine_a_need_advertise; /* CONFIRMED before offer ready */
    int                 rtpengine_a_reinvite_done; /* A re-INVITE advertised RTPengine ports */
    int                 rtpengine_a_ep_changed; /* A-facing port changed after B answer */
    char                rtpengine_a_play_file[256]; /* last play-media file on A */
    int                 rtpengine_a_play_loop;
    int                 rtpengine_media_blocked; /* A↔B forward blocked during B collect */
    cc_rtp_ep_t         rtpengine_ep_a; /* ports A should send to (answer) */
    cc_rtp_ep_t         rtpengine_ep_b; /* ports B should send to (offer)  */

    int                 update_a_pending; /* rewrite A UPDATE SDP with B RTP */
    int                 update_b_pending; /* rewrite B UPDATE SDP with A RTP */
    int                 reinvite_a_pending; /* rewrite A re-INVITE SDP with B RTP */
    int                 reinvite_b_pending; /* rewrite B re-INVITE SDP with A RTP */
    int                 update_a_sent;   /* A-leg UPDATE pjsua_call_update() succeeded */
    int                 update_b_sent;   /* B-leg UPDATE pjsua_call_update() succeeded */
    int                 update_a_acked;  /* A-leg UPDATE 200 OK received      */
    int                 update_b_acked;  /* B-leg UPDATE 200 OK received      */
    int                 update_b_retry_pending; /* retry thread spawned for 491 */
    int                 update_a_retry_pending; /* retry thread spawned for A-leg 491 */
    int                 update_ack_watchdog_started; /* 200 OK timeout watchdog spawned */
    int                 media_bypassed;  /* conf slots silenced after bypass  */
    int                 bypass_rtp_watchdog_started; /* fallback watchdog spawned */
    int                 b_reinvite_active; /* SBC re-INVITE in progress on B-leg; block UPDATE */
    int                 b_on_hold;       /* 1 while B-leg is on hold (sendonly) */
    pjsua_player_id     hold_player_a;   /* MOH player on A during B hold     */
    int                 a_on_hold;       /* 1 while A-leg is on hold (sendonly) */
    pjsua_player_id     hold_player_b;   /* MOH player on B during A hold     */

    /* Mode 2 (UPDATE relay) hold/resume propagation state */
    int                 b_update_allowed;    /* 1 if Allow: UPDATE seen on B-leg dialog */
    int                 hold_propagate_pending; /* 1 while hold UPDATE to B is in-flight */
    int                 resume_propagate_pending; /* 1 while resume UPDATE to B is in-flight */
    int                 hold_update_b_sent;  /* 1 after hold UPDATE/re-INVITE sent to B */
    int                 hold_update_b_acked; /* 1 after hold UPDATE 200 OK from B */
    int                 hold_sdp_b_pending;  /* rewrite B hold UPDATE SDP: rtp_a + sendonly */
    int                 resume_sdp_b_pending;/* rewrite B resume UPDATE SDP: rtp_a + sendrecv */
    int                 hold_sdp_direction;  /* 1=sendonly 2=sendrecv for hold/resume SDP rewrite */

	int a_update_allowed;           /* mirrors b_update_allowed */
	int hold_propagate_a_pending;
	int resume_propagate_a_pending;
	int hold_sdp_a_pending;         /* armed before pjsua_call_update(call_a) */
	int resume_sdp_a_pending;
	int hold_update_a_sent;
	int hold_update_a_acked;

    cc_sip_hdr_t        fwd_hdrs[CC_MAX_FWD_HDRS];
    int                 fwd_hdr_count;

    char                a_rr_host[128];  /* SSP/SBC extracted from A-leg topmost Record-Route */
    int                 a_rr_port;       /* SSP port; 0 = not captured */

    pjsua_player_id     player_a;      /* A-leg WAV player                  */
    pjsua_player_id     player_b;      /* B-leg WAV player                  */
    void               *originate_arg; /* cc_originate_arg_t* set before worker post */
} cc_session_t;

/* ── Pool lifecycle ─────────────────────────────────────────────────────── */
void          cc_session_pool_init(void);    /* call once after pjsua_create() */
void          cc_session_pool_destroy(void); /* call before pjsua_destroy()    */

/* ── Lifecycle ──────────────────────────────────────────────────────────── */
cc_session_t *cc_session_create(void);  /* uses internal bounded pool — no pf arg */
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
#define CC_SESSION_LOCK(s)   pthread_mutex_lock((s)->lock)
#define CC_SESSION_UNLOCK(s) pthread_mutex_unlock((s)->lock)

#endif /* CC_SESSION_H */
